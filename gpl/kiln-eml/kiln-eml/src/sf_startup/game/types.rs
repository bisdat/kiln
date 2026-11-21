// SPDX-License-Identifier: GPL-3.0-or-later
//! `game.types` and the Type userdata.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/bindings/game/types.rs:18-505. Same functions, argument casts (sfi-core's SF casts), return
//! shapes, texts and lazily cached collections. Differences, none observable from a package in normal use:
//! - the registry and Type cache are the captured `TypeSession`, not Lua app data;
//! - `of` asks the session's recognizers (the value wrappers and the asset session add the six SF value classes); with none it answers nil, as SF does
//!   for every value outside those classes;
//! - an index SF would `expect` to exist raises a Lua error instead of panicking;
//! - a Lua finalizer that the collector runs while the session builds a Type, and that asks for that Type, gets it
//!   (SF's cache entry is borrowed across the allocation, so there that call fails); see `TypeSession::get_type`.
use std::{cell::RefCell, ops::Deref, rc::Weak};

use kfc::reflection::{
    Attribute, EnumFieldMetadata, LookupKey, PrimitiveType, StructFieldMetadata, TypeFlags, TypeHandle, TypeIndex,
    TypeMetadata, TypeRegistry,
};
use mlua::{IntoLua, Table, UserData};
use sfi_core::lua::{FunctionArgs, LuaValue};
use std::rc::Rc;

use super::{
    super::{
        readonly::{ReadOnlyArray, ReadOnlyMap},
        session::{Inner, TypeSession},
    },
    add_function,
    resolve_type::{self, HashKind},
};

/// A package's `game.types` table over `session`.
pub fn create(lua: &mlua::Lua, session: &TypeSession) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    let s = session.clone();
    add_function(lua, &table, "get", move |lua, args| lua_get(lua, &s, args))?;
    let s = session.clone();
    add_function(lua, &table, "get_by_qualified_name", move |lua, args| {
        lua_get(lua, &s, args)
    })?;
    let s = session.clone();
    add_function(lua, &table, "get_by_impact_name", move |lua, args| {
        lua_get_by_impact_name(lua, &s, args)
    })?;
    let s = session.clone();
    add_function(lua, &table, "get_by_qualified_hash", move |lua, args| {
        get_by_hash(lua, &s, args, HashKind::Qualified)
    })?;
    let s = session.clone();
    add_function(lua, &table, "get_by_internal_hash", move |lua, args| {
        get_by_hash(lua, &s, args, HashKind::Internal)
    })?;
    let s = session.clone();
    add_function(lua, &table, "get_all", move |lua, args| lua_get_all(lua, &s, args))?;
    let s = session.clone();
    add_function(lua, &table, "resolve", move |lua, args| lua_resolve(lua, &s, args))?;
    let s = session.clone();
    add_function(lua, &table, "find_by_hash", move |lua, args| {
        lua_find_by_hash(lua, &s, args)
    })?;
    let s = session.clone();
    add_function(lua, &table, "get_by_index", move |lua, args| {
        lua_get_by_index(lua, &s, args)
    })?;
    let s = session.clone();
    add_function(lua, &table, "find", move |lua, args| lua_find(lua, &s, args))?;
    let s = session.clone();
    add_function(lua, &table, "of", move |lua, args| lua_of(lua, &s, args))?;
    table.raw_set("version", session.registry().version.clone())?;
    table.raw_set("count", session.registry().len())?;
    Ok(table)
}

fn lua_get(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<Option<LuaValue>> {
    let name = args.get::<String>(0)?;
    get_by_name(lua, session, LookupKey::Qualified(&name))
}

fn lua_get_by_impact_name(
    lua: &mlua::Lua,
    session: &TypeSession,
    args: FunctionArgs,
) -> mlua::Result<Option<LuaValue>> {
    let name = args.get::<String>(0)?;
    get_by_name(lua, session, LookupKey::Impact(&name))
}

fn get_by_hash(
    lua: &mlua::Lua,
    session: &TypeSession,
    args: FunctionArgs,
    kind: HashKind,
) -> mlua::Result<(Option<LuaValue>, Option<String>)> {
    let hash = args.get::<u32>(0)?;
    match resolve_type::by_hash(session.registry(), hash, kind) {
        Ok(Some(metadata)) => Ok((session.get_type(lua, metadata.index)?, None)),
        Ok(None) => Ok((None, Some(format!("type hash not found: 0x{hash:08x}")))),
        Err(reason) => Ok((None, Some(reason))),
    }
}

fn lua_get_all(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<Table> {
    let include_ds = args.get::<Option<bool>>(0)?.unwrap_or(true);
    let registry = session.registry();
    let result = lua.create_table_with_capacity(registry.len(), 0)?;
    for ty in registry.iter() {
        if !include_ds && ty.flags.contains(TypeFlags::HAS_DS) {
            continue;
        }
        result.push(existing_type(lua, session, ty.index)?)?;
    }
    Ok(result)
}

fn lua_resolve(
    lua: &mlua::Lua,
    session: &TypeSession,
    args: FunctionArgs,
) -> mlua::Result<(Option<LuaValue>, Option<String>)> {
    match resolve_type::resolve(session.registry(), args.get::<LuaValue>(0)?) {
        Ok(index) => Ok((session.get_type(lua, index)?, None)),
        Err(reason) => Ok((None, Some(reason))),
    }
}

fn lua_get_by_index(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<Option<LuaValue>> {
    session.get_type(lua, TypeIndex::new(args.get::<usize>(0)?))
}

fn lua_find_by_hash(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<Table> {
    let hash = args.get::<u32>(0)?;
    let domain = args.get::<Option<String>>(1)?.unwrap_or_else(|| "qualified".into());
    let kind = HashKind::parse(&domain).map_err(mlua::Error::runtime)?;
    let result = lua.create_table()?;
    for ty in session.registry().iter().filter(|ty| kind.value(ty) == hash) {
        result.push(session.get_type(lua, ty.index)?)?;
    }
    Ok(result)
}

fn lua_find(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<Table> {
    let options = args.get::<Option<Table>>(0)?;
    // Option values use mlua's own conversions (Table::get), as SF does: not the SF argument casts.
    let prefix = options
        .map(|o| o.get::<Option<String>>("prefix"))
        .transpose()?
        .flatten();
    let attribute = options
        .map(|o| o.get::<Option<String>>("attribute"))
        .transpose()?
        .flatten();
    let primitive = options
        .map(|o| o.get::<Option<String>>("primitive_type"))
        .transpose()?
        .flatten();
    let ds = options.map(|o| o.get::<Option<bool>>("has_ds")).transpose()?.flatten();
    let result = lua.create_table()?;
    for ty in session.registry().iter() {
        if prefix.as_ref().is_some_and(|p| !ty.qualified_name.starts_with(p))
            || attribute.as_ref().is_some_and(|a| !ty.attributes.contains_key(a))
            || primitive
                .as_ref()
                .is_some_and(|p| p != primitive_type_to_string(ty.primitive_type))
            || ds.is_some_and(|ds| ds != ty.flags.contains(TypeFlags::HAS_DS))
        {
            continue;
        }
        result.push(session.get_type(lua, ty.index)?)?;
    }
    Ok(result)
}

fn lua_of(lua: &mlua::Lua, session: &TypeSession, args: FunctionArgs) -> mlua::Result<LuaValue> {
    let value = args.get::<LuaValue>(0)?;
    let index = match session.type_of(value)? {
        Some(index) => index,
        None => return Ok(LuaValue::Nil),
    };
    existing_type(lua, session, index)
}

fn get_by_name(lua: &mlua::Lua, session: &TypeSession, name: LookupKey<&str>) -> mlua::Result<Option<LuaValue>> {
    // The registry looks names up by hash; only a type whose own name is exactly the one asked for counts.
    let index = session
        .registry()
        .get_by_name(name)
        .filter(|ty| match name {
            LookupKey::Qualified(name) => ty.qualified_name == name,
            LookupKey::Impact(name) => ty.impact_name == name,
        })
        .map(|ty| ty.index);
    match index {
        Some(index) => session.get_type(lua, index),
        None => Ok(None),
    }
}

/// A Type SF `expect`s to exist (an index taken from the registry itself, or a recognizer's answer).
fn existing_type(lua: &mlua::Lua, session: &TypeSession, index: TypeIndex) -> mlua::Result<LuaValue> {
    session
        .get_type(lua, index)?
        .ok_or_else(|| mlua::Error::runtime(format!("invalid type index {index}")))
}

/// A reflected type, cached per session by index (`TypeSession::get_type`).
pub struct Type {
    handle: TypeHandle<Rc<TypeRegistry>>,
    session: Weak<Inner>,
    struct_field_cache: RefCell<Option<LuaValue>>,
    enum_field_cache: RefCell<Option<LuaValue>>,
    attribute_cache: RefCell<Option<LuaValue>>,
}

impl Type {
    pub(crate) fn new(handle: TypeHandle<Rc<TypeRegistry>>, session: Weak<Inner>) -> Self {
        Self {
            handle,
            session,
            struct_field_cache: RefCell::new(None),
            enum_field_cache: RefCell::new(None),
            attribute_cache: RefCell::new(None),
        }
    }

    fn session(&self) -> mlua::Result<TypeSession> {
        TypeSession::upgrade(&self.session)
    }

    /// once_cell's `get_or_try_init`: a failed build is not cached.
    fn cached(
        cell: &RefCell<Option<LuaValue>>,
        build: impl FnOnce() -> mlua::Result<LuaValue>,
    ) -> mlua::Result<LuaValue> {
        if let Some(value) = cell.borrow().as_ref() {
            return Ok(value.clone());
        }
        let value = build()?;
        Ok(cell.borrow_mut().get_or_insert(value).clone())
    }
}

impl Deref for Type {
    type Target = TypeMetadata;

    fn deref(&self) -> &Self::Target {
        self.handle.deref()
    }
}

impl UserData for Type {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("index", |_, this| Ok(this.index.as_usize()));
        fields.add_field_method_get("name", |_, this| Ok(this.name.clone()));
        fields.add_field_method_get("impact_name", |_, this| Ok(this.impact_name.clone()));
        fields.add_field_method_get("qualified_name", |_, this| Ok(this.qualified_name.clone()));
        fields.add_field_method_get("qualified_hash", |_, this| Ok(this.qualified_hash));
        fields.add_field_method_get("internal_hash", |_, this| Ok(this.internal_hash));
        fields.add_field_method_get("name_hash", |_, this| Ok(this.name_hash));
        fields.add_field_method_get("impact_hash", |_, this| Ok(this.impact_hash));
        fields.add_field_method_get("namespace", |_, this| Ok(this.namespace.clone()));
        fields.add_field_method_get("inner_type", |lua, this| {
            this.inner_type
                .as_ref()
                .map(|inner| this.session()?.get_type(lua, *inner))
                .transpose()
        });
        fields.add_field_method_get("size", |_, this| Ok(this.size));
        fields.add_field_method_get("alignment", |_, this| Ok(this.alignment));
        fields.add_field_method_get("element_alignment", |_, this| Ok(this.element_alignment));
        fields.add_field_method_get("field_count", |_, this| Ok(this.field_count));
        fields.add_field_method_get("primitive_type", |_, this| {
            Ok(primitive_type_to_string(this.primitive_type))
        });
        fields.add_field_method_get("flags_bits", |_, this| Ok(this.flags.bits()));
        fields.add_field_method_get("flags", |lua, this| {
            lua.create_sequence_from(this.flags.iter_names().map(|(name, _)| name))
        });
        fields.add_field_method_get("has_ds", |_, this| Ok(this.flags.contains(TypeFlags::HAS_DS)));
        fields.add_field_method_get("default_bytes", |lua, this| {
            this.default_value
                .as_ref()
                .map(|bytes| lua.create_string(bytes))
                .transpose()
        });
        fields.add_field_method_get("fields", |lua, this| {
            let session = this.session()?;
            let result = lua.create_table()?;
            for (owner, field) in resolve_type::fields(session.registry(), this.index).map_err(mlua::Error::runtime)? {
                let entry = lua.create_table()?;
                entry.raw_set("name", field.name.clone())?;
                entry.raw_set("type", session.get_type(lua, field.r#type)?)?;
                entry.raw_set("declaring_type", session.get_type(lua, owner)?)?;
                entry.raw_set("data_offset", field.data_offset)?;
                entry.raw_set("attributes", attributes_to_lua(lua, &session, &field.attributes)?)?;
                result.push(entry)?;
            }
            Ok(result)
        });
        fields.add_field_method_get("struct_fields", |lua, this| {
            Type::cached(&this.struct_field_cache, || {
                struct_fields_to_lua(lua, &this.session()?, this)
            })
        });
        fields.add_field_method_get("enum_fields", |lua, this| {
            Type::cached(&this.enum_field_cache, || enum_fields_to_lua(lua, this))
        });
        fields.add_field_method_get("attributes", |lua, this| {
            Type::cached(&this.attribute_cache, || {
                attributes_to_lua(lua, &this.session()?, &this.attributes)
            })
        });
    }

    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("is_a", |_, this, selector: LuaValue| {
            let session = this.session()?;
            let parent = resolve_type::resolve(session.registry(), &selector).map_err(mlua::Error::runtime)?;
            Ok(resolve_type::inheritance(session.registry(), this.index)
                .map_err(mlua::Error::runtime)?
                .contains(&parent))
        });
    }
}

pub fn primitive_type_to_string(primitive_type: PrimitiveType) -> &'static str {
    match primitive_type {
        PrimitiveType::None => "None",
        PrimitiveType::Bool => "Bool",
        PrimitiveType::UInt8 => "UInt8",
        PrimitiveType::SInt8 => "SInt8",
        PrimitiveType::UInt16 => "UInt16",
        PrimitiveType::SInt16 => "SInt16",
        PrimitiveType::UInt32 => "UInt32",
        PrimitiveType::SInt32 => "SInt32",
        PrimitiveType::UInt64 => "UInt64",
        PrimitiveType::SInt64 => "SInt64",
        PrimitiveType::Float32 => "Float32",
        PrimitiveType::Float64 => "Float64",
        PrimitiveType::Enum => "Enum",
        PrimitiveType::Bitmask8 => "Bitmask8",
        PrimitiveType::Bitmask16 => "Bitmask16",
        PrimitiveType::Bitmask32 => "Bitmask32",
        PrimitiveType::Bitmask64 => "Bitmask64",
        PrimitiveType::Typedef => "Typedef",
        PrimitiveType::Struct => "Struct",
        PrimitiveType::StaticArray => "StaticArray",
        PrimitiveType::DsArray => "DsArray",
        PrimitiveType::DsString => "DsString",
        PrimitiveType::DsOptional => "DsOptional",
        PrimitiveType::DsVariant => "DsVariant",
        PrimitiveType::BlobArray => "BlobArray",
        PrimitiveType::BlobString => "BlobString",
        PrimitiveType::BlobOptional => "BlobOptional",
        PrimitiveType::BlobVariant => "BlobVariant",
        PrimitiveType::ObjectReference => "ObjectReference",
        PrimitiveType::Guid => "Guid",
    }
}

fn read_only_map(lua: &mlua::Lua, entries: Vec<(mlua::String, LuaValue)>) -> mlua::Result<LuaValue> {
    ReadOnlyMap::new(entries).into_lua(lua)
}

fn struct_fields_to_lua(lua: &mlua::Lua, session: &TypeSession, this: &Type) -> mlua::Result<LuaValue> {
    let mut map = Vec::with_capacity(this.struct_fields.len());
    for (name, field) in &this.struct_fields {
        map.push((lua.create_string(name)?, struct_field_to_lua(lua, session, field)?));
    }
    read_only_map(lua, map)
}

fn struct_field_to_lua(lua: &mlua::Lua, session: &TypeSession, field: &StructFieldMetadata) -> mlua::Result<LuaValue> {
    let name = LuaValue::String(lua.create_string(&field.name)?);
    let r#type = session.get_type(lua, field.r#type)?.unwrap_or(LuaValue::Nil);
    let data_offset = LuaValue::Integer(field.data_offset as i64);
    let attributes = attributes_to_lua(lua, session, &field.attributes)?;
    read_only_map(
        lua,
        vec![
            (lua.create_string("name")?, name),
            (lua.create_string("type")?, r#type),
            (lua.create_string("data_offset")?, data_offset),
            (lua.create_string("attributes")?, attributes),
        ],
    )
}

fn attributes_to_lua<'a>(
    lua: &mlua::Lua,
    session: &TypeSession,
    attributes: impl IntoIterator<Item = (&'a String, &'a Attribute)>,
) -> mlua::Result<LuaValue> {
    let mut map = Vec::new();
    for (name, attribute) in attributes {
        map.push((lua.create_string(name)?, attribute_to_lua(lua, session, attribute)?));
    }
    read_only_map(lua, map)
}

fn attribute_to_lua(lua: &mlua::Lua, session: &TypeSession, attribute: &Attribute) -> mlua::Result<LuaValue> {
    let name = LuaValue::String(lua.create_string(&attribute.name)?);
    let namespace = namespace_to_lua(lua, &attribute.namespace)?;
    let r#type = attribute
        .r#type
        .map(|r#type| session.get_type(lua, r#type))
        .transpose()?
        .and_then(|t| t)
        .unwrap_or(LuaValue::Nil);
    let value = LuaValue::String(lua.create_string(&attribute.value)?);
    read_only_map(
        lua,
        vec![
            (lua.create_string("name")?, name),
            (lua.create_string("namespace")?, namespace),
            (lua.create_string("type")?, r#type),
            (lua.create_string("value")?, value),
        ],
    )
}

fn namespace_to_lua(lua: &mlua::Lua, namespace: &[String]) -> mlua::Result<LuaValue> {
    let mut array = Vec::with_capacity(namespace.len());
    for name in namespace {
        array.push(LuaValue::String(lua.create_string(name)?));
    }
    ReadOnlyArray::new(array).into_lua(lua)
}

fn enum_fields_to_lua(lua: &mlua::Lua, this: &Type) -> mlua::Result<LuaValue> {
    let mut map = Vec::with_capacity(this.enum_fields.len());
    for (name, field) in &this.enum_fields {
        map.push((lua.create_string(name)?, enum_field_to_lua(lua, field)?));
    }
    read_only_map(lua, map)
}

fn enum_field_to_lua(lua: &mlua::Lua, field: &EnumFieldMetadata) -> mlua::Result<LuaValue> {
    let name = LuaValue::String(lua.create_string(&field.name)?);
    // The bits as Lua's integer (u64 above i64::MAX wraps negative), and the exact bits as text.
    let value = LuaValue::Integer(field.value as i64);
    read_only_map(
        lua,
        vec![
            (lua.create_string("name")?, name),
            (lua.create_string("value")?, value),
            (
                lua.create_string("value_hex")?,
                LuaValue::String(lua.create_string(format!("0x{:016x}", field.value))?),
            ),
        ],
    )
}
