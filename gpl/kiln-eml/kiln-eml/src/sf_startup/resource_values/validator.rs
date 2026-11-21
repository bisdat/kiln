// SPDX-License-Identifier: GPL-3.0-or-later
//! Validates a Lua value against a reflected type and returns its owned, unified copy (the input is never changed).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/resource_values/validate_resource_values.rs:25-735 and the Guid cast of
//! lua/bindings/game/content_id_casts.rs:11-45.
//!
//! Plain tables are read the way SF reads them, never flattened with `pairs`: a struct's fields one `Table::get` each, in
//! the type's field order (metamethods run; a failing get is `MissingField`); a variant's `type` then `value`; an array's
//! `Table::len` (as many calls as SF makes) and `sequence_values` (stops at the first nil). Unknown keys are ignored.
//! The owned results are built through the typed-object seam (`TypedObjects`), the value wrappers in production.
//!
//! Where SF panics this raises a Lua error with SF's panic text instead: `todo!()` on the four DS kinds, an
//! `expect` on a missing inner or field type, a userdata without a type id, and a negative `__len` (SF's
//! `Vec::with_capacity` overflow). Any other panic under the walk (the parser's `expect`s on a faulty registry) is
//! caught at the top and becomes an error at the current path. A `__len` too large to allocate aborts SF; here it only
//! bounds the reserved capacity. A value nested deeper than `MAX_DEPTH` (a self-referencing table of a recursive type)
//! overflows SF's stack; here it is an error, as is a type whose inheritance chain is a cycle (kfc would loop forever).
use std::collections::HashMap;

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{LookupKey, PrimitiveType},
    resource::mapped::MappingError,
};
use mlua::AnyUserData;
use sfi_core::lua::{LuaError, LuaValue};
use std::fmt;

use super::{
    MAX_DEPTH, ObjectKind, TypeHandle, TypedObjects, check_chain,
    converter::{host_failure, panic_text, userdata_label},
    name_of,
    path::TreePath,
    too_deep,
};

/// SF's `#[error("at {path}: {kind}")]` (thiserror there; written out here).
#[derive(Debug)]
pub struct LuaValidationError {
    pub path: String,
    pub kind: LuaValidationErrorKind,
}

#[derive(Debug)]
pub enum LuaValidationErrorKind {
    IncompatibleType { expected: String, found: String },
    MissingField { field: String, r#type: String },
    InvalidType(String),
    InvalidGuid(String),
    Lua(mlua::Error),
    Mapping(MappingError),
}

impl fmt::Display for LuaValidationError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "at {}: {}", self.path, self.kind)
    }
}
impl std::error::Error for LuaValidationError {}

impl fmt::Display for LuaValidationErrorKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleType { expected, found } => write!(f, "expected `{expected}`, got `{found}`"),
            Self::MissingField { field, r#type } => write!(f, "missing field `{field}` in struct `{type}`"),
            Self::InvalidType(name) => write!(f, "invalid type `{name}`"),
            Self::InvalidGuid(guid) => write!(f, "invalid guid `{guid}`"),
            Self::Lua(error) => write!(f, "{error}"),
            Self::Mapping(error) => write!(f, "{error}"),
        }
    }
}
impl std::error::Error for LuaValidationErrorKind {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Lua(error) => Some(error),
            Self::Mapping(error) => Some(error),
            _ => None,
        }
    }
}
impl From<mlua::Error> for LuaValidationErrorKind {
    fn from(error: mlua::Error) -> Self {
        Self::Lua(error)
    }
}
impl From<MappingError> for LuaValidationErrorKind {
    fn from(error: MappingError) -> Self {
        Self::Mapping(error)
    }
}

/// Reserved room for an array's items; the real length is whatever the sequence yields.
const CAPACITY_BOUND: i64 = 4096;

pub struct Validator<'a> {
    pub path: TreePath,
    pub lua: mlua::Lua,
    objects: &'a dyn TypedObjects,
    depth: usize,
}

impl<'a> Validator<'a> {
    pub fn validate_and_clone(
        value: &LuaValue,
        r#type: &TypeHandle,
        lua: &mlua::Lua,
        objects: &'a dyn TypedObjects,
    ) -> Result<LuaValue, LuaValidationError> {
        let mut validator = Self {
            path: TreePath::new(),
            lua: lua.clone(),
            objects,
            depth: 0,
        };
        // SF's own and the parser's panics (kfc's `expect`s on a faulty registry) end here as errors at their path
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| validator.process(value, r#type)))
            .unwrap_or_else(|panic| Err(host_failure(&panic_text(panic)).into()));
        result.map_err(|kind| LuaValidationError {
            path: validator.path.to_string(),
            kind,
        })
    }

    /// One level of the walk: [`MAX_DEPTH`] levels at most.
    pub fn process(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<LuaValue, LuaValidationErrorKind> {
        if self.depth >= MAX_DEPTH {
            return Err(too_deep().into());
        }
        self.depth += 1;
        let result = self.process_level(value, r#type);
        self.depth -= 1;
        result
    }

    fn process_level(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<LuaValue, LuaValidationErrorKind> {
        match r#type.primitive_type {
            PrimitiveType::None => match value {
                LuaValue::Nil => Ok(LuaValue::Nil),
                _ => Err(incompatible("nil", value)?),
            },
            PrimitiveType::Bool => match value {
                LuaValue::Boolean(b) => Ok(LuaValue::Boolean(*b)),
                _ => Err(incompatible("boolean", value)?),
            },
            PrimitiveType::UInt8 => self.process_uint(value, 0, u8::MAX as u64),
            PrimitiveType::SInt8 => self.process_sint(value, i8::MIN as i64, i8::MAX as i64),
            PrimitiveType::UInt16 => self.process_uint(value, 0, u16::MAX as u64),
            PrimitiveType::SInt16 => self.process_sint(value, i16::MIN as i64, i16::MAX as i64),
            PrimitiveType::UInt32 => self.process_uint(value, 0, u32::MAX as u64),
            PrimitiveType::SInt32 => self.process_sint(value, i32::MIN as i64, i32::MAX as i64),
            PrimitiveType::UInt64 => self.process_uint(value, 0, u64::MAX),
            PrimitiveType::SInt64 => self.process_sint(value, i64::MIN, i64::MAX),
            PrimitiveType::Float32 | PrimitiveType::Float64 => match value {
                LuaValue::Integer(i) => Ok(LuaValue::Number(*i as f64)),
                LuaValue::Number(n) => Ok(LuaValue::Number(*n)),
                _ => Err(incompatible("float", value)?),
            },
            PrimitiveType::Enum => self.process_enum(value, r#type),
            PrimitiveType::Bitmask8
            | PrimitiveType::Bitmask16
            | PrimitiveType::Bitmask32
            | PrimitiveType::Bitmask64
            | PrimitiveType::StaticArray
            | PrimitiveType::BlobArray => self.process_array_like(value, r#type),
            PrimitiveType::Typedef => {
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| host_failure("invalid typedef type"))?;
                self.process(value, &inner)
            }
            PrimitiveType::Struct => self.process_struct(value, r#type),
            PrimitiveType::DsArray | PrimitiveType::DsString | PrimitiveType::DsOptional | PrimitiveType::DsVariant => {
                Err(host_failure("not yet implemented").into())
            }
            PrimitiveType::BlobString => match value {
                LuaValue::String(s) => Ok(LuaValue::String(s.clone())),
                _ => Err(incompatible("string", value)?),
            },
            PrimitiveType::BlobOptional => {
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| host_failure("invalid optional type"))?;
                match value {
                    LuaValue::Nil => Ok(LuaValue::Nil),
                    _ => self.process(value, &inner),
                }
            }
            PrimitiveType::BlobVariant => self.process_blob_variant(value, r#type),
            PrimitiveType::ObjectReference => self.process_object_reference(value),
            PrimitiveType::Guid => self.process_guid(value),
        }
    }

    fn process_uint(&mut self, value: &LuaValue, min: u64, max: u64) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::Integer(i) if *i as u64 >= min && *i as u64 <= max => Ok(LuaValue::Integer(*i)),
            LuaValue::Number(n) if n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64 => {
                Ok(LuaValue::Integer(*n as i64))
            }
            _ => Err(incompatible(
                &format!("unsigned integer in range [{min}, {max}]"),
                value,
            )?),
        }
    }

    fn process_sint(&mut self, value: &LuaValue, min: i64, max: i64) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::Integer(i) if *i >= min && *i <= max => Ok(LuaValue::Integer(*i)),
            LuaValue::Number(n) if n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64 => {
                Ok(LuaValue::Integer(*n as i64))
            }
            _ => Err(incompatible(&format!("signed integer in range [{min}, {max}]"), value)?),
        }
    }

    fn process_enum(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::String(s) => {
                if r#type.enum_fields.get(&s.to_string_lossy()).is_some() {
                    return Ok(LuaValue::String(s.clone()));
                }
            }
            LuaValue::Integer(i) => {
                let i = *i as u64;
                if let Some(field) = r#type.enum_fields.values().find(|f| f.value == i) {
                    return Ok(LuaValue::String(self.lua.create_string(&field.name)?));
                }
            }
            _ => {}
        }
        let names = r#type
            .enum_fields
            .keys()
            .map(|k| format!("\"{k}\""))
            .collect::<Vec<_>>()
            .join(", ");
        Err(incompatible(&format!("one of [{names}]"), value)?)
    }

    fn process_struct(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::Table(table) => self.process_struct_fields(table, r#type),
            LuaValue::UserData(ud) => self.clone_struct_ud(ud, r#type),
            _ => Err(incompatible(&name_of(r#type), value)?),
        }
    }

    fn process_struct_fields(
        &mut self,
        table: &mlua::Table,
        r#type: &TypeHandle,
    ) -> Result<LuaValue, LuaValidationErrorKind> {
        check_chain(r#type)?;
        let mut fields = HashMap::new();
        for field in r#type.iter_fields() {
            let field_type = r#type
                .get_field_type(&field.name)
                .ok_or_else(|| host_failure("invalid field type"))?;
            let field_value = match table.get::<LuaValue>(field.name.as_str()) {
                Ok(value) => value,
                Err(_) => {
                    return Err(LuaValidationErrorKind::MissingField {
                        field: field.name.clone(),
                        r#type: name_of(r#type),
                    });
                }
            };
            self.path.push(&field.name);
            let field_value = self.process(&field_value, &field_type)?;
            fields.insert(field.name.clone(), field_value);
            self.path.pop();
        }
        Ok(self.objects.new_struct(&self.lua, r#type.clone(), fields)?)
    }

    fn process_blob_variant(
        &mut self,
        value: &LuaValue,
        r#type: &TypeHandle,
    ) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::Table(table) => {
                self.path.push("type");
                let type_name = table.get::<String>("type")?;
                let registry = r#type.type_registry();
                let variant = registry
                    .get_by_name(LookupKey::Qualified(&type_name))
                    .ok_or_else(|| LuaValidationErrorKind::InvalidType(type_name.clone()))?;
                let variant_type = TypeHandle::new(registry.clone(), variant.index);
                check_chain(&variant_type)?;
                let base = r#type
                    .inner_type()
                    .ok_or_else(|| host_failure("invalid variant type"))?;
                if !variant_type.is_sub_type_of(&base) {
                    return Err(LuaValidationErrorKind::IncompatibleType {
                        expected: name_of(&base),
                        found: name_of(&variant_type),
                    });
                }
                self.path.pop();
                self.path.push("value");
                let value = table.get::<LuaValue>("value")?;
                let result = self.process_struct(&value, &variant_type)?;
                self.path.pop();
                Ok(self
                    .objects
                    .new_variant(&self.lua, r#type.clone(), variant_type, result)?)
            }
            LuaValue::UserData(ud) => self.clone_typed(ud, r#type, ObjectKind::Variant),
            _ => Err(incompatible(&name_of(r#type), value)?),
        }
    }

    fn process_object_reference(&mut self, value: &LuaValue) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::String(s) => {
                let text = s.to_string_lossy();
                ContentHash::parse(&text).ok_or(LuaValidationErrorKind::InvalidGuid(text))?;
                Ok(LuaValue::String(s.clone()))
            }
            LuaValue::UserData(ud) => match self.objects.resource_guid(ud)? {
                // ResourceId's text is its GUID's
                Some(guid) => Ok(LuaValue::String(self.lua.create_string(guid.to_string())?)),
                None => Err(LuaValidationErrorKind::IncompatibleType {
                    expected: "object reference".into(),
                    found: userdata_label(ud)?,
                }),
            },
            _ => Err(incompatible("object reference", value)?),
        }
    }

    fn process_guid(&mut self, value: &LuaValue) -> Result<LuaValue, LuaValidationErrorKind> {
        let guid = cast_guid(value, self.objects)?.to_string();
        Ok(LuaValue::String(self.lua.create_string(&guid)?))
    }

    fn process_array_like(
        &mut self,
        value: &LuaValue,
        r#type: &TypeHandle,
    ) -> Result<LuaValue, LuaValidationErrorKind> {
        match value {
            LuaValue::Table(table) => {
                let capacity = table.len()?;
                if capacity < 0 {
                    return Err(host_failure("capacity overflow").into());
                }
                let mut result = Vec::with_capacity(capacity.min(CAPACITY_BOUND) as usize);
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| host_failure("invalid bitmask type"))?;
                if r#type.primitive_type == PrimitiveType::StaticArray && table.len()? != r#type.field_count as i64 {
                    return Err(LuaValidationErrorKind::IncompatibleType {
                        expected: format!("array of length {}", r#type.field_count),
                        found: format!("array of length {}", table.len()?),
                    });
                }
                for (i, entry) in table.sequence_values::<LuaValue>().enumerate() {
                    self.path.push_index(i);
                    let value = self.process(&entry?, &inner)?;
                    result.push(value);
                    self.path.pop();
                }
                Ok(self.objects.new_array(&self.lua, r#type.clone(), result)?)
            }
            LuaValue::UserData(ud) => self.clone_typed(ud, r#type, ObjectKind::Array),
            _ => Err(incompatible(&name_of(r#type), value)?),
        }
    }

    /// A struct userdata: an owned/mapped struct of exactly this type is deep-cloned; a Content becomes the table
    /// `{size, hash0, hash1, hash2}` validated as this struct.
    fn clone_struct_ud(&mut self, ud: &AnyUserData, r#type: &TypeHandle) -> Result<LuaValue, LuaValidationErrorKind> {
        if let Some((ObjectKind::Struct, _)) = self.objects.classify(ud)? {
            return self.clone_typed(ud, r#type, ObjectKind::Struct);
        }
        if let Some(hash) = self.objects.content_hash(ud)? {
            let table = self.lua.create_table()?;
            table.set("size", hash.size())?;
            table.set("hash0", hash.hash0())?;
            table.set("hash1", hash.hash1())?;
            table.set("hash2", hash.hash2())?;
            return self.process_struct_fields(&table, r#type);
        }
        Err(LuaValidationErrorKind::IncompatibleType {
            expected: name_of(r#type),
            found: userdata_label(ud)?,
        })
    }

    fn clone_typed(
        &mut self,
        ud: &AnyUserData,
        r#type: &TypeHandle,
        kind: ObjectKind,
    ) -> Result<LuaValue, LuaValidationErrorKind> {
        match self.objects.classify(ud)? {
            Some((found, object_type)) if found == kind => {
                if *object_type != **r#type {
                    return Err(LuaValidationErrorKind::IncompatibleType {
                        expected: name_of(r#type),
                        found: name_of(&object_type),
                    });
                }
                Ok(self.objects.clone_object(ud, &self.lua)?)
            }
            _ => Err(LuaValidationErrorKind::IncompatibleType {
                expected: name_of(r#type),
                found: userdata_label(ud)?,
            }),
        }
    }
}

fn incompatible(expected: &str, value: &LuaValue) -> Result<LuaValidationErrorKind, LuaValidationErrorKind> {
    Ok(LuaValidationErrorKind::IncompatibleType {
        expected: expected.to_owned(),
        found: value.to_string()?,
    })
}

/// SF's `Guid` argument cast: a GUID string, a Content (its hash's GUID) or a Resource (its id's GUID).
pub fn cast_guid(value: &LuaValue, objects: &dyn TypedObjects) -> mlua::Result<Guid> {
    let guid = match value {
        LuaValue::String(s) => Guid::parse(&s.to_str()?),
        LuaValue::UserData(ud) => match objects.content_hash(ud)? {
            Some(hash) => Some(hash.into_guid()),
            None => objects.resource_guid(ud)?,
        },
        _ => None,
    };
    guid.ok_or_else(|| LuaError::type_mismatch("guid", value))
}
