// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/transport/lua/network.rs:105-171; modules/transport/
// lib.rs:39-80,303-311.
//! `runtime.network.status()`, which always answers a table.
//!
//! - `available`: no denial and the lease AVAILABLE now; `role`: "server" or "client" from the
//!   package's side; ids are decimal strings and absent keys when unknown.
//! - A denied call reads no native status: every native field is absent or false.
//! - `local_dedicated_server_steam_id` / `remote_dedicated_server_steam_id` come from the maintenance's
//!   ports, only while the lease is available (ShroudForge reads them only then).
//! - `configured_server_steam_id` and `allowed_client_steam_ids` come from a second settings
//!   read, independent of the denial; both keys are absent while the settings are unavailable.
//! - `reason`: the denial, or ShroudForge's "not initialized" text when the lease is not
//!   available.
//!
//! The native status is read again on every call, never cached.

use super::{Network, l16::*, settings, steam_status, text};
use mlua::{Function, Lua};
use sfi_core::bindings::runtime::RuntimeOperation as Op;
use std::rc::Rc;

/// ShroudForge's `steam_network::Status`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NativeStatus {
    pub available: bool,
    pub local_steam_id: Option<String>,
    pub local_dedicated_server_steam_id: Option<String>,
    pub remote_dedicated_server_steam_id: Option<String>,
    pub service_ready: bool,
}

impl Network {
    /// ShroudForge's `steam_network::status()` on the lease (lib.rs:39-80): unavailable unless
    /// the lease's status answers with AVAILABLE.
    pub fn native_status(&self) -> NativeStatus {
        let Some(status) = steam_status(self.services()) else {
            return NativeStatus::default();
        };
        if status.flags & KILN_STEAM_MSG_AVAILABLE == 0 {
            return NativeStatus::default();
        }
        let nonzero = |id: Option<u64>| id.filter(|id| *id != 0).map(|id| id.to_string());
        let identity = status.flags & KILN_STEAM_MSG_IDENTITY != 0;
        let ports = self.shared().ports();
        NativeStatus {
            available: true,
            local_steam_id: nonzero(identity.then_some(status.local_steam_id)),
            local_dedicated_server_steam_id: nonzero(ports.local_dedicated_server_steam_id()),
            remote_dedicated_server_steam_id: nonzero(ports.remote_dedicated_server_steam_id()),
            service_ready: status.flags & KILN_STEAM_MSG_REGISTERED != 0,
        }
    }
}

/// `status()` (network.rs:105-171). Arguments are ignored, as ShroudForge's `()` closure does.
pub fn create(lua: &Lua, network: Rc<Network>) -> mlua::Result<Function> {
    lua.create_function(move |lua, ()| {
        let reason = network.denied(Op::NetworkStatus);
        let native = if reason.is_none() {
            network.native_status()
        } else {
            NativeStatus::default()
        };
        let result = lua.create_table()?;
        let available = reason.is_none() && native.available;
        result.raw_set("available", available)?;
        result.raw_set(
            "role",
            if network.is_server() {
                "server"
            } else {
                "client"
            },
        )?;
        result.raw_set("local_steam_id", native.local_steam_id)?;
        result.raw_set(
            "local_dedicated_server_steam_id",
            native.local_dedicated_server_steam_id,
        )?;
        result.raw_set(
            "remote_dedicated_server_steam_id",
            native.remote_dedicated_server_steam_id,
        )?;
        result.raw_set("service_ready", native.service_ready)?;
        let configured = network.shared().settings().read(|document| {
            (
                settings::configured_server_steam_id(document),
                settings::authorized_client_ids(document),
            )
        });
        if let Some((server, allowed_ids)) = configured {
            result.raw_set("configured_server_steam_id", server)?;
            let allowed = lua.create_table()?;
            for (index, peer) in allowed_ids.iter().enumerate() {
                allowed.raw_set(index + 1, peer.as_str())?;
            }
            result.raw_set("allowed_client_steam_ids", allowed)?;
        }
        result.raw_set(
            "reason",
            reason.or_else(|| (!native.available).then(|| text::NOT_INITIALIZED.to_owned())),
        )?;
        Ok(result)
    })
}
