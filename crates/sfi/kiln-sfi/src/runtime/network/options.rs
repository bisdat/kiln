// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/transport/lua/network.rs:34-55,185-189,312-321.
//! The arguments and options of the raw network functions, read exactly as ShroudForge's
//! typed closures read them (mlua 0.11.6 conversions in both): the options are read with
//! `raw_get` after the denial chain, so a bad option raises only for an admitted call.

use mlua::Table;

pub const CHANNEL_RANGE: &str =
    "Channel must be between 0 and 65534. 65535 is reserved for Network health checks";
pub const LIMIT_RANGE: &str = "receive limit must be between 1 and 32";
pub const PEER_NOT_DECIMAL: &str = "peer Steam ID must be a decimal uint64 string";
pub const PEER_ZERO: &str = "peer Steam ID must be nonzero";
/// ShroudForge's default receive limit.
pub const DEFAULT_LIMIT: usize = 16;

/// A peer Steam id: a decimal u64 (leading zeros accepted), never 0 (`steam_id`).
pub fn steam_id(value: &str) -> Result<u64, String> {
    let id = value
        .parse::<u64>()
        .map_err(|_| PEER_NOT_DECIMAL.to_owned())?;
    if id == 0 {
        return Err(PEER_ZERO.into());
    }
    Ok(id)
}

/// `options.channel` (default 0), raising outside 0..=65534 (`channel`).
pub fn channel(options: &Option<Table>) -> mlua::Result<i32> {
    let channel = match options {
        Some(options) => options.raw_get::<Option<i32>>("channel")?.unwrap_or(0),
        None => 0,
    };
    if !(0..=65_534).contains(&channel) {
        return Err(mlua::Error::runtime(CHANNEL_RANGE));
    }
    Ok(channel)
}

/// `options.reliable`: Lua truthiness, `true` when absent.
pub fn reliable(options: &Option<Table>) -> mlua::Result<bool> {
    Ok(match options {
        Some(options) => options.raw_get::<Option<bool>>("reliable")?.unwrap_or(true),
        None => true,
    })
}

/// `options.limit` (default 16), raising outside 1..=32.
pub fn limit(options: &Option<Table>) -> mlua::Result<usize> {
    let limit = match options {
        Some(options) => options
            .raw_get::<Option<usize>>("limit")?
            .unwrap_or(DEFAULT_LIMIT),
        None => DEFAULT_LIMIT,
    };
    if !(1..=32).contains(&limit) {
        return Err(mlua::Error::runtime(LIMIT_RANGE));
    }
    Ok(limit)
}
