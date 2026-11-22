// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/transport/lua/network.rs:173-199,234-257,305-348;
// modules/transport/lib.rs:7,82-118,199-231,252-301.
//! `runtime.network.send`, `receive` and `accept` on the Steam-messages lease.
//!
//! Each Lua call is one or a few direct lease calls on the worker; nothing waits or retries.
//! Results map to ShroudForge's texts (this result table):
//!
//! | lease result | send / accept | receive |
//! |---|---|---|
//! | member missing | "Steam Networking runtime module is unavailable" | same |
//! | UNAVAILABLE, or any other code | "Steam Networking Messages interface is unavailable" | "Steam Networking receive failed or returned an invalid message" |
//! | OK, EResult other than 1 | "Steam returned EResult <n>" | — |

use super::options::{self, steam_id};
use super::{CallError, KilnNetApi, Network, l16::*, text};
use crate::service_member;
use kiln_abi::KILN_OK;
use mlua::{Function, Lua, Table};
use sfi_core::bindings::runtime::RuntimeOperation as Op;
use std::rc::Rc;

/// ShroudForge's `MAX_MESSAGE_BYTES`.
pub const MAX_MESSAGE_BYTES: usize = 512 * 1024;
/// Steam's EResult for success.
const STEAM_OK: i32 = 1;

fn too_big() -> String {
    format!("payload exceeds {MAX_MESSAGE_BYTES} bytes")
}

/// One message off the lease, as ShroudForge's `steam_network::receive` answers it.
pub struct Message {
    pub peer_steam_id: u64,
    pub payload: Vec<u8>,
    pub reliable: bool,
}

impl Network {
    /// ShroudForge's `steam_network::send` after the Lua checks: the size, then the lease.
    pub fn send_bytes(
        &self,
        peer: u64,
        payload: &[u8],
        channel: u32,
        reliable: bool,
    ) -> Result<(), String> {
        if payload.len() > MAX_MESSAGE_BYTES {
            return Err(too_big());
        }
        let missing = || text::MODULE_UNAVAILABLE.to_owned();
        let table = self.table().map_err(|_| missing())?;
        let send = service_member!(table, KilnNetApi, steam_send).map_err(|_| missing())?;
        let lease = match self.lease() {
            Ok(lease) => lease,
            Err(CallError::Missing) => return Err(missing()),
            Err(_) => return Err(text::INTERFACE_UNAVAILABLE.into()),
        };
        // ShroudForge sends reliable with Steam's flags 40 and unreliable with 32.
        let flags = KILN_STEAM_SEND_RESTART_BROKEN
            | if reliable {
                KILN_STEAM_SEND_RELIABLE
            } else {
                KILN_STEAM_SEND_UNRELIABLE
            };
        let mut steam_result = 0i32;
        // SAFETY: kiln_net.h steam_send: `payload` is readable for its length (at most 512 KiB), a live out.
        let result = unsafe {
            self.services().call(
                send,
                (
                    self.owner,
                    lease,
                    peer,
                    payload.as_ptr().cast(),
                    payload.len() as u32,
                    channel,
                    flags,
                    &raw mut steam_result,
                ),
            )
        };
        if result != KILN_OK {
            return Err(text::INTERFACE_UNAVAILABLE.into());
        }
        if steam_result == STEAM_OK {
            Ok(())
        } else {
            Err(format!("Steam returned EResult {steam_result}"))
        }
    }

    /// ShroudForge's `steam_network::receive`: at most one message off `channel` into the one
    /// 512 KiB buffer; `Ok(None)` when there is none.
    pub fn receive_one(&self, channel: u32) -> Result<Option<Message>, String> {
        let missing = || text::MODULE_UNAVAILABLE.to_owned();
        let failed = || text::RECEIVE_FAILED.to_owned();
        let table = self.table().map_err(|_| missing())?;
        let receive = service_member!(table, KilnNetApi, steam_receive).map_err(|_| missing())?;
        let lease = match self.lease() {
            Ok(lease) => lease,
            Err(CallError::Missing) => return Err(missing()),
            Err(_) => return Err(failed()),
        };
        let mut buffer = self.shared().buffer.borrow_mut();
        if buffer.len() != MAX_MESSAGE_BYTES {
            buffer.resize(MAX_MESSAGE_BYTES, 0);
        }
        let mut info: KilnSteamMessageInfo = record();
        let mut got = 0u32;
        // SAFETY: kiln_net.h steam_receive: a buffer of `cap` writable bytes, a sized info record, live outs.
        let result = unsafe {
            self.services().call(
                receive,
                (
                    self.owner,
                    lease,
                    channel,
                    buffer.as_mut_ptr().cast(),
                    MAX_MESSAGE_BYTES as u32,
                    &raw mut info,
                    &raw mut got,
                ),
            )
        };
        match (result, got) {
            (KILN_OK, 0) => Ok(None),
            (KILN_OK, 1) if info.peer_steam_id != 0 && info.length as usize <= buffer.len() => {
                Ok(Some(Message {
                    peer_steam_id: info.peer_steam_id,
                    payload: buffer[..info.length as usize].to_vec(),
                    reliable: info.flags & KILN_STEAM_SEND_RELIABLE != 0,
                }))
            }
            _ => Err(failed()),
        }
    }

    /// ShroudForge's `steam_network::accept`: `Ok(false)` when Steam had no pending session.
    pub fn accept_peer(&self, peer: u64) -> Result<bool, String> {
        let missing = || text::MODULE_UNAVAILABLE.to_owned();
        let table = self.table().map_err(|_| missing())?;
        let accept = service_member!(table, KilnNetApi, steam_accept).map_err(|_| missing())?;
        let lease = match self.lease() {
            Ok(lease) => lease,
            Err(CallError::Missing) => return Err(missing()),
            Err(_) => return Err(text::INTERFACE_UNAVAILABLE.into()),
        };
        let mut accepted = 0u32;
        // SAFETY: kiln_net.h steam_accept: a live out.
        let result = unsafe {
            self.services()
                .call(accept, (self.owner, lease, peer, &raw mut accepted))
        };
        if result != KILN_OK {
            return Err(text::INTERFACE_UNAVAILABLE.into());
        }
        Ok(accepted == 1)
    }

    /// The server drops a message from a peer outside the authorized snapshot.
    fn dropped(&self, peer: u64) -> bool {
        self.is_server() && !self.shared().ports().authorized_server_peer(peer)
    }
}

/// `send(peer, payload, options)` (network.rs:173-199).
pub fn send(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(
        move |_, (peer, payload, options): (String, mlua::String, Option<Table>)| {
            if let Some(reason) = network.denied(Op::NetworkSend) {
                return Ok((false, Some(reason)));
            }
            let peer = match steam_id(&peer) {
                Ok(peer) => peer,
                Err(reason) => return Ok((false, Some(reason))),
            };
            let channel = options::channel(&options)?;
            let reliable = options::reliable(&options)?;
            let payload = payload.as_bytes().to_vec();
            Ok(
                match network.send_bytes(peer, &payload, channel as u32, reliable) {
                    Ok(()) => (true, None),
                    Err(reason) => (false, Some(reason)),
                },
            )
        },
    )
}

/// ShroudForge's receive loop, shared by `receive` and `receive_mod` (network.rs:323-346,
/// 368-406): up to `limit` attempts on `channel`, one message each. On a server a message from
/// a peer outside the authorized snapshot is dropped; `deliver` turns a kept message into its Lua
/// value or drops it (`None`). A dropped message still counts as an attempt; an empty queue ends
/// the loop; a failure returns what was taken so far and the reason.
pub(super) fn receive_loop(
    lua: &Lua,
    network: &Network,
    channel: u32,
    limit: usize,
    mut deliver: impl FnMut(&Lua, Message) -> mlua::Result<Option<Table>>,
) -> mlua::Result<(Option<Table>, Option<String>)> {
    let messages = lua.create_table()?;
    let mut output_index = 0;
    for _ in 1..=limit {
        match network.receive_one(channel) {
            Ok(Some(message)) => {
                if network.dropped(message.peer_steam_id) {
                    continue;
                }
                let Some(value) = deliver(lua, message)? else {
                    continue;
                };
                output_index += 1;
                messages.raw_set(output_index, value)?;
            }
            Ok(None) => break,
            Err(reason) => return Ok((Some(messages), Some(reason))),
        }
    }
    Ok((Some(messages), None))
}

/// `receive(options)` (network.rs:305-348): the raw messages of the receive loop.
pub fn receive(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(move |lua, options: Option<Table>| {
        if let Some(reason) = network.denied(Op::NetworkReceive) {
            return Ok((None, Some(reason)));
        }
        let channel = options::channel(&options)?;
        let limit = options::limit(&options)?;
        receive_loop(lua, &network, channel as u32, limit, |lua, message| {
            let value = lua.create_table()?;
            value.raw_set("peer_steam_id", message.peer_steam_id.to_string())?;
            value.raw_set("payload", lua.create_string(&message.payload)?)?;
            value.raw_set("reliable", message.reliable)?;
            Ok(Some(value))
        })
    })
}

/// `accept(peer)` (network.rs:234-257).
pub fn accept(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(move |_, peer: String| {
        if let Some(reason) = network.denied(Op::NetworkAccept) {
            return Ok((false, Some(reason)));
        }
        let peer = match steam_id(&peer) {
            Ok(peer) => peer,
            Err(reason) => return Ok((false, Some(reason))),
        };
        if network.is_server() && !network.shared().ports().authorized_server_peer(peer) {
            return Ok((false, Some(text::NOT_AUTHORIZED.into())));
        }
        Ok(match network.accept_peer(peer) {
            Ok(true) => (true, None),
            Ok(false) => (false, Some(text::NO_PENDING_SESSION.into())),
            Err(reason) => (false, Some(reason)),
        })
    })
}
