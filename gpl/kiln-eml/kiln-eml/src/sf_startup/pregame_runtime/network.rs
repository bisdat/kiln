// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.network` in the pregame VM (modules/transport/lua/network.rs:93-412): each function converts its typed
//! arguments, then refuses with the denial; `status` answers its table with no transport and the loader's network
//! settings. Kiln has no ShroudForge loader configuration, so those are ShroudForge's defaults
//! (modules/loader-config/loader.default.json: `serverSteamId` and `allowedClientSteamIds` empty), what a pregame
//! ShroudForge reads without a loader file. Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice
//! in sf_startup/mod.rs).
use mlua::{Lua, Table};
use sfi_core::host::Side;

use super::{
    PregameRuntime,
    access::{denial, past_denial},
};

pub(super) fn create(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let network = lua.create_table()?;
    let host = context.host.clone();
    let server = context.side() == Side::Server;
    network.set(
        "status",
        lua.create_function(move |lua, ()| {
            let reason =
                denial(host.as_ref(), "runtime.network.status").ok_or_else(|| past_denial("runtime.network.status"))?;
            let result = lua.create_table()?;
            result.raw_set("available", false)?;
            result.raw_set("role", if server { "server" } else { "client" })?;
            result.raw_set("service_ready", false)?;
            // read_loader with no loader file: the defaults (empty server id, no allowed clients)
            result.raw_set("allowed_client_steam_ids", lua.create_table()?)?;
            result.raw_set("reason", reason)?;
            Ok(result)
        })?,
    )?;
    let host = context.host.clone();
    network.set(
        "send",
        lua.create_function(
            move |_,
                  (_peer, _payload, _options): (String, mlua::String, Option<Table>)|
                  -> mlua::Result<(bool, Option<String>)> {
                Ok((
                    false,
                    Some(
                        denial(host.as_ref(), "runtime.network.send")
                            .ok_or_else(|| past_denial("runtime.network.send"))?,
                    ),
                ))
            },
        )?,
    )?;
    let host = context.host.clone();
    network.set(
        "send_mod",
        lua.create_function(
            move |_,
                  (_peer, _target, _payload, _options): (String, String, String, Option<Table>)|
                  -> mlua::Result<(bool, Option<String>)> {
                // send_mod checks runtime.network.send (network.rs:203)
                Ok((
                    false,
                    Some(
                        denial(host.as_ref(), "runtime.network.send")
                            .ok_or_else(|| past_denial("runtime.network.send_mod"))?,
                    ),
                ))
            },
        )?,
    )?;
    let host = context.host.clone();
    network.set(
        "accept",
        lua.create_function(move |_, _peer: String| -> mlua::Result<(bool, Option<String>)> {
            Ok((
                false,
                Some(
                    denial(host.as_ref(), "runtime.network.accept")
                        .ok_or_else(|| past_denial("runtime.network.accept"))?,
                ),
            ))
        })?,
    )?;
    let host = context.host.clone();
    network.set(
        "connected_peers",
        lua.create_function(move |_, ()| -> mlua::Result<(Option<Table>, Option<String>)> {
            Ok((
                None,
                Some(
                    denial(host.as_ref(), "runtime.network.connected_peers")
                        .ok_or_else(|| past_denial("runtime.network.connected_peers"))?,
                ),
            ))
        })?,
    )?;
    let host = context.host.clone();
    network.set(
        "receive",
        lua.create_function(
            move |_, _options: Option<Table>| -> mlua::Result<(Option<Table>, Option<String>)> {
                Ok((
                    None,
                    Some(
                        denial(host.as_ref(), "runtime.network.receive")
                            .ok_or_else(|| past_denial("runtime.network.receive"))?,
                    ),
                ))
            },
        )?,
    )?;
    let host = context.host.clone();
    network.set(
        "receive_mod",
        lua.create_function(
            move |_, _limit: Option<usize>| -> mlua::Result<(Option<Table>, Option<String>)> {
                // receive_mod checks runtime.network.receive (network.rs:349)
                Ok((
                    None,
                    Some(
                        denial(host.as_ref(), "runtime.network.receive")
                            .ok_or_else(|| past_denial("runtime.network.receive_mod"))?,
                    ),
                ))
            },
        )?,
    )?;
    Ok(network)
}
