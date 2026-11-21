// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9, fork pin): modules/mod-runtime/lua/resource_values/owned_values.rs:119-222, 380-474, 542-556 and
// mapped_values.rs:204-322, 546-654, 728-757 (the userdata shapes: names, metamethods, fields, argument casts and
// iterator states), and resource_values/mod.rs:203-252 (`type_of`: recognition by TypeId, a shared borrow of the
// value). The bodies are not adapted: each one delegates to the kiln.eml.values provider.
//! The frontend's userdata for typed values: one per graph node, interned weakly by node id, each owning
//! one lease (closed once, when Lua collects it).
//!
//! They have ShroudForge's shapes so that everything Lua sees before the provider is asked is Source's own behaviour:
//! - the same type names (`__name`: `StructValue`, `MappedArrayValue`, ...; Source's `{value:#?}` texts and its
//!   receiver/argument errors name them), and the same Rust type identity per class (Source's `TypeId` label);
//! - the same registrations: struct and array classes have `__len`, `__index`, `__newindex`, `__pairs` and
//!   `__tostring` as meta functions over sfi-core's [`MethodArgs`] (`this::<&Self>()`, a struct key cast to
//!   `String`, an array key to `usize`); variants have only the fields `type` and `value` (get) and `value` (set);
//! - the same iterator states (`StructValueIter`, ...), a fresh iterator function per `pairs`, arity 3;
//! - the same mlua borrows: meta functions hold a shared borrow of the receiver for the call, the variant's setter a
//!   mutable one.
//!
//! What each operation does (caching, laziness, clones and aliases, dirtiness, bounds, static arrays, error texts) is
//! the provider's, which ports Source's semantics.

use super::arena::{Selector, Values};
use kiln_abi::services::kiln_eml_values::*;
use mlua::{AnyUserData, Lua, MetaMethod, UserData, UserDataFields, UserDataMethods, Value};
use sfi_core::lua::{FunctionArgs, MethodArgs};
use std::any::TypeId;
use std::cell::Cell;

/// One lease of a node (or an iterator), closed once when the userdata goes.
pub(crate) struct Lease {
    values: Values,
    lease: Cell<u64>,
}

impl Lease {
    fn new(values: &Values, lease: u64) -> Self {
        Self {
            values: values.clone(),
            lease: Cell::new(lease),
        }
    }
    pub(crate) fn token(&self) -> u64 {
        self.lease.get()
    }
    pub(crate) fn values(&self) -> &Values {
        &self.values
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        // a finalizer: may run inside any reader callback; the provider queues it and drains after the outer call
        let lease = self.lease.replace(0);
        self.values.close_lease(lease);
    }
}

macro_rules! typed_userdata {
    ($($name:ident),*) => {$(
        /// A typed value of this class (ShroudForge's userdata of the same name).
        pub struct $name(pub(crate) Lease);
        impl $name {
            /// The lease this userdata owns.
            pub fn lease(&self) -> u64 {
                self.0.token()
            }
        }
    )*};
}
typed_userdata!(
    StructValue,
    MappedStructValue,
    ArrayValue,
    MappedArrayValue,
    VariantValue,
    MappedVariantValue,
    StructValueIter,
    ArrayValueIter,
    MappedStructValueIter,
    MappedArrayValueIter
);

/// Source's key conversion before its body runs (RC-2): a struct takes `String`, an array `usize`.
trait Key {
    fn selector(&self) -> Selector<'_>;
}
impl Key for String {
    fn selector(&self) -> Selector<'_> {
        Selector::Field(self.as_bytes())
    }
}
impl Key for usize {
    fn selector(&self) -> Selector<'_> {
        Selector::Index(*self as u64)
    }
}

macro_rules! aggregate {
    ($name:ident, $key:ty, $iter:ident, $state:ty) => {
        impl UserData for $name {
            fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
                methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
                    let this = args.this::<&Self>()?;
                    this.0.values().length(this.0.token())
                });
                methods.add_meta_function(MetaMethod::Index, |lua, args: MethodArgs| {
                    let this = args.this::<&Self>()?;
                    let key = args.get::<$key>(0)?;
                    this.0.values().child(lua, this.0.token(), key.selector())
                });
                methods.add_meta_function(MetaMethod::NewIndex, |_, args: MethodArgs| {
                    let this = args.this::<&Self>()?;
                    let key = args.get::<$key>(0)?;
                    let value = args.get::<Value>(1)?;
                    this.0
                        .values()
                        .set(this.0.token(), key.selector(), value.clone())
                });
                methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
                    let this = args.this::<&Self>()?;
                    let values = this.0.values();
                    let iterator = values.iter_begin(this.0.token())?;
                    let state = lua.create_userdata($iter(Lease::new(values, iterator)))?;
                    let step = lua.create_function(|lua, args: FunctionArgs| {
                        let state = args.get::<$state>(0)?;
                        state.0.values().iter_next(lua, state.0.token())
                    })?;
                    Ok((step, state, Value::Nil))
                });
                methods.add_meta_function(MetaMethod::ToString, |lua, args: MethodArgs| {
                    let this = args.this::<&Self>()?;
                    this.0
                        .values()
                        .stringify(lua, this.0.token())
                        .map(Value::String)
                });
            }
        }
    };
}
aggregate!(StructValue, String, StructValueIter, &StructValueIter);
aggregate!(
    MappedStructValue,
    String,
    MappedStructValueIter,
    &MappedStructValueIter
);
aggregate!(ArrayValue, usize, ArrayValueIter, &mut ArrayValueIter);
aggregate!(
    MappedArrayValue,
    usize,
    MappedArrayValueIter,
    &mut MappedArrayValueIter
);

macro_rules! variant {
    ($name:ident) => {
        impl UserData for $name {
            fn add_fields<F: UserDataFields<Self>>(fields: &mut F) {
                fields.add_field_method_get("type", |lua, this| {
                    this.0
                        .values()
                        .child(lua, this.0.token(), Selector::Field(b"type"))
                });
                fields.add_field_method_get("value", |lua, this| {
                    this.0
                        .values()
                        .child(lua, this.0.token(), Selector::Field(b"value"))
                });
                fields.add_field_method_set("value", |_, this, value: Value| {
                    this.0
                        .values()
                        .set(this.0.token(), Selector::Field(b"value"), value)
                });
            }
        }
    };
}
variant!(VariantValue);
variant!(MappedVariantValue);

impl UserData for StructValueIter {}
impl UserData for ArrayValueIter {}
impl UserData for MappedStructValueIter {}
impl UserData for MappedArrayValueIter {}

/// The userdata type of a `KILN_VALUE_*` class.
pub(crate) fn type_of_kind(kind: u32) -> Option<TypeId> {
    Some(match kind as i32 {
        KILN_VALUE_STRUCT_OWNED => TypeId::of::<StructValue>(),
        KILN_VALUE_STRUCT_MAPPED => TypeId::of::<MappedStructValue>(),
        KILN_VALUE_ARRAY_OWNED => TypeId::of::<ArrayValue>(),
        KILN_VALUE_ARRAY_MAPPED => TypeId::of::<MappedArrayValue>(),
        KILN_VALUE_VARIANT_OWNED => TypeId::of::<VariantValue>(),
        KILN_VALUE_VARIANT_MAPPED => TypeId::of::<MappedVariantValue>(),
        _ => return None,
    })
}

/// The userdata type of a `found_userdata.value_kind` (the error packet's spelling).
pub(crate) fn type_id_of_kind(value_kind: &str) -> Option<TypeId> {
    type_of_kind(match value_kind {
        "struct_owned" => KILN_VALUE_STRUCT_OWNED,
        "struct_mapped" => KILN_VALUE_STRUCT_MAPPED,
        "array_owned" => KILN_VALUE_ARRAY_OWNED,
        "array_mapped" => KILN_VALUE_ARRAY_MAPPED,
        "variant_owned" => KILN_VALUE_VARIANT_OWNED,
        "variant_mapped" => KILN_VALUE_VARIANT_MAPPED,
        _ => return None,
    } as u32)
}

macro_rules! with_lease {
    ($userdata:expr, |$lease:ident| $body:expr) => {{
        let id = $userdata.type_id()?;
        if id == TypeId::of::<StructValue>() {
            let $lease = &$userdata.borrow::<StructValue>().ok()?.0;
            Some($body)
        } else if id == TypeId::of::<MappedStructValue>() {
            let $lease = &$userdata.borrow::<MappedStructValue>().ok()?.0;
            Some($body)
        } else if id == TypeId::of::<ArrayValue>() {
            let $lease = &$userdata.borrow::<ArrayValue>().ok()?.0;
            Some($body)
        } else if id == TypeId::of::<MappedArrayValue>() {
            let $lease = &$userdata.borrow::<MappedArrayValue>().ok()?.0;
            Some($body)
        } else if id == TypeId::of::<VariantValue>() {
            let $lease = &$userdata.borrow::<VariantValue>().ok()?.0;
            Some($body)
        } else if id == TypeId::of::<MappedVariantValue>() {
            let $lease = &$userdata.borrow::<MappedVariantValue>().ok()?.0;
            Some($body)
        } else {
            None
        }
    }};
}

/// A new userdata of class `kind` that owns `lease` (the lease is also its user value, so the reader can name it
/// without borrowing the userdata, which an operation on it may hold).
///
/// The userdata takes the lease only once it stands: on any failure (not a value class, Lua out of memory) the lease
/// is handed back unclosed and the caller decides (a provisional is the provider's to close, an item the caller's).
pub(crate) fn create(
    lua: &Lua,
    values: &Values,
    kind: u32,
    lease: u64,
) -> mlua::Result<AnyUserData> {
    let owned = Lease::new(values, 0);
    let userdata = match kind as i32 {
        KILN_VALUE_STRUCT_OWNED => lua.create_userdata(StructValue(owned))?,
        KILN_VALUE_STRUCT_MAPPED => lua.create_userdata(MappedStructValue(owned))?,
        KILN_VALUE_ARRAY_OWNED => lua.create_userdata(ArrayValue(owned))?,
        KILN_VALUE_ARRAY_MAPPED => lua.create_userdata(MappedArrayValue(owned))?,
        KILN_VALUE_VARIANT_OWNED => lua.create_userdata(VariantValue(owned))?,
        KILN_VALUE_VARIANT_MAPPED => lua.create_userdata(MappedVariantValue(owned))?,
        _ => {
            return Err(mlua::Error::runtime(format!(
                "kiln: no typed userdata for value kind {kind}"
            )));
        }
    };
    userdata.set_user_value(lease as i64)?;
    // a fresh userdata nobody else holds: its borrow cannot fail
    let take = || with_lease!(userdata, |owned| owned.lease.set(lease));
    take().ok_or_else(|| mlua::Error::runtime("kiln: a fresh typed userdata refused its lease"))?;
    Ok(userdata)
}

/// Whether `userdata` is one of the frontend's typed values.
pub(crate) fn is_typed(userdata: &AnyUserData) -> bool {
    userdata.type_id().is_some_and(|id| {
        [
            TypeId::of::<StructValue>(),
            TypeId::of::<MappedStructValue>(),
            TypeId::of::<ArrayValue>(),
            TypeId::of::<MappedArrayValue>(),
            TypeId::of::<VariantValue>(),
            TypeId::of::<MappedVariantValue>(),
        ]
        .contains(&id)
    })
}

/// The lease of one of the frontend's typed values (never borrowing it), or None for any other userdata.
pub fn lease_of(userdata: &AnyUserData) -> Option<u64> {
    if !is_typed(userdata) {
        return None;
    }
    match userdata.user_value::<Option<i64>>() {
        Ok(Some(lease)) if lease != 0 => Some(lease as u64),
        _ => with_lease!(userdata, |lease| lease.token()).filter(|lease| *lease != 0),
    }
}

/// Source's `type_of` (`game.types.of`): the registry index of one of the frontend's typed values, `None` for any
/// other value. As in Source the value is borrowed (shared) to read its type, so one borrowed mutably now (a typed
/// `table.insert` running a callback, a variant's `value` being set) is mlua's borrow error, and a userdata with no
/// Rust type identity is Source's panic text as an error. The index is the provider's (an identity-only
/// `value_info`): its registry is the one the VM's arena was created over, the archive registry `game.types` reads.
pub fn type_index_of(value: &Value) -> mlua::Result<Option<usize>> {
    let Value::UserData(userdata) = value else {
        return Ok(None);
    };
    let id = userdata
        .type_id()
        .ok_or_else(|| mlua::Error::runtime("UserData has no type_id"))?;
    macro_rules! index_of {
        ($($class:ty),*) => {$(
            if id == TypeId::of::<$class>() {
                let this = userdata.borrow::<$class>()?;
                let info = this
                    .0
                    .values()
                    .identity(this.0.token())
                    .map_err(|e| e.to_lua())?;
                return Ok(Some(info.type_index as usize));
            }
        )*};
    }
    index_of!(
        StructValue,
        MappedStructValue,
        ArrayValue,
        MappedArrayValue,
        VariantValue,
        MappedVariantValue
    );
    Ok(None)
}

/// The arena handle of one of the frontend's typed values.
pub fn values_of(userdata: &AnyUserData) -> Option<Values> {
    with_lease!(userdata, |lease| lease.values().clone())
}

/// A proxy that must not keep its lease after all (not interned): it gives it back without closing it.
pub(crate) fn disown(userdata: &AnyUserData) {
    let give_back = || with_lease!(userdata, |lease| lease.lease.set(0));
    let _ = give_back();
    let _ = userdata.set_user_value(Value::Nil);
}
