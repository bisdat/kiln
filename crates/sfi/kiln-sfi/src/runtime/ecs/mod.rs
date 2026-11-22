// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/mod.rs:19-39 (the members of runtime.ecs),
// lua/ingame/mod.rs:129-149 (`runtime_type_name`), lua/ingame/check_ingame_access.rs:12-60 (`available`),
// game-runtime/bridge/windows/ecs/{read_component,read_entity_template_uuid}.rs (the provider's reads).
//! `runtime.ecs`: the ECS registry and catalog, the entity queries, the component reads and the plain component
//! writes.
//!
//! - [`registry`]: kiln.types' `ecs_registry` joined with the archive registry by qualified hash and name, with a
//!   size proof per row (ShroudForge's `runtime_provider::resolve`).
//! - [`catalog`]: `get_catalog`, `get_registry`, `get_component`, `get_components` on that join, the package
//!   host's facts and kiln.world's `ecs_write_policy`.
//! - [`handles`]: ShroudForge's opaque entity handles on kiln.world's self-validating refs.
//! - [`entities`]: `query`, `query_bounds` and `resolve` on kiln.world's `ecs_query` and `entity_open`.
//! - [`snapshots`]: `read` and `read_bytes` on `ecs_snapshot` (always a stable copy), decoded by the VM's typed
//!   values (kiln.eml.values).
//! - [`fields`]: ShroudForge's reflected inline field paths and `read_fields`.
//! - [`template`]: `get_template` on `ecs_template_uuid`.
//! - [`cas`]: the compare-exchange core on `ecs_compare_exchange` (conflicts, busy, the uncertain latch) and
//!   `write`; [`fields`] has `write_fields` on it, and `crate::runtime::overrides::targets::component` the
//!   component claim and override target the family installs in `runtime.overrides`.
//!
//! **The API other families build on** (the world cursor and props): one [`Ecs`] per VM holds the handle table and
//! the join, and offers [`Ecs::open`] (an entity id to a handle), [`Ecs::handle_for`] (a ref kiln.world gave to a
//! handle), [`Ecs::object`] (a handle back to its ref), [`Ecs::query_refs`], [`Ecs::snapshot`] (one component's
//! bytes, by hash) and [`Ecs::template_uuid`], each a direct any-thread kiln.world member under kiln-sfi's owner (also
//! every ref's reload epoch, so refs are the owner's own: kiln_world.h's owner scoping). A family that shares the
//! VM's [`Ecs`] (through [`family`]'s installs) gives mods handles that `runtime.ecs.read` accepts.
//!
//! The family ([`family`]) claims its names ([`CLAIMS`]) inside the installer's `runtime.ecs` and
//! `runtime.overrides` containers and
//! shares one [`Ecs`] between the packages of one runtime-create. Every call runs on the worker and calls Kiln members
//! directly through the service adapter: no hop, no wait.

pub mod attributes;
pub mod cas;
pub mod catalog;
pub mod entities;
pub mod fields;
pub mod handles;
pub mod registry;
pub mod snapshots;
pub mod template;
pub mod template_component;

use super::availability::RuntimeFacts;
use super::context::RuntimeContext;
use super::install::Family;
use super::services::{ServiceAdapter, ServiceError, ServiceTable};
use super::types::{Registry, Session, TypesHandle, selectors};
use super::values::Values;
use crate::service_member;
use fields::InlineField;
use handles::Handles;
use kiln_abi::services::kiln_world::{
    KILN_ECS_OK, KILN_WORLD_SERVICE, KILN_WORLD_VERSION, KilnEcsContextInfo, KilnObjectRef,
    KilnWorldApi,
};
use kiln_abi::{KILN_OK, KILN_WORLD_KINDS, KILN_WORLD_SERVER, KilnEntity, KilnOwner};
use mlua::{Lua, Table, Value};
use registry::{EcsRegistry, Resolved};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op};
use sfi_core::host::Phase;
use sfi_core::lua::{FunctionArgs, LuaError};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "ecs-registry";

/// The names this family adds inside the installer's `runtime.ecs` and `runtime.overrides` containers.
pub const CLAIMS: [&str; 18] = [
    "runtime.ecs.get_components",
    "runtime.ecs.get_catalog",
    "runtime.ecs.get_registry",
    "runtime.ecs.get_component",
    "runtime.ecs.query",
    "runtime.ecs.query_bounds",
    "runtime.ecs.resolve",
    "runtime.ecs.read",
    "runtime.ecs.read_bytes",
    "runtime.ecs.read_fields",
    "runtime.ecs.get_template",
    "runtime.ecs.get_template_component",
    "runtime.ecs.write",
    "runtime.ecs.write_fields",
    "runtime.overrides.get_component_claim",
    "runtime.overrides.component",
    "runtime.overrides.get_template_claim",
    "runtime.overrides.template",
];

/// The capability row of kiln.world's ECS reads (kiln.caps; docs/kiln/services/caps.md).
pub const READ_CAPABILITY: &str = "world.ecs_read";
/// The capability row of kiln.world's compare-exchange writes.
pub const WRITE_CAPABILITY: &str = "world.ecs_write";

/// The prefix every ECS type of the archive registry has.
pub const ECS_PREFIX: &str = registry::ECS_PREFIX;

/// What the entity calls need from the runtime-create besides the adapter.
#[derive(Clone)]
pub struct EcsRuntime {
    /// kiln-sfi's plugin owner: every ref's owner and reload epoch.
    pub owner: KilnOwner,
    /// The VM's typed values (kiln.eml.values), which decode component bytes; `None` when the plan gives the VM none.
    pub values: Option<Values>,
    /// The runtime-create's facts: the session write latch and the ownership store.
    pub facts: Rc<RuntimeFacts>,
    /// The world ShroudForge's ECS binds now and its world session marker (`runtime_provider::world_session_id`): on a
    /// server and on a hosting client the server world, on a joined client the client world
    /// (`RuntimeFacts::ecs_world`); the session is 0 without a live one.
    pub world: Rc<dyn Fn() -> (u32, u64)>,
}

/// What the packages of one VM share: its type session (the Types and the checked archive registry, not the
/// registry's source), the adapter, the world kind whose catalog, entities and write policy are asked, the join's
/// cache, the handle table and the inline field plans.
pub struct Ecs {
    session: Rc<Session>,
    services: Rc<ServiceAdapter>,
    /// The world kind without the runtime-create's part (the plan side's).
    default_kind: u32,
    /// The registry join of each world kind.
    registries: Vec<EcsRegistry>,
    handles: Handles,
    runtime: RefCell<Option<EcsRuntime>>,
    fields: RefCell<HashMap<(usize, String), Rc<InlineField>>>,
}

/// The most inline field plans one VM keeps (ShroudForge's own bound, session/mod.rs:157-173).
const FIELD_PLANS: usize = 4096;

impl Ecs {
    pub fn new(session: Rc<Session>, services: Rc<ServiceAdapter>, world_kind: u32) -> Self {
        Self {
            session,
            services,
            default_kind: world_kind,
            registries: (0..KILN_WORLD_KINDS as u32).map(EcsRegistry::new).collect(),
            handles: Handles::new(),
            runtime: RefCell::new(None),
            fields: RefCell::new(HashMap::new()),
        }
    }
    /// The same, with the runtime-create's part (the entity calls refuse without one: owner 0 is never accepted).
    pub fn with_runtime(self, runtime: EcsRuntime) -> Self {
        *self.runtime.borrow_mut() = Some(runtime);
        self
    }
    /// The checked archive registry.
    pub fn types(&self) -> &Rc<Registry> {
        self.session.registry()
    }
    /// The one Type of registry `index` in this VM, or nil.
    pub fn type_value(&self, lua: &Lua, index: usize) -> mlua::Result<Value> {
        Ok(self.session.get_type(lua, index)?.unwrap_or(Value::Nil))
    }
    pub fn services(&self) -> &Rc<ServiceAdapter> {
        &self.services
    }
    /// The world ShroudForge's ECS binds now and its live session, in one sample (module docs); without the
    /// runtime-create's part the plan side's world and 0.
    pub fn world_now(&self) -> (u32, u64) {
        let world = self.runtime.borrow().as_ref().map(|r| r.world.clone());
        world.map_or((self.default_kind, 0), |world| world())
    }
    /// The kind of [`Self::world_now`].
    pub fn world_kind(&self) -> u32 {
        self.world_now().0
    }
    pub fn runtime(&self) -> Option<EcsRuntime> {
        self.runtime.borrow().clone()
    }
    /// kiln-sfi's owner (0 without the runtime-create's part).
    pub fn owner(&self) -> KilnOwner {
        self.runtime.borrow().as_ref().map_or(0, |r| r.owner)
    }
    /// ShroudForge's world session marker now (0 without one, or without the runtime-create's part).
    pub fn world_session(&self) -> u64 {
        self.world_now().1
    }
    /// The registry join of the world the ECS binds now.
    fn registry(&self) -> &EcsRegistry {
        let kind = self.world_kind() as usize;
        &self.registries[if kind < self.registries.len() {
            kind
        } else {
            self.default_kind as usize
        }]
    }
    pub fn handles(&self) -> &Handles {
        &self.handles
    }

    /// kiln.world's table.
    pub fn world(&self) -> Result<ServiceTable<KilnWorldApi>, ServiceError> {
        self.services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
    }

    /// ShroudForge's `runtime_provider::resolve(name)`: the storage type the join proved for that exact name.
    pub fn resolve_component(&self, name: &str) -> Option<Resolved> {
        self.registry()
            .read(&self.services, self.types())
            .ok()?
            .resolve(name)
    }

    /// The handle of a ref kiln.world gave (0 once the handle space is used up).
    pub fn handle_for(&self, object: &KilnObjectRef) -> u32 {
        self.handles.handle_for(object)
    }
    /// The ref a handle names, if this VM gave it, it is of the newest world and session seen, and of the world the
    /// ECS binds now (a handle of the world a client hosted reads nothing once it joined another).
    pub fn object(&self, handle: u32) -> Option<KilnObjectRef> {
        self.handles
            .get(handle)
            .filter(|object| object.world_kind == self.world_kind())
    }

    /// `entity_open` of entity id `entity` in the family's world, live session, as a handle: kiln.world's result
    /// when it refuses (or KILN_ERR_UNAVAILABLE without the member), and 0 is never a handle.
    pub fn open(&self, entity: KilnEntity) -> Result<u32, kiln_abi::KilnResult> {
        let owner = self.owner();
        let open = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, entity_open))
            .map_err(|_| kiln_abi::KILN_ERR_UNAVAILABLE)?;
        // SAFETY: an all-zero ref is valid (integers only).
        let mut object: KilnObjectRef = unsafe { std::mem::zeroed() };
        object.size = std::mem::size_of::<KilnObjectRef>() as u32;
        // SAFETY: kiln_world.h entity_open: plain values and a caller record.
        let result = unsafe {
            self.services.call(
                open,
                (
                    owner,
                    owner,
                    self.world_kind(),
                    0u64,
                    entity,
                    &raw mut object,
                ),
            )
        };
        if result != KILN_OK {
            return Err(result);
        }
        match self.handle_for(&object) {
            0 => Err(kiln_abi::KILN_ERR_UNAVAILABLE),
            handle => Ok(handle),
        }
    }

    /// One component's bytes (component hash `component`, exactly `size` of them) of the entity `handle` names: a
    /// stable copy (`ecs_snapshot`), validated by kiln.world before and after. `None` for an unknown handle, a
    /// refusal (stale, absent, unreadable, unavailable) or another size.
    pub fn snapshot(&self, handle: u32, component: u32, size: u32) -> Option<Vec<u8>> {
        self.snapshot_ref(&self.object(handle)?, component, size)
    }

    /// [`Self::snapshot`] of a ref kiln.world gave (no handle needed).
    pub fn snapshot_ref(
        &self,
        object: &KilnObjectRef,
        component: u32,
        size: u32,
    ) -> Option<Vec<u8>> {
        let snapshot = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, ecs_snapshot))
            .ok()?;
        let mut bytes = vec![0u8; size as usize];
        let mut required = 0u32;
        // SAFETY: kiln_world.h ecs_snapshot: the caller's ref, a buffer of `size` bytes and an out count.
        let result = unsafe {
            self.services.call(
                snapshot,
                (
                    self.owner(),
                    std::ptr::from_ref(object),
                    component,
                    bytes.as_mut_ptr().cast::<core::ffi::c_void>(),
                    size,
                    &raw mut required,
                ),
            )
        };
        (result == KILN_OK && required == size).then_some(bytes)
    }

    /// The template an entity was spawned from (its definition uuid, `ecs_template_uuid`), validated by kiln.world
    /// before and after; `None` for an unknown handle, a refusal or an entity that names none.
    pub fn template_uuid(&self, handle: u32) -> Option<[u8; 16]> {
        let object = self.object(handle)?;
        let uuid = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, ecs_template_uuid))
            .ok()?;
        let mut guid = [0u8; 16];
        // SAFETY: kiln_world.h ecs_template_uuid: the caller's ref and 16 bytes.
        let result = unsafe {
            self.services
                .call(uuid, (self.owner(), &raw const object, guid.as_mut_ptr()))
        };
        (result == KILN_OK && guid != [0u8; 16]).then_some(guid)
    }

    /// The inline field plan of `path` in registry type `root`, kept per VM (at most [`FIELD_PLANS`]; a full cache
    /// still answers, it only stops keeping new plans).
    pub fn inline_field(&self, root: usize, path: &str) -> Result<Rc<InlineField>, String> {
        if let Some(plan) = self.fields.borrow().get(&(root, path.to_owned())) {
            return Ok(plan.clone());
        }
        let plan = Rc::new(fields::inline_field(self.types(), root, path)?);
        let mut fields = self.fields.borrow_mut();
        if fields.len() < FIELD_PLANS {
            fields.insert((root, path.to_owned()), plan.clone());
        }
        Ok(plan)
    }

    /// The VM's typed values, or Kiln's error when the VM has none.
    pub fn values(&self) -> mlua::Result<Values> {
        self.runtime
            .borrow()
            .as_ref()
            .and_then(|r| r.values.clone())
            .ok_or_else(|| mlua::Error::runtime(NO_VALUES))
    }
}

/// Kiln's error when a component's bytes need decoding and the VM has no typed values.
pub const NO_VALUES: &str = "kiln: typed values (kiln.eml.values) are not available in this VM";

/// The ECS reads' members (presence only): entity_open, entity_validate, ecs_context_snapshot, ecs_query, ecs_snapshot
/// and ecs_template_uuid.
pub fn probe_reads(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, entity_open)?;
    service_member!(table, KilnWorldApi, entity_validate)?;
    service_member!(table, KilnWorldApi, ecs_context_snapshot)?;
    service_member!(table, KilnWorldApi, ecs_query)?;
    service_member!(table, KilnWorldApi, ecs_snapshot)?;
    service_member!(table, KilnWorldApi, ecs_template_uuid)?;
    Ok(())
}

/// The compare-exchange writes' members (presence only): ecs_write_policy and ecs_compare_exchange.
pub fn probe_writes(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, ecs_write_policy)?;
    service_member!(table, KilnWorldApi, ecs_compare_exchange)?;
    Ok(())
}

/// ShroudForge's `runtime_provider::ready()` for world `world_kind`: kiln.world's ECS context reads, with an exact
/// engine profile, a world of that kind in a nonzero session.
pub fn ready(services: &ServiceAdapter, world_kind: u32) -> bool {
    let Ok(table) = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION) else {
        return false;
    };
    let Ok(snapshot) = service_member!(table, KilnWorldApi, ecs_context_snapshot) else {
        return false;
    };
    // SAFETY: an all-zero record is valid (integers only).
    let mut info: KilnEcsContextInfo = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnEcsContextInfo>() as u32;
    // SAFETY: kiln_world.h ecs_context_snapshot: a sized caller record.
    let result = unsafe { services.call(snapshot, (&raw mut info,)) };
    let kind = world_kind as usize;
    result == KILN_OK
        && info.status == KILN_ECS_OK as u32
        && info.exact_profile == 1
        && kind < info.sessions.len()
        && info.present_mask & (1u32 << world_kind) != 0
        && info.sessions[kind] != 0
}

/// The server world's kind (the only world the compare-exchange writes reach).
pub const SERVER_WORLD: u32 = KILN_WORLD_SERVER as u32;

/// ShroudForge's `available(state, mod, operation)` for the ECS operations (check_ingame_access.rs:37-48): the phase,
/// the operation in the contract, the runtime permission, then the provider's fact. No lifecycle, no write fault.
pub fn available(host: &dyn RuntimeHost, operation: Op) -> bool {
    let owner = host.owner_context();
    if owner.phase() != Phase::Runtime
        || !host.static_support(operation).is_available()
        || !owner.has_runtime()
    {
        return false;
    }
    let facts = host.provider_facts();
    match operation {
        Op::EcsQuery | Op::EcsResolve | Op::EcsRead => facts.ready,
        Op::EcsWrite => facts.can_write,
        _ => false,
    }
}

/// ShroudForge's `runtime_type_name(lua, args, index)` (lua/ingame/mod.rs:129-149): the selector at `index` resolves
/// in the archive registry (its reason is raised), names a `keen::ecs::` type (raised otherwise), and is the type a
/// registration stores (here always its own: part A of the join has no separate storage roles).
pub fn runtime_type_name(ecs: &Ecs, args: &FunctionArgs, index: usize) -> mlua::Result<String> {
    let registry = ecs.types();
    let selected =
        selectors::resolve(registry, args.get::<Value>(index)?).map_err(LuaError::generic)?;
    let name = registry
        .get(selected)
        .map(|ty| ty.qualified_name.clone())
        .unwrap_or_default();
    if !name.starts_with(ECS_PREFIX) {
        return Err(LuaError::generic(format!(
            "runtime ECS operation requires a {ECS_PREFIX}* type, got {name}"
        )));
    }
    Ok(name)
}

/// Puts the family's functions into one package's `runtime.ecs` table; `host` is that package's registrar host.
pub fn install_into(
    lua: &Lua,
    ecs_table: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: Rc<Ecs>,
) -> mlua::Result<()> {
    let (h, e) = (host.clone(), ecs.clone());
    ecs_table.raw_set(
        "get_components",
        lua.create_function(move |lua, _: FunctionArgs| {
            catalog::get_components(lua, &e, h.as_ref())
        })?,
    )?;
    let (h, e) = (host.clone(), ecs.clone());
    ecs_table.raw_set(
        "get_catalog",
        lua.create_function(move |lua, _: FunctionArgs| catalog::get_catalog(lua, &e, h.as_ref()))?,
    )?;
    let (h, e) = (host.clone(), ecs.clone());
    ecs_table.raw_set(
        "get_registry",
        lua.create_function(move |lua, _: FunctionArgs| {
            catalog::get_registry(lua, &e, h.as_ref())
        })?,
    )?;
    let (h, e) = (host.clone(), ecs.clone());
    ecs_table.raw_set(
        "get_component",
        lua.create_function(move |lua, args: FunctionArgs| {
            let selector = args.get::<Value>(0)?.clone();
            catalog::get_component(lua, &e, h.as_ref(), selector)
        })?,
    )?;
    macro_rules! member {
        ($name:literal, $body:path) => {{
            let (h, e) = (host.clone(), ecs.clone());
            ecs_table.raw_set(
                $name,
                lua.create_function(move |lua, args: FunctionArgs| {
                    $body(lua, &e, h.as_ref(), &args)
                })?,
            )?;
        }};
    }
    member!("query", entities::query);
    member!("query_bounds", entities::query_bounds);
    member!("resolve", entities::resolve);
    member!("read", snapshots::read);
    member!("read_bytes", snapshots::read_bytes);
    member!("read_fields", fields::read_fields);
    member!("get_template", template::get_template);
    let (h, e) = (host.clone(), ecs.clone());
    ecs_table.raw_set(
        "get_template_component",
        lua.create_function(move |lua, args: FunctionArgs| {
            template_component::get_template_component(lua, &e, &h, &args)
        })?,
    )?;
    member!("write", cas::write);
    member!("write_fields", fields::write_fields);
    Ok(())
}

/// The VM's shared [`Ecs`], as Lua app data.
struct VmEcs(Rc<Ecs>);

/// The VM's shared [`Ecs`] once the ECS family installed one of its packages (the world cursor and props build on it,
/// so the handles they give are the ones `runtime.ecs.read` and `write` accept); `None` before, or in a VM without
/// the family.
pub fn of_vm(lua: &Lua) -> Option<Rc<Ecs>> {
    lua.app_data_ref::<VmEcs>().map(|ecs| ecs.0.clone())
}

/// The production family on `types` (the wiring's handle) and the plan side's world kind. A package without a
/// `runtime` namespace gets nothing. The packages of one VM share one [`Ecs`]; the functions keep only the VM's
/// type session, so the registry's source is released when runtime-create ends, as before this family.
pub fn family(types: TypesHandle, world_kind: u32) -> Family {
    let shared: RefCell<Vec<Rc<Ecs>>> = RefCell::new(Vec::new());
    Family::new(
        FAMILY,
        &CLAIMS,
        move |lua, _, _, composed, cx: &RuntimeContext| {
            let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
                return Ok(());
            };
            let session = types.session(lua)?;
            let known = shared
                .borrow()
                .iter()
                .find(|ecs| Rc::ptr_eq(&ecs.session, &session))
                .cloned();
            let ecs = match known {
                Some(ecs) => ecs,
                None => {
                    let facts = cx.facts().clone();
                    let bound = facts.clone();
                    let ecs = Rc::new(
                        Ecs::new(session, cx.services().clone(), world_kind).with_runtime(
                            EcsRuntime {
                                owner: cx.owner(),
                                values: cx.values(),
                                facts,
                                world: Rc::new(move || bound.ecs_world()),
                            },
                        ),
                    );
                    shared.borrow_mut().push(ecs.clone());
                    ecs
                }
            };
            lua.set_app_data(VmEcs(ecs.clone()));
            let table: Table = runtime.raw_get("ecs")?;
            install_into(lua, &table, host.clone(), ecs.clone())?;
            crate::runtime::overrides::targets::component::install_into(
                lua,
                runtime,
                host.clone(),
                ecs.clone(),
            )?;
            crate::runtime::overrides::targets::template::install_into(lua, runtime, host, ecs)
        },
    )
}
