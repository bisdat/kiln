// SPDX-License-Identifier: GPL-3.0-or-later
//! Validated Lua values to kfc `Value`s (encode side), and mapped kfc values back to Lua (decode side).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/resource_values/convert_resource_values.rs:23-560;
//! - the container walks of owned_values.rs:46-65 (struct), 253-272 (array) and 512-523 (variant), here as
//!   `Converter::{struct_fields, array_items, variant}` so the value wrappers convert through the same code.
//!
//! `MappedWalk` follows EML's own kfc-resource value/read.rs `Value::from_impl` (GPL-3.0-or-later, Brabb3l, as
//! the rest of eml/), bounded.
//!
//! Same scalar ranges and casts, texts, typedef unwrapping and path. Where SF panics (`todo!()` on the four DS kinds, an
//! `expect` on a missing inner/field type, a userdata without a type id) this raises a Lua error with SF's panic text
//! instead (a recorded parity difference): a script that pcalls sees a failure either way.
use indexmap_free::OrderedFields;
use kfc::{
    guid::Guid,
    reflection::PrimitiveType,
    resource::{
        mapped::MappingError,
        value::{Value, Variant},
    },
};
use mlua::AnyUserData;
use sfi_core::lua::LuaValue;
use std::fmt;

use super::{
    MAX_DEPTH, MappedValue, ObjectKind, TypeHandle, TypedObjects, check_chain, name_of, path::TreePath, too_deep,
};

/// SF's `#[error("at {path}: {kind}")]` (thiserror there; written out here, kiln-eml has no thiserror).
#[derive(Debug)]
pub struct LuaConversionError {
    pub path: String,
    pub kind: LuaConversionErrorKind,
}

#[derive(Debug)]
pub enum LuaConversionErrorKind {
    IncompatibleType { expected: String, found: String },
    InvalidGuid(String),
    Lua(mlua::Error),
    Mapping(MappingError),
}

impl fmt::Display for LuaConversionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "at {}: {}", self.path, self.kind)
    }
}
impl std::error::Error for LuaConversionError {}

impl fmt::Display for LuaConversionErrorKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleType { expected, found } => write!(f, "expected `{expected}` but found `{found}`"),
            Self::InvalidGuid(guid) => write!(f, "invalid guid `{guid}`"),
            Self::Lua(error) => write!(f, "{error}"),
            Self::Mapping(error) => write!(f, "{error}"),
        }
    }
}
impl std::error::Error for LuaConversionErrorKind {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Lua(error) => Some(error),
            Self::Mapping(error) => Some(error),
            _ => None,
        }
    }
}
impl From<mlua::Error> for LuaConversionErrorKind {
    fn from(error: mlua::Error) -> Self {
        Self::Lua(error)
    }
}
impl From<MappingError> for LuaConversionErrorKind {
    fn from(error: MappingError) -> Self {
        Self::Mapping(error)
    }
}

/// SF's panic texts, raised as Lua errors.
pub(super) fn host_failure(text: &str) -> mlua::Error {
    mlua::Error::runtime(text)
}

/// A caught panic's message.
pub(super) fn panic_text(panic: Box<dyn std::any::Any + Send>) -> String {
    panic
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| panic.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "no message".into())
}

pub struct Converter<'a> {
    pub path: TreePath,
    objects: &'a dyn TypedObjects,
    depth: usize,
}

impl<'a> Converter<'a> {
    pub fn convert(
        value: &LuaValue,
        r#type: &TypeHandle,
        objects: &'a dyn TypedObjects,
    ) -> Result<Value, LuaConversionError> {
        let mut converter = Self {
            path: TreePath::new(),
            objects,
            depth: 0,
        };
        // SF's own and the parser's panics (kfc's `expect`s on a faulty registry) end here as errors at their path
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| converter.process(value, r#type)))
            .unwrap_or_else(|panic| Err(host_failure(&panic_text(panic)).into()));
        result.map_err(|kind| LuaConversionError {
            path: converter.path.to_string(),
            kind,
        })
    }

    /// One level of the walk: [`MAX_DEPTH`] levels at most (wrappers linked into a cycle end here too).
    pub fn process(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<Value, LuaConversionErrorKind> {
        if self.depth >= MAX_DEPTH {
            return Err(too_deep().into());
        }
        self.depth += 1;
        let result = self.process_level(value, r#type);
        self.depth -= 1;
        result
    }

    fn process_level(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<Value, LuaConversionErrorKind> {
        Ok(match r#type.primitive_type {
            PrimitiveType::None => self.convert_nil(value)?,
            PrimitiveType::Bool => self.convert_bool(value)?.into(),
            PrimitiveType::UInt8 => self.convert_uint(value, 0, u8::MAX as u64)?.into(),
            PrimitiveType::SInt8 => self.convert_sint(value, i8::MIN as i64, i8::MAX as i64)?.into(),
            PrimitiveType::UInt16 => self.convert_uint(value, 0, u16::MAX as u64)?.into(),
            PrimitiveType::SInt16 => self.convert_sint(value, i16::MIN as i64, i16::MAX as i64)?.into(),
            PrimitiveType::UInt32 => self.convert_uint(value, 0, u32::MAX as u64)?.into(),
            PrimitiveType::SInt32 => self.convert_sint(value, i32::MIN as i64, i32::MAX as i64)?.into(),
            PrimitiveType::UInt64 => self.convert_uint(value, 0, u64::MAX)?.into(),
            PrimitiveType::SInt64 => self.convert_sint(value, i64::MIN, i64::MAX)?.into(),
            PrimitiveType::Float32 | PrimitiveType::Float64 => self.convert_float(value)?.into(),
            PrimitiveType::Enum => self.convert_enum(value, r#type)?,
            PrimitiveType::Bitmask8
            | PrimitiveType::Bitmask16
            | PrimitiveType::Bitmask32
            | PrimitiveType::Bitmask64 => self.convert_container(value, r#type, ObjectKind::Array)?,
            PrimitiveType::Typedef => {
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| host_failure("invalid typedef type"))?;
                self.process(value, &inner)?
            }
            PrimitiveType::Struct => self.convert_container(value, r#type, ObjectKind::Struct)?,
            PrimitiveType::StaticArray | PrimitiveType::BlobArray => {
                self.convert_container(value, r#type, ObjectKind::Array)?
            }
            PrimitiveType::DsArray | PrimitiveType::DsString | PrimitiveType::DsOptional | PrimitiveType::DsVariant => {
                return Err(host_failure("not yet implemented").into());
            }
            PrimitiveType::BlobString => match value {
                LuaValue::String(s) => Value::String(s.to_string_lossy()),
                _ => return Err(incompatible("string", value)?),
            },
            PrimitiveType::BlobOptional => match value {
                LuaValue::Nil => Value::None,
                _ => {
                    let inner = r#type
                        .inner_type()
                        .ok_or_else(|| host_failure("invalid optional type"))?;
                    self.process(value, &inner)?
                }
            },
            PrimitiveType::BlobVariant => self.convert_container(value, r#type, ObjectKind::Variant)?,
            PrimitiveType::ObjectReference => self.convert_guid(value, "object reference")?,
            PrimitiveType::Guid => self.convert_guid(value, "guid")?,
        })
    }

    fn convert_nil(&mut self, value: &LuaValue) -> Result<Value, LuaConversionErrorKind> {
        match value {
            LuaValue::Nil => Ok(Value::None),
            _ => Err(incompatible("nil", value)?),
        }
    }

    fn convert_bool(&mut self, value: &LuaValue) -> Result<bool, LuaConversionErrorKind> {
        match value {
            LuaValue::Boolean(b) => Ok(*b),
            _ => Err(incompatible("boolean", value)?),
        }
    }

    fn convert_uint(&mut self, value: &LuaValue, min: u64, max: u64) -> Result<u64, LuaConversionErrorKind> {
        match value {
            // the Integer's bits as u64: -1 is u64::MAX, as SF reads it
            LuaValue::Integer(i) if *i as u64 >= min && *i as u64 <= max => Ok(*i as u64),
            LuaValue::Number(n) if n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64 => Ok(*n as u64),
            _ => Err(incompatible(
                &format!("unsigned integer in range [{min}, {max}]"),
                value,
            )?),
        }
    }

    fn convert_sint(&mut self, value: &LuaValue, min: i64, max: i64) -> Result<i64, LuaConversionErrorKind> {
        match value {
            LuaValue::Integer(i) if *i >= min && *i <= max => Ok(*i),
            LuaValue::Number(n) if n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64 => Ok(*n as i64),
            _ => Err(incompatible(&format!("signed integer in range [{min}, {max}]"), value)?),
        }
    }

    fn convert_float(&mut self, value: &LuaValue) -> Result<f64, LuaConversionErrorKind> {
        match value {
            LuaValue::Integer(i) => Ok(*i as f64),
            LuaValue::Number(n) => Ok(*n),
            _ => Err(incompatible("float", value)?),
        }
    }

    fn convert_enum(&mut self, value: &LuaValue, r#type: &TypeHandle) -> Result<Value, LuaConversionErrorKind> {
        match value {
            LuaValue::String(s) => {
                if let Some(field) = r#type.enum_fields.get(&s.to_string_lossy()) {
                    return Ok(Value::String(field.name.clone()));
                }
            }
            LuaValue::Integer(i) => {
                let i = *i as u64;
                if let Some(field) = r#type.enum_fields.values().find(|f| f.value == i) {
                    return Ok(Value::String(field.name.clone()));
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

    fn convert_guid(&mut self, value: &LuaValue, expected: &str) -> Result<Value, LuaConversionErrorKind> {
        match value {
            LuaValue::String(s) => {
                let text = s.to_string_lossy();
                let guid = Guid::parse(&text).ok_or(LuaConversionErrorKind::InvalidGuid(text))?;
                Ok(Value::Guid(guid))
            }
            _ => Err(incompatible(expected, value)?),
        }
    }

    /// Struct, array/bitmask and variant values: only the typed userdata of the matching class converts.
    fn convert_container(
        &mut self,
        value: &LuaValue,
        r#type: &TypeHandle,
        kind: ObjectKind,
    ) -> Result<Value, LuaConversionErrorKind> {
        let LuaValue::UserData(ud) = value else {
            return Err(incompatible(&name_of(r#type), value)?);
        };
        let objects = self.objects;
        match objects.classify(ud)? {
            Some((found, object_type)) if found == kind => {
                // TypeMetadata equality: the same index and qualified hash
                if *object_type != **r#type {
                    return Err(LuaConversionErrorKind::IncompatibleType {
                        expected: name_of(r#type),
                        found: name_of(&object_type),
                    });
                }
                objects.convert_object(ud, self)
            }
            _ => Err(LuaConversionErrorKind::IncompatibleType {
                expected: name_of(r#type),
                found: userdata_label(ud)?,
            }),
        }
    }

    /// An owned struct's fields, in the order given (SF: its field map's own order), each by its field type.
    pub fn struct_fields<'v>(
        &mut self,
        r#type: &TypeHandle,
        fields: impl IntoIterator<Item = (&'v str, &'v LuaValue)>,
    ) -> Result<Value, LuaConversionErrorKind> {
        check_chain(r#type)?;
        let mut result = OrderedFields::new();
        for (key, value) in fields {
            self.path.push(key);
            let field_type = r#type
                .get_field_type(key)
                .ok_or_else(|| host_failure("field type must be present"))?;
            let converted = self.process(value, &field_type)?;
            result.insert(key.to_owned(), converted);
            self.path.pop();
        }
        Ok(result.into_value())
    }

    /// An owned array's items, each by the array's element type; the path names zero-based indices.
    pub fn array_items<'v>(
        &mut self,
        r#type: &TypeHandle,
        items: impl IntoIterator<Item = &'v LuaValue>,
    ) -> Result<Value, LuaConversionErrorKind> {
        let element = r#type
            .inner_type()
            .ok_or_else(|| host_failure("array type must have an inner type"))?;
        let mut result = Vec::new();
        for (i, value) in items.into_iter().enumerate() {
            self.path.push_index(i);
            result.push(self.process(value, &element)?);
            self.path.pop();
        }
        Ok(result.into())
    }

    /// An owned variant: its value converted as the selected variant type, which must give a struct.
    pub fn variant(&mut self, variant_type: &TypeHandle, value: &LuaValue) -> Result<Value, LuaConversionErrorKind> {
        let value = self
            .process(value, variant_type)?
            .into_struct()
            .ok_or_else(|| host_failure("variant value must be convertible to struct"))?;
        Ok(Variant {
            type_index: variant_type.index(),
            value,
        }
        .into())
    }

    /// An unread part of a decoded value as a kfc value, where SF calls kfc's `Value::from` (the wrappers' `convert`
    /// of fields and elements nobody read, and of an unread variant). The same result as kfc's default (compact)
    /// conversion, but bounded: kfc recurses without a limit and reserves an array's stored count up
    /// front, so crafted bytes of a recursive type (a node whose child is itself) overflowed the worker's stack, a
    /// count of 2^32 asked for hundreds of GiB, and children that share their grandchildren expanded exponentially.
    /// Here the walk continues this converter's depth ([`MAX_DEPTH`]) and stops after [`mapped_value_limit`] values
    /// of `source_len`, the length of the bytes the value was decoded from.
    pub fn mapped(&mut self, value: MappedValue, source_len: usize) -> Result<Value, LuaConversionErrorKind> {
        MappedWalk {
            depth: self.depth,
            values: 0,
            limit: mapped_value_limit(source_len),
        }
        .value(value)
    }
}

/// [`Converter::mapped`] from depth 0, outside a conversion.
pub fn mapped_value(value: MappedValue, source_len: usize) -> Result<Value, LuaConversionErrorKind> {
    MappedWalk {
        depth: 0,
        values: 0,
        limit: mapped_value_limit(source_len),
    }
    .value(value)
}

/// The most kfc values one unread decoded subtree may expand to: eight per byte of its source plus 64 Ki. A value that
/// does not share bytes with itself has at most about one value per byte (each scalar has at least one byte of its own;
/// only structs nested at the same offset, empty structs and `None`s have none), so real data stays far below this;
/// only bytes that point back into themselves reach it.
pub const fn mapped_value_limit(source_len: usize) -> usize {
    source_len.saturating_mul(8).saturating_add(1 << 16)
}

/// Room an unread array's walk reserves up front at most; the real length is whatever its elements yield (a stored
/// count is not trusted: kfc reserved it whole; kfc now also refuses a count its bytes cannot hold, eml-array-count).
const RESERVE_BOUND: usize = 4096;

/// kfc-resource value/read.rs `Value::from_impl` with `ConversionOptions::COMPACT` (enums and bitmasks as their
/// values, variants as `Variant`, GUIDs as GUIDs, a nil GUID as `None`), counting depth and values.
///
/// kfc itself recurses or loops where the walk's counters cannot see it, on a faulty registry: it lists a struct's
/// fields by walking its base chain (a cycle there never ends) and maps an optional or a typedef field eagerly through
/// its inner chain (an optional of itself recurses once per offset in the bytes, until the stack overflows). So every
/// struct, field and array type is checked with `check_chain` before kfc lists or maps it, as the value wrappers check
/// before each lazy read.
struct MappedWalk {
    depth: usize,
    values: usize,
    limit: usize,
}

impl MappedWalk {
    fn value(&mut self, value: MappedValue) -> Result<Value, LuaConversionErrorKind> {
        self.values += 1;
        if self.values > self.limit {
            return Err(host_failure(&format!("unread value expands to more than {} values", self.limit)).into());
        }
        Ok(match value {
            MappedValue::None => Value::None,
            MappedValue::Bool(b) => Value::Bool(b),
            MappedValue::UInt8(v) => Value::UInt(v.into()),
            MappedValue::SInt8(v) => Value::SInt(v.into()),
            MappedValue::UInt16(v) => Value::UInt(v.into()),
            MappedValue::SInt16(v) => Value::SInt(v.into()),
            MappedValue::UInt32(v) => Value::UInt(v.into()),
            MappedValue::SInt32(v) => Value::SInt(v.into()),
            MappedValue::UInt64(v) => Value::UInt(v),
            MappedValue::SInt64(v) => Value::SInt(v),
            MappedValue::Float32(v) => Value::Float(v.into()),
            MappedValue::Float64(v) => Value::Float(v),
            MappedValue::Enum(v) => Value::UInt(v.value()),
            MappedValue::Bitmask(v) => Value::UInt(v.value()),
            MappedValue::Struct(s) => self.nested(|walk| walk.structure(s))?,
            MappedValue::Array(array) => self.nested(|walk| {
                // the array's inner chain is its element type's: kfc maps every element through it
                check_chain(array.r#type())?;
                // kfc reserves the stored count whole; here a bounded start, grown by the elements that exist
                let mut items = Vec::with_capacity(array.len().min(walk.limit - walk.values).min(RESERVE_BOUND));
                for item in array.iter() {
                    items.push(walk.value(item?)?);
                }
                Ok(Value::Array(items))
            })?,
            MappedValue::String(s) => Value::String(s.as_str()?.to_string()),
            MappedValue::Optional(optional) => match optional.into_value() {
                Some(inner) => self.nested(|walk| walk.value(inner))?,
                None => Value::None,
            },
            MappedValue::Variant(variant) => self.nested(|walk| {
                let value = variant.into_value();
                let type_index = value.r#type().index;
                let value = walk
                    .structure(value)?
                    .into_struct()
                    .ok_or_else(|| host_failure("Expected variant value to be a struct"))?;
                Ok(Variant { type_index, value }.into())
            })?,
            MappedValue::Guid(guid) => Self::guid(guid),
            MappedValue::Reference(reference) => Self::guid(reference.into_guid()),
        })
    }

    fn nested(
        &mut self,
        walk: impl FnOnce(&mut Self) -> Result<Value, LuaConversionErrorKind>,
    ) -> Result<Value, LuaConversionErrorKind> {
        if self.depth >= MAX_DEPTH {
            return Err(too_deep().into());
        }
        self.depth += 1;
        let result = walk(self);
        self.depth -= 1;
        result
    }

    fn structure(&mut self, value: super::mapped::MappedStruct) -> Result<Value, LuaConversionErrorKind> {
        // the base chain kfc walks to list the fields, then each field's chain kfc maps it through (a variant's value
        // type comes from the bytes, so this is its first check)
        check_chain(value.r#type())?;
        for field in value.r#type().iter_fields() {
            if let Some(field_type) = TypeHandle::try_new(value.r#type().type_registry().clone(), field.r#type) {
                check_chain(&field_type)?;
            }
        }
        let mut fields = OrderedFields::new();
        for field in value.iter() {
            let (name, field) = field?;
            let name = name.to_owned();
            fields.insert(name, self.value(field)?);
        }
        Ok(fields.into_value())
    }

    fn guid(guid: Guid) -> Value {
        if guid.is_none() { Value::None } else { Value::Guid(guid) }
    }
}

/// `expected ... but found <tostring>`; a failing `tostring` (a throwing __tostring) is the error instead.
fn incompatible(expected: &str, value: &LuaValue) -> Result<LuaConversionErrorKind, LuaConversionErrorKind> {
    Ok(LuaConversionErrorKind::IncompatibleType {
        expected: expected.to_owned(),
        found: value.to_string()?,
    })
}

/// SF's label for a userdata of no expected class: its Rust type id.
pub(super) fn userdata_label(ud: &AnyUserData) -> mlua::Result<String> {
    match ud.type_id() {
        Some(type_id) => Ok(format!("userdata `{type_id:?}`")),
        None => Err(host_failure("UserData has no type_id")),
    }
}

/// Decoded (mapped) values to Lua: scalars here, containers through the typed-object seam.
pub fn value_to_lua(value: &MappedValue, lua: &mlua::Lua, objects: &dyn TypedObjects) -> mlua::Result<LuaValue> {
    Ok(match value {
        MappedValue::None => LuaValue::Nil,
        MappedValue::Bool(b) => LuaValue::Boolean(*b),
        MappedValue::UInt8(v) => LuaValue::Integer(*v as i64),
        MappedValue::SInt8(v) => LuaValue::Integer(*v as i64),
        MappedValue::UInt16(v) => LuaValue::Integer(*v as i64),
        MappedValue::SInt16(v) => LuaValue::Integer(*v as i64),
        MappedValue::UInt32(v) => LuaValue::Integer(*v as i64),
        MappedValue::SInt32(v) => LuaValue::Integer(*v as i64),
        // the bits: values above i64::MAX read negative
        MappedValue::UInt64(v) => LuaValue::Integer(*v as i64),
        MappedValue::SInt64(v) => LuaValue::Integer(*v),
        MappedValue::Float32(v) => LuaValue::Number(*v as f64),
        MappedValue::Float64(v) => LuaValue::Number(*v),
        MappedValue::Enum(v) => LuaValue::String(match v.name() {
            Some(name) => lua.create_string(name)?,
            None => lua.create_string(v.value().to_string())?,
        }),
        MappedValue::Bitmask(v) => {
            let bits = v
                .iter()
                .map(|bit| match bit.name() {
                    Some(name) => lua.create_string(name).map(LuaValue::String),
                    None => lua.create_string(bit.value().to_string()).map(LuaValue::String),
                })
                .collect::<mlua::Result<Vec<_>>>()?;
            objects.new_array(lua, v.r#type().clone(), bits)?
        }
        MappedValue::Struct(_) | MappedValue::Array(_) | MappedValue::Variant(_) => objects.wrap_mapped(lua, value)?,
        MappedValue::String(s) => LuaValue::String(lua.create_string(s.as_str().map_err(mlua::Error::external)?)?),
        MappedValue::Optional(v) => match v.value() {
            Some(inner) => value_to_lua(inner, lua, objects)?,
            None => LuaValue::Nil,
        },
        MappedValue::Guid(guid) => LuaValue::String(lua.create_string(guid.to_string())?),
        MappedValue::Reference(reference) => LuaValue::String(lua.create_string(reference.guid().to_string())?),
    })
}

/// kfc's struct map without naming its map crate here (kfc's `Value::Struct` is `From` an IndexMap that kfc-resource
/// re-exports only through `Value`).
mod indexmap_free {
    use kfc::resource::value::Value;

    pub struct OrderedFields(Vec<(String, Value)>);
    impl OrderedFields {
        pub fn new() -> Self {
            Self(Vec::new())
        }
        pub fn insert(&mut self, key: String, value: Value) {
            match self.0.iter_mut().find(|(k, _)| *k == key) {
                Some(slot) => slot.1 = value,
                None => self.0.push((key, value)),
            }
        }
        pub fn into_value(self) -> Value {
            Value::Struct(Box::new(self.0.into_iter().collect()))
        }
    }
}
