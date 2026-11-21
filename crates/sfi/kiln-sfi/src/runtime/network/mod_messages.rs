// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): modules/transport/lua/network.rs:92-100
// (mod_channel), 201-232 (send_mod), 350-409 (receive_mod); modules/transport/lib.rs:82-118.
//! `runtime.network.send_mod` and `receive_mod`, the `shroudforge.mod-message.v1`
//! envelope and the FNV-1a channel of a mod id.
//!
//! - The channel of a mod id is `(FNV-1a32(id bytes) % 65534) + 1` ([`mod_channel`]): never 0
//!   (the raw default) and never 65535 (health). Different ids can share a channel; the envelope
//!   carries the full target id, and Steam's per-channel queue is destructive, so on a collision
//!   the first mod to poll drops the other's message (as in ShroudForge).
//! - [`envelope`]: compact JSON with ShroudForge's key order (`protocol`, `from_mod`,
//!   `to_mod`, `payload`; its workspace builds serde_json with `preserve_order`), written here
//!   key by key so the bytes do not depend on this build's serde_json features. The size cap is
//!   checked on the serialized bytes, by R1's [`Network::send_bytes`].
//! - [`open`]: what `receive_mod` keeps of a message. Anything else is dropped silently: not
//!   JSON, another protocol, a `to_mod` other than the local id, a `from_mod` or `payload`
//!   that is not a string. Extra keys are ignored. The sender is not verified beyond that.
//! - Both functions use R1's denial chain under `runtime.network.send` / `receive` (not their
//!   own operation names), R1's send ([`Network::send_bytes`]) and R1's receive loop
//!   ([`super::raw::receive_loop`]): no second receive path, no requeue, no buffering.

use super::Network;
use super::options::{self, steam_id};
use super::raw::receive_loop;
use mlua::{Function, Lua, Table};
use sfi_core::bindings::runtime::RuntimeOperation as Op;
use std::rc::Rc;

/// The envelope's protocol string.
pub const PROTOCOL: &str = "shroudforge.mod-message.v1";
/// ShroudForge's text for an empty or longer than 96-byte target id.
pub const TARGET_LENGTH: &str = "target mod ID must contain 1 to 96 bytes";
/// The longest target id in bytes.
pub const TARGET_MAX_BYTES: usize = 96;

/// The Steam channel of a mod id: 32-bit FNV-1a (basis 0x811c9dc5, prime 0x01000193) over its
/// UTF-8 bytes, modulo 65534, plus 1 (ShroudForge's `mod_channel`).
pub fn mod_channel(id: &str) -> u32 {
    let hash = id.bytes().fold(0x811c_9dc5u32, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    });
    (hash % 65_534) + 1
}

/// The serialized envelope: `{"protocol":…,"from_mod":…,"to_mod":…,"payload":…}`, compact, each
/// value escaped as serde_json escapes a string.
pub fn envelope(from_mod: &str, to_mod: &str, payload: &str) -> serde_json::Result<Vec<u8>> {
    let mut bytes = Vec::with_capacity(72 + from_mod.len() + to_mod.len() + payload.len());
    for (index, (key, value)) in [
        ("protocol", PROTOCOL),
        ("from_mod", from_mod),
        ("to_mod", to_mod),
        ("payload", payload),
    ]
    .into_iter()
    .enumerate()
    {
        bytes.push(if index == 0 { b'{' } else { b',' });
        serde_json::to_writer(&mut bytes, key)?;
        bytes.push(b':');
        serde_json::to_writer(&mut bytes, value)?;
    }
    bytes.push(b'}');
    Ok(bytes)
}

/// A kept mod message: the sender's claimed mod id and the payload text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModMessage {
    pub from_mod: String,
    pub payload: String,
}

/// What `receive_mod` keeps of `bytes` for the local mod `local_id`; `None` drops it.
pub fn open(bytes: &[u8], local_id: &str) -> Option<ModMessage> {
    let envelope = serde_json::from_slice::<serde_json::Value>(bytes).ok()?;
    if envelope["protocol"] != PROTOCOL || envelope["to_mod"].as_str() != Some(local_id) {
        return None;
    }
    Some(ModMessage {
        from_mod: envelope["from_mod"].as_str()?.to_owned(),
        payload: envelope["payload"].as_str()?.to_owned(),
    })
}

/// `send_mod(peer, target_mod_id, payload, options)` (network.rs:201-232). Only
/// `options.reliable` is read; `options.channel` is ignored.
pub fn send_mod(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(
        move |_, (peer, target, payload, options): (String, String, String, Option<Table>)| {
            if let Some(reason) = network.denied(Op::NetworkSend) {
                return Ok((false, Some(reason)));
            }
            let peer = match steam_id(&peer) {
                Ok(peer) => peer,
                Err(reason) => return Ok((false, Some(reason))),
            };
            if target.is_empty() || target.len() > TARGET_MAX_BYTES {
                return Ok((false, Some(TARGET_LENGTH.to_owned())));
            }
            let reliable = options::reliable(&options)?;
            let bytes = envelope(network.id(), &target, &payload).map_err(mlua::Error::external)?;
            Ok(
                match network.send_bytes(peer, &bytes, mod_channel(&target), reliable) {
                    Ok(()) => (true, None),
                    Err(reason) => (false, Some(reason)),
                },
            )
        },
    )
}

/// `receive_mod(limit)` (network.rs:350-409): R1's receive loop on the local mod's channel; a
/// kept message is `{peer_steam_id, from_mod, to_mod = the local id, payload, reliable}`.
pub fn receive_mod(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(move |lua, limit: Option<usize>| {
        if let Some(reason) = network.denied(Op::NetworkReceive) {
            return Ok((None, Some(reason)));
        }
        let limit = limit.unwrap_or(options::DEFAULT_LIMIT);
        if !(1..=32).contains(&limit) {
            return Err(mlua::Error::runtime(options::LIMIT_RANGE));
        }
        let local_id = network.id().to_owned();
        receive_loop(
            lua,
            &network,
            mod_channel(&local_id),
            limit,
            |lua, message| {
                let Some(kept) = open(&message.payload, &local_id) else {
                    return Ok(None);
                };
                let value = lua.create_table()?;
                value.raw_set("peer_steam_id", message.peer_steam_id.to_string())?;
                value.raw_set("from_mod", kept.from_mod)?;
                value.raw_set("to_mod", local_id.as_str())?;
                value.raw_set("payload", kept.payload)?;
                value.raw_set("reliable", message.reliable)?;
                Ok(Some(value))
            },
        )
    })
}
