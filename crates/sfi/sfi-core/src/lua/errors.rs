// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/errors.rs:3-96.
use super::LuaValue;

/// SF's external-error roots. mlua supplies callback wrappers and Lua tracebacks.
pub struct LuaError;
impl LuaError {
    pub fn generic(message: impl Into<String>) -> mlua::Error {
        mlua::Error::external(message.into())
    }
    pub fn bad_argument_type(
        index: usize,
        expected: impl Into<String>,
        got: &LuaValue,
    ) -> mlua::Error {
        Self::generic(format!(
            "bad argument #{} ({} expected, got {})",
            index + 1,
            expected.into(),
            type_name(got)
        ))
    }
    pub fn bad_method_call(expected: impl Into<String>, got: &LuaValue) -> mlua::Error {
        Self::generic(format!(
            "attempt to call method on invalid receiver ({} expected, got {})",
            expected.into(),
            type_name(got)
        ))
    }
    pub fn type_mismatch(expected: impl Into<String>, got: &LuaValue) -> mlua::Error {
        Self::generic(format!(
            "{} expected, got {}",
            expected.into(),
            type_name(got)
        ))
    }
    pub fn position_out_of_bounds(position: usize, start: usize, end: usize) -> mlua::Error {
        Self::generic(format!(
            "position out of bounds: expected index in range [{start}, {end}], got {position}"
        ))
    }
    pub fn position_out_of_bounds_named(
        name: impl AsRef<str>,
        position: usize,
        start: usize,
        end: usize,
    ) -> mlua::Error {
        Self::generic(format!(
            "position out of bounds: expected {} in range [{start}, {end}], got {position}",
            name.as_ref()
        ))
    }
    pub fn module_not_found(path: impl Into<String>) -> mlua::Error {
        Self::generic(format!("module not found: {}", path.into()))
    }
    pub fn module_load(
        path: impl Into<String>,
        cause: impl Into<Box<dyn std::error::Error + Send + Sync>>,
    ) -> mlua::Error {
        Self::generic(format!(
            "failed to load module: {} ({})",
            path.into(),
            cause.into()
        ))
    }
    pub fn mod_not_found(id: impl Into<String>) -> mlua::Error {
        Self::generic(format!("mod not found: {}", id.into()))
    }
    pub fn circular_dependency(path: impl Into<String>) -> mlua::Error {
        Self::generic(format!("circular dependency detected: {}", path.into()))
    }
    pub fn content_not_found(guid: impl Into<String>) -> mlua::Error {
        Self::generic(format!("content not found: {}", guid.into()))
    }
    pub fn external(error: impl Into<Box<dyn std::error::Error + Send + Sync>>) -> mlua::Error {
        Self::generic(error.into().to_string())
    }
}
fn type_name(value: &LuaValue) -> String {
    match value {
        LuaValue::UserData(_) => format!("{value:#?}"),
        _ => value.type_name().to_string(),
    }
}
