// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9, fork pin): modules/mod-runtime/lua/resource_values/mod.rs:91-200 (push, insert, pop, remove, clear:
// recognition by TypeId, a mutable borrow of the receiver held through the operation, anything else not handled).
//! Typed `table.insert/remove/clear` for the frontend's arrays, as sfi-core's [`TypedTablePort`].
//!
//! sfi-core's dispatcher does Source's argument counting and position casts, then asks this port. An `ArrayValue` or
//! `MappedArrayValue` is borrowed mutably (Source's `borrow_mut`, so a re-entrant use of the same array from a
//! validation callback sees mlua's borrow error, as in Source) and the provider runs Source's operation; any other
//! userdata is not handled (the dispatcher falls back to Lua's function, or raises Source's `clear` error). A
//! userdata without a Rust type identity is Source's panic text as an error, as the provider's port raises it.

use super::proxies::{ArrayValue, MappedArrayValue};
use mlua::{AnyUserData, Lua, Value};
use sfi_core::lua::{Dispatch, TypedTablePort};
use std::any::TypeId;

/// The frontend's typed table port (attach it with `VmAttachments.typed_tables`).
#[derive(Clone, Copy, Debug, Default)]
pub struct ValuesTables;

enum Array {
    Owned,
    Mapped,
}

fn array(receiver: &AnyUserData) -> mlua::Result<Option<Array>> {
    let id = receiver
        .type_id()
        .ok_or_else(|| mlua::Error::runtime("UserData has no type_id"))?;
    Ok(if id == TypeId::of::<ArrayValue>() {
        Some(Array::Owned)
    } else if id == TypeId::of::<MappedArrayValue>() {
        Some(Array::Mapped)
    } else {
        None
    })
}

macro_rules! on_array {
    ($receiver:expr, |$lease:ident| $body:expr) => {
        match array(&$receiver)? {
            Some(Array::Owned) => {
                let this = $receiver.borrow_mut::<ArrayValue>()?;
                let $lease = &this.0;
                Dispatch::Handled($body?)
            }
            Some(Array::Mapped) => {
                let this = $receiver.borrow_mut::<MappedArrayValue>()?;
                let $lease = &this.0;
                Dispatch::Handled($body?)
            }
            None => Dispatch::NotHandled,
        }
    };
}

impl TypedTablePort for ValuesTables {
    fn push(&self, _: &Lua, receiver: AnyUserData, value: Value) -> mlua::Result<Dispatch<()>> {
        Ok(on_array!(receiver, |lease| lease
            .values()
            .push(lease.token(), value)))
    }
    fn insert(
        &self,
        _: &Lua,
        receiver: AnyUserData,
        position: usize,
        value: Value,
    ) -> mlua::Result<Dispatch<()>> {
        Ok(on_array!(receiver, |lease| lease.values().insert(
            lease.token(),
            position as u64,
            value
        )))
    }
    fn pop(&self, lua: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<Value>> {
        Ok(on_array!(receiver, |lease| lease
            .values()
            .pop(lua, lease.token())))
    }
    fn remove(
        &self,
        lua: &Lua,
        receiver: AnyUserData,
        position: usize,
    ) -> mlua::Result<Dispatch<Value>> {
        Ok(on_array!(receiver, |lease| lease.values().remove(
            lua,
            lease.token(),
            position as u64
        )))
    }
    fn clear(&self, _: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<()>> {
        Ok(on_array!(receiver, |lease| lease
            .values()
            .clear(lease.token())))
    }
}
