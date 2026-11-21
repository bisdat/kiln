// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.impact` and `runtime.services` in the pregame VM: each refuses with the read denial before it looks at its
//! arguments, in its `nil, reason` shape. Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice
//! in sf_startup/mod.rs): lua/ingame/impact/mod.rs:12-160, lua/ingame/access_named_service.rs:21-105.
use mlua::{Lua, Table};
use sfi_core::lua::FunctionArgs;

use super::{
    PregameRuntime,
    access::{add, denial, past_denial},
};

fn read_denied(lua: &Lua, table: &Table, context: &PregameRuntime, name: &str, path: &'static str) -> mlua::Result<()> {
    let host = context.host.clone();
    add(
        lua,
        table,
        name,
        move |_, _: FunctionArgs| -> mlua::Result<(Option<Table>, Option<String>)> {
            match denial(host.as_ref(), "runtime.ecs.read") {
                Some(reason) => Ok((None, Some(reason))),
                None => Err(past_denial(path)),
            }
        },
    )
}

/// SF `impact::create`: `call`.
pub(super) fn create_impact(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    read_denied(lua, &table, context, "call", "runtime.impact.call")?;
    Ok(table)
}

/// SF `access_named_service::create`: `get`.
pub(super) fn create_services(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    read_denied(lua, &table, context, "get", "runtime.services.get")?;
    Ok(table)
}
