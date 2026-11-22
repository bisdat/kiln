// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/claim_runtime_storage.rs:319-342
// (get_service_claim), create_override_targets.lua:171-205,263-266 (view_target and the service target: service.lua);
// lua/values/normalize_field.rs (the canonical value).
//! `runtime.overrides.get_service_claim` and `runtime.overrides.service`.
//!
//! - **get_service_claim(view)**: one argument, a `runtime.services.get` view of this package (another package's
//!   view refuses with ShroudForge's text), then a claim on the service in the view's session
//!   ([`crate::runtime::overrides::service_claim`]): every acquire then checks ShroudForge's operation (`runtime.ecs.write`)
//!   and the session, and another mod's claim on the same service refuses with its label.
//! - **service(view, path, expected_paths)**: ShroudForge's own view target ([`TARGET`], its Lua): it reads through
//!   `view:read_fields` and writes through `view:write_field` with the snapshot's value and, when given, the snapshot's
//!   expected fields, so a restore never lands over another writer's change; its `normalize` is the field codec's
//!   canonical value of the view's type ([`crate::runtime::ecs::fields::normalize_field`]).

use crate::runtime::ecs::{Ecs, fields};
use crate::runtime::named_services::RuntimeService;
use crate::runtime::overrides::{HostAuthority, RuntimeWriteClaim, claim_result, service_claim};
use crate::runtime::types::selectors;
use mlua::{AnyUserData, Function, Lua, Table, Value};
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::lua::FunctionArgs;
use std::rc::Rc;

/// The names the named-services family installs inside the installer's `runtime.overrides` container.
pub const CLAIMS: [&str; 2] = [
    "runtime.overrides.get_service_claim",
    "runtime.overrides.service",
];

/// ShroudForge's service target factory.
pub const TARGET: &str = include_str!("service.lua");

/// ShroudForge's texts (claim_runtime_storage.rs:326).
pub mod text {
    pub const ARGUMENTS: &str = "get_service_claim expects one current RuntimeService view";
}

/// `runtime.overrides.get_service_claim(view)`: `claim` or `nil, reason`.
pub fn get_service_claim(
    lua: &Lua,
    ecs: &Rc<Ecs>,
    host: Rc<dyn RuntimeHost>,
    args: &FunctionArgs,
) -> mlua::Result<(Option<AnyUserData>, Option<String>)> {
    let claim = || -> Result<RuntimeWriteClaim, String> {
        if args.len() != 1 {
            return Err(text::ARGUMENTS.into());
        }
        let view = args.get::<&RuntimeService>(0).map_err(|e| e.to_string())?;
        let (key, session) = view.ownership_identity(host.owner_context().id())?;
        let runtime = ecs.runtime().ok_or(text::ARGUMENTS)?;
        let live = ecs.clone();
        let authority = Rc::new(HostAuthority::new(host.clone(), move || {
            live.world_session()
        }));
        service_claim(runtime.facts.ownership(), authority, key, session)
    };
    claim_result(lua, claim())
}

/// The canonical value of `value` at `path` of the type `selector` names: `value` or `nil, reason`.
fn normalize(
    lua: &Lua,
    ecs: &Ecs,
    selector: &Value,
    path: &str,
    value: &Value,
) -> (Option<Value>, Option<String>) {
    let canonical = selectors::resolve(ecs.types(), selector)
        .and_then(|root| fields::normalize_field(lua, ecs, root, path, value));
    match canonical {
        Ok(value) => (Some(value), None),
        Err(reason) => (None, Some(reason)),
    }
}

/// Puts `get_service_claim` and `service` into one package's `runtime.overrides` table (`runtime` is the package's
/// runtime table, which the target reads its members from when it is called, as ShroudForge's does).
pub fn install_into(
    lua: &Lua,
    runtime: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: Rc<Ecs>,
) -> mlua::Result<()> {
    let overrides: Table = runtime.raw_get("overrides")?;
    let e = ecs.clone();
    overrides.raw_set(
        "get_service_claim",
        lua.create_function(move |lua, args: FunctionArgs| {
            get_service_claim(lua, &e, host.clone(), &args)
        })?,
    )?;
    let normalize_field = lua.create_function(
        move |lua, (selector, path, value): (Value, String, Value)| {
            Ok(normalize(lua, &ecs, &selector, &path, &value))
        },
    )?;
    let factory: Function = lua
        .load(TARGET)
        .set_name("=runtime.overrides.service")
        .eval()?;
    let service: Function = factory.call((runtime.clone(), normalize_field))?;
    overrides.raw_set("service", service)
}
