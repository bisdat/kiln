// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (byte-identical in sf-1.7.9 d8a394bc and sf-1.8.0 38ef448d):
// modules/mod-runtime/lua/bindings/buffer/{mod.rs:30-343,366-455,482-489,
// manage_buffer.rs:3-203,read_buffer.rs:5-108,write_buffer.rs:5-99,
// byte_order.rs:5-41,errors.rs:3-26}.
//! The single Buffer userdata shared by hashing, IO and image/texture codecs.
//!
//! Public byte boundary (all worker-local; no FFI or reflected/GPL types):
//! - `Buffer::new() -> Buffer`: empty writable storage, little endian.
//! - `Buffer::with_capacity(usize) -> mlua::Result<Buffer>`: zero-filled exposed
//!   storage length; head=tail=0. Checked/fallible allocation is intentional panic
//!   hardening (no panic reaches Lua), not a memory quota.
//! - `Buffer::wrap(Vec<u8>) -> Buffer`: returned bytes, writable, head=0,
//!   tail=capacity=len (SF IO and image results use this, NOT read-only storage).
//! - `set_writable(&mut self, bool) -> mlua::Result<()>`: internal state transition;
//!   no Lua setter. `false` retains readable storage and cursors.
//! - `data(&self) -> mlua::Result<&[u8]>`: unread byte view, closed checked,
//!   nonadvancing; empty if head>=tail. Both open states are readable.
//! - `ByteView<'a>: CastLua<'a, Output=ByteView<'a>>`: exact string-or-Buffer
//!   conversion, no UTF-8/numeric coercion. Use `args.get::<ByteView>(index)?`.
//!   `ByteView::as_bytes(&self) -> mlua::Result<&[u8]>` checks Buffer state.
//! - `write_bytes(&mut self, &[u8]) -> mlua::Result<()>`: checked append at tail;
//!   reserves/zero-grows, leaves head alone. Also available as `copy`.
//!
//! A view borrows its `Buffer` or `ByteView`, never only the Lua handle. Keep the
//! `UserDataRef<Buffer>`/`ByteView` alive for every use; drop it before mutable
//! userdata operations. No view may outlive that guard or a mutation. Conflicting
//! borrows intentionally propagate mlua's errors. No copied compatibility userdata.
//!
//! `read_resource`/`write_resource` remain absent unless the phase host attaches
//! a `lua::BufferResourcePort` via `LuaVm::attach` BEFORE any Lua access. It extends
//! this exact userdata, retaining all ordinary methods. The resource codecs live elsewhere.
//! Ordinary methods match SF, including eager copy defaults and retained close
//! storage. There is no terminator/encoding processing in SF's string methods.
//!
//! Byte views cannot survive a mutation (the same restriction holds through a
//! `UserDataRef` or `ByteView` guard):
//! ```compile_fail
//! use sfi_core::eml::buffer::Buffer;
//! let mut buffer = Buffer::wrap(vec![1, 2]);
//! let bytes = buffer.data().unwrap();
//! buffer.write_bytes(&[3]).unwrap();
//! assert_eq!(bytes, &[1, 2]);
//! ```
use crate::lua::{CastLua, Either, FunctionArgs, LuaError, MethodArgs};
use half::f16;
use mlua::{IntoLua, MetaMethod, UserData, Value};
use std::{borrow::Cow, ops::Deref};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ByteOrder {
    BigEndian,
    LittleEndian,
}
impl CastLua<'_> for ByteOrder {
    type Output = Self;
    fn try_cast_lua(value: &Value) -> mlua::Result<Option<Self>> {
        Ok(match value {
            Value::String(s) if s == "big" => Some(Self::BigEndian),
            Value::String(s) if s == "little" => Some(Self::LittleEndian),
            _ => None,
        })
    }
    fn name() -> Cow<'static, str> {
        "'big' or 'little'".into()
    }
}
impl IntoLua for ByteOrder {
    fn into_lua(self, lua: &mlua::Lua) -> mlua::Result<Value> {
        match self {
            Self::BigEndian => "big",
            Self::LittleEndian => "little",
        }
        .into_lua(lua)
    }
}

/// A counted Lua string or a live, shared borrow of the single Buffer userdata.
pub enum ByteView<'a> {
    String(mlua::BorrowedBytes<'a>),
    Buffer(mlua::UserDataRef<Buffer>),
}
impl<'a> CastLua<'a> for ByteView<'a> {
    type Output = Self;
    fn try_cast_lua(value: &'a Value) -> mlua::Result<Option<Self>> {
        Ok(
            Either::<&[u8], &Buffer>::try_cast_lua(value)?.map(|v| match v {
                Either::A(v) => Self::String(v),
                Either::B(v) => Self::Buffer(v),
            }),
        )
    }
    fn name() -> Cow<'static, str> {
        "string or Buffer".into()
    }
}
impl ByteView<'_> {
    pub fn as_bytes(&self) -> mlua::Result<&[u8]> {
        match self {
            Self::String(v) => Ok(v),
            Self::Buffer(v) => v.data(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    ReadWrite,
    Read,
    Closed,
}
#[derive(Debug, Clone)]
pub struct Buffer {
    data: Vec<u8>,
    head: usize,
    tail: usize,
    state: State,
    order: ByteOrder,
}
impl Default for Buffer {
    fn default() -> Self {
        Self::new()
    }
}
fn capacity_error() -> mlua::Error {
    LuaError::generic("capacity exceeds maximum possible size")
}
fn overflow(head: usize, tail: usize) -> mlua::Error {
    LuaError::generic(format!("buffer overflow: head {head} exceeds tail {tail}"))
}
impl Buffer {
    pub fn new() -> Self {
        Self {
            data: Vec::new(),
            head: 0,
            tail: 0,
            state: State::ReadWrite,
            order: ByteOrder::LittleEndian,
        }
    }
    pub fn with_capacity(capacity: usize) -> mlua::Result<Self> {
        let mut this = Self::new();
        this.reserve(capacity)?;
        Ok(this)
    }
    pub fn wrap(data: Vec<u8>) -> Self {
        Self {
            tail: data.len(),
            data,
            ..Self::new()
        }
    }
    pub fn set_writable(&mut self, writable: bool) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.state = if writable {
            State::ReadWrite
        } else {
            State::Read
        };
        Ok(())
    }
    pub fn close(&mut self) {
        self.state = State::Closed;
    }
    pub fn head(&self) -> mlua::Result<usize> {
        self.check_not_closed()?;
        Ok(self.head)
    }
    pub fn set_head(&mut self, position: usize) -> mlua::Result<()> {
        self.check_not_closed()?;
        if position > self.tail {
            return Err(overflow(position, self.tail));
        }
        self.head = position;
        Ok(())
    }
    pub fn tail(&self) -> mlua::Result<usize> {
        self.check_not_closed()?;
        Ok(self.tail)
    }
    pub fn set_tail(&mut self, position: usize) -> mlua::Result<()> {
        self.check_not_closed()?;
        if position > self.data.len() {
            return Err(LuaError::generic(format!(
                "buffer overflow: tail {position} exceeds capacity {}",
                self.data.len()
            )));
        }
        self.tail = position;
        Ok(())
    }
    pub fn remaining(&self) -> mlua::Result<usize> {
        self.check_not_closed()?;
        Ok(self.tail.saturating_sub(self.head))
    }
    pub fn capacity(&self) -> mlua::Result<usize> {
        self.check_not_closed()?;
        Ok(self.data.len())
    }
    pub fn reset(&mut self) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.head = 0;
        self.tail = 0;
        Ok(())
    }
    pub fn reserve(&mut self, additional: usize) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.check_writable()?;
        let new_capacity = self
            .tail
            .checked_add(additional)
            .filter(|n| *n <= isize::MAX as usize)
            .ok_or_else(capacity_error)?;
        if new_capacity > self.data.len() {
            self.data
                .try_reserve_exact(new_capacity - self.data.len())
                .map_err(|_| capacity_error())?;
            self.data.resize(new_capacity, 0);
        }
        Ok(())
    }
    pub fn order(&self) -> mlua::Result<ByteOrder> {
        self.check_not_closed()?;
        Ok(self.order)
    }
    pub fn set_order(&mut self, order: ByteOrder) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.order = order;
        Ok(())
    }
    pub fn skip(&mut self, size: usize) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.check_read_bounds(size)?;
        self.head += size;
        Ok(())
    }
    pub fn copy(&mut self, bytes: &[u8]) -> mlua::Result<()> {
        self.write_bytes(bytes)
    }
    pub fn copy_within(&mut self, offset: usize, length: usize) -> mlua::Result<()> {
        self.check_not_closed()?;
        // All open states are readable; SF checks readability here before writable.
        self.check_writable()?;
        self.check_read_bounds(offset.saturating_add(length))?;
        let start = self.head + offset;
        self.reserve(length)?;
        self.data.copy_within(start..start + length, self.tail);
        self.tail += length;
        Ok(())
    }
    pub fn data(&self) -> mlua::Result<&[u8]> {
        self.check_not_closed()?;
        Ok(if self.head >= self.tail {
            &[]
        } else {
            &self.data[self.head..self.tail]
        })
    }
    fn check_not_closed(&self) -> mlua::Result<()> {
        if self.state == State::Closed {
            Err(LuaError::generic("buffer is closed"))
        } else {
            Ok(())
        }
    }
    /// Validate closed/writable state before resource type/value conversions.
    /// Providers retain their actual mutable userdata guard across codec callbacks.
    pub fn check_writable(&self) -> mlua::Result<()> {
        self.check_not_closed()?;
        if self.state == State::ReadWrite {
            Ok(())
        } else {
            Err(LuaError::generic("buffer is not writable"))
        }
    }
    fn check_read_bounds(&self, size: usize) -> mlua::Result<()> {
        let end = self.head.saturating_add(size);
        if end > self.tail {
            Err(overflow(end, self.tail))
        } else {
            Ok(())
        }
    }
    pub fn read_bytes(&mut self, size: usize) -> mlua::Result<&[u8]> {
        self.check_not_closed()?;
        self.check_read_bounds(size)?;
        let start = self.head;
        self.head += size;
        Ok(&self.data[start..self.head])
    }
    pub fn write_bytes(&mut self, bytes: &[u8]) -> mlua::Result<()> {
        self.check_not_closed()?;
        self.check_writable()?;
        self.reserve(bytes.len())?;
        self.data[self.tail..self.tail + bytes.len()].copy_from_slice(bytes);
        self.tail += bytes.len();
        Ok(())
    }
    pub fn read_u8(&mut self) -> mlua::Result<u8> {
        Ok(self.read_bytes(1)?[0])
    }
    pub fn read_i8(&mut self) -> mlua::Result<i8> {
        Ok(self.read_u8()? as i8)
    }
    pub fn read_bool(&mut self) -> mlua::Result<bool> {
        Ok(self.read_u8()? != 0)
    }
    pub fn write_u8(&mut self, value: u8) -> mlua::Result<()> {
        self.write_bytes(&[value])
    }
    pub fn write_i8(&mut self, value: i8) -> mlua::Result<()> {
        self.write_u8(value as u8)
    }
    pub fn write_bool(&mut self, value: bool) -> mlua::Result<()> {
        self.write_u8(u8::from(value))
    }
}
macro_rules! primitives {
    ($(($ty:ty, $read:ident, $write:ident, $size:expr)),* $(,)?) => {
        impl Buffer { $(
            pub fn $read(&mut self) -> mlua::Result<$ty> {
                let order = self.order;
                let bytes = self.read_bytes($size)?.try_into().expect("checked primitive width");
                Ok(match order { ByteOrder::BigEndian => <$ty>::from_be_bytes(bytes), ByteOrder::LittleEndian => <$ty>::from_le_bytes(bytes) })
            }
            pub fn $write(&mut self, value: $ty) -> mlua::Result<()> {
                let bytes = match self.order { ByteOrder::BigEndian => value.to_be_bytes(), ByteOrder::LittleEndian => value.to_le_bytes() };
                self.write_bytes(&bytes)
            }
        )* }
    };
}
primitives!(
    (u16, read_u16, write_u16, 2),
    (i16, read_i16, write_i16, 2),
    (u32, read_u32, write_u32, 4),
    (i32, read_i32, write_i32, 4),
    (u64, read_u64, write_u64, 8),
    (i64, read_i64, write_i64, 8),
    (f16, read_f16, write_f16, 2),
    (f32, read_f32, write_f32, 4),
    (f64, read_f64, write_f64, 8)
);

// Only LuaVm's pre-access initialization boundary may call this. Re-registering
// after a Buffer metatable/instance exists would split mlua's userdata type cache.
pub(crate) fn register_resources(
    lua: &mlua::Lua,
    port: std::rc::Rc<dyn crate::lua::BufferResourcePort>,
) -> mlua::Result<()> {
    use mlua::UserDataMethods;
    lua.register_userdata_type::<Buffer>(|registry| {
        // mlua registration replaces, rather than extends, the default registry.
        // Explicitly retain every existing field, method and metamethod first.
        Buffer::register(registry);
        let read = port.clone();
        registry.add_function("read_resource", move |lua, args: MethodArgs| {
            read.read_resource(lua, args)
        });
        registry.add_function("write_resource", move |lua, args: MethodArgs| {
            port.write_resource(lua, args)
        });
    })?;
    // mlua 0.11.6 consumes pending custom registration only in create_any_userdata,
    // not create_userdata (state/raw.rs:944-979). Initialize the real Buffer cache
    // now, AFTER registering all methods, so every subsequent constructor, including
    // automatic IntoLua and downstream IO/image producers, shares this metatable.
    drop(lua.create_any_userdata(Buffer::new())?);
    Ok(())
}

pub fn create(lua: &mlua::Lua) -> mlua::Result<mlua::Table> {
    let table = lua.create_table()?;
    table.set(
        "create",
        lua.create_function(
            |_, args: FunctionArgs| match args.get::<Option<usize>>(0)? {
                Some(n) => Buffer::with_capacity(n),
                None => Ok(Buffer::new()),
            },
        )?,
    )?;
    table.set(
        "wrap",
        lua.create_function(|_, args: FunctionArgs| {
            let bytes = args.get::<&[u8]>(0)?;
            let mut data = Buffer::with_capacity(bytes.len())?;
            data.write_bytes(&bytes)?;
            Ok(data)
        })?,
    )?;
    Ok(table)
}
fn lua_copy(_: &mlua::Lua, args: MethodArgs) -> mlua::Result<()> {
    let this = args.this::<&Buffer>()?;
    let other = args.get::<&Buffer>(0)?;
    let offset = args.get::<Option<usize>>(1)?;
    let length = args.get::<Option<usize>>(2)?;
    let same = std::ptr::eq(this.deref(), other.deref());
    drop(this);
    drop(other);
    if same {
        let mut this = args.this::<&mut Buffer>()?;
        let offset = offset.unwrap_or(0);
        // Deliberately eager, including when length was supplied.
        let length = length.unwrap_or(this.remaining()?);
        this.copy_within(offset, length)?;
    } else {
        let mut this = args.this::<&mut Buffer>()?;
        let other = args.get::<&Buffer>(0)?;
        let offset = offset.unwrap_or(0);
        let length = length.unwrap_or(other.remaining()?);
        other.check_not_closed()?;
        other.check_read_bounds(offset.saturating_add(length))?;
        this.copy(&other.data()?[offset..offset + length])?;
    }
    Ok(())
}
fn lua_to_string(lua: &mlua::Lua, args: MethodArgs) -> mlua::Result<mlua::String> {
    let this = args.this::<&Buffer>()?;
    lua.create_string(this.data()?)
}
impl UserData for Buffer {
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_function("head", |lua, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            match args.get::<Option<usize>>(0)? {
                Some(n) => {
                    this.set_head(n)?;
                    Ok(Value::Nil)
                }
                None => this.head()?.into_lua(lua),
            }
        });
        methods.add_function("tail", |lua, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            match args.get::<Option<usize>>(0)? {
                Some(n) => {
                    this.set_tail(n)?;
                    Ok(Value::Nil)
                }
                None => this.tail()?.into_lua(lua),
            }
        });
        methods.add_function("order", |lua, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            match args.get::<Option<ByteOrder>>(0)? {
                Some(n) => {
                    this.set_order(n)?;
                    Ok(Value::Nil)
                }
                None => this.order()?.into_lua(lua),
            }
        });
        methods.add_function("remaining", |_, args: MethodArgs| {
            args.this::<&Buffer>()?.remaining()
        });
        methods.add_function("capacity", |_, args: MethodArgs| {
            args.this::<&Buffer>()?.capacity()
        });
        methods.add_function("reset", |_, args: MethodArgs| {
            args.this::<&mut Buffer>()?.reset()
        });
        methods.add_function("reserve", |_, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let n = args.get::<usize>(0)?;
            this.reserve(n)
        });
        methods.add_function("skip", |_, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let n = args.get::<usize>(0)?;
            this.skip(n)
        });
        methods.add_function("copy", lua_copy);
        methods.add_function("close", |_, args: MethodArgs| {
            args.this::<&mut Buffer>()?.close();
            Ok(())
        });
        macro_rules! binding {
            ($ty:ty, $read:ident, $write:ident) => {
                methods.add_function(stringify!($read), |_, args: MethodArgs| {
                    args.this::<&mut Buffer>()?.$read()
                });
                methods.add_function(stringify!($write), |_, args: MethodArgs| {
                    let mut this = args.this::<&mut Buffer>()?;
                    let value = args.get::<$ty>(0)?;
                    this.$write(value)
                });
            };
        }
        binding!(bool, read_bool, write_bool);
        binding!(u8, read_u8, write_u8);
        binding!(i8, read_i8, write_i8);
        binding!(u16, read_u16, write_u16);
        binding!(i16, read_i16, write_i16);
        binding!(u32, read_u32, write_u32);
        binding!(i32, read_i32, write_i32);
        binding!(i64, read_i64, write_i64);
        binding!(f32, read_f32, write_f32);
        binding!(f64, read_f64, write_f64);
        methods.add_function("read_u64", |_, args: MethodArgs| {
            Ok(Value::Integer(
                args.this::<&mut Buffer>()?.read_u64()? as i64
            ))
        });
        methods.add_function("write_u64", |_, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let value = args.get::<u64>(0)?;
            this.write_u64(value)
        });
        methods.add_function("read_f16", |_, args: MethodArgs| {
            Ok(Value::Number(
                args.this::<&mut Buffer>()?.read_f16()?.to_f64(),
            ))
        });
        methods.add_function("write_f16", |_, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let value = args.get::<f16>(0)?;
            this.write_f16(value)
        });
        methods.add_function("read_string", |lua, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let size = args.get::<usize>(0)?;
            lua.create_string(this.read_bytes(size)?)
        });
        methods.add_function("write_string", |_, args: MethodArgs| {
            let mut this = args.this::<&mut Buffer>()?;
            let bytes = args.get::<&[u8]>(0)?;
            this.write_bytes(&bytes)
        });
        methods.add_function("to_string", lua_to_string);
        methods.add_meta_function(MetaMethod::ToString, lua_to_string);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn close_retains_all_storage_and_cursors() {
        let mut b = Buffer::wrap(vec![1, 2, 3]);
        b.skip(1).unwrap();
        b.set_order(ByteOrder::BigEndian).unwrap();
        b.close();
        b.close();
        assert_eq!(b.data, [1, 2, 3]);
        assert_eq!((b.head, b.tail), (1, 3));
        assert_eq!(b.order, ByteOrder::BigEndian);
        assert!(b.data().is_err());
        assert!(b.reset().is_err());
    }
}
