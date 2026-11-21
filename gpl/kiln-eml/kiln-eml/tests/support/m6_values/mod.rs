// SPDX-License-Identifier: GPL-3.0-or-later
//! Scalar, enum and bitmask types appended to the type session's synthetic registry, and a TEST-ONLY implementation of the
//! codec's typed-object seam. The test wrappers are not the wrappers': they hold exactly what the seam passes (owned fields,
//! items, the variant pair, or a decoded mapped value) so the codec can be exercised before the real wrappers exist.
#![allow(dead_code)]
use std::{collections::HashMap, rc::Rc};

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{EnumFieldMetadata, PrimitiveType as P, StructFieldMetadata, TypeFlags as F, TypeIndex, TypeMetadata},
    resource::value::Value,
};
use kiln_eml::sf_startup::resource_values::{
    Converter, LuaConversionErrorKind, MappedValue, ObjectKind, TypeHandle, TypedObjects, try_clone_lua_value,
};
use mlua::{AnyUserData, UserData};
use sfi_core::lua::LuaValue;

use crate::reg;

pub const U8: usize = reg::COUNT; // 25
pub const S8: usize = U8 + 1;
pub const U16: usize = U8 + 2;
pub const S16: usize = U8 + 3;
pub const S32: usize = U8 + 4;
pub const U64: usize = U8 + 5;
pub const S64: usize = U8 + 6;
pub const F32: usize = U8 + 7;
pub const F64: usize = U8 + 8;
pub const BOOL: usize = U8 + 9;
pub const GUID: usize = U8 + 10;
pub const REF: usize = U8 + 11;
pub const COLOR: usize = U8 + 12; // enum over UInt8: Red 0, Green 1, Blue 7
pub const BITS: usize = U8 + 13; // enum of bit indices: A 0, B 3
pub const FLAGS: usize = U8 + 14; // Bitmask8 over BITS
pub const SCALARS: usize = U8 + 15; // struct with one field of each of the above
pub const NONE: usize = U8 + 16;
pub const COUNT: usize = U8 + 17;

fn scalar(index: usize, name: &str, primitive_type: P, size: u32) -> TypeMetadata {
    let qualified = format!("m6.{name}");
    TypeMetadata {
        index: TypeIndex::new(index),
        name: name.into(),
        impact_name: format!("M6_{name}"),
        qualified_hash: kfc::hash::fnv(&qualified),
        internal_hash: 0xc600_0000 + index as u32,
        name_hash: kfc::hash::fnv(name),
        impact_hash: kfc::hash::fnv(&format!("M6_{name}")),
        qualified_name: qualified,
        namespace: vec!["m6".into()],
        inner_type: None,
        size,
        alignment: size.clamp(1, 8) as u16,
        element_alignment: size.clamp(1, 8) as u16,
        field_count: 0,
        primitive_type,
        flags: F::NONE,
        struct_fields: Default::default(),
        enum_fields: Default::default(),
        default_value: None,
        attributes: Default::default(),
    }
}

/// (field name, type index, data offset) of m6.Scalars; offset 51 is padding.
pub const SCALAR_FIELDS: [(&str, usize, u64); 16] = [
    ("u8", U8, 0),
    ("s8", S8, 1),
    ("u16", U16, 2),
    ("s16", S16, 4),
    ("s32", S32, 8),
    ("u32", 0, 12),
    ("u64", U64, 16),
    ("s64", S64, 24),
    ("f32", F32, 32),
    ("f64", F64, 40),
    ("b", BOOL, 48),
    ("color", COLOR, 49),
    ("flags", FLAGS, 50),
    ("guid", GUID, 52),
    ("ref", REF, 68),
    ("alias", reg::ALIAS, 84),
];
pub const SCALARS_SIZE: u32 = 88;
pub const PADDING: usize = 51;

pub fn types() -> Vec<TypeMetadata> {
    let mut types = reg::types();
    for (index, name, p, size) in [
        (U8, "U8", P::UInt8, 1),
        (S8, "S8", P::SInt8, 1),
        (U16, "U16", P::UInt16, 2),
        (S16, "S16", P::SInt16, 2),
        (S32, "S32", P::SInt32, 4),
        (U64, "U64", P::UInt64, 8),
        (S64, "S64", P::SInt64, 8),
        (F32, "F32", P::Float32, 4),
        (F64, "F64", P::Float64, 8),
        (BOOL, "Bool", P::Bool, 1),
        (GUID, "Guid", P::Guid, 16),
        (REF, "Ref", P::ObjectReference, 16),
    ] {
        types.push(scalar(index, name, p, size));
    }
    let mut color = scalar(COLOR, "Color", P::Enum, 1);
    color.inner_type = Some(TypeIndex::new(U8));
    for (name, value) in [("Red", 0u64), ("Green", 1), ("Blue", 7)] {
        color.enum_fields.insert(
            name.into(),
            EnumFieldMetadata {
                name: name.into(),
                value,
            },
        );
    }
    color.field_count = 3;
    types.push(color);
    let mut bits = scalar(BITS, "Bits", P::Enum, 1);
    bits.inner_type = Some(TypeIndex::new(U8));
    for (name, value) in [("A", 0u64), ("B", 3)] {
        bits.enum_fields.insert(
            name.into(),
            EnumFieldMetadata {
                name: name.into(),
                value,
            },
        );
    }
    types.push(bits);
    let mut flags = scalar(FLAGS, "Flags", P::Bitmask8, 1);
    flags.inner_type = Some(TypeIndex::new(BITS));
    types.push(flags);
    let mut scalars = scalar(SCALARS, "Scalars", P::Struct, SCALARS_SIZE);
    scalars.alignment = 8;
    for (name, index, data_offset) in SCALAR_FIELDS {
        scalars.struct_fields.insert(
            name.into(),
            StructFieldMetadata {
                name: name.into(),
                r#type: TypeIndex::new(index),
                data_offset,
                attributes: Default::default(),
            },
        );
    }
    scalars.field_count = SCALAR_FIELDS.len() as u32;
    types.push(scalars);
    types.push(scalar(NONE, "Nothing", P::None, 0));
    assert_eq!(types.len(), COUNT);
    types
}

pub fn registry() -> Rc<kfc::reflection::TypeRegistry> {
    Rc::new(reg::registry_of("m6-values", types()))
}

pub fn handle(registry: &Rc<kfc::reflection::TypeRegistry>, index: usize) -> TypeHandle {
    TypeHandle::new(registry.clone(), TypeIndex::new(index))
}

// ---- test-only wrappers -------------------------------------------------------------------------------------------

pub struct TestStruct {
    pub r#type: TypeHandle,
    pub fields: HashMap<String, LuaValue>,
}
impl UserData for TestStruct {}
pub struct TestArray {
    pub r#type: TypeHandle,
    pub items: Vec<LuaValue>,
}
impl UserData for TestArray {}
pub struct TestVariant {
    pub r#type: TypeHandle,
    pub variant: TypeHandle,
    pub value: LuaValue,
}
impl UserData for TestVariant {}
pub struct TestMapped(pub MappedValue);
impl UserData for TestMapped {}
pub struct TestContent(pub ContentHash);
impl UserData for TestContent {}
pub struct TestResource(pub Guid);
impl UserData for TestResource {}
/// A userdata the seam does not know.
pub struct Foreign;
impl UserData for Foreign {}

pub struct TestObjects;

impl TypedObjects for TestObjects {
    fn classify(&self, ud: &AnyUserData) -> mlua::Result<Option<(ObjectKind, TypeHandle)>> {
        Ok(if let Ok(s) = ud.borrow::<TestStruct>() {
            Some((ObjectKind::Struct, s.r#type.clone()))
        } else if let Ok(a) = ud.borrow::<TestArray>() {
            Some((ObjectKind::Array, a.r#type.clone()))
        } else if let Ok(v) = ud.borrow::<TestVariant>() {
            Some((ObjectKind::Variant, v.r#type.clone()))
        } else if let Ok(m) = ud.borrow::<TestMapped>() {
            match &m.0 {
                MappedValue::Struct(s) => Some((ObjectKind::Struct, s.r#type().clone())),
                MappedValue::Array(a) => Some((ObjectKind::Array, a.r#type().clone())),
                MappedValue::Variant(v) => Some((ObjectKind::Variant, v.base_type().clone())),
                _ => None,
            }
        } else {
            None
        })
    }

    fn clone_object(&self, ud: &AnyUserData, lua: &mlua::Lua) -> mlua::Result<LuaValue> {
        if let Ok(s) = ud.borrow::<TestStruct>() {
            let mut fields = HashMap::new();
            for (k, v) in &s.fields {
                fields.insert(k.clone(), try_clone_lua_value(v, lua, self)?);
            }
            return lua
                .create_userdata(TestStruct {
                    r#type: s.r#type.clone(),
                    fields,
                })
                .map(LuaValue::UserData);
        }
        if let Ok(a) = ud.borrow::<TestArray>() {
            let items = a
                .items
                .iter()
                .map(|v| try_clone_lua_value(v, lua, self))
                .collect::<mlua::Result<_>>()?;
            return lua
                .create_userdata(TestArray {
                    r#type: a.r#type.clone(),
                    items,
                })
                .map(LuaValue::UserData);
        }
        if let Ok(v) = ud.borrow::<TestVariant>() {
            let value = try_clone_lua_value(&v.value, lua, self)?;
            return lua
                .create_userdata(TestVariant {
                    r#type: v.r#type.clone(),
                    variant: v.variant.clone(),
                    value,
                })
                .map(LuaValue::UserData);
        }
        let m = ud.borrow::<TestMapped>()?;
        lua.create_userdata(TestMapped(m.0.clone())).map(LuaValue::UserData)
    }

    fn convert_object(&self, ud: &AnyUserData, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        if let Ok(s) = ud.borrow::<TestStruct>() {
            return converter.struct_fields(&s.r#type, s.fields.iter().map(|(k, v)| (k.as_str(), v)));
        }
        if let Ok(a) = ud.borrow::<TestArray>() {
            return converter.array_items(&a.r#type, a.items.iter());
        }
        if let Ok(v) = ud.borrow::<TestVariant>() {
            return converter.variant(&v.variant, &v.value);
        }
        let m = ud.borrow::<TestMapped>()?;
        Ok(Value::from(m.0.clone())?)
    }

    fn new_struct(
        &self,
        lua: &mlua::Lua,
        r#type: TypeHandle,
        fields: HashMap<String, LuaValue>,
    ) -> mlua::Result<LuaValue> {
        lua.create_userdata(TestStruct { r#type, fields })
            .map(LuaValue::UserData)
    }

    fn new_array(&self, lua: &mlua::Lua, r#type: TypeHandle, items: Vec<LuaValue>) -> mlua::Result<LuaValue> {
        lua.create_userdata(TestArray { r#type, items }).map(LuaValue::UserData)
    }

    fn new_variant(
        &self,
        lua: &mlua::Lua,
        r#type: TypeHandle,
        variant: TypeHandle,
        value: LuaValue,
    ) -> mlua::Result<LuaValue> {
        lua.create_userdata(TestVariant { r#type, variant, value })
            .map(LuaValue::UserData)
    }

    fn wrap_mapped(&self, lua: &mlua::Lua, value: &MappedValue) -> mlua::Result<LuaValue> {
        lua.create_userdata(TestMapped(value.clone())).map(LuaValue::UserData)
    }

    fn resource_guid(&self, ud: &AnyUserData) -> mlua::Result<Option<Guid>> {
        Ok(ud.borrow::<TestResource>().ok().map(|r| r.0))
    }

    fn content_hash(&self, ud: &AnyUserData) -> mlua::Result<Option<ContentHash>> {
        Ok(ud.borrow::<TestContent>().ok().map(|c| c.0))
    }
}
