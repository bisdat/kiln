// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/arguments.rs:3-100.
use super::{CastLua, CastLuaExt, LuaError, LuaValue};

pub trait Args {
    fn get<'a, T: CastLua<'a>>(&'a self, index: usize) -> mlua::Result<T::Output>;
}

#[derive(Default, Debug, Clone)]
#[repr(transparent)]
pub struct FunctionArgs(mlua::MultiValue);
impl FunctionArgs {
    pub fn get<'a, T: CastLua<'a>>(&'a self, index: usize) -> mlua::Result<T::Output> {
        let value = self.0.get(index).unwrap_or(&LuaValue::Nil);
        value
            .try_cast::<T>()?
            .ok_or_else(|| LuaError::bad_argument_type(index, T::name(), value))
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    /// Unlike get, distinguishes an omitted argument from an explicit nil.
    pub fn value(&self, index: usize) -> Option<&LuaValue> {
        self.0.get(index)
    }
}
impl Args for FunctionArgs {
    fn get<'a, T: CastLua<'a>>(&'a self, index: usize) -> mlua::Result<T::Output> {
        self.get::<T>(index)
    }
}
impl mlua::FromLuaMulti for FunctionArgs {
    fn from_lua_multi(values: mlua::MultiValue, _: &mlua::Lua) -> mlua::Result<Self> {
        Ok(Self(values))
    }
}
impl mlua::IntoLuaMulti for FunctionArgs {
    fn into_lua_multi(self, _: &mlua::Lua) -> mlua::Result<mlua::MultiValue> {
        Ok(self.0)
    }
}

#[derive(Default, Debug, Clone)]
#[repr(transparent)]
pub struct MethodArgs(mlua::MultiValue);
impl MethodArgs {
    pub fn this<'a, T: CastLua<'a>>(&'a self) -> mlua::Result<T::Output> {
        let value = self
            .0
            .front()
            .ok_or_else(|| LuaError::bad_method_call(T::name(), &LuaValue::Nil))?;
        value
            .try_cast::<T>()?
            .ok_or_else(|| LuaError::bad_method_call(T::name(), value))
    }
    pub fn get<'a, T: CastLua<'a>>(&'a self, index: usize) -> mlua::Result<T::Output> {
        let value = self.0.get(index + 1).unwrap_or(&LuaValue::Nil);
        value
            .try_cast::<T>()?
            .ok_or_else(|| LuaError::bad_argument_type(index, T::name(), value))
    }
    /// Number of arguments after the receiver, preserving trailing nils.
    pub fn len(&self) -> usize {
        self.0.len().saturating_sub(1)
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn value(&self, index: usize) -> Option<&LuaValue> {
        self.0.get(index + 1)
    }
}
impl Args for MethodArgs {
    fn get<'a, T: CastLua<'a>>(&'a self, index: usize) -> mlua::Result<T::Output> {
        self.get::<T>(index)
    }
}
impl mlua::FromLuaMulti for MethodArgs {
    fn from_lua_multi(values: mlua::MultiValue, _: &mlua::Lua) -> mlua::Result<Self> {
        Ok(Self(values))
    }
}
impl mlua::IntoLuaMulti for MethodArgs {
    fn into_lua_multi(self, _: &mlua::Lua) -> mlua::Result<mlua::MultiValue> {
        Ok(self.0)
    }
}
