// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Binding source (identical in 1.7.9 d8a394bc and 1.8.0 38ef448d):
// modules/mod-runtime/lua/bindings/hash_bytes.rs:9-52.
//! Raw-byte FNV-1a32 and standard CRCs, with nonadvancing Buffer views.
use super::buffer::ByteView;
use crate::lua::FunctionArgs;
use crc::Crc;
use mlua::{Lua, Table, Value};

// Independently authored from FNV-1a's public width32/offset/prime definition.
// No text encoding, terminator, or archive/content type is involved.
pub(crate) fn fnv1a32(bytes: &[u8]) -> u32 {
    bytes.iter().fold(0x811c9dc5_u32, |hash, &byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x01000193)
    })
}

pub fn create(lua: &Lua) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    table.raw_set(
        "fnv1a32",
        lua.create_function(|_, args: FunctionArgs| {
            let view = args.get::<ByteView>(0)?;
            Ok(fnv1a32(view.as_bytes()?))
        })?,
    )?;
    table.raw_set(
        "crc32",
        lua.create_function(|_, args: FunctionArgs| {
            let view = args.get::<ByteView>(0)?;
            Ok(Crc::<u32>::new(&crc::CRC_32_ISO_HDLC).checksum(view.as_bytes()?))
        })?,
    )?;
    table.raw_set(
        "crc64",
        lua.create_function(|_, args: FunctionArgs| {
            let view = args.get::<ByteView>(0)?;
            let hash = Crc::<u64>::new(&crc::CRC_64_ECMA_182).checksum(view.as_bytes()?);
            Ok(Value::Integer(hash as i64))
        })?,
    )?;
    Ok(table)
}
