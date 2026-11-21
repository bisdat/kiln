// SPDX-License-Identifier: GPL-3.0-or-later
//! The reader-driven validator: the codec's `validate_and_clone` over host objects. GPL drives the type walk and asks
//! the protected reader for each read Source makes, in Source's order and count; the result is an owned graph value.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs) through the codec's port:
//! modules/mod-runtime/lua/resource_values/validate_resource_values.rs:25-735 and the Guid cast of
//! lua/bindings/game/content_id_casts.rs:11-45.
//!
//! - A struct's fields: one GET_FIELD_VALUE each, in the type's field order (metamethods run once); a get that raises is
//!   `MissingField` and its error is not kept. Unknown keys are never read.
//! - A variant table: GET_FIELD_STRING `type`, then GET_FIELD_VALUE `value`; their errors are the `lua` kind with the host error.
//! - An array table: LENGTH as many times as Source calls `len` (once; a static array twice, three times when it does
//!   not fit), then RAW_SEQUENCE_INDEX from 1 until the first nil.
//! - An incompatible value's text is one SOURCE_TO_STRING of it (its `__tostring` runs once); a failing one is the error.
//! - A typed value of this arena is cloned as Source clones its wrapper (shared borrow); a Content userdata becomes the
//!   struct `{size, hash0, hash1, hash2}`; a Resource userdata gives its GUID.
//! - Where Source panics this is an error with Source's text; a reader transport failure is a Kiln fault that
//!   aborts the whole operation (the root `internal` level), never a `MissingField` or a nested Source level.
//!
//! Not exact yet (the frontend's precision row, with Source's `userdata \`TypeId(..)\`` label): the type name Source's
//! `guid expected, got ..` text prints for a userdata or a function/thread/lightuserdata value.
use std::{cell::RefCell, collections::HashMap, panic::AssertUnwindSafe, rc::Rc};

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{LookupKey, PrimitiveType as P},
};

use super::{
    arena::ArenaLocal,
    lifetime::{
        Access, Adapter, CheckKind, Fault, Found, HostError, HostKind, HostValue, Pin, Transport, UserdataKind,
    },
    node::{self, Body, Node, Slot, Val, clone_node, content_hash, resource_guid, val_text},
};
use crate::sf_startup::resource_values::{MAX_DEPTH, ObjectKind, TypeHandle, check_chain, name_of, path::TreePath};

/// Reserved room for an array's items; the real length is whatever the sequence yields.
const CAPACITY_BOUND: i64 = 4096;

/// What the type name of a value Source prints in `guid expected, got ..` is, where only the frontend knows it.
const FRONTEND_TYPE_NAME: &str = "<frontend type name>";

/// A value to validate: a host object (its reader reply) or a graph value.
#[derive(Clone)]
pub(crate) enum Src {
    Host(HostValue),
    Local(Val),
}

enum Seen {
    Nil,
    Bool(bool),
    Int(i64),
    Num(f64),
    Str(Rc<[u8]>),
    Table(u64),
    Typed(Rc<Node>),
    Known { adapter: Adapter, object: u64 },
    OtherUserdata,
    Other,
}

impl Seen {
    fn is_userdata(&self) -> bool {
        matches!(self, Seen::Typed(_) | Seen::Known { .. } | Seen::OtherUserdata)
    }

    /// Source's `userdata \`TypeId(..)\`` label, as the found userdata's class.
    fn label(&self) -> Found {
        match self {
            Seen::Typed(node) => Found::Userdata {
                kind: UserdataKind::TypedValue,
                adapter: Adapter::None,
                value_kind: Some(node.kind()),
            },
            Seen::Known { adapter, .. } => Found::Userdata {
                kind: UserdataKind::KnownUserdata,
                adapter: *adapter,
                value_kind: None,
            },
            _ => Found::Userdata {
                kind: UserdataKind::OtherUserdata,
                adapter: Adapter::None,
                value_kind: None,
            },
        }
    }

    /// mlua's `type_name` (sfi-core's `type_mismatch` text).
    fn type_name(&self) -> &'static str {
        match self {
            Seen::Nil => "nil",
            Seen::Bool(_) => "boolean",
            Seen::Int(_) => "integer",
            Seen::Num(_) => "number",
            Seen::Str(_) => "string",
            Seen::Table(_) => "table",
            _ => FRONTEND_TYPE_NAME,
        }
    }
}

enum Fields {
    Host(u64),
    Local(HashMap<&'static str, Val>),
}

fn lua(fault: Fault) -> CheckKind {
    CheckKind::Lua(Box::new(fault))
}

/// A reader transport failure: a Kiln fault.
pub(crate) fn transport(operation: &str, refused: Transport) -> Fault {
    Fault::internal(format!(
        "the protected reader refused {operation} (KilnResult {})",
        refused.0
    ))
}

/// A Lua error the reader returned: its text, and its object retained before the outermost call ends (absent when the
/// retain fails; the error is still reported).
pub(crate) fn host_error(local: &ArenaLocal, object: u64) -> Fault {
    let text = match local.host.copy(object) {
        Ok(text) => text,
        Err(refused) => return transport("copy of an error", refused),
    };
    let pin = Pin::retain(&*local.host, &local.sink, object).ok();
    Fault::Lua(HostError { pin, text })
}

/// The reader's describe reply for a value the caller passed.
pub(crate) fn describe(local: &ArenaLocal, object: u64) -> Result<Src, Fault> {
    let reply = local
        .host
        .access(&Access::Describe { object })
        .map_err(|refused| transport("DESCRIBE", refused))?;
    if reply.error_object != 0 {
        return Err(host_error(local, reply.error_object));
    }
    Ok(Src::Host(reply))
}

pub(crate) struct Validator<'a> {
    local: &'a ArenaLocal,
    path: TreePath,
    depth: usize,
}

impl<'a> Validator<'a> {
    /// SF `validate_and_clone_lua_value`: the value's owned, unified copy, or the validation error at its path.
    pub(crate) fn validate(local: &'a ArenaLocal, src: &Src, r#type: &TypeHandle) -> Result<Val, Fault> {
        let mut validator = Self {
            local,
            path: TreePath::new(),
            depth: 0,
        };
        // the parser's panics (kfc's `expect`s on a faulty registry) end here as errors at their path
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| validator.process(src, r#type)))
            .unwrap_or_else(|panic| Err(lua(Fault::runtime(node::panic_text(panic)))));
        result.map_err(|kind| {
            Fault::Validation {
                path: validator.path.to_string(),
                kind,
            }
            .lift_internal()
        })
    }

    fn access(&self, access: Access<'_>) -> Result<HostValue, CheckKind> {
        self.local
            .host
            .access(&access)
            .map_err(|refused| lua(transport(access.name(), refused)))
    }

    fn copy(&self, object: u64) -> Result<Vec<u8>, CheckKind> {
        self.local
            .host
            .copy(object)
            .map_err(|refused| lua(transport("copy", refused)))
    }

    fn raised(&self, reply: &HostValue) -> Result<(), CheckKind> {
        if reply.error_object != 0 {
            return Err(lua(host_error(self.local, reply.error_object)));
        }
        Ok(())
    }

    fn see(&self, src: &Src) -> Result<Seen, CheckKind> {
        Ok(match src {
            Src::Local(val) => match val {
                Val::Nil => Seen::Nil,
                Val::Bool(b) => Seen::Bool(*b),
                Val::Int(i) => Seen::Int(*i),
                Val::Num(n) => Seen::Num(*n),
                Val::Str(s) => Seen::Str(s.clone()),
                Val::Node(node) => match node.scalar_val() {
                    Some(val) => return self.see(&Src::Local(val.clone())),
                    None => Seen::Typed(node.clone()),
                },
            },
            Src::Host(reply) => match reply.kind {
                HostKind::Nil => Seen::Nil,
                HostKind::Boolean => Seen::Bool(reply.bits != 0),
                HostKind::Integer => Seen::Int(reply.bits as i64),
                HostKind::Number => Seen::Num(f64::from_bits(reply.bits)),
                HostKind::String => Seen::Str(self.copy(reply.object)?.into()),
                HostKind::Table => Seen::Table(reply.object),
                HostKind::TypedValue => match self.local.own_value(reply.typed_value) {
                    Some(node) => match node.scalar_val() {
                        Some(val) => return self.see(&Src::Local(val.clone())),
                        None => Seen::Typed(node),
                    },
                    None => {
                        return Err(lua(Fault::internal(
                            "the reader passed a typed value that is no value lease of this arena",
                        )));
                    }
                },
                HostKind::KnownUserdata => Seen::Known {
                    adapter: reply.adapter,
                    object: reply.object,
                },
                HostKind::OtherUserdata => Seen::OtherUserdata,
                HostKind::Other => Seen::Other,
            },
        })
    }

    /// Source's `value.to_string()`: one SOURCE_TO_STRING of a host object; a graph value's own text.
    fn to_string(&self, src: &Src) -> Result<Vec<u8>, CheckKind> {
        match src {
            Src::Host(reply) => {
                let text = self.access(Access::SourceToString { object: reply.object })?;
                self.raised(&text)?;
                self.copy(text.object)
            }
            Src::Local(val) => val_text(val, self.local, 0).map_err(lua),
        }
    }

    /// `expected .., got <to_string>`; a failing `to_string` is the error instead.
    fn incompatible(&self, expected: &str, src: &Src) -> CheckKind {
        match self.to_string(src) {
            Ok(found) => CheckKind::IncompatibleType {
                expected: expected.to_owned(),
                found: Found::Text(found),
            },
            Err(kind) => kind,
        }
    }

    /// One level of the walk: [`MAX_DEPTH`] levels at most.
    fn process(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        if self.depth >= MAX_DEPTH {
            return Err(lua(node::too_deep()));
        }
        self.depth += 1;
        let result = self.process_level(src, r#type);
        self.depth -= 1;
        result
    }

    fn process_level(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        match r#type.primitive_type {
            P::None => match self.see(src)? {
                Seen::Nil => Ok(Val::Nil),
                _ => Err(self.incompatible("nil", src)),
            },
            P::Bool => match self.see(src)? {
                Seen::Bool(b) => Ok(Val::Bool(b)),
                _ => Err(self.incompatible("boolean", src)),
            },
            P::UInt8 => self.uint(src, u8::MAX as u64),
            P::SInt8 => self.sint(src, i8::MIN as i64, i8::MAX as i64),
            P::UInt16 => self.uint(src, u16::MAX as u64),
            P::SInt16 => self.sint(src, i16::MIN as i64, i16::MAX as i64),
            P::UInt32 => self.uint(src, u32::MAX as u64),
            P::SInt32 => self.sint(src, i32::MIN as i64, i32::MAX as i64),
            P::UInt64 => self.uint(src, u64::MAX),
            P::SInt64 => self.sint(src, i64::MIN, i64::MAX),
            P::Float32 | P::Float64 => match self.see(src)? {
                Seen::Int(i) => Ok(Val::Num(i as f64)),
                Seen::Num(n) => Ok(Val::Num(n)),
                _ => Err(self.incompatible("float", src)),
            },
            P::Enum => self.enumeration(src, r#type),
            P::Bitmask8 | P::Bitmask16 | P::Bitmask32 | P::Bitmask64 | P::StaticArray | P::BlobArray => {
                self.array_like(src, r#type)
            }
            P::Typedef => {
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| lua(Fault::runtime("invalid typedef type")))?;
                self.process(src, &inner)
            }
            P::Struct => self.structure(src, r#type),
            P::DsArray | P::DsString | P::DsOptional | P::DsVariant => Err(lua(Fault::runtime("not yet implemented"))),
            P::BlobString => match self.see(src)? {
                Seen::Str(s) => Ok(Val::Str(s)),
                _ => Err(self.incompatible("string", src)),
            },
            P::BlobOptional => {
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| lua(Fault::runtime("invalid optional type")))?;
                match self.see(src)? {
                    Seen::Nil => Ok(Val::Nil),
                    _ => self.process(src, &inner),
                }
            }
            P::BlobVariant => self.variant(src, r#type),
            P::ObjectReference => self.object_reference(src),
            P::Guid => self.guid(src),
        }
    }

    fn uint(&mut self, src: &Src, max: u64) -> Result<Val, CheckKind> {
        match self.see(src)? {
            // the Integer's bits as u64: -1 is u64::MAX, as SF reads it
            Seen::Int(i) if i as u64 <= max => Ok(Val::Int(i)),
            Seen::Num(n) if n.fract() == 0.0 && n >= 0.0 && n <= max as f64 => Ok(Val::Int(n as i64)),
            _ => Err(self.incompatible(&format!("unsigned integer in range [0, {max}]"), src)),
        }
    }

    fn sint(&mut self, src: &Src, min: i64, max: i64) -> Result<Val, CheckKind> {
        match self.see(src)? {
            Seen::Int(i) if i >= min && i <= max => Ok(Val::Int(i)),
            Seen::Num(n) if n.fract() == 0.0 && n >= min as f64 && n <= max as f64 => Ok(Val::Int(n as i64)),
            _ => Err(self.incompatible(&format!("signed integer in range [{min}, {max}]"), src)),
        }
    }

    fn enumeration(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        match self.see(src)? {
            Seen::Str(s) => {
                if r#type.enum_fields.get(&*String::from_utf8_lossy(&s)).is_some() {
                    return Ok(Val::Str(s));
                }
            }
            Seen::Int(i) => {
                let i = i as u64;
                if let Some(field) = r#type.enum_fields.values().find(|f| f.value == i) {
                    return Ok(Val::str(&field.name));
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
        Err(self.incompatible(&format!("one of [{names}]"), src))
    }

    fn structure(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        let seen = self.see(src)?;
        match seen {
            Seen::Table(object) => self.struct_fields(Fields::Host(object), r#type),
            ref ud if ud.is_userdata() => self.struct_userdata(ud, r#type),
            _ => Err(self.incompatible(&name_of(r#type), src)),
        }
    }

    fn struct_fields(&mut self, fields: Fields, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        check_chain(r#type).map_err(|e| lua(e.into()))?;
        let mut values = HashMap::new();
        for field in r#type.iter_fields() {
            let field_type = r#type
                .get_field_type(&field.name)
                .ok_or_else(|| lua(Fault::runtime("invalid field type")))?;
            let value = match &fields {
                Fields::Host(object) => {
                    let reply = self.access(Access::GetFieldValue {
                        object: *object,
                        key: field.name.as_bytes(),
                    })?;
                    if reply.error_object != 0 {
                        // Source discards the get's error (it is not retained either)
                        return Err(CheckKind::MissingField {
                            field: field.name.clone(),
                            r#type: name_of(r#type),
                        });
                    }
                    Src::Host(reply)
                }
                Fields::Local(map) => Src::Local(map.get(field.name.as_str()).cloned().unwrap_or(Val::Nil)),
            };
            self.path.push(&field.name);
            let value = self.process(&value, &field_type)?;
            values.insert(field.name.clone(), Slot::new(value));
            self.path.pop();
        }
        Ok(Val::Node(Node::new(
            self.local,
            Body::Struct {
                ty: r#type.clone(),
                fields: RefCell::new(values),
            },
        )))
    }

    /// A struct userdata: a typed struct of exactly this type is deep-cloned; a Content becomes the table
    /// `{size, hash0, hash1, hash2}` validated as this struct.
    fn struct_userdata(&mut self, seen: &Seen, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        if let Seen::Typed(node) = seen
            && matches!(node.classify(), Some((ObjectKind::Struct, _)))
        {
            return self.clone_typed(seen, r#type, ObjectKind::Struct);
        }
        if let Some(hash) = self.content(seen)? {
            let fields = HashMap::from([
                ("size", Val::Int(hash.size() as i64)),
                ("hash0", Val::Int(hash.hash0() as i64)),
                ("hash1", Val::Int(hash.hash1() as i64)),
                ("hash2", Val::Int(hash.hash2() as i64)),
            ]);
            return self.struct_fields(Fields::Local(fields), r#type);
        }
        Err(CheckKind::IncompatibleType {
            expected: name_of(r#type),
            found: seen.label(),
        })
    }

    fn variant(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        let seen = self.see(src)?;
        match seen {
            Seen::Table(object) => {
                self.path.push("type");
                let reply = self.access(Access::GetFieldString { object, key: b"type" })?;
                self.raised(&reply)?;
                let type_name = String::from_utf8_lossy(&self.copy(reply.object)?).into_owned();
                let registry = r#type.type_registry();
                let variant = registry
                    .get_by_name(LookupKey::Qualified(&type_name))
                    .ok_or_else(|| CheckKind::InvalidType(type_name.clone()))?;
                let variant_type = TypeHandle::new(registry.clone(), variant.index);
                check_chain(&variant_type).map_err(|e| lua(e.into()))?;
                let base = r#type
                    .inner_type()
                    .ok_or_else(|| lua(Fault::runtime("invalid variant type")))?;
                if !variant_type.is_sub_type_of(&base) {
                    return Err(CheckKind::IncompatibleType {
                        expected: name_of(&base),
                        found: Found::Text(name_of(&variant_type).into_bytes()),
                    });
                }
                self.path.pop();
                self.path.push("value");
                let reply = self.access(Access::GetFieldValue { object, key: b"value" })?;
                self.raised(&reply)?;
                // Source calls process_struct directly here: no depth level of its own
                let value = self.structure(&Src::Host(reply), &variant_type)?;
                self.path.pop();
                Ok(Val::Node(Node::new(
                    self.local,
                    Body::Variant {
                        ty: r#type.clone(),
                        variant: variant_type,
                        value: RefCell::new(Slot::new(value)),
                    },
                )))
            }
            ref ud if ud.is_userdata() => self.clone_typed(ud, r#type, ObjectKind::Variant),
            _ => Err(self.incompatible(&name_of(r#type), src)),
        }
    }

    fn length(&self, object: u64) -> Result<i64, CheckKind> {
        let reply = self.access(Access::Length { object })?;
        self.raised(&reply)?;
        match reply.kind {
            HostKind::Integer => Ok(reply.bits as i64),
            _ => Err(lua(Fault::internal("the reader's LENGTH reply is no integer"))),
        }
    }

    fn array_like(&mut self, src: &Src, r#type: &TypeHandle) -> Result<Val, CheckKind> {
        let seen = self.see(src)?;
        match seen {
            Seen::Table(object) => {
                let capacity = self.length(object)?;
                if capacity < 0 {
                    return Err(lua(Fault::runtime("capacity overflow")));
                }
                let mut items = Vec::with_capacity(capacity.min(CAPACITY_BOUND) as usize);
                let inner = r#type
                    .inner_type()
                    .ok_or_else(|| lua(Fault::runtime("invalid bitmask type")))?;
                if r#type.primitive_type == P::StaticArray && self.length(object)? != r#type.field_count as i64 {
                    let found = self.length(object)?;
                    return Err(CheckKind::IncompatibleType {
                        expected: format!("array of length {}", r#type.field_count),
                        found: Found::Text(format!("array of length {found}").into_bytes()),
                    });
                }
                for index in 1i64.. {
                    let reply = self.access(Access::RawSequenceIndex { object, index })?;
                    // The codec pushes the entry's index before the entry's own error (`sequence_values` yields it)
                    self.path.push_index((index - 1) as usize);
                    self.raised(&reply)?;
                    if reply.kind == HostKind::Nil {
                        self.path.pop();
                        break;
                    }
                    let value = self.process(&Src::Host(reply), &inner)?;
                    items.push(Slot::new(value));
                    self.path.pop();
                }
                Ok(Val::Node(Node::new(
                    self.local,
                    Body::Array {
                        ty: r#type.clone(),
                        inner,
                        items: RefCell::new(items),
                    },
                )))
            }
            ref ud if ud.is_userdata() => self.clone_typed(ud, r#type, ObjectKind::Array),
            _ => Err(self.incompatible(&name_of(r#type), src)),
        }
    }

    fn clone_typed(&mut self, seen: &Seen, r#type: &TypeHandle, kind: ObjectKind) -> Result<Val, CheckKind> {
        if let Seen::Typed(node) = seen
            && let Some((found, object_type)) = node.classify()
            && found == kind
        {
            if **object_type != **r#type {
                return Err(CheckKind::IncompatibleType {
                    expected: name_of(r#type),
                    found: Found::Text(name_of(object_type).into_bytes()),
                });
            }
            return Ok(Val::Node(clone_node(node, self.local, 0).map_err(lua)?));
        }
        Err(CheckKind::IncompatibleType {
            expected: name_of(r#type),
            found: seen.label(),
        })
    }

    fn content(&self, seen: &Seen) -> Result<Option<ContentHash>, CheckKind> {
        match seen {
            Seen::Known {
                adapter: Adapter::Content,
                object,
            } => {
                let record = self.copy(*object)?;
                content_hash(&record)
                    .map(Some)
                    .ok_or_else(|| lua(Fault::internal("the reader's Content record is not 16 bytes")))
            }
            _ => Ok(None),
        }
    }

    fn resource(&self, seen: &Seen) -> Result<Option<Guid>, CheckKind> {
        match seen {
            Seen::Known {
                adapter: Adapter::Resource,
                object,
            } => {
                let record = self.copy(*object)?;
                resource_guid(&record)
                    .map(Some)
                    .ok_or_else(|| lua(Fault::internal("the reader's Resource record is not 24 bytes")))
            }
            _ => Ok(None),
        }
    }

    fn object_reference(&mut self, src: &Src) -> Result<Val, CheckKind> {
        let seen = self.see(src)?;
        match &seen {
            Seen::Str(s) => {
                let text = String::from_utf8_lossy(s).into_owned();
                if ContentHash::parse(&text).is_none() {
                    return Err(CheckKind::InvalidGuid(text));
                }
                Ok(Val::Str(s.clone()))
            }
            ud if ud.is_userdata() => match self.resource(ud)? {
                // ResourceId's text is its GUID's
                Some(guid) => Ok(Val::str(&guid.to_string())),
                None => Err(CheckKind::IncompatibleType {
                    expected: "object reference".into(),
                    found: ud.label(),
                }),
            },
            _ => Err(self.incompatible("object reference", src)),
        }
    }

    /// SF's `Guid` argument cast: a GUID string, a Content (its hash's GUID) or a Resource (its id's GUID).
    fn guid(&mut self, src: &Src) -> Result<Val, CheckKind> {
        let seen = self.see(src)?;
        let guid = match &seen {
            Seen::Str(s) => {
                let text = std::str::from_utf8(s)
                    .map_err(|e| lua(Fault::external(format!("error converting Lua string to &str ({e})"))))?;
                Guid::parse(text)
            }
            ud if ud.is_userdata() => match self.content(ud)? {
                Some(hash) => Some(hash.into_guid()),
                None => self.resource(ud)?,
            },
            _ => None,
        };
        match guid {
            Some(guid) => Ok(Val::str(&guid.to_string())),
            None => Err(lua(Fault::external(format!("guid expected, got {}", seen.type_name())))),
        }
    }
}
