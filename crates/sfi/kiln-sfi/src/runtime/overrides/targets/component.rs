// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/claim_runtime_storage.rs:176-213 (a component
// claim's acquire check), :261-294 (get_component_claim); create_override_targets.lua:234-260 (component.lua);
// lua/values/normalize_field.rs (the canonical value).
//! `runtime.overrides.get_component_claim` and `runtime.overrides.component`.
//!
//! - **get_component_claim(entity, type)**: two arguments, the entity handle, ShroudForge's `keen::ecs::` selector,
//!   the layout the registry join resolved, the archive registry's type and the size proof, then a claim on the
//!   component in the family's live world session ([`crate::runtime::overrides::component_claim`]). Each acquire then
//!   checks, after ShroudForge's operation checks, that the layout still resolves, kiln.world's write policy allows
//!   the component and the entity still has it (a stable copy reads).
//! - **component(entity, type, path, expected_paths)**: ShroudForge's own target table ([`TARGET`], its Lua),
//!   reading through `runtime.ecs.read_fields` and writing through `runtime.ecs.write_fields` with the snapshot's
//!   fields as the expected values, so a restore never lands over another writer's change; its `normalize` is the
//!   field codec's canonical value ([`super::super::super::ecs::fields::normalize_field`]).

use crate::runtime::ecs::{Ecs, fields, runtime_type_name};
use crate::runtime::overrides::{
    AcquireCheck, HostAuthority, RuntimeWriteClaim, claim_result, component_claim,
};
use crate::runtime::types::selectors;
use mlua::{AnyUserData, Function, Lua, Table, Value};
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::lua::FunctionArgs;
use std::rc::Rc;

/// The names the ECS family installs inside the installer's `runtime.overrides` container.
pub const CLAIMS: [&str; 2] = [
    "runtime.overrides.get_component_claim",
    "runtime.overrides.component",
];

/// ShroudForge's component target factory.
pub const TARGET: &str = include_str!("component.lua");

/// ShroudForge's texts (claim_runtime_storage.rs:198-280).
pub mod text {
    pub const ARGUMENTS: &str = "get_component_claim expects entity and original component type";
    pub const LAYOUT: &str = "claimed component layout is unavailable";
    pub const REFLECTION: &str = "claimed component is absent from active reflection";
    pub const DISAGREE: &str = "claimed component reflection and live storage disagree";
    pub const AUTHORITY: &str =
        "claimed entity generation or component write authority is unavailable";
}

/// `runtime.overrides.get_component_claim(entity, type)`: `claim` or `nil, reason`.
pub fn get_component_claim(
    lua: &Lua,
    ecs: &Rc<Ecs>,
    host: Rc<dyn RuntimeHost>,
    args: &FunctionArgs,
) -> mlua::Result<(Option<AnyUserData>, Option<String>)> {
    let claim = || -> Result<RuntimeWriteClaim, String> {
        if args.len() != 2 {
            return Err(text::ARGUMENTS.into());
        }
        let entity = args.get::<u32>(0).map_err(|e| e.to_string())?;
        let name = runtime_type_name(ecs, args, 1).map_err(|e| e.to_string())?;
        let component = ecs.resolve_component(&name).ok_or(text::LAYOUT)?;
        let ty = ecs
            .types()
            .by_qualified_name(&name)
            .ok_or(text::REFLECTION)?;
        if ty.size != component.size {
            return Err(text::DISAGREE.into());
        }
        let runtime = ecs.runtime().ok_or(text::LAYOUT)?;
        let live = ecs.clone();
        let authority = Rc::new(HostAuthority::new(host.clone(), move || {
            live.world_session()
        }));
        let (checked, checked_name) = (ecs.clone(), name.clone());
        let check: Rc<dyn AcquireCheck> = Rc::new(move || -> Result<(), String> {
            let component = checked
                .resolve_component(&checked_name)
                .ok_or_else(|| text::LAYOUT.to_owned())?;
            if !checked.can_write_component(component.component)
                || checked
                    .snapshot(entity, component.component, component.size)
                    .is_none()
            {
                return Err(text::AUTHORITY.into());
            }
            Ok(())
        });
        component_claim(
            runtime.facts.ownership(),
            authority,
            entity,
            &name,
            Some(check),
        )
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

/// Puts `get_component_claim` and `component` into one package's `runtime.overrides` table (`runtime` is the
/// package's runtime table, which the target reads its members from when it is called, as ShroudForge's does).
pub fn install_into(
    lua: &Lua,
    runtime: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: Rc<Ecs>,
) -> mlua::Result<()> {
    let overrides: Table = runtime.raw_get("overrides")?;
    let e = ecs.clone();
    overrides.raw_set(
        "get_component_claim",
        lua.create_function(move |lua, args: FunctionArgs| {
            get_component_claim(lua, &e, host.clone(), &args)
        })?,
    )?;
    let normalize_field = lua.create_function(
        move |lua, (selector, path, value): (Value, String, Value)| {
            Ok(normalize(lua, &ecs, &selector, &path, &value))
        },
    )?;
    let factory: Function = lua
        .load(TARGET)
        .set_name("=runtime.overrides.component")
        .eval()?;
    let component: Function = factory.call((runtime.clone(), normalize_field))?;
    overrides.raw_set("component", component)
}
