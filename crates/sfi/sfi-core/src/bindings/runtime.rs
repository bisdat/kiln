// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (byte-identical in both pins): modules/mod-runtime/lua/ingame/check_ingame_access.rs:12-414;
// lua/ingame/mod.rs:45-62,84-113; lua/bindings/mod.rs:71-75;
// game-runtime/compatibility/lib.rs:30-44 (unknown operation reason only).
//! Shared pure registrar. The phase host installs the REAL game/types table first.
use super::common::{child, namespace};
use crate::{
    host::{HostContext, Package, Phase, Side},
    lua::{FunctionArgs, LuaError},
};
use mlua::{Lua, Table};
use std::rc::Rc;
#[path = "runtime_ports.rs"]
mod ports;
use RuntimeOperation as Op;
pub use ports::{
    Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext,
    RuntimeProviderFacts,
};

/// Register before package execution, inside LuaVm::add_package. No service/FFI work.
/// Returns the same mutable table published under env.runtime and builtin.runtime.
/// Missing/mismatched phase-owned game/types or captured identity is a setup error.
pub fn register_runtime(
    lua: &Lua,
    env: &Table,
    owner: &Package,
    context: &HostContext,
    game: &Table,
    ports: Rc<dyn RuntimeHost>,
) -> mlua::Result<Table> {
    let captured = ports.owner_context();
    if captured.id() != owner.id()
        || captured.phase() != context.phase
        || captured.side() != context.side
        || captured.version() != context.version
    {
        return Err(mlua::Error::runtime(
            "runtime owner context does not match registering package/host context",
        ));
    }
    let installed: Table = env
        .raw_get("game")
        .map_err(|_| mlua::Error::runtime("runtime requires env.game to be a table"))?;
    let builtin: Table = env
        .raw_get::<Table>("builtin")?
        .raw_get("game")
        .map_err(|_| mlua::Error::runtime("runtime requires builtin.game to be a table"))?;
    if installed != *game || builtin != *game {
        return Err(mlua::Error::runtime(
            "runtime requires the installed env.game/builtin.game table",
        ));
    }
    let types: Table = game
        .raw_get("types")
        .map_err(|_| mlua::Error::runtime("runtime requires the phase host's game.types table"))?;
    let runtime = namespace(lua, env, "runtime")?;
    runtime.raw_set(
        "phase",
        if context.phase == Phase::Startup {
            "pregame"
        } else {
            "ingame"
        },
    )?;
    runtime.raw_set("schema_version", 1)?;
    runtime.raw_set("is_client", context.side == Side::Client)?;
    runtime.raw_set("is_server", context.side == Side::Server)?;
    runtime.raw_set("types", types)?;
    let host = ports.clone();
    runtime.raw_set(
        "has",
        lua.create_function(move |_, args: FunctionArgs| {
            Ok(available(host.as_ref(), &args.get::<String>(0)?))
        })?,
    )?;
    let host = ports.clone();
    runtime.raw_set(
        "require",
        lua.create_function(move |_, args: FunctionArgs| {
            let feature = args.get::<String>(0)?;
            if available(host.as_ref(), &feature) {
                Ok(())
            } else {
                Err(LuaError::generic(format!(
                    "ShroudForge feature is unavailable: {feature}"
                )))
            }
        })?,
    )?;
    let host = ports.clone();
    runtime.raw_set(
        "status",
        lua.create_function(move |lua, args: FunctionArgs| {
            availability_to_lua(
                lua,
                operation_status(host.as_ref(), &args.get::<String>(0)?),
            )
        })?,
    )?;
    let host = ports.clone();
    runtime.raw_set(
        "get_operations",
        lua.create_function(move |lua, _: FunctionArgs| operation_catalog(lua, host.as_ref()))?,
    )?;
    let host = ports.clone();
    runtime.raw_set("report_effect", lua.create_function(move |_, args: FunctionArgs| {
        let owner = host.owner_context();
        if owner.phase() != Phase::Runtime || !owner.has_runtime() {
            return Ok((false, Some("runtime effect reporting requires an active ingame runtime mod".to_owned())));
        }
        if !host.lifecycle_active() { return Ok((false, Some("runtime mod is not active".to_owned()))); }
        let state = EffectState::from_name(&args.get::<String>(0)?).ok_or_else(|| LuaError::generic(
            "runtime.report_effect state must be waiting, no-target, no-change, write-confirmed, or write-failed"))?;
        let detail = args.get::<Option<String>>(1)?.unwrap_or_default();
        host.report_effect(state, &detail);
        Ok((true, None))
    })?)?;
    let host = ports.clone();
    runtime.raw_set(
        "is_main_menu",
        lua.create_function(move |_, _: FunctionArgs| {
            if let Some(reason) = runtime_denial_reason(host.as_ref(), Op::IsMainMenu) {
                return Ok((None, Some(reason)));
            }
            let value = host.provider_facts().is_main_menu;
            Ok((
                value,
                value
                    .is_none()
                    .then(|| "main menu state is unknown or transitioning".to_owned()),
            ))
        })?,
    )?;
    let player = child(lua, &runtime, "player")?;
    player.raw_set(
        "name",
        lua.create_function(move |_, _: FunctionArgs| {
            if let Some(reason) = runtime_denial_reason(ports.as_ref(), Op::PlayerName) {
                return Ok((None, Some(reason)));
            }
            let name = ports.provider_facts().selected_character_name;
            let reason = name
                .is_none()
                .then(|| "selected character name is unavailable".to_owned());
            Ok((name, reason))
        })?,
    )?;
    Ok(runtime)
}

/// Extension seam for the functions owner. Installs ONLY this checked catalog wrapper
/// into its actual functions table; no successful placeholders or second dispatcher.
pub fn install_function_operations(
    lua: &Lua,
    functions: &Table,
    ports: Rc<dyn RuntimeHost>,
) -> mlua::Result<()> {
    functions.raw_set(
        "get_operations",
        lua.create_function(move |lua, _: FunctionArgs| operation_catalog(lua, ports.as_ref()))?,
    )
}

fn static_availability(host: &dyn RuntimeHost, feature: &str) -> Availability {
    match Op::from_name(feature) {
        Some(operation) => host.static_support(operation),
        None => Availability::Unavailable {
            reason: format!(
                "operation {feature} has not been verified for game build {}",
                host.owner_context().game_build()
            ),
        },
    }
}

// check_ingame_access.rs:12-135: has/require do NOT consult lifecycle or write_fault.
fn available(host: &dyn RuntimeHost, feature: &str) -> bool {
    let owner = host.owner_context();
    match feature {
        "game.assets.write" => return owner.global_patch() && owner.has_patch(),
        "export" => return owner.global_export() && owner.has_export(),
        _ => {}
    }
    let Some(op) = Op::from_name(feature) else {
        return false;
    };
    if owner.phase() != Phase::Runtime
        || !host.static_support(op).is_available()
        || !owner.has_runtime()
    {
        return false;
    }
    if op == Op::Lifecycle {
        return true;
    }
    let facts = host.provider_facts();
    match op {
        Op::Lifecycle => true,
        Op::PlayerName => facts.selected_character_name.is_some(),
        Op::IsMainMenu => facts.is_main_menu.is_some(),
        Op::EcsQuery | Op::EcsResolve | Op::EcsRead => facts.ready,
        Op::EcsWrite => facts.can_write,
        Op::AssetsWrite => facts.resource_can_write,
        Op::GameplayPatch => facts.gameplay_patch_available,
        Op::NetworkStatus
        | Op::NetworkSend
        | Op::NetworkSendMod
        | Op::NetworkReceive
        | Op::NetworkReceiveMod
        | Op::NetworkAccept
        | Op::NetworkConnectedPeers => facts.network_available && facts.network_service_ready,
        _ => world_ready(&facts, op),
    }
}

fn world_ready(facts: &RuntimeProviderFacts, op: Op) -> bool {
    match op {
        Op::WorldCursorGet | Op::WorldBuildingInput | Op::WorldVoxelRead => {
            facts.world_operation_available(op)
        }
        Op::WorldVoxelGridSpec => facts.world_operation_available(Op::WorldVoxelRead),
        Op::WorldEntitySetScale => {
            facts.world_entity_query_props_ready && facts.can_write_current_transform
        }
        Op::WorldEntityDestroy | Op::WorldEntitySpawn => {
            facts.world_operation_available(op) && facts.world_entity_query_props_ready
        }
        Op::WorldEntityQueryProps
        | Op::WorldEntityQueryPropsInBounds
        | Op::WorldEntityRegisterPropRecipes
        | Op::WorldEntityGetTransform => facts.world_entity_query_props_ready,
        Op::WorldEntityPlace | Op::WorldEntityFinishBuilding => {
            facts.world_operation_available(op) && facts.world_entity_context_ready
        }
        _ => facts.world_operation_available(op) && facts.world_context_active,
    }
}

// check_ingame_access.rs:179-299: status has no blanket phase/active/write-fault gate.
fn operation_status(host: &dyn RuntimeHost, feature: &str) -> Availability {
    let owner = host.owner_context();
    let capable = match feature {
        "game.assets.write" => owner.has_patch(),
        "export" => owner.has_export(),
        name if name.starts_with("runtime.") => owner.has_runtime(),
        _ => true,
    };
    if !capable {
        return Availability::Unavailable {
            reason: format!("mod '{}' lacks the required capability", owner.id()),
        };
    }
    let availability = static_availability(host, feature);
    let Some(op) = Op::from_name(feature) else {
        return availability;
    };
    // The source computes world_ready even when static support is unavailable. Other
    // readiness branches are short-circuited by static support; lifecycle/patch need none.
    let world = feature.starts_with("runtime.world.");
    if !world && (!availability.is_available() || matches!(op, Op::Lifecycle | Op::GameplayPatch)) {
        return availability;
    }
    let facts = host.provider_facts();
    let reason = match op {
        Op::IsMainMenu if facts.is_main_menu.is_none() => Some("main menu state is unknown or transitioning".into()),
        Op::PlayerName if facts.selected_character_name.is_none() => Some("selected character name is unavailable".into()),
        Op::EcsQuery | Op::EcsResolve | Op::EcsRead | Op::EcsWrite if !facts.ready => Some(concat!("K", "FC Runtime is waiting for the live Keen ECS world").into()),
        Op::NetworkStatus | Op::NetworkSend | Op::NetworkSendMod | Op::NetworkReceive | Op::NetworkReceiveMod | Op::NetworkAccept | Op::NetworkConnectedPeers if !facts.network_available || !facts.network_service_ready => Some("Steam Networking Messages transport or session callback is unavailable in this process".into()),
        Op::EcsWrite if !facts.can_write => Some(concat!("K", "FC Runtime has no writable live component context").into()),
        Op::AssetsWrite if !facts.resource_can_write => Some(concat!("K", "FC Runtime has no validated live resource scalar write context").into()),
        _ if world && !world_ready(&facts, op) => Some(format!("{}{}", concat!("K", "FC Runtime world operation is not ready: "), feature)),
        _ => None,
    };
    if availability.is_available()
        && let Some(reason) = reason
    {
        return Availability::Unavailable { reason };
    }
    availability
}

/// Shared checked denial for later finite runtime wrappers. Phase -> permission ->
/// lifecycle -> same-session write latch (only writes) -> static contract; no facts.
/// Source: check_ingame_access.rs:386-414 (both pins).
pub fn runtime_denial_reason(
    host: &dyn RuntimeHost,
    operation: RuntimeOperation,
) -> Option<String> {
    let owner = host.owner_context();
    if owner.phase() != Phase::Runtime {
        return Some("runtime APIs are available only during the ingame phase".into());
    }
    if !owner.has_runtime() {
        return Some(format!(
            "mod '{}' requires capabilities: [\"runtime\"]",
            owner.id()
        ));
    }
    if !host.lifecycle_active() {
        return Some(
            "runtime API access is available only while this mod's lifecycle is active".into(),
        );
    }
    if matches!(operation, Op::EcsWrite | Op::AssetsWrite)
        && let Some(reason) = host.write_fault()
    {
        return Some(reason);
    }
    match host.static_support(operation) {
        Availability::Available => None,
        Availability::Unavailable { reason } => Some(reason),
    }
}

fn availability_to_lua(lua: &Lua, availability: Availability) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    match availability {
        Availability::Available => table.raw_set("available", true)?,
        Availability::Unavailable { reason } => {
            table.raw_set("available", false)?;
            table.raw_set("reason", reason)?;
        }
    }
    Ok(table)
}

// check_ingame_access.rs:151-177, including source call order and reason precedence.
fn operation_catalog(lua: &Lua, host: &dyn RuntimeHost) -> mlua::Result<Table> {
    let result = lua.create_table()?;
    for op in Op::ALL {
        let name = op.as_str();
        let status = availability_to_lua(lua, operation_status(host, name))?;
        let denial = runtime_denial_reason(host, *op);
        let ready = available(host, name) && denial.is_none();
        status.raw_set("available", ready)?;
        if let Some(reason) = denial {
            status.raw_set("reason", reason)?;
        } else if !ready && status.raw_get::<Option<String>>("reason")?.is_none() {
            status.raw_set("reason", "runtime provider is not ready for this operation")?;
        }
        result.raw_set(name, status)?;
    }
    Ok(result)
}
