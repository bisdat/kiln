// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs registry family.
//! The ECS registry and catalog, the metadata half of `runtime.ecs`.
//!
//! - [`registry`]: kiln.types' `ecs_registry` joined with the archive registry by qualified hash and name,
//!   with a size proof per row.
//! - [`catalog`]: `runtime.ecs.get_catalog`, `get_registry`, `get_component`, `get_components` on that join, the
//!   package host's facts and kiln.world's `ecs_write_policy`.
//!
//! The family ([`family`]) claims the four names ([`CLAIMS`]) inside the installer's `runtime.ecs` container and
//! shares one [`Ecs`] (the join's cache) between the packages of one runtime-create. Every call runs on the worker
//! and calls Kiln members directly through the service adapter: no hop, no wait.
//!
//! A later version (on `kiln.types.ecs_registration`; callback code through `read_original`) fills the
//! registration detail ([`catalog::RegistrationDetail`], template roles, callbacks); it is not here.

pub mod catalog;
pub mod registry;

use super::context::RuntimeContext;
use super::install::Family;
use super::services::ServiceAdapter;
use super::types::{Registry, Session, TypesHandle};
use mlua::{Lua, Table, Value};
use registry::EcsRegistry;
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::lua::FunctionArgs;
use std::cell::RefCell;
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "ecs-registry";

/// The names this family adds inside the installer's `runtime.ecs` container.
pub const CLAIMS: [&str; 4] = [
    "runtime.ecs.get_components",
    "runtime.ecs.get_catalog",
    "runtime.ecs.get_registry",
    "runtime.ecs.get_component",
];

/// What the packages of one VM share: its type session (the Types and the checked archive registry, not the
/// registry's source), the adapter, the world kind whose catalog and write policy are asked, and the join's cache.
pub struct Ecs {
    session: Rc<Session>,
    services: Rc<ServiceAdapter>,
    world_kind: u32,
    registry: EcsRegistry,
}

impl Ecs {
    pub fn new(session: Rc<Session>, services: Rc<ServiceAdapter>, world_kind: u32) -> Self {
        Self {
            session,
            services,
            world_kind,
            registry: EcsRegistry::new(world_kind),
        }
    }
    /// The checked archive registry.
    fn types(&self) -> &Rc<Registry> {
        self.session.registry()
    }
    /// The one Type of registry `index` in this VM, or nil.
    fn type_value(&self, lua: &Lua, index: usize) -> mlua::Result<Value> {
        Ok(self.session.get_type(lua, index)?.unwrap_or(Value::Nil))
    }
}

/// Puts the four functions into one package's `runtime.ecs` table; `host` is that package's registrar host.
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
    ecs_table.raw_set(
        "get_component",
        lua.create_function(move |lua, args: FunctionArgs| {
            let selector = args.get::<Value>(0)?.clone();
            catalog::get_component(lua, &ecs, host.as_ref(), selector)
        })?,
    )
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
                    let ecs = Rc::new(Ecs::new(session, cx.services().clone(), world_kind));
                    shared.borrow_mut().push(ecs.clone());
                    ecs
                }
            };
            let table: Table = runtime.raw_get("ecs")?;
            install_into(lua, &table, host, ecs)
        },
    )
}
