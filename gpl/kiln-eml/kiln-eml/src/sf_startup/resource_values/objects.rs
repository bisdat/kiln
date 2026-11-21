// SPDX-License-Identifier: GPL-3.0-or-later
//! The production typed-object seam: the six value wrappers behind the codec's `TypedObjects`, their dirtiness, type
//! recognition for `game.types.of`, and the typed half of `table.insert/remove/clear` behind sfi-core's
//! `TypedTablePort` (the core's dispatcher owns argument counts, conversion and the fallback to Lua's own functions).
//!
//! Adapted from ShroudForge 1.7.9, Kiln's fork pin 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs): modules/mod-runtime/lua/resource_values/mod.rs:51-253 (`is_dirty_lua_value`,
//! `mapped_source_bytes`, `push`/`insert`/`pop`/`remove`/`clear`, `type_of`) and validate_resource_values.rs:685-735
//! (`try_clone_lua_value`'s classes). SF matches on `TypeId` and panics on a userdata without one; here that is a Lua
//! error with SF's panic text.
use std::{any::TypeId, cell::Cell, collections::HashMap, rc::Rc};

use kfc::{
    guid::{ContentHash, Guid},
    reflection::TypeIndex,
    resource::value::Value,
};
use mlua::{AnyUserData, Lua};
use sfi_core::lua::{Dispatch, LuaValue, TypedTablePort};

use super::{
    Converter, LuaConversionErrorKind, MAX_DEPTH, MappedValue, ObjectKind, TypeHandle, TypedObjects, check_chain,
    converter::host_failure,
    mapped::{MappedArrayValue, MappedStructValue, MappedVariantValue},
    owned::{ArrayValue, StructValue, VariantValue},
};
use crate::sf_startup::TypeRecognizer;

/// The asset userdata the codec accepts as GUID sources: a Resource (its id's GUID) and a Content (its hash).
///
/// The asset session's `Resource`/`Content` userdata (sf_startup/game/{resource,content}.rs) implement this. Without
/// them (`ValueObjects::new()`) every userdata answers `None`, exactly as SF does for a userdata of neither class.
pub trait AssetUserdata {
    fn resource_guid(&self, ud: &AnyUserData) -> mlua::Result<Option<Guid>>;
    fn content_hash(&self, ud: &AnyUserData) -> mlua::Result<Option<ContentHash>>;
}

/// kiln-eml (R1, the startup change journal): what a typed value belongs to. Every wrapper made for a resource's value
/// (decoded from it, validated into it, cloned into it) carries its resource's owner, and each write through such a
/// wrapper (an assignment, insertion, removal or clear) tells it, so the journal knows exactly which resources a
/// package's entrypoint wrote, without walking any value. A value without an owner (a Buffer's, `runtime.values`') tells
/// nobody.
pub trait WriteOwner {
    fn wrote(&self);
}

/// The production `TypedObjects`: one per startup VM, cloned into every wrapper it creates.
#[derive(Clone, Default)]
pub struct ValueObjects {
    assets: Option<Rc<dyn AssetUserdata>>,
    /// kiln-eml (R1): the resource the wrappers made from this belong to (`owned_by`), None for the session's own.
    owner: Option<Rc<dyn WriteOwner>>,
}

impl ValueObjects {
    /// Without asset userdata (until the asset session's types are wired).
    pub fn new() -> Self {
        Self {
            assets: None,
            owner: None,
        }
    }

    pub fn with_assets(assets: Rc<dyn AssetUserdata>) -> Self {
        Self {
            assets: Some(assets),
            owner: None,
        }
    }

    /// kiln-eml (R1): the same seam for the values of one resource: every wrapper made from it (decoded, validated or
    /// cloned into it, and their children) reports its writes to `owner`.
    pub fn owned_by(&self, owner: Rc<dyn WriteOwner>) -> Self {
        Self {
            assets: self.assets.clone(),
            owner: Some(owner),
        }
    }

    /// kiln-eml (R1): a wrapper made from this seam was written (after the write took effect).
    pub(crate) fn wrote(&self) {
        if let Some(owner) = &self.owner {
            owner.wrote();
        }
    }
}

thread_local! {
    static LEVEL: Cell<usize> = const { Cell::new(0) };
}

/// One level of a walk that recurses through nested wrappers (a deep clone, dirtiness), bounded by the codec's
/// [`MAX_DEPTH`]. Assignments clone, so a wrapper never contains itself, but repeated assignments nest a clone of a
/// clone without end, and a mapped value of self-referencing bytes reads as deep as a mod walks it: unbounded, those
/// walks overflowed the worker's stack (SF aborts). The count is the worker's own (all Lua runs on one thread).
struct Level;

impl Level {
    fn enter() -> mlua::Result<Self> {
        LEVEL.with(|level| {
            if level.get() >= MAX_DEPTH {
                return Err(super::too_deep());
            }
            level.set(level.get() + 1);
            Ok(Level)
        })
    }
}

impl Drop for Level {
    fn drop(&mut self) {
        LEVEL.with(|level| level.set(level.get() - 1));
    }
}

fn type_id(ud: &AnyUserData) -> mlua::Result<TypeId> {
    ud.type_id().ok_or_else(|| host_failure("UserData has no type_id"))
}

impl TypedObjects for ValueObjects {
    fn classify(&self, ud: &AnyUserData) -> mlua::Result<Option<(ObjectKind, TypeHandle)>> {
        let id = type_id(ud)?;
        Ok(Some(if id == TypeId::of::<StructValue>() {
            (ObjectKind::Struct, ud.borrow::<StructValue>()?.r#type().clone())
        } else if id == TypeId::of::<MappedStructValue>() {
            (ObjectKind::Struct, ud.borrow::<MappedStructValue>()?.r#type().clone())
        } else if id == TypeId::of::<ArrayValue>() {
            (ObjectKind::Array, ud.borrow::<ArrayValue>()?.r#type().clone())
        } else if id == TypeId::of::<MappedArrayValue>() {
            (ObjectKind::Array, ud.borrow::<MappedArrayValue>()?.r#type().clone())
        } else if id == TypeId::of::<VariantValue>() {
            (ObjectKind::Variant, ud.borrow::<VariantValue>()?.r#type().clone())
        } else if id == TypeId::of::<MappedVariantValue>() {
            (ObjectKind::Variant, ud.borrow::<MappedVariantValue>()?.r#type().clone())
        } else {
            return Ok(None);
        }))
    }

    fn clone_object(&self, ud: &AnyUserData, lua: &Lua) -> mlua::Result<LuaValue> {
        let _level = Level::enter()?;
        let id = type_id(ud)?;
        // kiln-eml (R1): the copy belongs to this seam's resource (where it is assigned), not to its source's
        let created = if id == TypeId::of::<StructValue>() {
            lua.create_userdata(ud.borrow::<StructValue>()?.try_clone_lua(lua, self)?)
        } else if id == TypeId::of::<MappedStructValue>() {
            lua.create_userdata(ud.borrow::<MappedStructValue>()?.try_clone_lua(lua, self)?)
        } else if id == TypeId::of::<ArrayValue>() {
            lua.create_userdata(ud.borrow::<ArrayValue>()?.try_clone_lua(lua, self)?)
        } else if id == TypeId::of::<MappedArrayValue>() {
            lua.create_userdata(ud.borrow::<MappedArrayValue>()?.try_clone_lua(lua, self)?)
        } else if id == TypeId::of::<VariantValue>() {
            lua.create_userdata(ud.borrow::<VariantValue>()?.try_clone_lua(lua, self)?)
        } else if id == TypeId::of::<MappedVariantValue>() {
            lua.create_userdata(ud.borrow::<MappedVariantValue>()?.try_clone_lua(lua, self)?)
        } else {
            return Err(host_failure("unexpected user data type in clone_lua_value"));
        };
        created.map(LuaValue::UserData)
    }

    fn convert_object(&self, ud: &AnyUserData, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        let id = type_id(ud)?;
        if id == TypeId::of::<StructValue>() {
            ud.borrow::<StructValue>()?.convert(converter)
        } else if id == TypeId::of::<MappedStructValue>() {
            ud.borrow::<MappedStructValue>()?.convert(converter)
        } else if id == TypeId::of::<ArrayValue>() {
            ud.borrow::<ArrayValue>()?.convert(converter)
        } else if id == TypeId::of::<MappedArrayValue>() {
            ud.borrow::<MappedArrayValue>()?.convert(converter)
        } else if id == TypeId::of::<VariantValue>() {
            ud.borrow::<VariantValue>()?.convert(converter)
        } else if id == TypeId::of::<MappedVariantValue>() {
            ud.borrow::<MappedVariantValue>()?.convert(converter)
        } else {
            Err(host_failure("convert_object called with a userdata of no value class").into())
        }
    }

    fn new_struct(&self, lua: &Lua, r#type: TypeHandle, fields: HashMap<String, LuaValue>) -> mlua::Result<LuaValue> {
        lua.create_userdata(StructValue::new_with_data(r#type, fields, self.clone()))
            .map(LuaValue::UserData)
    }

    fn new_array(&self, lua: &Lua, r#type: TypeHandle, items: Vec<LuaValue>) -> mlua::Result<LuaValue> {
        lua.create_userdata(ArrayValue::new_with_data(r#type, items, self.clone())?)
            .map(LuaValue::UserData)
    }

    fn new_variant(
        &self,
        lua: &Lua,
        r#type: TypeHandle,
        variant_type: TypeHandle,
        value: LuaValue,
    ) -> mlua::Result<LuaValue> {
        lua.create_userdata(VariantValue::new_with_data(r#type, variant_type, value, self.clone()))
            .map(LuaValue::UserData)
    }

    fn wrap_mapped(&self, lua: &Lua, value: &MappedValue) -> mlua::Result<LuaValue> {
        // kfc walks a wrapper's base chain (fields, `#`) and its element chain without a bound: a faulty registry's
        // cyclic chain is refused here, once per wrapper, as the codec's decode refuses it for the root
        match value {
            MappedValue::Struct(v) => check_chain(v.r#type())?,
            MappedValue::Array(v) => check_chain(v.r#type())?,
            MappedValue::Variant(v) => {
                check_chain(v.base_type())?;
                check_chain(v.variant_type())?;
            }
            _ => {}
        }
        let created = match value {
            MappedValue::Struct(v) => lua.create_userdata(MappedStructValue::new(v.clone(), self.clone())),
            MappedValue::Array(v) => lua.create_userdata(MappedArrayValue::new(v.clone(), self.clone())),
            MappedValue::Variant(v) => lua.create_userdata(MappedVariantValue::new(v.clone(), self.clone())),
            _ => return Err(host_failure("wrap_mapped called with a value of no container class")),
        };
        created.map(LuaValue::UserData)
    }

    fn resource_guid(&self, ud: &AnyUserData) -> mlua::Result<Option<Guid>> {
        match &self.assets {
            Some(assets) => assets.resource_guid(ud),
            None => Ok(None),
        }
    }

    fn content_hash(&self, ud: &AnyUserData) -> mlua::Result<Option<ContentHash>> {
        match &self.assets {
            Some(assets) => assets.content_hash(ud),
            None => Ok(None),
        }
    }
}

/// SF `is_dirty_lua_value`: owned wrappers always, mapped ones when edited (recursively); anything else never.
pub fn is_dirty_lua_value(value: &LuaValue) -> mlua::Result<bool> {
    let LuaValue::UserData(ud) = value else {
        return Ok(false);
    };
    let _level = Level::enter()?;
    let id = type_id(ud)?;
    if id == TypeId::of::<StructValue>() {
        ud.borrow::<StructValue>()?.is_dirty()
    } else if id == TypeId::of::<MappedStructValue>() {
        ud.borrow::<MappedStructValue>()?.is_dirty()
    } else if id == TypeId::of::<ArrayValue>() {
        ud.borrow::<ArrayValue>()?.is_dirty()
    } else if id == TypeId::of::<MappedArrayValue>() {
        ud.borrow::<MappedArrayValue>()?.is_dirty()
    } else if id == TypeId::of::<VariantValue>() {
        ud.borrow::<VariantValue>()?.is_dirty()
    } else if id == TypeId::of::<MappedVariantValue>() {
        ud.borrow::<MappedVariantValue>()?.is_dirty()
    } else {
        Ok(false)
    }
}

/// SF `mapped_source_bytes`: the original bytes of a mapped struct, `None` for anything else.
pub fn mapped_source_bytes(value: &LuaValue) -> mlua::Result<Option<Vec<u8>>> {
    let LuaValue::UserData(ud) = value else { return Ok(None) };
    match ud.type_id() {
        Some(id) if id == TypeId::of::<MappedStructValue>() => {
            Ok(Some(ud.borrow::<MappedStructValue>()?.source_bytes()))
        }
        _ => Ok(None),
    }
}

/// SF `type_of`: the reflected type of one of the six wrappers (a variant's declared type), else `None`.
pub fn type_of(value: &LuaValue) -> mlua::Result<Option<TypeIndex>> {
    let LuaValue::UserData(ud) = value else { return Ok(None) };
    Ok(ValueObjects::new().classify(ud)?.map(|(_, r#type)| r#type.index()))
}

/// `type_of` as the session's recognizer for `game.types.of` (the pass composition adds it).
pub fn recognizer() -> TypeRecognizer {
    Rc::new(type_of)
}

/// The typed half of `table.insert/remove/clear` (SF resource_values/mod.rs:89-198) for the core dispatcher: owned and
/// mapped arrays only. The receiver stays mutably borrowed through validation, so a re-entrant access from a
/// metamethod sees SF's borrow error.
#[derive(Clone, Copy, Default)]
pub struct ValueTables;

impl TypedTablePort for ValueTables {
    fn push(&self, lua: &Lua, receiver: AnyUserData, value: LuaValue) -> mlua::Result<Dispatch<()>> {
        let id = type_id(&receiver)?;
        if id == TypeId::of::<ArrayValue>() {
            let mut array = receiver.borrow_mut::<ArrayValue>()?;
            let len = array.len();
            array.insert(len + 1, &value, lua)?;
        } else if id == TypeId::of::<MappedArrayValue>() {
            let mut array = receiver.borrow_mut::<MappedArrayValue>()?;
            let len = array.len();
            array.insert(len + 1, &value, lua)?;
        } else {
            return Ok(Dispatch::NotHandled);
        }
        Ok(Dispatch::Handled(()))
    }

    fn insert(&self, lua: &Lua, receiver: AnyUserData, position: usize, value: LuaValue) -> mlua::Result<Dispatch<()>> {
        let id = type_id(&receiver)?;
        if id == TypeId::of::<ArrayValue>() {
            receiver.borrow_mut::<ArrayValue>()?.insert(position, &value, lua)?;
        } else if id == TypeId::of::<MappedArrayValue>() {
            receiver
                .borrow_mut::<MappedArrayValue>()?
                .insert(position, &value, lua)?;
        } else {
            return Ok(Dispatch::NotHandled);
        }
        Ok(Dispatch::Handled(()))
    }

    fn pop(&self, lua: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<LuaValue>> {
        let id = type_id(&receiver)?;
        let removed = if id == TypeId::of::<ArrayValue>() {
            let mut array = receiver.borrow_mut::<ArrayValue>()?;
            let len = array.len();
            array.remove(len)?
        } else if id == TypeId::of::<MappedArrayValue>() {
            let mut array = receiver.borrow_mut::<MappedArrayValue>()?;
            let len = array.len();
            array.remove(len, lua)?
        } else {
            return Ok(Dispatch::NotHandled);
        };
        Ok(Dispatch::Handled(removed.unwrap_or(LuaValue::Nil)))
    }

    fn remove(&self, lua: &Lua, receiver: AnyUserData, position: usize) -> mlua::Result<Dispatch<LuaValue>> {
        let id = type_id(&receiver)?;
        let removed = if id == TypeId::of::<ArrayValue>() {
            receiver.borrow_mut::<ArrayValue>()?.remove(position)?
        } else if id == TypeId::of::<MappedArrayValue>() {
            receiver.borrow_mut::<MappedArrayValue>()?.remove(position, lua)?
        } else {
            return Ok(Dispatch::NotHandled);
        };
        Ok(Dispatch::Handled(removed.unwrap_or(LuaValue::Nil)))
    }

    fn clear(&self, _lua: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<()>> {
        let id = type_id(&receiver)?;
        if id == TypeId::of::<ArrayValue>() {
            receiver.borrow_mut::<ArrayValue>()?.clear()?;
        } else if id == TypeId::of::<MappedArrayValue>() {
            receiver.borrow_mut::<MappedArrayValue>()?.clear()?;
        } else {
            return Ok(Dispatch::NotHandled);
        }
        Ok(Dispatch::Handled(()))
    }
}
