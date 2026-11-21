// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/transport/lua/network.rs:17-32,61-81,142-160
// (what the network API reads of the loader settings), src/loader/package/src/config/
// loader.default.json:278-282 and loader.schema.json:757-773 (the network block's defaults).
//! The loader settings the network family reads, as one worker-local snapshot.
//!
//! ShroudForge reads its whole loader configuration (`read_loader`) on every network call.
//! The interpreter never reads a file on the worker: it
//! reads the latest validated document, or the reason it could not be read, from a single-key
//! state ([`SettingsSnapshot`]): a newer value replaces the older one. Until Kiln decides
//! where these settings live in kiln mode and a producer delivers them (on kiln-sfi's
//! IO thread), it holds ShroudForge's defaults ([`default_document`]): enabled, no configured
//! server, no allowlist. `<game>/shroudforge` is never read.

use serde_json::{Value, json};
use std::cell::RefCell;

/// The merged loader settings document, or why it is unavailable (ShroudForge's
/// `read_loader` error: the network API answers "Network module settings are unavailable").
pub type LoaderSettings = Result<Value, String>;

/// ShroudForge's default network block (loader.default.json:278-282).
pub fn default_document() -> Value {
    json!({
        "modules": {
            "network": {
                "enabled": true,
                "serverSteamId": "",
                "allowedClientSteamIds": ""
            }
        }
    })
}

/// The single-key settings state of one runtime-create (worker only).
pub struct SettingsSnapshot(RefCell<LoaderSettings>);
impl Default for SettingsSnapshot {
    fn default() -> Self {
        Self(RefCell::new(Ok(default_document())))
    }
}
impl SettingsSnapshot {
    /// The newest value replaces the one held (the state class's coalescing).
    pub fn replace(&self, settings: LoaderSettings) {
        *self.0.borrow_mut() = settings;
    }
    /// The document now, or why it is unavailable (a copy).
    pub fn snapshot(&self) -> LoaderSettings {
        self.0.borrow().clone()
    }
    /// `f` over the document now, or `None` while the settings are unavailable.
    pub fn read<R>(&self, f: impl FnOnce(&Value) -> R) -> Option<R> {
        self.0.borrow().as_ref().ok().map(f)
    }
}

/// ShroudForge's disabled check (network.rs:65): only an explicit `false` disables.
pub fn disabled(document: &Value) -> bool {
    document["modules"]["network"]["enabled"] == false
}

/// `status().configured_server_steam_id` (network.rs:144-150): the text, when it parses as a
/// nonzero u64 (leading zeros kept).
pub fn configured_server_steam_id(document: &Value) -> Option<String> {
    document
        .pointer("/modules/network/serverSteamId")
        .and_then(Value::as_str)
        .filter(|value| value.parse::<u64>().is_ok_and(|id| id != 0))
        .map(str::to_owned)
}

/// ShroudForge's `authorized_client_ids` (network.rs:17-32): the allowlist split on `,` and
/// `;`, trimmed, each kept when it is 16 to 20 ASCII digits and a nonzero u64; duplicates and
/// leading zeros are kept, as written.
pub fn authorized_client_ids(document: &Value) -> Vec<String> {
    document
        .pointer("/modules/network/allowedClientSteamIds")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .split([',', ';'])
        .map(str::trim)
        .filter(|peer| {
            (16..=20).contains(&peer.len())
                && peer.bytes().all(|byte| byte.is_ascii_digit())
                && peer.parse::<u64>().is_ok_and(|id| id != 0)
        })
        .map(str::to_owned)
        .collect()
}
