// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.overrides` in the pregame VM: ShroudForge's own tracker factory (one per package, cached in the Lua
//! registry under its id), its four ownership claims and its five target factories. The tracker and the targets are
//! ShroudForge's Lua, verbatim ([`super::overrides_lua`]); the tracker's world session is the provider's, 0 without it.
//! The claims have no phase check: each converts its arguments and refuses where its source does without a provider
//! (no component layout; a Resource here is never a loaded runtime snapshot; service and template views cannot exist
//! before the game, so any argument fails their conversion).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! lua/ingame/overrides/mod.rs:6-24, track_runtime_overrides.rs:4-36, claim_runtime_storage.rs:240-370,
//! lua/ingame/mod.rs:130-149 (`runtime_type_name`), session/loaded_resources.rs:99-110.
use std::convert::Infallible;

use mlua::{Function, Lua, Table, UserData};
use sfi_core::lua::{FunctionArgs, LuaValue};

use super::{
    PregameRuntime,
    access::{WORLD_SESSION, add},
    overrides_lua::{TARGETS, TRACKER},
};
use crate::sf_startup::game::{resolve_type::resolve, resource::Resource};

/// The type of ShroudForge's `runtime.services.get` view; none exists before the game (`services.get` refuses), so
/// this one has no value. Its name is the one a failed argument conversion reports.
pub struct RuntimeService(Infallible);
impl UserData for RuntimeService {}

/// The type of ShroudForge's `runtime.ecs.get_template_component` view; as [`RuntimeService`].
pub struct RuntimeTemplateComponent(Infallible);
impl UserData for RuntimeTemplateComponent {}

/// SF `track_runtime_overrides::create`: the package's tracker factory, made once per package id.
pub(super) fn create(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let key = format!("shroudforge.runtime.overrides.{}", context.host.owner_context().id());
    let factory_key = format!("{key}.factory");
    if let Some(factory) = lua.named_registry_value::<Option<Table>>(&factory_key)? {
        return Ok(factory);
    }
    let factory: Table = lua.load(TRACKER).set_name("runtime.overrides").eval()?;
    let new: Function = factory.raw_get("new")?;
    let trackers = lua.create_table()?;
    lua.set_named_registry_value(&key, trackers.clone())?;
    let get_session = lua.create_function(|_, ()| Ok(WORLD_SESSION))?;
    factory.raw_set(
        "new",
        lua.create_function(move |_, ()| {
            let tracker: Table = new.call(get_session.clone())?;
            trackers.raw_set(trackers.raw_len() + 1, tracker.clone())?;
            Ok(tracker)
        })?,
    )?;
    lua.set_named_registry_value(&factory_key, factory.clone())?;
    Ok(factory)
}

/// SF `overrides::attach`: the claims, then the targets over the package's `runtime` and `game.assets`.
pub(super) fn attach(lua: &Lua, runtime: &Table, game: &Table, context: &PregameRuntime) -> mlua::Result<()> {
    let overrides: Table = runtime.raw_get("overrides")?;
    claims(lua, &overrides, context)?;
    let factory: Function = lua.load(TARGETS).set_name("runtime.overrides.targets").eval()?;
    let targets: Table = factory.call((runtime.clone(), game.raw_get::<Table>("assets")?))?;
    for pair in targets.pairs::<String, LuaValue>() {
        let (name, target) = pair?;
        overrides.raw_set(name, target)?;
    }
    Ok(())
}

type Claim = (Option<Table>, Option<String>);

fn outcome(result: Result<Infallible, String>) -> Claim {
    match result {
        Err(reason) => (None, Some(reason)),
    }
}

/// SF `claim_runtime_storage::attach` (claim_runtime_storage.rs:240-370) without a provider.
fn claims(lua: &Lua, overrides: &Table, context: &PregameRuntime) -> mlua::Result<()> {
    let registry = context.types.registry().clone();
    add(
        lua,
        overrides,
        "get_component_claim",
        move |_, args: FunctionArgs| -> mlua::Result<Claim> {
            Ok(outcome((|| {
                if args.len() != 2 {
                    return Err("get_component_claim expects entity and original component type".into());
                }
                let _entity = args.get::<u32>(0).map_err(|e| e.to_string())?;
                // SF `runtime_type_name`: the selector resolves, and must be a keen::ecs type
                let selector = args.get::<LuaValue>(1).map_err(|e| e.to_string())?;
                let index = resolve(&registry, selector)?;
                let ty = registry.get(index).ok_or("unresolved component type")?;
                if !ty.qualified_name.starts_with("keen::ecs::") {
                    return Err(format!(
                        "runtime ECS operation requires a keen::ecs::* type, got {}",
                        ty.qualified_name
                    ));
                }
                // runtime_provider::resolve is None without the provider
                Err("claimed component layout is unavailable".into())
            })()))
        },
    )?;
    add(
        lua,
        overrides,
        "get_resource_claim",
        |_, args: FunctionArgs| -> mlua::Result<Claim> {
            Ok(outcome((|| {
                if args.len() != 1 {
                    return Err("get_resource_claim expects one current loaded Resource".into());
                }
                let _resource = args.get::<&Resource>(0).map_err(|e| e.to_string())?;
                // require_runtime_write_session: an archive or created record is never a loaded runtime snapshot
                Err("write_loaded_field requires a ResourceService snapshot".into())
            })()))
        },
    )?;
    add(
        lua,
        overrides,
        "get_service_claim",
        |_, args: FunctionArgs| -> mlua::Result<Claim> {
            Ok(outcome((|| {
                if args.len() != 1 {
                    return Err("get_service_claim expects one current RuntimeService view".into());
                }
                let view = args.get::<&RuntimeService>(0).map_err(|e| e.to_string())?;
                match view.0 {}
            })()))
        },
    )?;
    add(
        lua,
        overrides,
        "get_template_claim",
        |_, args: FunctionArgs| -> mlua::Result<Claim> {
            Ok(outcome((|| {
                if args.len() != 1 {
                    return Err("get_template_claim expects one current RuntimeTemplateComponent view".into());
                }
                let view = args.get::<&RuntimeTemplateComponent>(0).map_err(|e| e.to_string())?;
                match view.0 {}
            })()))
        },
    )
}
