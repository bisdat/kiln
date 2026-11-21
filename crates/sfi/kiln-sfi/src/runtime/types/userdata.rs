// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical at both pins, 1.7.9 d8a394bc and 1.8.0 38ef448d, except the
// session cache's line numbers):
// modules/mod-runtime/lua/bindings/game/types.rs:18-505 (the `game.types` table, the
// Type userdata and its metadata containers);
// modules/mod-runtime/lua/bindings/game/mod.rs:13-22 (`game.version`, `game.types`);
// modules/mod-runtime/lua/bindings/mod.rs:88 (`runtime.types` is `game.types`);
// modules/mod-runtime/session/mod.rs:177-196 in 1.7.9, :186-205 in 1.8.0 (one Type per
// registry index per VM).
//! `game.types` (and so `runtime.types`, the registrar's alias of the same table) and
//! the Type userdata, on the checked archive [`Registry`].
//!
//! - **The table:** `get` and `get_by_qualified_name` (one function, string-only:
//!   ShroudForge's string cast turns a number into its decimal text), `get_by_impact_name`,
//!   `get_by_qualified_hash` / `get_by_internal_hash` (`Type, nil` or `nil, reason`),
//!   `get_all([include_ds = true])`, `resolve(selector)` (`Type, nil` or `nil,
//!   reason`), `find_by_hash(hash[, domain = "qualified"])` (every match, collisions
//!   included), `get_by_index`, `find(options)`, `of(value)`, plus `version` (the
//!   registry's version text) and `count`. `game.version` is the selected archive's game
//!   version, a different text, set by the types family.
//! - **The session:** one per VM. It makes at most one Type per registry index, so the
//!   same type is always the same userdata; every package gets its own `game.types`
//!   table over it.
//! - **The Type:** 23 metadata fields and `is_a`. `fields`, `flags` and `namespace`
//!   build a fresh, mutable table on every read; `struct_fields`, `enum_fields` and
//!   `attributes` are read-only containers built once per Type. An enum value is the
//!   signed 64-bit integer of its bits, `value_hex` the unsigned bits as 16 hex digits.
//!
//! Kiln's differences, none visible to a mod that does not try to break the runtime:
//! - when a Lua finalizer asks for the type being made inside the allocation, the Type
//!   cached first wins and both calls get it (ShroudForge's re-entered borrow fails);
//! - `of` answers through the recognizers later families register (the value classes);
//!   until then it answers nil;
//! - a Type outlives its session only as metadata: its own fields keep working, while a
//!   read that must make another Type (`inner_type`, `fields`, the containers, `is_a`)
//!   is an error once every `game.types` table of its VM is gone.

use super::readonly::{ReadOnlyArray, ReadOnlyMap};
use super::registry::{Attribute, EnumField, Registry, StructField, TypeMeta};
use super::selectors::{self, HashKind};
use mlua::{
    AnyUserData, IntoLua, IntoLuaMulti, Lua, Table, UserData, UserDataFields, UserDataMethods,
    Value, WeakLua,
};
use sfi_core::lua::FunctionArgs;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::{Rc, Weak};

/// Answers which registry index a Lua value is an instance of (`game.types.of`).
pub type Recognizer = Rc<dyn Fn(&Value) -> mlua::Result<Option<usize>>>;

/// One VM's view of the registry: the Type cache and the recognizers.
pub struct Session {
    registry: Rc<Registry>,
    lua: WeakLua,
    types: RefCell<HashMap<usize, AnyUserData>>,
    recognizers: Rc<RefCell<Vec<Recognizer>>>,
}

impl Session {
    pub(crate) fn new(
        lua: &Lua,
        registry: Rc<Registry>,
        recognizers: Rc<RefCell<Vec<Recognizer>>>,
    ) -> Rc<Self> {
        Rc::new(Self {
            registry,
            lua: lua.weak(),
            types: RefCell::new(HashMap::new()),
            recognizers,
        })
    }

    pub(crate) fn belongs_to(&self, lua: &Lua) -> bool {
        self.lua == lua.weak()
    }

    pub fn registry(&self) -> &Rc<Registry> {
        &self.registry
    }

    /// The one Type of `index` in this VM, or `None` when the registry has no such
    /// index.
    pub fn get_type(self: &Rc<Self>, lua: &Lua, index: usize) -> mlua::Result<Option<Value>> {
        if !self.belongs_to(lua) {
            return Err(mlua::Error::runtime(
                "this type registry session belongs to another Lua state",
            ));
        }
        if let Some(cached) = self.types.borrow().get(&index) {
            return Ok(Some(Value::UserData(cached.clone())));
        }
        if self.registry.get(index).is_none() {
            return Ok(None);
        }
        // Lua may run finalizers inside this allocation, and one may ask for this index:
        // no borrow is held across it, and the Type cached first wins.
        let made = lua.create_userdata(Type {
            registry: self.registry.clone(),
            index,
            session: Rc::downgrade(self),
            struct_field_cache: RefCell::new(None),
            enum_field_cache: RefCell::new(None),
            attribute_cache: RefCell::new(None),
        })?;
        let kept = self.types.borrow_mut().entry(index).or_insert(made).clone();
        Ok(Some(Value::UserData(kept)))
    }

    fn type_value(self: &Rc<Self>, lua: &Lua, index: usize) -> mlua::Result<Value> {
        Ok(self.get_type(lua, index)?.unwrap_or(Value::Nil))
    }
}

/// A reflected type (ShroudForge's `Type` userdata).
pub struct Type {
    registry: Rc<Registry>,
    index: usize,
    session: Weak<Session>,
    struct_field_cache: RefCell<Option<Value>>,
    enum_field_cache: RefCell<Option<Value>>,
    attribute_cache: RefCell<Option<Value>>,
}

impl Type {
    /// This Type's metadata, in the registry it was made from.
    pub fn meta(&self) -> &TypeMeta {
        self.registry
            .get(self.index)
            .expect("a Type is made only for an index of its registry")
    }
    pub fn index(&self) -> usize {
        self.index
    }
    /// Whether this Type was made from `registry` (the same loaded registry).
    pub fn is_from(&self, registry: &Rc<Registry>) -> bool {
        Rc::ptr_eq(&self.registry, registry)
    }
    fn session(&self) -> mlua::Result<Rc<Session>> {
        self.session
            .upgrade()
            .ok_or_else(|| mlua::Error::runtime("the type registry session of this Type has ended"))
    }
    /// The cached container, built once; a value built meanwhile by a re-entered read
    /// wins.
    fn cached(
        cache: &RefCell<Option<Value>>,
        build: impl FnOnce() -> mlua::Result<Value>,
    ) -> mlua::Result<Value> {
        if let Some(value) = cache.borrow().as_ref() {
            return Ok(value.clone());
        }
        let value = build()?;
        Ok(cache.borrow_mut().get_or_insert(value).clone())
    }
}

impl UserData for Type {
    fn add_fields<F: UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("index", |_, this| Ok(this.index));
        fields.add_field_method_get("name", |_, this| Ok(this.meta().name.clone()));
        fields.add_field_method_get("impact_name", |_, this| Ok(this.meta().impact_name.clone()));
        fields.add_field_method_get("qualified_name", |_, this| {
            Ok(this.meta().qualified_name.clone())
        });
        fields.add_field_method_get("qualified_hash", |_, this| Ok(this.meta().qualified_hash));
        fields.add_field_method_get("internal_hash", |_, this| Ok(this.meta().internal_hash));
        fields.add_field_method_get("name_hash", |_, this| Ok(this.meta().name_hash));
        fields.add_field_method_get("impact_hash", |_, this| Ok(this.meta().impact_hash));
        fields.add_field_method_get("namespace", |_, this| Ok(this.meta().namespace.clone()));
        fields.add_field_method_get("inner_type", |lua, this| {
            this.meta()
                .inner_type
                .map(|inner| this.session()?.get_type(lua, inner))
                .transpose()
                .map(Option::flatten)
        });
        fields.add_field_method_get("size", |_, this| Ok(this.meta().size));
        fields.add_field_method_get("alignment", |_, this| Ok(this.meta().alignment));
        fields.add_field_method_get("element_alignment", |_, this| {
            Ok(this.meta().element_alignment)
        });
        fields.add_field_method_get("field_count", |_, this| Ok(this.meta().field_count));
        fields.add_field_method_get("primitive_type", |_, this| Ok(this.meta().primitive_type));
        fields.add_field_method_get("flags_bits", |_, this| Ok(this.meta().flags_bits));
        fields.add_field_method_get("flags", |lua, this| {
            lua.create_sequence_from(this.meta().flags())
        });
        fields.add_field_method_get("has_ds", |_, this| Ok(this.meta().has_ds()));
        fields.add_field_method_get("default_bytes", |lua, this| {
            this.meta()
                .default_bytes
                .as_ref()
                .map(|bytes| lua.create_string(bytes))
                .transpose()
        });
        fields.add_field_method_get("fields", |lua, this| {
            let session = this.session()?;
            let result = lua.create_table()?;
            for (owner, field) in
                selectors::fields(&this.registry, this.index).map_err(mlua::Error::runtime)?
            {
                let entry = lua.create_table()?;
                entry.raw_set("name", field.name.clone())?;
                entry.raw_set("type", session.get_type(lua, field.type_index)?)?;
                entry.raw_set("declaring_type", session.get_type(lua, owner)?)?;
                entry.raw_set("data_offset", field.data_offset.into_lua(lua)?)?;
                entry.raw_set(
                    "attributes",
                    attributes_to_lua(lua, &session, &field.attributes)?,
                )?;
                result.push(entry)?;
            }
            Ok(result)
        });
        fields.add_field_method_get("struct_fields", |lua, this| {
            Type::cached(&this.struct_field_cache, || {
                struct_fields_to_lua(lua, &this.session()?, &this.meta().struct_fields)
            })
        });
        fields.add_field_method_get("enum_fields", |lua, this| {
            Type::cached(&this.enum_field_cache, || {
                enum_fields_to_lua(lua, &this.meta().enum_fields)
            })
        });
        fields.add_field_method_get("attributes", |lua, this| {
            Type::cached(&this.attribute_cache, || {
                attributes_to_lua(lua, &this.session()?, &this.meta().attributes)
            })
        });
    }

    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("is_a", |lua, this, selector: Value| {
            let session = this.session()?;
            if !session.belongs_to(lua) {
                return Err(mlua::Error::runtime(
                    "this type registry session belongs to another Lua state",
                ));
            }
            let parent =
                selectors::resolve(&session.registry, &selector).map_err(mlua::Error::runtime)?;
            Ok(selectors::inheritance(&session.registry, this.index)
                .map_err(mlua::Error::runtime)?
                .contains(&parent))
        });
    }
}

fn map(lua: &Lua, entries: Vec<(&str, Value)>) -> mlua::Result<Value> {
    ReadOnlyMap::new(
        entries
            .into_iter()
            .map(|(key, value)| (key.as_bytes().to_vec(), value)),
    )
    .into_lua(lua)
}

fn struct_fields_to_lua(
    lua: &Lua,
    session: &Rc<Session>,
    fields: &[StructField],
) -> mlua::Result<Value> {
    let mut entries = Vec::with_capacity(fields.len());
    for field in fields {
        let value = map(
            lua,
            vec![
                ("name", Value::String(lua.create_string(&field.name)?)),
                ("type", session.type_value(lua, field.type_index)?),
                ("data_offset", Value::Integer(field.data_offset as i64)),
                (
                    "attributes",
                    attributes_to_lua(lua, session, &field.attributes)?,
                ),
            ],
        )?;
        entries.push((field.name.as_bytes().to_vec(), value));
    }
    ReadOnlyMap::new(entries).into_lua(lua)
}

fn attributes_to_lua(
    lua: &Lua,
    session: &Rc<Session>,
    attributes: &[Attribute],
) -> mlua::Result<Value> {
    let mut entries = Vec::with_capacity(attributes.len());
    for attribute in attributes {
        let namespace = attribute
            .namespace
            .iter()
            .map(|name| lua.create_string(name).map(Value::String))
            .collect::<mlua::Result<Vec<_>>>()?;
        let value = map(
            lua,
            vec![
                ("name", Value::String(lua.create_string(&attribute.name)?)),
                ("namespace", ReadOnlyArray::new(namespace).into_lua(lua)?),
                (
                    "type",
                    match attribute.type_index {
                        Some(index) => session.type_value(lua, index)?,
                        None => Value::Nil,
                    },
                ),
                ("value", Value::String(lua.create_string(&attribute.value)?)),
            ],
        )?;
        entries.push((attribute.key.as_bytes().to_vec(), value));
    }
    ReadOnlyMap::new(entries).into_lua(lua)
}

fn enum_fields_to_lua(lua: &Lua, fields: &[EnumField]) -> mlua::Result<Value> {
    let mut entries = Vec::with_capacity(fields.len());
    for field in fields {
        let value = map(
            lua,
            vec![
                ("name", Value::String(lua.create_string(&field.name)?)),
                ("value", Value::Integer(field.value as i64)),
                (
                    "value_hex",
                    Value::String(lua.create_string(format!("0x{:016x}", field.value))?),
                ),
            ],
        )?;
        entries.push((field.name.as_bytes().to_vec(), value));
    }
    ReadOnlyMap::new(entries).into_lua(lua)
}

// ---- the game.types table ----

fn add(
    lua: &Lua,
    table: &Table,
    name: &str,
    session: &Rc<Session>,
    function: impl Fn(&Lua, &Rc<Session>, FunctionArgs) -> mlua::Result<mlua::MultiValue> + 'static,
) -> mlua::Result<()> {
    let session = session.clone();
    table.raw_set(
        name,
        lua.create_function(move |lua, args: FunctionArgs| function(lua, &session, args))?,
    )
}

fn values(lua: &Lua, items: impl IntoLuaMulti) -> mlua::Result<mlua::MultiValue> {
    items.into_lua_multi(lua)
}

fn hash_lookup(
    lua: &Lua,
    session: &Rc<Session>,
    args: &FunctionArgs,
    kind: HashKind,
) -> mlua::Result<(Option<Value>, Option<String>)> {
    let hash = args.get::<u32>(0)?;
    match selectors::by_hash(&session.registry, hash, kind) {
        Ok(Some(ty)) => Ok((session.get_type(lua, ty.index)?, None)),
        Ok(None) => Ok((None, Some(format!("type hash not found: 0x{hash:08x}")))),
        Err(reason) => Ok((None, Some(reason))),
    }
}

/// One package's `game.types` table over `session`.
pub fn create(lua: &Lua, session: &Rc<Session>) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    let get = |lua: &Lua, session: &Rc<Session>, args: FunctionArgs| {
        let name = args.get::<String>(0)?;
        let found = session.registry.by_qualified_name(&name).map(|ty| ty.index);
        let value = match found {
            Some(index) => session.get_type(lua, index)?,
            None => None,
        };
        values(lua, value)
    };
    add(lua, &table, "get", session, get)?;
    add(lua, &table, "get_by_qualified_name", session, get)?;
    add(
        lua,
        &table,
        "get_by_impact_name",
        session,
        |lua, session, args| {
            let name = args.get::<String>(0)?;
            let found = session.registry.by_impact_name(&name).map(|ty| ty.index);
            let value = match found {
                Some(index) => session.get_type(lua, index)?,
                None => None,
            };
            values(lua, value)
        },
    )?;
    add(
        lua,
        &table,
        "get_by_qualified_hash",
        session,
        |lua, session, args| values(lua, hash_lookup(lua, session, &args, HashKind::Qualified)?),
    )?;
    add(
        lua,
        &table,
        "get_by_internal_hash",
        session,
        |lua, session, args| values(lua, hash_lookup(lua, session, &args, HashKind::Internal)?),
    )?;
    add(lua, &table, "get_all", session, |lua, session, args| {
        let include_ds = args.get::<Option<bool>>(0)?.unwrap_or(true);
        let result = lua.create_table_with_capacity(session.registry.len(), 0)?;
        for ty in session.registry.iter() {
            if !include_ds && ty.has_ds() {
                continue;
            }
            result.push(session.type_value(lua, ty.index)?)?;
        }
        values(lua, result)
    })?;
    add(lua, &table, "resolve", session, |lua, session, args| {
        let selector = args.get::<Value>(0)?;
        match selectors::resolve(&session.registry, selector) {
            Ok(index) => values(lua, (session.get_type(lua, index)?, None::<String>)),
            Err(reason) => values(lua, (None::<Value>, Some(reason))),
        }
    })?;
    add(
        lua,
        &table,
        "find_by_hash",
        session,
        |lua, session, args| {
            let hash = args.get::<u32>(0)?;
            let domain = args
                .get::<Option<String>>(1)?
                .unwrap_or_else(|| "qualified".into());
            let kind = HashKind::parse(&domain).map_err(mlua::Error::runtime)?;
            let result = lua.create_table()?;
            for ty in session.registry.iter().filter(|ty| kind.value(ty) == hash) {
                result.push(session.type_value(lua, ty.index)?)?;
            }
            values(lua, result)
        },
    )?;
    add(
        lua,
        &table,
        "get_by_index",
        session,
        |lua, session, args| {
            let index = args.get::<usize>(0)?;
            values(lua, session.get_type(lua, index)?)
        },
    )?;
    add(lua, &table, "find", session, |lua, session, args| {
        let options = args.get::<Option<Table>>(0)?;
        let option = |key: &str| -> mlua::Result<Option<String>> {
            options
                .map(|o| o.get::<Option<String>>(key))
                .transpose()
                .map(Option::flatten)
        };
        let prefix = option("prefix")?;
        let attribute = option("attribute")?;
        let primitive = option("primitive_type")?;
        let ds = options
            .map(|o| o.get::<Option<bool>>("has_ds"))
            .transpose()?
            .flatten();
        let result = lua.create_table()?;
        for ty in session.registry.iter() {
            if prefix
                .as_ref()
                .is_some_and(|p| !ty.qualified_name.starts_with(p.as_str()))
                || attribute
                    .as_ref()
                    .is_some_and(|a| !ty.attributes.iter().any(|x| &x.key == a))
                || primitive.as_ref().is_some_and(|p| p != ty.primitive_type)
                || ds.is_some_and(|ds| ds != ty.has_ds())
            {
                continue;
            }
            result.push(session.type_value(lua, ty.index)?)?;
        }
        values(lua, result)
    })?;
    add(lua, &table, "of", session, |lua, session, args| {
        let value = args.get::<Value>(0)?;
        let recognizers: Vec<Recognizer> = session.recognizers.borrow().clone();
        for recognize in recognizers {
            if let Some(index) = recognize(value)? {
                return values(lua, session.type_value(lua, index)?);
            }
        }
        values(lua, Value::Nil)
    })?;
    table.raw_set("version", session.registry.registry_version())?;
    table.raw_set("count", session.registry.len())?;
    Ok(table)
}
