// SPDX-License-Identifier: GPL-3.0-or-later
//! Owned typed values: `StructValue`, `ArrayValue`, `VariantValue`, created by validation or deep cloning.
//!
//! Adapted from ShroudForge 1.7.9, Kiln's fork pin 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs): modules/mod-runtime/lua/resource_values/owned_values.rs:21-556. Same userdata names,
//! metamethods, casts, texts and identities:
//! - a get returns the stored value itself (a nested wrapper keeps its identity: aliases see each other's edits);
//! - an assignment or insertion validates and deep-clones (so it never aliases its source);
//! - owned values are always dirty.
//!
//! The fork's setter fix applies (RULES 2d): `StructValue.__newindex` validates against the FIELD's type and
//! `ArrayValue.__newindex` against the ELEMENT type, as the mapped wrappers do. Upstream 1.8.0 validates against the
//! enclosing struct/array type there, which refuses every valid scalar.
//!
//! Upstream iterates struct fields with an `ouroboros` borrow of the type; here the iterator owns the field names.
//! `__tostring` keeps SF's own field-map order, which is a hash map's order in both (not stable, not the type order).
use std::{cell::RefCell, collections::HashMap, rc::Rc};

use kfc::{reflection::PrimitiveType, resource::value::Value};
use mlua::{AnyUserData, Function, MetaMethod, UserData};
use sfi_core::lua::{FunctionArgs, LuaError, LuaValue, MethodArgs};

use super::{
    Converter, LuaConversionErrorKind, TypeHandle, name_of, objects::ValueObjects, try_clone_lua_value,
    validate_and_clone,
};

/// SF's `__tostring` body: values joined, strings quoted.
pub(super) fn push_display(result: &mut String, value: &LuaValue) -> mlua::Result<()> {
    if value.is_string() {
        result.push('"');
        result.push_str(&value.to_string()?);
        result.push('"');
    } else {
        result.push_str(&value.to_string()?);
    }
    Ok(())
}

pub struct StructValue {
    r#type: TypeHandle,
    fields: Rc<RefCell<HashMap<String, LuaValue>>>,
    objects: ValueObjects,
}

impl StructValue {
    /// The fields must already be validated for `type` (SF's contract).
    pub fn new_with_data(r#type: TypeHandle, map: HashMap<String, LuaValue>, objects: ValueObjects) -> Self {
        Self {
            r#type,
            fields: Rc::new(RefCell::new(map)),
            objects,
        }
    }

    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        let fields = self.fields.borrow();
        converter.struct_fields(&self.r#type, fields.iter().map(|(k, v)| (k.as_str(), v)))
    }

    /// A deep copy whose wrappers belong to `objects` (kiln-eml: the seam of the value it is copied into).
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        let table = self.fields.borrow();
        let mut fields = HashMap::with_capacity(table.len());
        for (key, value) in table.iter() {
            fields.insert(key.clone(), try_clone_lua_value(value, lua, objects)?);
        }
        Ok(Self {
            r#type: self.r#type.clone(),
            fields: Rc::new(RefCell::new(fields)),
            objects: objects.clone(),
        })
    }

    pub fn get(&self, key: String) -> mlua::Result<LuaValue> {
        Ok(self.fields.borrow().get(&key).cloned().unwrap_or(LuaValue::Nil))
    }

    pub fn set(&self, key: String, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let known = self.fields.borrow().contains_key(&key);
        let field_type = match self.r#type.get_field_metadata(&key) {
            Some(_) if known => super::field_type(&self.r#type, &key)?,
            _ => {
                return Err(LuaError::generic(format!(
                    "attempt to set invalid property '{key}' on {}",
                    name_of(&self.r#type)
                )));
            }
        };
        // the field's type (fork fix), as MappedStructValue::set checks it
        let value = validate_and_clone(value, &field_type, lua, &self.objects).map_err(LuaError::external)?;
        self.fields.borrow_mut().insert(key, value);
        self.objects.wrote();
        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }
}

impl UserData for StructValue {
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.fields.borrow().len())
        });
        methods.add_meta_function(MetaMethod::Index, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<String>(0)?;
            this.get(key)
        });
        methods.add_meta_function(MetaMethod::NewIndex, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<String>(0)?;
            let value = args.get::<LuaValue>(1)?;
            this.set(key, value, lua)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            StructValueIter::create(&this, lua)
        });
        methods.add_meta_function(MetaMethod::ToString, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let values = this.fields.borrow();
            let mut result = String::from('{');
            for (i, (key, value)) in values.iter().enumerate() {
                if i > 0 {
                    result.push(',');
                }
                result.push('"');
                result.push_str(key);
                result.push_str("\":");
                push_display(&mut result, value)?;
            }
            result.push('}');
            Ok(LuaValue::String(lua.create_string(result)?))
        });
    }
}

/// Field names in the type's order (base first), each value from the live field map.
struct StructValueIter {
    keys: Vec<String>,
    fields: Rc<RefCell<HashMap<String, LuaValue>>>,
    next: std::cell::Cell<usize>,
}

impl StructValueIter {
    fn create(value: &StructValue, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let keys = value.r#type.iter_fields().map(|field| field.name.clone()).collect();
        let state = Self {
            keys,
            fields: value.fields.clone(),
            next: std::cell::Cell::new(0),
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|lua, args: FunctionArgs| {
            let state = args.get::<&Self>(0)?;
            state.step(lua)
        })?;
        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn step(&self, lua: &mlua::Lua) -> mlua::Result<(LuaValue, LuaValue)> {
        let Some(key) = self.keys.get(self.next.get()) else {
            return Ok((LuaValue::Nil, LuaValue::Nil));
        };
        self.next.set(self.next.get() + 1);
        let value = self.fields.borrow().get(key).cloned().unwrap_or(LuaValue::Nil);
        Ok((LuaValue::String(lua.create_string(key)?), value))
    }
}

impl UserData for StructValueIter {}

pub struct ArrayValue {
    r#type: TypeHandle,
    inner_type: TypeHandle,
    values: Rc<RefCell<Vec<LuaValue>>>,
    objects: ValueObjects,
}

impl ArrayValue {
    /// The items must already be validated for `type` (SF's contract).
    pub fn new_with_data(r#type: TypeHandle, data: Vec<LuaValue>, objects: ValueObjects) -> mlua::Result<Self> {
        let inner_type = super::inner_type(&r#type, "array type must have an inner type")?;
        Ok(Self {
            r#type,
            inner_type,
            values: Rc::new(RefCell::new(data)),
            objects,
        })
    }

    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        let values = self.values.borrow();
        converter.array_items(&self.r#type, values.iter())
    }

    /// A deep copy whose wrappers belong to `objects`.
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        let values = self.values.borrow();
        let mut result = Vec::with_capacity(values.len());
        for value in values.iter() {
            result.push(try_clone_lua_value(value, lua, objects)?);
        }
        Ok(Self {
            r#type: self.r#type.clone(),
            inner_type: self.inner_type.clone(),
            values: Rc::new(RefCell::new(result)),
            objects: objects.clone(),
        })
    }

    pub fn len(&self) -> usize {
        self.values.borrow().len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, index: usize) -> mlua::Result<LuaValue> {
        if index == 0 {
            return Ok(LuaValue::Nil);
        }
        Ok(self.values.borrow().get(index - 1).cloned().unwrap_or(LuaValue::Nil))
    }

    pub fn set(&self, index: usize, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let index = check_range(index, 1, self.len())?;
        // the element type (fork fix), as MappedArrayValue::set checks it
        let value = validate_and_clone(value, &self.inner_type, lua, &self.objects).map_err(LuaError::external)?;
        self.values.borrow_mut()[index] = value;
        self.objects.wrote();
        Ok(())
    }

    pub fn insert(&mut self, index: usize, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let index = check_range(index, 1, self.len() + 1)?;
        check_static(&self.r#type)?;
        let value = validate_and_clone(value, &self.inner_type, lua, &self.objects).map_err(LuaError::external)?;
        self.values.borrow_mut().insert(index, value);
        self.objects.wrote();
        Ok(())
    }

    pub fn remove(&mut self, index: usize) -> mlua::Result<Option<LuaValue>> {
        if index == 0 {
            return Ok(None);
        }
        let index = check_range(index, 1, self.len() + 1)?;
        check_static(&self.r#type)?;
        if index >= self.values.borrow().len() {
            return Ok(None);
        }
        let removed = self.values.borrow_mut().remove(index);
        self.objects.wrote();
        Ok(Some(removed))
    }

    pub fn clear(&mut self) -> mlua::Result<()> {
        check_static(&self.r#type)?;
        self.values.borrow_mut().clear();
        self.objects.wrote();
        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }
}

/// 1-based `index` in `[start, end]`, as a 0-based index.
pub(super) fn check_range(index: usize, start: usize, end: usize) -> mlua::Result<usize> {
    if index == 0 || index > end {
        Err(LuaError::position_out_of_bounds(index, start, end))
    } else {
        Ok(index - 1)
    }
}

pub(super) fn check_static(r#type: &TypeHandle) -> mlua::Result<()> {
    if r#type.primitive_type == PrimitiveType::StaticArray {
        Err(LuaError::generic("attempt to modify static array"))
    } else {
        Ok(())
    }
}

impl UserData for ArrayValue {
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.values.borrow().len())
        });
        methods.add_meta_function(MetaMethod::Index, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<usize>(0)?;
            this.get(key)
        });
        methods.add_meta_function(MetaMethod::NewIndex, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let index = args.get::<usize>(0)?;
            let value = args.get::<LuaValue>(1)?;
            this.set(index, value, lua)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            ArrayValueIter::create(&this, lua)
        });
        methods.add_meta_function(MetaMethod::ToString, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let values = this.values.borrow();
            let mut result = String::from('[');
            for (i, value) in values.iter().enumerate() {
                if i > 0 {
                    result.push(',');
                }
                push_display(&mut result, value)?;
            }
            result.push(']');
            Ok(LuaValue::String(lua.create_string(result)?))
        });
    }
}

struct ArrayValueIter {
    values: Rc<RefCell<Vec<LuaValue>>>,
    index: usize,
}

impl ArrayValueIter {
    fn create(value: &ArrayValue, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            values: value.values.clone(),
            index: 0,
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|_, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            state.step()
        })?;
        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn step(&mut self) -> mlua::Result<(LuaValue, LuaValue)> {
        if self.index >= self.values.borrow().len() {
            return Ok((LuaValue::Nil, LuaValue::Nil));
        }
        let key = self.index;
        let value = self.values.borrow()[key].clone();
        self.index += 1;
        Ok((LuaValue::Integer(key as i64 + 1), value))
    }
}

impl UserData for ArrayValueIter {}

pub struct VariantValue {
    r#type: TypeHandle,
    variant_type: TypeHandle,
    value: LuaValue,
    objects: ValueObjects,
}

impl VariantValue {
    /// The value must already be validated for `variant_type` (SF's contract).
    pub fn new_with_data(r#type: TypeHandle, variant_type: TypeHandle, value: LuaValue, objects: ValueObjects) -> Self {
        Self {
            r#type,
            variant_type,
            value,
            objects,
        }
    }

    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    pub fn variant_type(&self) -> &TypeHandle {
        &self.variant_type
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        converter.variant(&self.variant_type, &self.value)
    }

    /// A deep copy whose wrappers belong to `objects`.
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        Ok(Self {
            r#type: self.r#type.clone(),
            variant_type: self.variant_type.clone(),
            value: try_clone_lua_value(&self.value, lua, objects)?,
            objects: objects.clone(),
        })
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }
}

impl UserData for VariantValue {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("type", |_, this| Ok(this.variant_type().qualified_name.clone()));
        fields.add_field_method_get("value", |_, this| Ok(this.value.clone()));
        fields.add_field_method_set("value", |lua, this, value: LuaValue| {
            this.value =
                validate_and_clone(&value, &this.variant_type, lua, &this.objects).map_err(LuaError::external)?;
            this.objects.wrote();
            Ok(())
        });
    }
}
