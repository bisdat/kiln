// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (identical in both pins): modules/mod-runtime/lua/ingame/overrides/
// claim_runtime_storage.rs:141-363 (RuntimeWriteClaim, require_session, get_claim, acquire,
// release, the Lua methods and the factories' common tail); lua/ingame/check_ingame_access.rs:
// 37-54 (availability of runtime.ecs.write / runtime.assets.write), 386-414 (the denial).
//! The `RuntimeWriteClaim` token and the claim factories.
//!
//! A factory ([`get_claim`] and the per-kind helpers) only needs a nonzero live session equal
//! to the one the storage was resolved in, and a token: it does not look at the package's
//! lifecycle, its permissions or the write fault. Those are checked by every
//! [`RuntimeWriteClaim::acquire`], in ShroudForge's order:
//! 1. the session the claim was made in is still the live one;
//! 2. for each operation of the storage (`runtime.ecs.write`, plus `runtime.assets.write` for
//!    a resource): the registrar's denial (phase, `runtime` permission, active lifecycle, the session
//!    write fault, static support), then its availability (`<op> is currently unavailable`);
//! 3. the family's own [`AcquireCheck`] (a component's layout and write authority);
//! 4. the session again, then the store, pruned to that session, records the token
//!    (another mod's record refuses with its label).
//!
//! `release` takes the token off only when this claim had acquired it, once. A claim dropped
//! while active is not released (no `Drop`): its record stays, as ShroudForge's does.
//!
//! Every claim's owner is the package whose host the [`ClaimAuthority`] wraps, bound when
//! the family made it, never a caller's.

use super::store::{OwnershipStore, ResourceKey, StorageKey};
use crate::runtime::availability::RuntimeFacts;
use mlua::{AnyUserData, Lua, UserData, UserDataMethods};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation, runtime_denial_reason};
use sfi_core::host::Phase;
use std::cell::Cell;
use std::rc::Rc;

/// The refusal when a claim's session is 0 or no longer the live one.
pub const SESSION_REFUSAL: &str = "runtime claim belongs to another or an inactive world session";

/// What a claim checks against: its package and the live world session.
pub trait ClaimAuthority {
    /// The owning package's id (the store's owner).
    fn owner_id(&self) -> &str;
    /// The live world session now (0: none).
    fn world_session(&self) -> u64;
    /// The registrar's denial of `operation` for this package now (`runtime_denial_reason`).
    fn denial(&self, operation: RuntimeOperation) -> Option<String>;
    /// Whether `operation` is available to this package now (ShroudForge's `available`).
    /// Claims only ask for `runtime.ecs.write` and `runtime.assets.write`.
    fn available(&self, operation: RuntimeOperation) -> bool;
}

/// The production authority: the package's runtime host (the registrar's checks) and a source of
/// the live world session.
pub struct HostAuthority {
    host: Rc<dyn RuntimeHost>,
    session: Box<dyn Fn() -> u64>,
}

impl HostAuthority {
    /// `session` gives the live world session the family's storage belongs to.
    pub fn new(host: Rc<dyn RuntimeHost>, session: impl Fn() -> u64 + 'static) -> Self {
        Self {
            host,
            session: Box::new(session),
        }
    }
    /// The session is ShroudForge's world session marker of `world_kind`, read through `facts`
    /// ([`RuntimeFacts::live_session`], the live world context): 0 once that world stops, though
    /// kiln.world's raw generation stays nonzero then.
    pub fn on_world(host: Rc<dyn RuntimeHost>, facts: Rc<RuntimeFacts>, world_kind: u32) -> Self {
        Self::new(host, move || facts.live_session(world_kind))
    }
}

impl ClaimAuthority for HostAuthority {
    fn owner_id(&self) -> &str {
        self.host.owner_context().id()
    }
    fn world_session(&self) -> u64 {
        (self.session)()
    }
    fn denial(&self, operation: RuntimeOperation) -> Option<String> {
        runtime_denial_reason(self.host.as_ref(), operation)
    }
    fn available(&self, operation: RuntimeOperation) -> bool {
        // check_ingame_access.rs:43-54: phase, the operation in the runtime contract, the
        // runtime permission, then the provider's write fact.
        let owner = self.host.owner_context();
        if owner.phase() != Phase::Runtime
            || !self.host.static_support(operation).is_available()
            || !owner.has_runtime()
        {
            return false;
        }
        let facts = self.host.provider_facts();
        match operation {
            RuntimeOperation::EcsWrite => facts.can_write,
            RuntimeOperation::AssetsWrite => facts.resource_can_write,
            _ => false,
        }
    }
}

/// A family's own acquire check, run after the operation checks (ShroudForge's component
/// branch: layout resolved, write authority, the entity readable).
pub trait AcquireCheck {
    fn check(&self) -> Result<(), String>;
}

impl<F: Fn() -> Result<(), String>> AcquireCheck for F {
    fn check(&self) -> Result<(), String> {
        self()
    }
}

/// One mod's claim token on one storage.
pub struct RuntimeWriteClaim {
    authority: Rc<dyn ClaimAuthority>,
    store: Rc<OwnershipStore>,
    key: StorageKey,
    session: u64,
    token: u64,
    active: Cell<bool>,
    check: Option<Rc<dyn AcquireCheck>>,
}

fn require_session(authority: &dyn ClaimAuthority, session: u64) -> Result<(), String> {
    if session == 0 || authority.world_session() != session {
        return Err(SESSION_REFUSAL.into());
    }
    Ok(())
}

impl RuntimeWriteClaim {
    pub fn key(&self) -> &StorageKey {
        &self.key
    }
    pub fn session(&self) -> u64 {
        self.session
    }
    pub fn token(&self) -> u64 {
        self.token
    }
    pub fn owner(&self) -> &str {
        self.authority.owner_id()
    }
    /// Whether this claim holds its token in the store now.
    pub fn is_active(&self) -> bool {
        self.active.get()
    }

    /// Takes (or keeps) this claim's token on its storage, after the checks in the module
    /// docs. A failed acquire leaves an earlier successful one in place.
    pub fn acquire(&self) -> Result<(), String> {
        require_session(self.authority.as_ref(), self.session)?;
        for operation in self.key.operations() {
            if let Some(reason) = self.authority.denial(*operation) {
                return Err(reason);
            }
            if !self.authority.available(*operation) {
                return Err(format!("{} is currently unavailable", operation.as_str()));
            }
        }
        if let Some(check) = &self.check {
            check.check()?;
        }
        require_session(self.authority.as_ref(), self.session)?;
        self.store.prune(self.session);
        self.store
            .acquire(self.authority.owner_id(), &self.key, self.token)?;
        self.active.set(true);
        Ok(())
    }

    /// Gives the token back if this claim holds it; otherwise nothing.
    pub fn release(&self) {
        if self.active.replace(false) {
            self.store
                .release(self.authority.owner_id(), &self.key, self.token);
        }
    }
}

impl UserData for RuntimeWriteClaim {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("acquire", |_, this, ()| {
            Ok(match this.acquire() {
                Ok(()) => (true, None),
                Err(reason) => (false, Some(reason)),
            })
        });
        methods.add_method("release", |_, this, ()| {
            this.release();
            Ok(())
        });
    }
}

/// ShroudForge's `get_claim`: a new, inactive claim of `key` for the authority's package, if
/// `session` is nonzero and live; then a token (`runtime claim token space exhausted`).
pub fn get_claim(
    store: &Rc<OwnershipStore>,
    authority: Rc<dyn ClaimAuthority>,
    key: StorageKey,
    session: u64,
    check: Option<Rc<dyn AcquireCheck>>,
) -> Result<RuntimeWriteClaim, String> {
    require_session(authority.as_ref(), session)?;
    let token = store.token()?;
    Ok(RuntimeWriteClaim {
        authority,
        store: store.clone(),
        key,
        session,
        token,
        active: Cell::new(false),
        check,
    })
}

/// ShroudForge's `component_target`: a component's key in the live session.
pub fn component_key(
    authority: &dyn ClaimAuthority,
    entity: u32,
    qualified_name: &str,
) -> StorageKey {
    StorageKey::Component {
        session: authority.world_session(),
        entity,
        qualified_name: qualified_name.into(),
    }
}

/// The common tail of `get_component_claim`, after the component family resolved and
/// checked the type: the live session's key, and `check` (layout, write authority) for each
/// acquire. The live session is read once for the key and the token, as ShroudForge does: a
/// world change after that read is refused by `get_claim`'s session check.
pub fn component_claim(
    store: &Rc<OwnershipStore>,
    authority: Rc<dyn ClaimAuthority>,
    entity: u32,
    qualified_name: &str,
    check: Option<Rc<dyn AcquireCheck>>,
) -> Result<RuntimeWriteClaim, String> {
    let session = authority.world_session();
    let key = StorageKey::Component {
        session,
        entity,
        qualified_name: qualified_name.into(),
    };
    get_claim(store, authority, key, session, check)
}

/// The tail of `get_resource_claim`: `session` is the session the loaded resource's write
/// authority belongs to (the resource family's `require_runtime_write_session`).
pub fn resource_claim(
    store: &Rc<OwnershipStore>,
    authority: Rc<dyn ClaimAuthority>,
    resource: ResourceKey,
    session: u64,
) -> Result<RuntimeWriteClaim, String> {
    get_claim(
        store,
        authority,
        StorageKey::Resource(resource),
        session,
        None,
    )
}

/// The tail of `get_template_claim`: a template component shares its resource's ownership.
pub fn template_claim(
    store: &Rc<OwnershipStore>,
    authority: Rc<dyn ClaimAuthority>,
    resource: ResourceKey,
    session: u64,
) -> Result<RuntimeWriteClaim, String> {
    resource_claim(store, authority, resource, session)
}

/// The tail of `get_service_claim`: the view's ownership identity (key, session).
pub fn service_claim(
    store: &Rc<OwnershipStore>,
    authority: Rc<dyn ClaimAuthority>,
    key: &str,
    session: u64,
) -> Result<RuntimeWriteClaim, String> {
    get_claim(
        store,
        authority,
        StorageKey::Service {
            session,
            key: key.into(),
        },
        session,
        None,
    )
}

/// ShroudForge's `check_write` for a typed writer of `authority`'s package.
pub fn check_write(
    store: &OwnershipStore,
    authority: &dyn ClaimAuthority,
    key: &StorageKey,
) -> Result<(), String> {
    store.check_write(authority.owner_id(), key, authority.world_session())
}

/// A factory's Lua answer: `(claim, nil)` or `(nil, reason)`.
pub fn claim_result(
    lua: &Lua,
    claim: Result<RuntimeWriteClaim, String>,
) -> mlua::Result<(Option<AnyUserData>, Option<String>)> {
    Ok(match claim {
        Ok(claim) => (Some(lua.create_userdata(claim)?), None),
        Err(reason) => (None, Some(reason)),
    })
}
