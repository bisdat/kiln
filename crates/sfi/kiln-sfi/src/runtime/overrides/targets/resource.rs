// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/claim_runtime_storage.rs:176-200 (a resource
// claim's acquire checks: runtime.ecs.write and runtime.assets.write), :296-316 (get_resource_claim);
// create_override_targets.lua:199-232 (resource.lua); lua/values/{encode_owned_value,decode_owned_value}.rs (the
// canonical value the target normalizes to).
//! `runtime.overrides.get_resource_claim` and `runtime.overrides.resource`.
//!
//! - **get_resource_claim(resource)**: one argument, a loaded `Resource` of the live world session (its write session,
//!   [`crate::runtime::assets::resource::LoadedResource::require_write_session`]), then a claim on the resource
//!   ([`crate::runtime::overrides::resource_claim`]): its key carries no session (a loaded resource outlives its world);
//!   each acquire checks runtime.ecs.write and runtime.assets.write.
//! - **resource(resource, path)**: ShroudForge's own target table ([`TARGET`], its Lua), reading through
//!   `game.assets.get_loaded_resource` and `read_loaded_field` and writing through `write_loaded_field` with the
//!   snapshot's value as the expected one, so a restore never lands over another writer's change. Its `normalize` is the
//!   field type's canonical value: the value encoded and decoded again by the VM's typed values.

use crate::runtime::assets::Assets;
use crate::runtime::assets::resource::{LoadedResource, authority, ownership_key};
use crate::runtime::ecs::fields;
use crate::runtime::overrides::{RuntimeWriteClaim, claim_result, resource_claim};
use crate::runtime::types::selectors;
use mlua::{AnyUserData, Function, Lua, Table, Value};
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::lua::FunctionArgs;
use std::rc::Rc;

/// ShroudForge's resource target factory.
pub const TARGET: &str = include_str!("resource.lua");

/// ShroudForge's texts (claim_runtime_storage.rs:303).
pub mod text {
    pub const ARGUMENTS: &str = "get_resource_claim expects one current loaded Resource";
}

/// `runtime.overrides.get_resource_claim(resource)`: `claim` or `nil, reason`.
pub fn get_resource_claim(
    lua: &Lua,
    assets: &Rc<Assets>,
    host: &Rc<dyn RuntimeHost>,
    args: &FunctionArgs,
) -> mlua::Result<(Option<AnyUserData>, Option<String>)> {
    let claim = || -> Result<RuntimeWriteClaim, String> {
        if args.len() != 1 {
            return Err(text::ARGUMENTS.into());
        }
        let resource = args.get::<&LoadedResource>(0).map_err(|e| e.to_string())?;
        let session = resource.require_write_session(assets.world_session())?;
        let facts = assets.facts().ok_or(text::ARGUMENTS)?;
        resource_claim(
            facts.ownership(),
            authority(host, assets),
            ownership_key(resource.key()),
            session,
        )
    };
    claim_result(lua, claim())
}

/// The canonical value of `value` as the field type `ty` (a Type object or a selector): `value` or `nil, reason`.
fn normalize(
    lua: &Lua,
    assets: &Assets,
    ty: &Value,
    value: &Value,
) -> (Option<Value>, Option<String>) {
    let canonical = || -> Result<Value, String> {
        let index = selectors::resolve(assets.types(), ty)?;
        let meta = assets.types().get(index).ok_or("unresolved field type")?;
        let values = assets
            .values()
            .ok_or_else(|| crate::runtime::ecs::NO_VALUES.to_owned())?;
        let bytes = values
            .encode(&meta.qualified_name, value.clone())
            .map_err(|e| e.to_string())?;
        fields::decode(
            lua,
            values,
            &meta.qualified_name,
            meta.size as usize,
            &bytes,
        )
    };
    match canonical() {
        Ok(value) => (Some(value), None),
        Err(reason) => (None, Some(reason)),
    }
}

/// Puts `get_resource_claim` and `resource` into one package's `runtime.overrides` table; `assets_table` is the
/// package's `game.assets` (the target reads its members when it is called, as ShroudForge's does).
pub fn install_into(
    lua: &Lua,
    runtime: &Table,
    assets_table: &Table,
    host: Rc<dyn RuntimeHost>,
    assets: Rc<Assets>,
) -> mlua::Result<()> {
    let overrides: Table = runtime.raw_get("overrides")?;
    let a = assets.clone();
    overrides.raw_set(
        "get_resource_claim",
        lua.create_function(move |lua, args: FunctionArgs| {
            get_resource_claim(lua, &a, &host, &args)
        })?,
    )?;
    let normalize_type = lua.create_function(move |lua, (ty, value): (Value, Value)| {
        Ok(normalize(lua, &assets, &ty, &value))
    })?;
    let factory: Function = lua
        .load(TARGET)
        .set_name("=runtime.overrides.resource")
        .eval()?;
    let resource: Function =
        factory.call((runtime.clone(), assets_table.clone(), normalize_type))?;
    overrides.raw_set("resource", resource)
}
