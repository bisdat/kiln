// SPDX-License-Identifier: GPL-3.0-or-later
//! The loaded-resource functions of `game.assets` in the pregame VM: `get_loaded_resource`,
//! `get_loaded_resource_by_field`, `prepare_loaded_index`, `resolve_loaded_resource`, `read_loaded_field`,
//! `write_loaded_field` and `get_loaded_resources_by_type`. ShroudForge registers them on every package's
//! `game.assets` in both phases (lua/bindings/game/assets/mod.rs:31, between `get_resources_by_type` and
//! `get_all_resources`); they read and write the game's live ResourceService, which does not exist before the game
//! starts. In the pregame phase each one answers ShroudForge's pregame answer in its own shape:
//! - `get_loaded_resource` and `get_loaded_resources_by_type` check the phase before anything else and raise
//!   "loaded ResourceService access is available only in the ingame phase";
//! - `resolve_loaded_resource` (`nil, reason`), `prepare_loaded_index` (`false, reason, nil`) and
//!   `get_loaded_resource_by_field` (`nil, reason, nil`) answer the `runtime.ecs.read` denial first;
//! - `write_loaded_field` answers the `runtime.assets.write` denial first (`nil, reason`): it never reaches a write, so
//!   it adds no write boundary and no writer beside the pass's one stage;
//! - `read_loaded_field` has no phase check: it takes exactly a Resource and a path (converted as ShroudForge converts
//!   them) and then answers `nil, "resource is not a runtime snapshot", nil`, because every Resource of the pass is an
//!   archive or created record (only `get_loaded_resource` makes runtime snapshots).
//!
//! The registrar refuses any phase but `Startup`, so the denial is always present; a function that would continue past
//! it is a setup error, never a stand-in.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! lua/ingame/resources/{mod,read_loaded_resources,index_loaded_resources,write_loaded_field}.rs and
//! session/loaded_resources.rs:116-119 (`loaded_snapshot_bytes`).
use std::rc::Rc;

use mlua::{Lua, Table};
use sfi_core::{
    bindings::runtime::RuntimeHost,
    lua::{FunctionArgs, LuaError, LuaValue},
};

use super::access::{add, denial, past_denial};
use crate::sf_startup::game::resource::Resource;

/// ShroudForge's raise of the two loaded lookups outside the ingame phase.
pub const INGAME_ONLY: &str = "loaded ResourceService access is available only in the ingame phase";
/// `loaded_snapshot_bytes` of a Resource that is not a loaded runtime snapshot.
pub const NOT_A_RUNTIME_SNAPSHOT: &str = "resource is not a runtime snapshot";

/// SF `resources::attach` (lua/ingame/resources/mod.rs:17-55) into one package's `game.assets`, in its order.
pub fn attach(lua: &Lua, table: &Table, host: Rc<dyn RuntimeHost>) -> mlua::Result<()> {
    // read_loaded_resources.rs:79-88: the phase first, before any argument
    add(
        lua,
        table,
        "get_loaded_resource",
        |_, _: FunctionArgs| -> mlua::Result<()> { Err(LuaError::generic(INGAME_ONLY)) },
    )?;
    // index_loaded_resources.rs:209-220: the read denial, then (nil, reason, nil)
    let h = host.clone();
    add(
        lua,
        table,
        "get_loaded_resource_by_field",
        move |_, _: FunctionArgs| -> mlua::Result<(Option<Table>, Option<String>, Option<Table>)> {
            Ok((
                None,
                Some(read_denial(h.as_ref(), "get_loaded_resource_by_field")?),
                None,
            ))
        },
    )?;
    // index_loaded_resources.rs:161-172: the read denial, then (false, reason, nil)
    let h = host.clone();
    add(
        lua,
        table,
        "prepare_loaded_index",
        move |_, _: FunctionArgs| -> mlua::Result<(bool, Option<String>, Option<Table>)> {
            Ok((false, Some(read_denial(h.as_ref(), "prepare_loaded_index")?), None))
        },
    )?;
    // read_loaded_resources.rs:3-13: the read denial, then (nil, reason)
    let h = host.clone();
    add(
        lua,
        table,
        "resolve_loaded_resource",
        move |_, _: FunctionArgs| -> mlua::Result<(Option<Table>, Option<String>)> {
            Ok((None, Some(read_denial(h.as_ref(), "resolve_loaded_resource")?)))
        },
    )?;
    // read_loaded_resources.rs:159-189: no phase check
    add(
        lua,
        table,
        "read_loaded_field",
        |_, args: FunctionArgs| -> mlua::Result<(LuaValue, Option<String>, LuaValue)> {
            let read = || -> Result<(), String> {
                if args.len() != 2 {
                    return Err("read_loaded_field expects a loaded Resource and reflected field path".into());
                }
                let _resource = args.get::<&Resource>(0).map_err(|error| error.to_string())?;
                let _path = args.get::<String>(1).map_err(|error| error.to_string())?;
                // loaded_snapshot_bytes: an archive or created record is never a runtime snapshot
                Err(NOT_A_RUNTIME_SNAPSHOT.into())
            };
            let reason = read().err();
            Ok((LuaValue::Nil, reason, LuaValue::Nil))
        },
    )?;
    // write_loaded_field.rs:7-17: the assets write denial (then the ECS one), then (nil, reason)
    let h = host;
    add(
        lua,
        table,
        "write_loaded_field",
        move |_, _: FunctionArgs| -> mlua::Result<(Option<Table>, Option<String>)> {
            for operation in ["runtime.assets.write", "runtime.ecs.write"] {
                if let Some(reason) = denial(h.as_ref(), operation) {
                    return Ok((None, Some(reason)));
                }
            }
            Err(past_denial("game.assets.write_loaded_field"))
        },
    )?;
    // read_loaded_resources.rs:124-134: the phase first, before any argument
    add(
        lua,
        table,
        "get_loaded_resources_by_type",
        |_, _: FunctionArgs| -> mlua::Result<()> { Err(LuaError::generic(INGAME_ONLY)) },
    )
}

/// The `runtime.ecs.read` denial the pregame phase always has.
fn read_denial(host: &dyn RuntimeHost, name: &str) -> mlua::Result<String> {
    denial(host, "runtime.ecs.read").ok_or_else(|| past_denial(&format!("game.assets.{name}")))
}
