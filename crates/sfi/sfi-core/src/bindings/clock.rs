// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/shroudforge/v1/read_monotonic_clock.rs:6-9;
// lua/shroudforge/v1/create_mod_api.rs:22.
use crate::host::HostContext;
use mlua::{Function, Lua};

pub(super) fn create(lua: &Lua, context: &HostContext) -> mlua::Result<Function> {
    let clock = context
        .clock
        .clone()
        .ok_or_else(|| mlua::Error::runtime("clock host port is required"))?;
    lua.create_function(move |_, ()| Ok(clock.seconds()))
}
