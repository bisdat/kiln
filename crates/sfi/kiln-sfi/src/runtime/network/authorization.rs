// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): modules/transport/
// read_network_allowlist.rs:1-18, native/steam_network.cpp:192-246 (the server log scan), :344-374 (the
// authorized snapshot), :503-514 and lib.rs:233-250 (connected peers), lua/network.rs:259-303.
//! Server authorization (row 44) and `runtime.network.connected_peers` (row 5).
//!
//! - [`read_allowlist`]: ShroudForge's `read_network_allowlist`: the valid tokens (kept as written: duplicates and
//!   leading zeros) and whether the list restricts at all (its trimmed raw text is not empty, so an invalid-only
//!   list authorizes nobody).
//! - [`LogAuth`]: the dedicated server log's line state machine: `[online] Added peer <key> ... (steamid:N)` puts
//!   a live peer under its key, `[online] Removed peer <key> ` takes it out, `Client 'N' ... authenticated by steam`
//!   adds N to the authenticated set and `... not authenticated by steam` removes it. Peers = live ∩ authenticated,
//!   ascending, each once. [`full_scan`] is ShroudForge's per-call scan of the whole file; kiln-sfi follows the
//!   log incrementally instead ([`super::log_follow`], on its IO thread, parity row P-N1). The log is untrusted
//!   text, so the state is bounded where ShroudForge's is not: at most [`MAX_LIVE_PEERS`] live keys of at most
//!   [`MAX_PEER_KEY`] bytes and [`MAX_AUTHENTICATED`] ids (P-N1; no game log comes near them).
//! - [`connected_peers`]: the Lua function. Server only; the follower's latest state, at most 64 peers
//!   (ShroudForge's fixed buffer), filtered by the allowlist's literal strings only when that list is not empty.

use super::Network;
use mlua::{Function, Lua};
use serde_json::Value;
use sfi_core::bindings::runtime::RuntimeOperation as Op;
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::rc::Rc;

/// More valid allowlist tokens than this skip the whole maintenance pass (the native service tick,
/// steam_network.cpp:347).
pub const MAX_ALLOWLIST: usize = 256;
/// ShroudForge's `connected_peers` buffer (lib.rs:241).
pub const MAX_CONNECTED_PEERS: usize = 64;
/// Live peer keys the server log state holds at most; an `Added peer` line with a new key past this is ignored (a
/// bound, not a ShroudForge behaviour: the game has at most 16 players, and each `Removed peer` line frees a key).
pub const MAX_LIVE_PEERS: usize = 16_384;
/// The longest live peer key kept; an `Added peer` line with a longer key adds nothing (the game's keys are short).
pub const MAX_PEER_KEY: usize = 256;
/// Authenticated Steam ids the server log state holds at most; a new id past this is ignored.
pub const MAX_AUTHENTICATED: usize = 65_536;
/// `connected_peers` outside a dedicated server (network.rs:271-277).
pub const SERVER_ONLY: &str = "connected_peers is available only in the Dedicated Server runtime";
/// `connected_peers` when the log could not be read or holds more than 64 peers (lib.rs:243-244).
pub const UNREADABLE: &str = "could not read authenticated Enshrouded server peers";

/// The allowlist as ShroudForge reads it from the loader settings.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Allowlist {
    /// The valid tokens, as written (16 to 20 ASCII digits, a nonzero u64).
    pub ids: Vec<String>,
    /// The trimmed raw text is not empty: only listed peers are authorized.
    pub restrict: bool,
}
impl Allowlist {
    /// The ids the native side gets (`service_tick`: each token parsed, duplicates kept).
    pub fn native_ids(&self) -> Vec<u64> {
        self.ids
            .iter()
            .filter_map(|id| id.parse::<u64>().ok())
            .collect()
    }
}

/// ShroudForge's `read_network_allowlist`.
pub fn read_allowlist(document: &Value) -> Allowlist {
    let raw = document
        .pointer("/modules/network/allowedClientSteamIds")
        .and_then(Value::as_str)
        .unwrap_or_default();
    Allowlist {
        ids: super::settings::authorized_client_ids(document),
        restrict: !raw.trim().is_empty(),
    }
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// `parse_peer_key`: the bytes after `marker` up to the next space (none, or an empty key: no match).
fn peer_key<'a>(line: &'a [u8], marker: &[u8]) -> Option<&'a [u8]> {
    let value = find(line, marker)? + marker.len();
    let end = value + line[value..].iter().position(|b| *b == b' ')?;
    (end != value).then(|| &line[value..end])
}

/// `parse_steam_id`: from `start`, skip to the first digit, take the digits; they must fit a u64 (`std::stoull`
/// throws otherwise) and not be 0.
fn steam_id(line: &[u8], start: usize) -> Option<u64> {
    let rest = line.get(start..)?;
    let first = rest.iter().position(u8::is_ascii_digit)?;
    let digits = &rest[first..];
    let len = digits
        .iter()
        .position(|b| !b.is_ascii_digit())
        .unwrap_or(digits.len());
    let id = std::str::from_utf8(&digits[..len])
        .ok()?
        .parse::<u64>()
        .ok()?;
    (id != 0).then_some(id)
}

const ADDED: &[u8] = b"[online] Added peer ";
const REMOVED: &[u8] = b"[online] Removed peer ";
const STEAMID: &[u8] = b"(steamid:";
const AUTHENTICATED: &[u8] = b"authenticated by steam";
const NOT_AUTHENTICATED: &[u8] = b"not authenticated by steam";
const CLIENT: &[u8] = b"Client '";

/// The server log's line state machine (`read_authenticated_server_peers`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LogAuth {
    live: BTreeMap<Vec<u8>, u64>,
    authenticated: BTreeSet<u64>,
}
impl LogAuth {
    /// One line, without its line break (the branches in ShroudForge's order).
    pub fn apply(&mut self, line: &[u8]) {
        if let Some(key) = peer_key(line, ADDED) {
            if key.len() <= MAX_PEER_KEY
                && let Some(at) = find(line, STEAMID)
                && let Some(id) = steam_id(line, at + STEAMID.len())
                && (self.live.len() < MAX_LIVE_PEERS || self.live.contains_key(key))
            {
                self.live.insert(key.to_vec(), id);
            }
        } else if find(line, AUTHENTICATED).is_some() && find(line, NOT_AUTHENTICATED).is_none() {
            if let Some(at) = find(line, CLIENT)
                && let Some(id) = steam_id(line, at + CLIENT.len())
                && (self.authenticated.len() < MAX_AUTHENTICATED
                    || self.authenticated.contains(&id))
            {
                self.authenticated.insert(id);
            }
        } else if find(line, NOT_AUTHENTICATED).is_some() {
            if let Some(at) = find(line, CLIENT)
                && let Some(id) = steam_id(line, at + CLIENT.len())
            {
                self.authenticated.remove(&id);
            }
        } else if let Some(key) = peer_key(line, REMOVED) {
            self.live.remove(key);
        }
    }
    /// Live and authenticated peers, ascending, each once.
    pub fn peers(&self) -> Vec<u64> {
        let active: BTreeSet<u64> = self
            .live
            .values()
            .filter(|id| self.authenticated.contains(id))
            .copied()
            .collect();
        active.into_iter().collect()
    }
}

/// The lines `std::getline` gives over a text-mode file: split at `\n`, a `\r` right before it dropped (CRLF
/// translation), the last piece only when it is not empty.
pub fn lines(log: &[u8]) -> impl Iterator<Item = &[u8]> {
    let mut pieces: Vec<&[u8]> = log.split(|b| *b == b'\n').collect();
    let last = pieces.pop().filter(|piece| !piece.is_empty());
    let complete = pieces
        .into_iter()
        .map(|line| line.strip_suffix(b"\r").unwrap_or(line));
    complete.chain(last)
}

/// ShroudForge's scan of the whole log (each maintenance pass and each `connected_peers` call).
pub fn full_scan(log: &[u8]) -> Vec<u64> {
    let mut auth = LogAuth::default();
    for line in lines(log) {
        auth.apply(line);
    }
    auth.peers()
}

/// `runtime.network.connected_peers()` (network.rs:259-303).
pub fn connected_peers(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(move |lua, ()| {
        if let Some(reason) = network.denied(Op::NetworkConnectedPeers) {
            return Ok((None, Some(reason)));
        }
        if !network.is_server() {
            return Ok((None, Some(SERVER_ONLY.to_owned())));
        }
        let peers = match network.shared().ports().connected_peers() {
            Some(peers) if peers.len() <= MAX_CONNECTED_PEERS => peers,
            _ => return Ok((None, Some(UNREADABLE.to_owned()))),
        };
        // A second settings read; its own failure answers the settings' error text.
        let allowed = match network.shared().settings().snapshot() {
            Ok(document) => super::settings::authorized_client_ids(&document),
            Err(reason) => return Ok((None, Some(reason))),
        };
        let allowed = (!allowed.is_empty()).then(|| allowed.into_iter().collect::<HashSet<_>>());
        let result = lua.create_table()?;
        let mut index = 0;
        for peer in peers.iter().map(u64::to_string) {
            if allowed
                .as_ref()
                .is_none_or(|allowed| allowed.contains(&peer))
            {
                index += 1;
                result.raw_set(index, peer)?;
            }
        }
        Ok((Some(result), None))
    })
}
