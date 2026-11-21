// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.patch` and `runtime.functions` in the pregame VM. The patch functions refuse with the gameplay-patch
//! denial (after `available` converts its name); the function catalog has no native inventory without the provider, so
//! `list`, `find`, `get` and `bind_modifier` answer the bridge's refusal after their own argument conversions, and the
//! checked `get`/`bind` wrappers dispatch over the operations' captured functions as ShroudForge's do.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! lua/ingame/set_profile_patch_enabled.rs:3-30, functions/mod.rs:10-110, functions/read_function_catalog.rs:33-60 and
//! 393-516, functions/bind_profile_modifier.rs:3-18.
use mlua::{Function, Lua, Table};
use sfi_core::{
    bindings::runtime::{RuntimeOperation, install_function_operations},
    lua::LuaValue,
};

use super::{
    PregameRuntime,
    access::{FUNCTION_INVENTORY_MISSING, add, denial, past_denial},
};
use crate::sf_startup::game::resolve_type::resolve;

type Described = (Option<Table>, Option<String>);

/// SF `create_patch`.
pub(super) fn create_patch(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let patch = lua.create_table()?;
    let host = context.host.clone();
    add(lua, &patch, "available", move |_, args| {
        let _name = args.get::<String>(0)?;
        match denial(host.as_ref(), "runtime.gameplay.patch") {
            Some(_) => Ok(false),
            None => Err(past_denial("runtime.patch.available")),
        }
    })?;
    let host = context.host.clone();
    add(
        lua,
        &patch,
        "set_enabled",
        move |_, _| -> mlua::Result<(bool, Option<String>)> {
            match denial(host.as_ref(), "runtime.gameplay.patch") {
                Some(reason) => Ok((false, Some(reason))),
                None => Err(past_denial("runtime.patch.set_enabled")),
            }
        },
    )?;
    Ok(patch)
}

/// SF `Catalog::refresh` without the provider: the inventory never loads.
fn refresh() -> Result<(), String> {
    Err(FUNCTION_INVENTORY_MISSING.into())
}

/// SF `create_functions`: `get_operations`, the catalog's members, then the checked `get` and `bind`.
pub(super) fn create(lua: &Lua, runtime: &Table, context: &PregameRuntime) -> mlua::Result<Table> {
    let result = lua.create_table()?;
    install_function_operations(lua, &result, context.host.clone())?;
    attach_catalog(lua, &result, &runtime.raw_get::<Table>("patch")?, context)?;
    // Capture the existing checked wrappers (functions/mod.rs:23-48).
    let bindings = lua.create_table()?;
    for operation in RuntimeOperation::ALL {
        let id = operation.as_str();
        let path = match id {
            "runtime.world.context.active" => "runtime.world.context_active",
            "runtime.world.voxel.grid_spec" => "runtime.world.voxel.get_grid_spec",
            "runtime.lifecycle" | "runtime.gameplay.patch" => continue,
            id => id,
        };
        let mut value = LuaValue::Table(runtime.clone());
        for part in path.split('.').skip(1) {
            value = match value {
                LuaValue::Table(table) => table.raw_get::<LuaValue>(part)?,
                _ => LuaValue::Nil,
            };
        }
        if matches!(value, LuaValue::Function(_)) {
            bindings.raw_set(id, value)?;
        }
    }
    let native_get = result.raw_get::<Function>("get")?;
    let operation_get = native_get.clone();
    result.raw_set(
        "get",
        lua.create_function(move |_, selector: LuaValue| {
            // Without the inventory the native get never describes anything, so the operation wrapper (which marks a
            // described operation callable or not) has nothing to mark.
            let (descriptor, reason): Described = operation_get.call(selector)?;
            if descriptor.is_some() {
                return Err(past_denial("runtime.functions.get"));
            }
            Ok((descriptor, reason))
        })?,
    )?;
    let host = context.host.clone();
    result.raw_set(
        "bind",
        lua.create_function(
            move |_, id: String| -> mlua::Result<(Option<Function>, Option<String>)> {
                let function = bindings.raw_get::<Option<Function>>(id.as_str())?;
                if function.is_none() {
                    let (descriptor, reason): Described = native_get.call(id.clone())?;
                    if descriptor.is_none() {
                        return Ok((None, reason));
                    }
                    return Err(past_denial("runtime.functions.bind"));
                }
                match denial(host.as_ref(), &id) {
                    Some(reason) => Ok((None, Some(reason))),
                    None => Err(past_denial("runtime.functions.bind")),
                }
            },
        )?,
    )?;
    Ok(result)
}

/// SF `read_function_catalog::attach` and `bind_profile_modifier::attach` without the native inventory.
fn attach_catalog(lua: &Lua, table: &Table, patch: &Table, context: &PregameRuntime) -> mlua::Result<()> {
    let list = lua.create_function(
        |_, (_offset, _limit): (Option<usize>, Option<usize>)| -> mlua::Result<Described> {
            match refresh() {
                Err(reason) => Ok((None, Some(reason))),
                Ok(()) => Err(past_denial("runtime.functions.list")),
            }
        },
    )?;
    table.raw_set("list", list.clone())?;
    table.raw_set("list_native", list)?;
    let registry = context.types.registry().clone();
    table.raw_set(
        "find",
        lua.create_function(
            move |_,
                  (options, _offset, _limit): (Option<Table>, Option<usize>, Option<usize>)|
                  -> mlua::Result<Described> {
                let _name = options
                    .as_ref()
                    .map(|options| options.get::<Option<String>>("name"))
                    .transpose()?
                    .flatten();
                let component = options
                    .as_ref()
                    .map(|options| options.get::<LuaValue>("component"))
                    .transpose()?
                    .unwrap_or(LuaValue::Nil);
                if !matches!(component, LuaValue::Nil)
                    && let Err(reason) = resolve(&registry, &component)
                {
                    return Ok((None, Some(reason)));
                }
                match refresh() {
                    Err(reason) => Ok((None, Some(reason))),
                    Ok(()) => Err(past_denial("runtime.functions.find")),
                }
            },
        )?,
    )?;
    // bind_profile_modifier::attach captures the checked patch functions it would wrap.
    let _available = patch.raw_get::<Function>("available")?;
    let _set_enabled = patch.raw_get::<Function>("set_enabled")?;
    table.raw_set(
        "bind_modifier",
        lua.create_function(|_, _id: String| -> mlua::Result<Described> {
            match refresh() {
                Err(reason) => Ok((None, Some(reason))),
                Ok(()) => Err(past_denial("runtime.functions.bind_modifier")),
            }
        })?,
    )?;
    let get = lua.create_function(|_, _selector: LuaValue| -> mlua::Result<Described> {
        match refresh() {
            Err(reason) => Ok((None, Some(reason))),
            Ok(()) => Err(past_denial("runtime.functions.get")),
        }
    })?;
    table.raw_set("get", get.clone())?;
    table.raw_set("get_native", get)
}
