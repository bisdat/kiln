// SPDX-License-Identifier: GPL-3.0-or-later
//! The pregame answers every family shares: the denial (check_ingame_access.rs:386-414) and the facts of a process
//! without the native provider (game-runtime/bridge/windows). Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026
//! bonsaibauer; notice in sf_startup/mod.rs).
use mlua::{IntoLuaMulti, Lua, Table};
use sfi_core::{
    bindings::runtime::{RuntimeHost, RuntimeOperation, runtime_denial_reason},
    host::Phase,
    lua::FunctionArgs,
};

/// ShroudForge's refusal of every runtime operation outside the ingame phase.
pub const PHASE_REFUSAL: &str = "runtime APIs are available only during the ingame phase";
/// `runtime_provider::registry()` without the provider (bridge/windows/ecs/read_component_registry.rs:5-7).
pub const REGISTRY_MISSING: &str =
    "Native provider lacks the component registry extension. Install the matching provider";
/// `runtime_provider::functions()` without the provider (bridge/windows/functions/read_function_catalog.rs:8-10).
pub const FUNCTION_INVENTORY_MISSING: &str = "native provider lacks the function inventory extension";

/// SF `runtime_denial_reason` for any feature name: the shared runtime registrar check for a catalog operation; for another name
/// the same order (phase, permission, lifecycle) and then the contract's unknown-operation reason.
pub(super) fn denial(host: &dyn RuntimeHost, feature: &str) -> Option<String> {
    if let Some(operation) = RuntimeOperation::from_name(feature) {
        return runtime_denial_reason(host, operation);
    }
    let owner = host.owner_context();
    if owner.phase() != Phase::Runtime {
        return Some(PHASE_REFUSAL.into());
    }
    if !owner.has_runtime() {
        return Some(format!("mod '{}' requires capabilities: [\"runtime\"]", owner.id()));
    }
    if !host.lifecycle_active() {
        return Some("runtime API access is available only while this mod's lifecycle is active".into());
    }
    Some(format!(
        "operation {feature} has not been verified for game build {}",
        owner.game_build()
    ))
}

/// The error for a path past a denial, which a startup-phase host never lets through (the registrar checks the phase).
pub(super) fn past_denial(name: &str) -> mlua::Error {
    mlua::Error::runtime(format!("{name} has no ingame provider in the startup pass"))
}

/// SF `add_function_with_mod` (bindings/register_functions.rs:23-41): one closure over `FunctionArgs`, set raw.
pub(super) fn add<R, F>(lua: &Lua, table: &Table, name: &str, f: F) -> mlua::Result<()>
where
    R: IntoLuaMulti,
    F: Fn(&Lua, FunctionArgs) -> mlua::Result<R> + 'static,
{
    table.raw_set(name, lua.create_function(f)?)
}

/// The world session the provider reports: none without it (bridge/windows/world_context/read_world_session.rs).
pub(super) const WORLD_SESSION: u64 = 0;
