// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only Lua views of ordered maps and arrays (Type.struct_fields, enum_fields, attributes, attribute namespaces).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/readonly_collections.rs:1-159. Same userdata names, metamethods, argument casts and
//! iteration order. SF keeps an `IndexMap` and an `ouroboros` iterator; this keeps the insertion-ordered entries in a
//! vector with a byte-key index and iterates by position, which observes the same: `__index` by exact string bytes,
//! `__len` = entry count, `__pairs` = (iterator function, state userdata, nil) in insertion order.
use std::{collections::HashMap, rc::Rc};

use mlua::{AnyUserData, Function, MetaMethod, UserData, UserDataMethods};
use sfi_core::lua::{FunctionArgs, LuaValue, MethodArgs};

struct Entries {
    entries: Vec<(mlua::String, LuaValue)>,
    by_key: HashMap<Vec<u8>, usize>,
}

pub struct ReadOnlyMap {
    table: Rc<Entries>,
}

impl ReadOnlyMap {
    /// `IndexMap::insert` order: a repeated key keeps its first slot and takes the later value.
    pub fn new(entries: impl IntoIterator<Item = (mlua::String, LuaValue)>) -> Self {
        let mut ordered: Vec<(mlua::String, LuaValue)> = Vec::new();
        let mut by_key: HashMap<Vec<u8>, usize> = HashMap::new();
        for (key, value) in entries {
            match by_key.get(key.as_bytes().as_ref()) {
                Some(&slot) => ordered[slot].1 = value,
                None => {
                    by_key.insert(key.as_bytes().to_vec(), ordered.len());
                    ordered.push((key, value));
                }
            }
        }
        Self {
            table: Rc::new(Entries {
                entries: ordered,
                by_key,
            }),
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
                .map(|&slot| this.table.entries[slot].1.clone())
                .unwrap_or(LuaValue::Nil))
        });

        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.table.entries.len() as i64)
        });

        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            ReadOnlyMapIter::create(&this, lua)
        });
    }
}

struct ReadOnlyMapIter {
    table: Rc<Entries>,
    index: usize,
}

impl ReadOnlyMapIter {
    fn create(value: &ReadOnlyMap, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            table: value.table.clone(),
            index: 0,
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|lua, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            state.next(lua)
        })?;
        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn next(&mut self, lua: &mlua::Lua) -> mlua::Result<(LuaValue, LuaValue)> {
        let Some((key, value)) = self.table.entries.get(self.index) else {
            return Ok((LuaValue::Nil, LuaValue::Nil));
        };
        self.index += 1;
        // SF creates a fresh string from the key's bytes on every step.
        Ok((LuaValue::String(lua.create_string(key.as_bytes())?), value.clone()))
    }
}

impl UserData for ReadOnlyMapIter {}

pub struct ReadOnlyArray {
    array: Rc<Vec<LuaValue>>,
}

impl ReadOnlyArray {
    pub fn new(array: Vec<LuaValue>) -> Self {
        Self { array: Rc::new(array) }
    }
}

impl UserData for ReadOnlyArray {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Index, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<usize>(0)?;
            if key == 0 {
                return Ok(LuaValue::Nil);
            }
            Ok(this.array.get(key - 1).cloned().unwrap_or(LuaValue::Nil))
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

struct ReadOnlyArrayIter {
    array: Rc<Vec<LuaValue>>,
    index: usize,
}

impl ReadOnlyArrayIter {
    fn create(value: &ReadOnlyArray, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            array: value.array.clone(),
            index: 0,
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|_, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            state.next()
        })?;
        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn next(&mut self) -> mlua::Result<(LuaValue, LuaValue)> {
        if self.index >= self.array.len() {
            return Ok((LuaValue::Nil, LuaValue::Nil));
        }
        let key = self.index;
        let value = self.array[self.index].clone();
        self.index += 1;
        Ok((LuaValue::Integer(key as i64 + 1), value))
    }
}

impl UserData for ReadOnlyArrayIter {}
