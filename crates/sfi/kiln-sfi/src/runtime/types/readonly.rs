// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (identical at both pins, 1.7.9 d8a394bc and 1.8.0 38ef448d):
// modules/mod-runtime/lua/readonly_collections.rs:1-159.
//! ShroudForge's read-only metadata containers, `ReadOnlyMap` and
//! `ReadOnlyArray`, as the Type userdata hands them out (`struct_fields`,
//! `enum_fields`, `attributes`, an attribute's `namespace`).
//!
//! - `__index` takes the key as given: a map key must be a Lua string (its bytes are the
//!   key), an array position goes through ShroudForge's unsigned cast, and position 0 is
//!   nil. There is no `__newindex`: a write is a Lua error, the contents never change.
//! - `__len` is the entry count; `__pairs` returns a fresh iterator function, its own
//!   iterator state and nil, and walks the entries in insertion order, with fresh key
//!   strings for a map.
//! - The contents are shared (`Rc`) by the container and every iterator made from it,
//!   so an iterator taken earlier keeps walking the same entries whatever the caller does
//!   in between; there is no generation to go stale.
//!
//! The type names (`ReadOnlyMap`, `ReadOnlyMapIter`, `ReadOnlyArray`,
//! `ReadOnlyArrayIter`) are ShroudForge's: they show in Lua as the userdata names and in
//! argument errors.

use mlua::{AnyUserData, Function, Lua, MetaMethod, UserData, UserDataMethods, Value};
use sfi_core::lua::{FunctionArgs, MethodArgs};
use std::collections::HashMap;
use std::rc::Rc;

/// Ordered entries with their keys' bytes indexed (Lua strings compare by bytes).
struct Entries {
    order: Vec<(Vec<u8>, Value)>,
    by_key: HashMap<Vec<u8>, usize>,
}

/// A read-only, ordered string-keyed map.
pub struct ReadOnlyMap {
    table: Rc<Entries>,
}

impl ReadOnlyMap {
    /// The entries in order. A key given twice keeps its first position and takes the
    /// later value (an ordered map's insert).
    pub fn new(entries: impl IntoIterator<Item = (Vec<u8>, Value)>) -> Self {
        let mut order: Vec<(Vec<u8>, Value)> = Vec::new();
        let mut by_key: HashMap<Vec<u8>, usize> = HashMap::new();
        for (key, value) in entries {
            match by_key.get(&key) {
                Some(&at) => order[at].1 = value,
                None => {
                    by_key.insert(key.clone(), order.len());
                    order.push((key, value));
                }
            }
        }
        Self {
            table: Rc::new(Entries { order, by_key }),
        }
    }
}

impl UserData for ReadOnlyMap {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Index, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<&[u8]>(0)?;
            Ok(this
                .table
                .by_key
                .get(key.as_ref())
                .map(|&at| this.table.order[at].1.clone())
                .unwrap_or(Value::Nil))
        });
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.table.order.len() as i64)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            ReadOnlyMapIter::create(&this, lua)
        });
    }
}

/// The state of one `pairs` walk over a [`ReadOnlyMap`].
pub struct ReadOnlyMapIter {
    table: Rc<Entries>,
    next: usize,
}

impl ReadOnlyMapIter {
    fn create(map: &ReadOnlyMap, lua: &Lua) -> mlua::Result<(Function, AnyUserData, Value)> {
        let state = lua.create_userdata(Self {
            table: map.table.clone(),
            next: 0,
        })?;
        let step = lua.create_function(|lua, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            state.step(lua)
        })?;
        Ok((step, state, Value::Nil))
    }

    fn step(&mut self, lua: &Lua) -> mlua::Result<(Value, Value)> {
        let Some((key, value)) = self.table.order.get(self.next) else {
            return Ok((Value::Nil, Value::Nil));
        };
        self.next += 1;
        Ok((Value::String(lua.create_string(key)?), value.clone()))
    }
}

impl UserData for ReadOnlyMapIter {}

/// A read-only, 1-based array.
pub struct ReadOnlyArray {
    array: Rc<Vec<Value>>,
}

impl ReadOnlyArray {
    pub fn new(array: Vec<Value>) -> Self {
        Self {
            array: Rc::new(array),
        }
    }
}

impl UserData for ReadOnlyArray {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Index, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<usize>(0)?;
            if key == 0 {
                return Ok(Value::Nil);
            }
            Ok(this.array.get(key - 1).cloned().unwrap_or(Value::Nil))
        });
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.array.len() as i64)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            ReadOnlyArrayIter::create(&this, lua)
        });
    }
}

/// The state of one `pairs` walk over a [`ReadOnlyArray`].
pub struct ReadOnlyArrayIter {
    array: Rc<Vec<Value>>,
    index: usize,
}

impl ReadOnlyArrayIter {
    fn create(array: &ReadOnlyArray, lua: &Lua) -> mlua::Result<(Function, AnyUserData, Value)> {
        let state = lua.create_userdata(Self {
            array: array.array.clone(),
            index: 0,
        })?;
        let step = lua.create_function(|_, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            Ok(state.step())
        })?;
        Ok((step, state, Value::Nil))
    }

    fn step(&mut self) -> (Value, Value) {
        let Some(value) = self.array.get(self.index) else {
            return (Value::Nil, Value::Nil);
        };
        self.index += 1;
        (Value::Integer(self.index as i64), value.clone())
    }
}

impl UserData for ReadOnlyArrayIter {}
