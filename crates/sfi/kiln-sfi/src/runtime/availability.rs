// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources: modules/mod-runtime/lua/ingame/check_ingame_access.rs:12-414 (identical in both
// pins; what the host answers for each check), session/record_runtime_state.rs:3-30,61-81 in
// 1.7.9 (3-35,66-86 in 1.8.0, which adds only the native-bridge game-access line; active owners
// and the effect record), game-runtime/compatibility/lib.rs:30-44 (an operation outside the
// contract).
//! The runtime host the runtime registrar gets for each package in production.
//!
//! The registrar (`sfi_core::bindings::runtime`) owns ShroudForge's whole policy: the order of the
//! checks, the reasons, the argument coercion. This module answers its questions with Kiln
//! facts:
//! - [`KilnRuntimeHost::owner_context`]: bound once, at registration, from the package's own
//!   manifest (id, Runtime/Patch/Export permissions) and the phase context, with the phase's
//!   global patch/export switches kept apart from those permissions. Never a caller's.
//! - static support and provider facts: [`RuntimeFacts`], from the [`Catalog`] and the
//!   service adapter (member present + kiln.caps row), read again on every query.
//! - lifecycle activity: the package's own lifecycle state ([`OwnerActivity`]).
//! - the write fault: the runtime-create's [`WriteFaultLatch`] against the live session
//!   (`kiln.world.game_world_session`).
//! - The live world context ([`WorldContext`], attached by the `runtime.world` context family's hook):
//!   the `WorldContextActive` fact and `runtime.world.context.active`'s world-operation fact are one sample of it
//!   per query ([`RuntimeFacts::world_snapshot`] for the family's own functions, behind the same row).
//! - effect reports: [`EffectStore`] (truncated, deduplicated, one record per package).
//!   The records stay in memory, in this runtime-create, until the status writer (the
//!   runtime status file ShroudForge's `runtime_effects()` feeds) publishes them; that writer
//!   also clears a package's record when its lifecycle stops, as ShroudForge's
//!   `set_runtime_mod_active(false)` does (record_runtime_state.rs:18-25), keeping a
//!   `restart-required` record.
//!
//! One [`RuntimeFacts`] per runtime-create, on the worker thread, shared by every package's
//! host and reachable by the families through `RuntimeContext::facts` (the CAS core and the guarded-write
//! callers report their outcomes to its latch).

use super::operation_catalog::{Backing, Catalog, Fact, Need};
use super::overrides::{OwnershipStore, session_world_kind};
use super::services::ServiceAdapter;
use super::world::context::{Snapshot, WorldContext};
use super::write_fault::WriteFaultLatch;
use crate::service_member;
use crate::worker::compose::RuntimeHosts;
use kiln_abi::KILN_WORLD_SERVER;
use kiln_abi::KilnStr;
use kiln_abi::services::kiln_caps::{
    KILN_CAP_AVAILABLE, KILN_CAP_UNAVAILABLE, KILN_CAPS_SERVICE, KILN_CAPS_VERSION, KilnCapRow,
    KilnCapsApi,
};
use kiln_abi::services::kiln_world::{KILN_WORLD_SERVICE, KILN_WORLD_VERSION, KilnWorldApi};
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::host::{HostContext, Side};
use sfi_core::lifecycle::OwnerState;
use sfi_core::package::Manifest;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

/// The game build text of `operation <op> has not been verified for game build <build>` when
/// the plan has none (ShroudForge prints its archive's game version there).
pub const GAME_BUILD_UNKNOWN: &str = "unknown";

/// The phase-wide facts every package's host shares.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FactsConfig {
    /// `loader.features.patch`'s global half (ShroudForge's `AppFeatures::PATCH`).
    pub global_patch: bool,
    /// `loader.features.export`'s global half (`AppFeatures::EXPORT`).
    pub global_export: bool,
    /// The game build text of the unverified-operation reason.
    pub game_build: String,
}

/// A kiln.caps row as availability reads it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CapabilityRow {
    Available,
    Pending,
    Unavailable,
    /// No row of that name, or no kiln.caps: treated as unavailable.
    Unknown,
}

/// The runtime facts of one runtime-create.
pub struct RuntimeFacts {
    services: Rc<ServiceAdapter>,
    catalog: &'static Catalog,
    config: FactsConfig,
    latch: WriteFaultLatch,
    effects: EffectStore,
    ownership: Rc<OwnershipStore>,
    /// The runtime-create's live world context, once the context family's hook made it.
    world: RefCell<Option<Rc<WorldContext>>>,
}

impl RuntimeFacts {
    pub fn new(
        services: Rc<ServiceAdapter>,
        catalog: &'static Catalog,
        config: FactsConfig,
    ) -> Self {
        Self {
            services,
            catalog,
            config,
            latch: WriteFaultLatch::new(),
            effects: EffectStore::default(),
            ownership: Rc::new(OwnershipStore::new()),
            world: RefCell::new(None),
        }
    }
    pub fn catalog(&self) -> &'static Catalog {
        self.catalog
    }
    pub(crate) fn for_reload(&self) -> Self {
        Self {
            latch: self.latch.for_reload(),
            ..Self::new(self.services.clone(), self.catalog, self.config.clone())
        }
    }
    pub fn config(&self) -> &FactsConfig {
        &self.config
    }
    /// The session write latch: the CAS core and the guarded-write callers report their outcomes here.
    pub fn write_faults(&self) -> &WriteFaultLatch {
        &self.latch
    }
    pub fn effects(&self) -> &EffectStore {
        &self.effects
    }
    /// The runtime-create's ownership store, shared by every package's claims.
    pub fn ownership(&self) -> &Rc<OwnershipStore> {
        &self.ownership
    }

    /// Attaches the runtime-create's live world context (the first one stays: a reload's facts get the
    /// same one).
    pub fn attach_world(&self, world: Rc<WorldContext>) {
        let mut slot = self.world.borrow_mut();
        if slot.is_none() {
            *slot = Some(world);
        }
    }
    /// The attached live world context, if any.
    pub fn world(&self) -> Option<Rc<WorldContext>> {
        self.world.borrow().clone()
    }
    fn world_read(&self) -> Snapshot {
        self.world().map_or(Snapshot::NONE, |world| world.read())
    }
    /// The live world context now, behind the `WorldContextActive` fact's row (its members and capability
    /// row, as [`Self::provider_facts`] reads it); [`Snapshot::NONE`] without that row, without an attached context
    /// or without a live world.
    pub fn world_snapshot(&self) -> Snapshot {
        if self.context_row() {
            self.world_read()
        } else {
            Snapshot::NONE
        }
    }
    /// Whether the `WorldContextActive` fact's row (its members and capability row) holds now.
    fn context_row(&self) -> bool {
        self.catalog
            .facts
            .iter()
            .find(|row| row.fact == Fact::WorldContextActive)
            .is_some_and(|row| self.satisfied(row.backing))
    }

    /// The row of capability `name` now (kiln.caps `find`).
    pub fn capability(&self, name: &str) -> CapabilityRow {
        let Ok(table) = self
            .services
            .bind::<KilnCapsApi>(KILN_CAPS_SERVICE, KILN_CAPS_VERSION)
        else {
            return CapabilityRow::Unknown;
        };
        let Ok(find) = service_member!(table, KilnCapsApi, find) else {
            return CapabilityRow::Unknown;
        };
        // SAFETY: plain C record; every field valid when zeroed.
        let mut row: KilnCapRow = unsafe { std::mem::zeroed() };
        row.size = std::mem::size_of::<KilnCapRow>() as u32;
        let text = KilnStr {
            ptr: name.as_ptr().cast(),
            len: name.len(),
        };
        // SAFETY: kiln_caps.h find: a name readable for len bytes, out with its size set.
        if unsafe { self.services.call(find, (text, &raw mut row)) } != kiln_abi::KILN_OK {
            return CapabilityRow::Unknown;
        }
        match row.state {
            s if s == KILN_CAP_AVAILABLE as u32 => CapabilityRow::Available,
            s if s == KILN_CAP_UNAVAILABLE as u32 => CapabilityRow::Unavailable,
            // kiln_caps.h: PENDING, and any state a newer core adds, is "not decided yet".
            _ => CapabilityRow::Pending,
        }
    }

    fn present(&self, need: &Need) -> bool {
        (need.member)(&self.services).is_ok()
    }

    /// Static support: the interpreter implements it, or its members are all here and none
    /// of its capability rows is unavailable (or unknown); a pending row is not a no.
    pub fn static_support(&self, operation: RuntimeOperation) -> Availability {
        let supported = match self.catalog.row(operation).map(|r| r.backing) {
            Some(Backing::Interpreter) => true,
            Some(Backing::Kiln(needs)) => needs.iter().all(|need| {
                self.present(need)
                    && matches!(
                        self.capability(need.capability),
                        CapabilityRow::Available | CapabilityRow::Pending
                    )
            }),
            Some(Backing::NotLanded(_)) | None => false,
        };
        if supported {
            Availability::Available
        } else {
            Availability::Unavailable {
                reason: format!(
                    "operation {} has not been verified for game build {}",
                    operation.as_str(),
                    self.config.game_build
                ),
            }
        }
    }

    fn satisfied(&self, backing: Backing) -> bool {
        match backing {
            Backing::Interpreter => true,
            Backing::Kiln(needs) => needs.iter().all(|need| {
                self.present(need) && self.capability(need.capability) == CapabilityRow::Available
            }),
            Backing::NotLanded(_) => false,
        }
    }

    /// One coherent copy of the provider facts, read now. The world context is sampled once for every fact it
    /// decides.
    pub fn provider_facts(&self) -> RuntimeProviderFacts {
        let mut facts = RuntimeProviderFacts::default();
        let mut world: Option<Snapshot> = None;
        let mut active = || world.get_or_insert_with(|| self.world_read()).active();
        for row in self.catalog.facts {
            if !self.satisfied(row.backing) {
                continue;
            }
            if let Some(read) = row.read {
                read(&self.services, &mut facts);
                continue;
            }
            match row.fact {
                // ShroudForge's ready(): the family's world as kiln.world's ECS context sees it.
                Fact::Ready => facts.ready = self.ecs_ready(),
                // ShroudForge's can_write(): the compare-exchange writes reach the server world only.
                Fact::CanWrite => facts.can_write = self.ecs_can_write(),
                // ShroudForge's resource can_write(): the loaded-resource writes reach the server world only.
                Fact::ResourceCanWrite => facts.resource_can_write = self.resource_can_write(),
                Fact::GameplayPatchAvailable => facts.gameplay_patch_available = true,
                Fact::NetworkAvailable => facts.network_available = true,
                Fact::NetworkServiceReady => facts.network_service_ready = true,
                // ShroudForge's world_context_active() is the live context, not a presence.
                Fact::WorldContextActive => facts.world_context_active = active(),
                Fact::WorldEntityQueryPropsReady => facts.world_entity_query_props_ready = true,
                Fact::WorldEntityContextReady => facts.world_entity_context_ready = true,
                Fact::CanWriteCurrentTransform => facts.can_write_current_transform = true,
                // `world_operation_available("runtime.world.context.active")` is the live context too
                // (track_world_context.cpp:185-186); the others are presence until their families say more.
                Fact::WorldOperation(op) => {
                    facts.world_operations[op as usize] =
                        op != RuntimeOperation::WorldContextActive || active()
                }
                // Values come from a reader only (Catalog::check).
                Fact::MainMenu | Fact::CharacterName => {}
            }
        }
        facts
    }

    /// The world ShroudForge's ECS binds now and its live session ([`WorldContext::ecs_world`]: on a server and on a
    /// hosting client the server world, on a joined client the client world), behind the `WorldContextActive` fact's
    /// row as [`Self::world_snapshot`]. Without that row or a live world: the plan side's world and 0; without an
    /// attached context: the server world and 0.
    pub fn ecs_world(&self) -> (u32, u64) {
        let Some(world) = self.world() else {
            return (KILN_WORLD_SERVER as u32, 0);
        };
        if self.context_row() {
            let (kind, session) = world.ecs_world();
            if session != 0 {
                return (kind, session);
            }
        }
        (session_world_kind(world.side()), 0)
    }
    /// The world the ECS facts are about ([`Self::ecs_world`]'s kind).
    pub fn ecs_world_kind(&self) -> u32 {
        self.ecs_world().0
    }
    /// ShroudForge's `runtime_provider::ready()` (behind the `Ready` row): kiln.world's ECS context has an exact
    /// profile and a live session of [`Self::ecs_world_kind`].
    fn ecs_ready(&self) -> bool {
        super::ecs::ready(&self.services, self.ecs_world_kind())
    }
    /// ShroudForge's resource `can_write()` (behind the `ResourceCanWrite` row): the ECS world is the server world (a
    /// dedicated server's, or the one a client hosts); a joined client's resources are never written. Each write still
    /// answers its own refusal outside a world (kiln.assets' compare-exchange needs the live server session).
    fn resource_can_write(&self) -> bool {
        !self.latch.resource_latched() && self.ecs_world_kind() == super::ecs::SERVER_WORLD
    }
    fn ecs_can_write(&self) -> bool {
        self.ecs_world_kind() == super::ecs::SERVER_WORLD && self.ecs_ready()
    }

    /// ShroudForge's world session marker for world `world_kind` (its `world_session_id`, which claims and
    /// override targets bind to): the live context's session ([`Self::world_snapshot`]) when `world_kind` is the
    /// attached context's session world, the hosted server world's on a hosting client ([`Self::ecs_world`]), else
    /// 0. Unlike [`Self::world_session`] (kiln.world's raw generation, which stays nonzero after a world stops), it is
    /// 0 once the world is no longer live.
    pub fn live_session(&self, world_kind: u32) -> u64 {
        match self.world() {
            Some(world) if session_world_kind(world.side()) == world_kind => {
                self.world_snapshot().session
            }
            Some(_) => match self.ecs_world() {
                (kind, session) if kind == world_kind => session,
                _ => 0,
            },
            None => 0,
        }
    }

    /// The live session of a world kind (`kiln.world.game_world_session`), 0 when there is
    /// none or this core cannot say.
    pub fn world_session(&self, world_kind: u32) -> u64 {
        let Ok(table) = self
            .services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        else {
            return 0;
        };
        let Ok(session) = service_member!(table, KilnWorldApi, game_world_session) else {
            return 0;
        };
        // SAFETY: kiln_world.h: any thread, lock-free, any world kind.
        unsafe { self.services.call(session, (world_kind,)) }
    }

    /// ShroudForge's `runtime_write_fault()`: the latched reason while its session is live.
    pub fn write_fault(&self) -> Option<String> {
        self.latch.active(|kind| self.world_session(kind))
    }
}

/// One package's `runtime.report_effect` record (`record_runtime_state.rs:70-79`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Effect {
    pub state: EffectState,
    /// "server" or "client".
    pub process_target: &'static str,
    /// Always "this-process".
    pub scope: &'static str,
    /// The first 240 Unicode scalar values of the detail.
    pub detail: String,
    /// Unix seconds of the last change.
    pub updated_at: u64,
}

/// The effect records of one runtime-create, by package id.
#[derive(Debug, Default)]
pub struct EffectStore(RefCell<BTreeMap<String, Effect>>);

impl EffectStore {
    /// ShroudForge's `report_runtime_effect`: the detail cut to 240 scalar values; the same
    /// state and detail again changes nothing (the time stays).
    pub fn report(&self, id: &str, side: Side, state: EffectState, detail: &str) {
        let detail: String = detail.chars().take(240).collect();
        let mut effects = self.0.borrow_mut();
        if effects
            .get(id)
            .is_some_and(|e| e.state == state && e.detail == detail)
        {
            return;
        }
        let updated_at = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        effects.insert(
            id.to_owned(),
            Effect {
                state,
                process_target: if side == Side::Server {
                    "server"
                } else {
                    "client"
                },
                scope: "this-process",
                detail,
                updated_at,
            },
        );
    }
    pub fn get(&self, id: &str) -> Option<Effect> {
        self.0.borrow().get(id).cloned()
    }
    pub fn snapshot(&self) -> BTreeMap<String, Effect> {
        self.0.borrow().clone()
    }
}

/// Whether a package's lifecycle is active now (`runtime_mod_is_active`).
pub trait OwnerActivity {
    fn active(&self) -> bool;
}
impl OwnerActivity for OwnerState {
    fn active(&self) -> bool {
        OwnerState::active(self)
    }
}

/// The production runtime host of one package.
pub struct KilnRuntimeHost {
    owner: RuntimeOwnerContext,
    activity: Rc<dyn OwnerActivity>,
    facts: Rc<RuntimeFacts>,
}

impl KilnRuntimeHost {
    /// Binds the package's identity and permissions now, from its own manifest.
    pub fn new(
        manifest: &Manifest,
        context: &HostContext,
        activity: Rc<dyn OwnerActivity>,
        facts: Rc<RuntimeFacts>,
    ) -> Self {
        let config = facts.config();
        Self {
            owner: RuntimeOwnerContext::new(
                manifest,
                context,
                config.global_patch,
                config.global_export,
                config.game_build.clone(),
            ),
            activity,
            facts,
        }
    }
    pub fn facts(&self) -> &Rc<RuntimeFacts> {
        &self.facts
    }
}

impl RuntimeHost for KilnRuntimeHost {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.owner
    }
    fn static_support(&self, operation: RuntimeOperation) -> Availability {
        self.facts.static_support(operation)
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        self.facts.provider_facts()
    }
    fn lifecycle_active(&self) -> bool {
        self.activity.active()
    }
    fn write_fault(&self) -> Option<String> {
        self.facts.write_fault()
    }
    fn report_effect(&self, state: EffectState, detail: &str) {
        self.facts
            .effects
            .report(self.owner.id(), self.owner.side(), state, detail);
    }
}

/// Each package's lifecycle state, by id, shared by its host and its lifecycle registration.
pub type OwnerStates = Rc<RefCell<BTreeMap<String, Rc<OwnerState>>>>;

/// The production `RuntimeHosts` of one runtime-create: a [`KilnRuntimeHost`] per package,
/// with that package's own entry of `states` (made here when it is not there yet).
pub fn hosts(facts: Rc<RuntimeFacts>, states: OwnerStates) -> RuntimeHosts {
    Rc::new(move |manifest: &Manifest, context: &HostContext| {
        let state = states
            .borrow_mut()
            .entry(manifest.id.clone())
            .or_default()
            .clone();
        Rc::new(KilnRuntimeHost::new(
            manifest,
            context,
            state,
            facts.clone(),
        )) as Rc<dyn RuntimeHost>
    })
}