// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Lua binding source (identical in 1.7.9 d8a394bc and 1.8.0 38ef448d):
// modules/mod-runtime/lua/bindings/game/guid.rs:9-68.
//! Pure GUID helpers. Byte layout and parsing are independently authored from the
//! frozen public contract and synthetic oracle observations, not upstream's types.
use crate::lua::{CastLuaExt, FunctionArgs, LuaError, install_namespace};
use mlua::{Lua, ObjectLike, Table, Value};

const NONE: &str = "00000000-0000-0000-0000-000000000000";
const FIELDS: [&str; 4] = ["size", "hash0", "hash1", "hash2"];

/// Extend the owning package's existing `game` table, preserving all other family members.
/// `builtin.game` is the same table; no global `guid` is installed.
pub fn install(lua: &Lua, env: &Table) -> mlua::Result<()> {
    let game = match env.raw_get::<Value>("game")? {
        Value::Nil => lua.create_table()?,
        Value::Table(table) => table,
        _ => return Err(mlua::Error::runtime("namespace game must be a table")),
    };
    game.raw_set("guid", create(lua)?)?;
    install_namespace(env, "game", game)
}

pub fn create(lua: &Lua) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    table.raw_set("NONE", NONE)?;
    table.raw_set("from_content_hash", lua.create_function(from_content_hash)?)?;
    table.raw_set("to_content_hash", lua.create_function(to_content_hash)?)?;
    table.raw_set("hash", lua.create_function(hash)?)?;
    Ok(table)
}

fn from_content_hash(_: &Lua, args: FunctionArgs) -> mlua::Result<String> {
    let value = args.get::<Value>(0)?;
    let mut words = [0_u32; 4];
    for (word, key) in words.iter_mut().zip(FIELDS) {
        *word = match value {
            Value::Table(table) => table.get::<Value>(key)?.cast::<u32>()?,
            Value::UserData(data) => data.get::<Value>(key)?.cast::<u32>()?,
            _ => return Err(LuaError::bad_argument_type(0, "userdata or table", value)),
        };
    }
    let bytes = words.map(u32::to_le_bytes).concat();
    Ok(format!(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        words[0],
        words[1] & 0xffff,
        words[1] >> 16,
        bytes[8],
        bytes[9],
        bytes[10],
        bytes[11],
        bytes[12],
        bytes[13],
        bytes[14],
        bytes[15]
    ))
}

fn parsed(args: FunctionArgs) -> mlua::Result<[u8; 16]> {
    // SF's String conversion precedes parsing: even ignored suffix bytes must be UTF-8.
    let text = args.get::<String>(0)?;
    parse(&text).ok_or_else(|| LuaError::generic("invalid GUID format"))
}

fn to_content_hash(lua: &Lua, args: FunctionArgs) -> mlua::Result<Table> {
    let bytes = parsed(args)?;
    let table = lua.create_table()?;
    for (key, &word) in FIELDS.into_iter().zip(bytes.as_chunks::<4>().0) {
        table.raw_set(key, u32::from_le_bytes(word))?;
    }
    Ok(table)
}

fn hash(_: &Lua, args: FunctionArgs) -> mlua::Result<u32> {
    Ok(super::hasher::fnv1a32(&parsed(args)?))
}

fn parse(text: &str) -> Option<[u8; 16]> {
    let text = text.as_bytes().get(..36)?;
    for i in [8, 13, 18, 23] {
        if text[i] != b'-' {
            return None;
        }
    }
    // Both pins' synthetic parser probes reject '+' even in a numeric group or
    // byte pair. Validate hexadecimal digits before using Rust's radix parser.
    let hex = |start, end| {
        let digits = &text[start..end];
        if !digits.iter().all(u8::is_ascii_hexdigit) {
            return None;
        }
        u32::from_str_radix(std::str::from_utf8(digits).ok()?, 16).ok()
    };
    let mut bytes = [0_u8; 16];
    bytes[..4].copy_from_slice(&hex(0, 8)?.to_le_bytes());
    bytes[4..6].copy_from_slice(&(hex(9, 13)? as u16).to_le_bytes());
    bytes[6..8].copy_from_slice(&(hex(14, 18)? as u16).to_le_bytes());
    for (output, start) in [19, 21, 24, 26, 28, 30, 32, 34].into_iter().enumerate() {
        bytes[8 + output] = hex(start, start + 2)? as u8;
    }
    Some(bytes)
}
