// SPDX-License-Identifier: GPL-3.0-or-later
//! The typed value codec of the SF-dialect startup pass: validation of Lua values against reflected types,
//! conversion to kfc values and resource bytes, and decoding back to Lua.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/resource_values/{mod,validate_resource_values,convert_resource_values,value_path}.rs, the
//! container walks of owned_values.rs, lua/bindings/game/content_id_casts.rs (the Guid cast) and
//! lua/values/encode_owned_value.rs (the validate, convert, serialize sequence). Identical at both pins.
//! The wrappers' `owned`, `mapped` and `objects` adapt resource_values/{owned_values,mapped_values,mod}.rs (each file's header
//! names its lines).
//!
//! # The typed-object seam
//! SF's codec names its six value wrappers (owned and mapped struct/array/variant) and the asset userdata (Resource,
//! Content) directly. Those belong to later builders here (the value wrappers, the asset session's userdata), so the codec
//! reaches them only through [`TypedObjects`]: the value wrappers implement it over the real wrappers and the pass hands that one
//! implementation to every call. The wrappers' production implementation is `objects::ValueObjects` over the wrappers in
//! `owned` and `mapped`; its Resource/Content answers wait for the asset session's userdata (`objects::AssetUserdata`).
use std::{collections::HashMap, panic::AssertUnwindSafe, rc::Rc};

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{PrimitiveType, TypeMetadata, TypeRegistry},
    resource::value::Value,
};
use mlua::AnyUserData;
use sfi_core::lua::LuaValue;

pub mod converter;
// SF's six value wrappers and the production seam behind them.
pub mod mapped;
pub mod objects;
pub mod owned;
pub mod path;
pub mod validator;

pub use converter::{Converter, LuaConversionError, LuaConversionErrorKind, value_to_lua};
pub use mapped::{MappedArrayValue, MappedStructValue, MappedVariantValue};
pub use objects::{
    AssetUserdata, ValueObjects, ValueTables, WriteOwner, is_dirty_lua_value, mapped_source_bytes, recognizer, type_of,
};
pub use owned::{ArrayValue, StructValue, VariantValue};
pub use validator::{LuaValidationError, LuaValidationErrorKind, Validator, cast_guid};

/// SF's `type_aliases::TypeHandle`: a type of one shared registry.
pub type TypeHandle = kfc::reflection::TypeHandle<Rc<TypeRegistry>>;
/// SF's `type_aliases::MappedValue`: a decoded value viewing shared resource bytes.
pub type MappedValue = kfc::resource::mapped::MappedValue<Rc<[u8]>, Rc<TypeRegistry>>;

/// The three container classes a typed userdata can be (owned or mapped alike).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObjectKind {
    Struct,
    Array,
    Variant,
}

/// What the codec needs from typed userdata it does not own. One implementation per pass, from the value wrappers and
/// the asset session (Resource/Content answers). Every method runs on the pass's Lua worker.
pub trait TypedObjects {
    /// The container class and exact type of one of the six value wrappers; `None` for any other userdata.
    fn classify(&self, ud: &AnyUserData) -> mlua::Result<Option<(ObjectKind, TypeHandle)>>;
    /// A deep, owned copy of a classified wrapper (SF `try_clone_lua`).
    fn clone_object(&self, ud: &AnyUserData, lua: &mlua::Lua) -> mlua::Result<LuaValue>;
    /// A classified wrapper as a kfc value. Owned wrappers use `Converter::{struct_fields, array_items, variant}`.
    fn convert_object(&self, ud: &AnyUserData, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind>;
    /// A new owned struct from validated fields (SF `StructValue::new_with_data`).
    fn new_struct(
        &self,
        lua: &mlua::Lua,
        r#type: TypeHandle,
        fields: HashMap<String, LuaValue>,
    ) -> mlua::Result<LuaValue>;
    /// A new owned array (or bitmask) from validated items (SF `ArrayValue::new_with_data`).
    fn new_array(&self, lua: &mlua::Lua, r#type: TypeHandle, items: Vec<LuaValue>) -> mlua::Result<LuaValue>;
    /// A new owned variant (SF `VariantValue::new_with_data`).
    fn new_variant(
        &self,
        lua: &mlua::Lua,
        r#type: TypeHandle,
        variant_type: TypeHandle,
        value: LuaValue,
    ) -> mlua::Result<LuaValue>;
    /// A decoded struct, array or variant as its mapped wrapper.
    fn wrap_mapped(&self, lua: &mlua::Lua, value: &MappedValue) -> mlua::Result<LuaValue>;
    /// A Resource userdata's GUID (its id's); `None` for any other userdata. Required (no default), so an
    /// implementation cannot leave Resource recognition out by accident.
    fn resource_guid(&self, ud: &AnyUserData) -> mlua::Result<Option<Guid>>;
    /// A Content userdata's content hash; `None` for any other userdata. Required, as `resource_guid`.
    fn content_hash(&self, ud: &AnyUserData) -> mlua::Result<Option<ContentHash>>;
}

/// The deepest a value may nest (struct, array, optional, variant and typedef levels alike) before the walk stops with
/// an error. SF has no bound: a self-referencing table of a recursive type, or a deep enough chain, overflows the
/// worker's stack and aborts the process (an error instead). Each level costs about 2 KiB of stack in release.
pub const MAX_DEPTH: usize = 128;

/// The error at the level that would pass [`MAX_DEPTH`].
pub(crate) fn too_deep() -> mlua::Error {
    converter::host_failure(&format!("value nested more than {MAX_DEPTH} levels deep"))
}

/// A faulty registry whose inheritance or inner-type chain is a cycle would make kfc's own walks (`iter_fields`,
/// `get_field_metadata`, `is_sub_type`, typedef unwrapping) loop forever; this walks the raw chain once, bounded by the
/// registry's size, and refuses such a type. A chain that ends at a missing index ends there (kfc does the same).
pub(crate) fn check_chain(r#type: &TypeHandle) -> mlua::Result<()> {
    let registry = r#type.type_registry();
    let mut next = r#type.inner_type;
    for _ in 0..=registry.len() {
        match next.and_then(|index| registry.get(index)) {
            Some(t) => next = t.inner_type,
            None => return Ok(()),
        }
    }
    Err(converter::host_failure(&format!(
        "cyclic type chain at `{}`",
        r#type.qualified_name
    )))
}

/// SF `name_of`: a typedef shows the type it names.
pub fn name_of(r#type: &TypeHandle) -> String {
    let registry = r#type.type_registry();
    let mut current: &TypeMetadata = r#type;
    // bounded: a typedef cycle in a faulty registry ends at the last typedef instead of overflowing the stack
    for _ in 0..=registry.len() {
        match (
            current.primitive_type,
            current.inner_type.and_then(|index| registry.get(index)),
        ) {
            (PrimitiveType::Typedef, Some(inner)) => current = inner,
            // SF would panic on a typedef without its inner type; naming the typedef itself keeps the error readable
            _ => break,
        }
    }
    current.qualified_name.clone()
}

/// A struct field's type, through inheritance. kfc's `get_field_type` panics on a field whose type the registry lacks;
/// this is SF's panic text as an error.
pub(crate) fn field_type(r#type: &TypeHandle, field: &str) -> mlua::Result<TypeHandle> {
    let metadata = r#type
        .get_field_metadata(field)
        .ok_or_else(|| converter::host_failure("field type must be present"))?;
    TypeHandle::try_new(r#type.type_registry().clone(), metadata.r#type)
        .ok_or_else(|| converter::host_failure("field type must exist in the type registry"))
}

/// A type's inner (element/base/wrapped) type, which must exist in the registry; `text` is SF's panic text.
pub(crate) fn inner_type(r#type: &TypeHandle, text: &str) -> mlua::Result<TypeHandle> {
    r#type
        .inner_type
        .and_then(|index| TypeHandle::try_new(r#type.type_registry().clone(), index))
        .ok_or_else(|| converter::host_failure(text))
}

/// SF `validate_and_clone_lua_value`.
pub fn validate_and_clone(
    value: &LuaValue,
    r#type: &TypeHandle,
    lua: &mlua::Lua,
    objects: &dyn TypedObjects,
) -> Result<LuaValue, LuaValidationError> {
    Validator::validate_and_clone(value, r#type, lua, objects)
}

/// SF `convert_lua_to_value` (of a validated value).
pub fn convert_lua_to_value(
    value: &LuaValue,
    r#type: &TypeHandle,
    objects: &dyn TypedObjects,
) -> Result<Value, LuaConversionError> {
    Converter::convert(value, r#type, objects)
}

/// SF `try_clone_lua_value`: wrappers deep-cloned, everything else as is.
pub fn try_clone_lua_value(value: &LuaValue, lua: &mlua::Lua, objects: &dyn TypedObjects) -> mlua::Result<LuaValue> {
    match value {
        LuaValue::UserData(ud) => match objects.classify(ud)? {
            Some(_) => objects.clone_object(ud, lua),
            None => Err(converter::host_failure("unexpected user data type in clone_lua_value")),
        },
        _ => Ok(value.clone()),
    }
}

/// SF `encode_owned_value`'s sequence: validate, convert, serialize. Errors are their texts.
pub fn encode(
    value: &LuaValue,
    r#type: &TypeHandle,
    lua: &mlua::Lua,
    objects: &dyn TypedObjects,
) -> Result<Vec<u8>, String> {
    let checked = validate_and_clone(value, r#type, lua, objects).map_err(|e| e.to_string())?;
    let value = convert_lua_to_value(&checked, r#type, objects).map_err(|e| e.to_string())?;
    let registry = r#type.type_registry().clone();
    contained("serializing", || {
        value.to_bytes(&registry, r#type).map_err(|e| e.to_string())
    })
}

/// A validated kfc value as resource bytes (the serialize step of `encode`; SF's `write_into`/`to_bytes`, whose
/// `unwrap` in the startup commit becomes an error here).
pub fn value_bytes(value: &Value, r#type: &TypeHandle) -> Result<Vec<u8>, String> {
    let registry = r#type.type_registry().clone();
    contained("serializing", || {
        value.to_bytes(&registry, r#type).map_err(|e| e.to_string())
    })
}

/// An unread decoded value as a kfc value: kfc's `Value::from` bounded by [`MAX_DEPTH`] and
/// [`converter::mapped_value_limit`] of `source_len` (see `Converter::mapped`). For a later caller of
/// `Value::from` on decoded data (the typed graph's export and dirty roots call it today: the typed graph's node.rs).
pub fn mapped_to_value(value: MappedValue, source_len: usize) -> Result<Value, String> {
    contained("converting", || {
        converter::mapped_value(value, source_len).map_err(|e| e.to_string())
    })
}

/// Resource bytes as a mapped value of `type` (SF asset_resources.rs `MappedValue::from_bytes`). Truncated or
/// malformed bytes are an error, never a panic.
pub fn decode_mapped(bytes: Rc<[u8]>, r#type: &TypeHandle) -> Result<MappedValue, String> {
    // kfc maps optionals and typedefs eagerly: a cyclic inner-type chain would recurse without end
    check_chain(r#type).map_err(|e| e.to_string())?;
    let registry = r#type.type_registry().clone();
    contained("decoding", || {
        MappedValue::from_bytes(&registry, r#type, &bytes).map_err(|e| e.to_string())
    })
}

/// Resource bytes as the Lua value a mod sees.
pub fn decode(
    bytes: Rc<[u8]>,
    r#type: &TypeHandle,
    lua: &mlua::Lua,
    objects: &dyn TypedObjects,
) -> Result<LuaValue, String> {
    let mapped = decode_mapped(bytes, r#type)?;
    value_to_lua(&mapped, lua, objects).map_err(|e| e.to_string())
}

/// The parser's own code under a panic guard (no Lua runs inside): a panic becomes an error naming the step.
fn contained<R>(step: &str, run: impl FnOnce() -> Result<R, String>) -> Result<R, String> {
    std::panic::catch_unwind(AssertUnwindSafe(run))
        .unwrap_or_else(|panic| Err(format!("{step} failed: {}", converter::panic_text(panic))))
}
