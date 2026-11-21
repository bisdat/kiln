// SPDX-License-Identifier: GPL-3.0-or-later
//! Mapped typed values: `MappedStructValue`, `MappedArrayValue`, `MappedVariantValue`, lazy views of decoded
//! resource bytes with a local edit cache.
//!
//! Adapted from ShroudForge 1.7.9, Kiln's fork pin 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs): modules/mod-runtime/lua/resource_values/mapped_values.rs:29-758. Same behaviour:
//! - reads decode on first use and cache complex results (strings and userdata), so a nested wrapper keeps its
//!   identity; scalars are decoded again on every read;
//! - the original bytes are never changed: edits live in the cache and set `dirty`;
//! - `#` of a mapped array is its current (logical) length, `pairs` walks the ORIGINAL mapped length (upstream does
//!   not normalize this, nor does this);
//! - a mapped array's `#` is an `i32`, a mapped struct's `#` the mapped field count.
//!
//! Where SF panics (an `expect` on a field the bytes lack while converting or printing, a modified array element that
//! does not convert) this raises a Lua error with SF's panic text. So do the parser's own panics on a faulty
//! registry, which kfc's lazy mapping defers to these reads (`lazily`). Upstream iterates struct keys through an
//! `ouroboros` borrow; here the iterator owns them.
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    rc::Rc,
};

use kfc::{
    reflection::TypeRegistry,
    resource::{
        mapped::{MappedArray as KfcArray, MappedStruct as KfcStruct, MappedVariant as KfcVariant},
        value::{Value, Variant},
    },
};
use mlua::{AnyUserData, Function, MetaMethod, UserData};
use sfi_core::lua::{FunctionArgs, LuaError, LuaValue, MethodArgs};

use super::{
    Converter, LuaConversionErrorKind, MappedValue, TypeHandle, check_chain,
    converter::{host_failure, panic_text},
    name_of,
    objects::{ValueObjects, is_dirty_lua_value},
    owned::{check_range, check_static, push_display},
    try_clone_lua_value, validate_and_clone, value_to_lua,
};

pub type MappedStruct = KfcStruct<Rc<[u8]>, Rc<TypeRegistry>>;
pub type MappedArray = KfcArray<Rc<[u8]>, Rc<TypeRegistry>>;
pub type MappedVariant = KfcVariant<Rc<[u8]>, Rc<TypeRegistry>>;

/// One lazy read of the original bytes. kfc parses a field or element only when it is read, and on a faulty registry
/// its parser panics (an `expect`/`unwrap` on a missing type). Unguarded, mlua would carry that panic through every
/// Lua `pcall` up to the host; here it is a Lua error with the panic's text, as the codec's `contained` does for a
/// whole decode. No wrapper state changes inside the guard.
fn lazily<R>(read: impl FnOnce() -> mlua::Result<R>) -> mlua::Result<R> {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(read))
        .unwrap_or_else(|panic| Err(host_failure(&panic_text(panic))))
}

/// A struct field's type chain, checked before kfc maps the field: kfc maps optionals and typedefs eagerly and would
/// recurse along a faulty registry's cyclic chain without end. The wrapper's own chain is checked when it is made
/// (`ValueObjects::wrap_mapped`), so the field lookup itself terminates; a missing field type is kfc's own error.
fn check_field_chain(data: &MappedStruct, key: &str) -> mlua::Result<()> {
    let Some(field) = data.r#type().get_field_metadata(key) else {
        return Ok(());
    };
    match TypeHandle::try_new(data.r#type().type_registry().clone(), field.r#type) {
        Some(field_type) => check_chain(&field_type),
        None => Ok(()),
    }
}

/// Strings and userdata are cached, so repeated reads return the same object.
fn cacheable(value: &LuaValue) -> bool {
    !matches!(
        value,
        LuaValue::Nil | LuaValue::Boolean(_) | LuaValue::LightUserData(_) | LuaValue::Integer(_) | LuaValue::Number(_)
    )
}

pub struct MappedStructValue {
    data: MappedStruct,
    cache: Rc<RefCell<HashMap<String, LuaValue>>>,
    dirty: Cell<bool>,
    objects: ValueObjects,
}

impl MappedStructValue {
    pub fn new(data: MappedStruct, objects: ValueObjects) -> Self {
        Self {
            data,
            cache: Default::default(),
            dirty: Cell::new(false),
            objects,
        }
    }

    pub fn r#type(&self) -> &TypeHandle {
        self.data.r#type()
    }

    /// The exact bytes the value was decoded from (SF: what a runtime write compares against).
    pub fn source_bytes(&self) -> Vec<u8> {
        self.data.data().as_ref().to_vec()
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        let cache = self.cache.borrow();
        let mut result: Vec<(String, Value)> = Vec::new();
        for field in self.data.r#type().iter_fields() {
            let key = field.name.as_str();
            converter.path.push(key);
            let value = match cache.get(key) {
                Some(value) => {
                    let field_type = super::field_type(self.data.r#type(), key)?;
                    converter.process(value, &field_type)?
                }
                None => {
                    check_field_chain(&self.data, key)?;
                    let value = self
                        .data
                        .get(key)?
                        .ok_or_else(|| host_failure("expected a valid field in struct"))?;
                    // SF: kfc's Value::from, unbounded on crafted bytes
                    converter.mapped(value, self.data.data().len())?
                }
            };
            match result.iter_mut().find(|(k, _)| k == key) {
                Some(slot) => slot.1 = value,
                None => result.push((key.to_owned(), value)),
            }
            converter.path.pop();
        }
        Ok(Value::Struct(Box::new(result.into_iter().collect())))
    }

    /// A deep copy whose wrappers belong to `objects` (kiln-eml R1: the seam of the value it is copied into).
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        let mut cache = HashMap::with_capacity(self.cache.borrow().len());
        for (key, value) in self.cache.borrow().iter() {
            cache.insert(key.clone(), try_clone_lua_value(value, lua, objects)?);
        }
        Ok(Self {
            data: self.data.clone(),
            cache: Rc::new(RefCell::new(cache)),
            dirty: Cell::new(self.dirty.get()),
            objects: objects.clone(),
        })
    }

    pub fn get(&self, key: String, lua: &mlua::Lua) -> mlua::Result<LuaValue> {
        Self::get_field(&self.data, &mut self.cache.borrow_mut(), lua, &key, &self.objects)
    }

    pub fn set(&self, key: &str, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let field_type = match self.data.r#type().get_field_metadata(key) {
            Some(_) => super::field_type(self.data.r#type(), key)?,
            None => {
                return Err(LuaError::generic(format!(
                    "attempt to set invalid property '{key}' on {}",
                    name_of(self.r#type())
                )));
            }
        };
        let value = validate_and_clone(value, &field_type, lua, &self.objects).map_err(LuaError::external)?;
        self.cache.borrow_mut().insert(key.to_string(), value);
        self.dirty.set(true);
        self.objects.wrote();
        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        if self.dirty.get() {
            return Ok(true);
        }
        for value in self.cache.borrow().values() {
            if is_dirty_lua_value(value)? {
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn get_field(
        data: &MappedStruct,
        cache: &mut HashMap<String, LuaValue>,
        lua: &mlua::Lua,
        key: &str,
        objects: &ValueObjects,
    ) -> mlua::Result<LuaValue> {
        if let Some(value) = cache.get(key) {
            return Ok(value.clone());
        }
        // an unknown key is not an error: nil
        let decoded = lazily(|| {
            check_field_chain(data, key)?;
            match data.get(key).map_err(LuaError::external)? {
                Some(value) => value_to_lua(&value, lua, objects).map(Some),
                None => Ok(None),
            }
        })?;
        match decoded {
            Some(lua_value) => {
                if cacheable(&lua_value) {
                    cache.insert(key.to_string(), lua_value.clone());
                }
                Ok(lua_value)
            }
            None => Ok(LuaValue::Nil),
        }
    }
}

impl UserData for MappedStructValue {
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            lazily(|| Ok(this.data.len()))
        });
        methods.add_meta_function(MetaMethod::Index, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<String>(0)?;
            this.get(key, lua)
        });
        methods.add_meta_function(MetaMethod::NewIndex, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<String>(0)?;
            let value = args.get::<LuaValue>(1)?;
            this.set(&key, value, lua)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            MappedStructValueIter::create(&this, lua)
        });
        methods.add_meta_function(MetaMethod::ToString, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let cache = this.cache.borrow();
            let mut result = String::from('{');
            for (i, field) in this.r#type().iter_fields().enumerate() {
                if i > 0 {
                    result.push(',');
                }
                let key = field.name.as_str();
                let decoded;
                let value = match cache.get(key) {
                    Some(value) => value,
                    None => {
                        decoded = lazily(|| {
                            check_field_chain(&this.data, key)?;
                            match this.data.get(key).map_err(LuaError::external)? {
                                Some(value) => value_to_lua(&value, lua, &this.objects),
                                None => Err(host_failure("expected a valid field in struct")),
                            }
                        })?;
                        &decoded
                    }
                };
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

/// The mapped struct's own keys, values through the shared cache.
struct MappedStructValueIter {
    data: MappedStruct,
    keys: Vec<String>,
    cache: Rc<RefCell<HashMap<String, LuaValue>>>,
    next: Cell<usize>,
    objects: ValueObjects,
}

impl MappedStructValueIter {
    fn create(value: &MappedStructValue, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            data: value.data.clone(),
            keys: value.data.iter_keys().map(str::to_owned).collect(),
            cache: value.cache.clone(),
            next: Cell::new(0),
            objects: value.objects.clone(),
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
        let value = MappedStructValue::get_field(&self.data, &mut self.cache.borrow_mut(), lua, key, &self.objects)?;
        Ok((LuaValue::String(lua.create_string(key)?), value))
    }
}

impl UserData for MappedStructValueIter {}

#[derive(Debug, Clone)]
enum Entry {
    Value(LuaValue),
    Uninitialized(usize),
}

pub struct MappedArrayValue {
    data: MappedArray,
    values: Rc<RefCell<Vec<Entry>>>,
    dirty: Cell<bool>,
    objects: ValueObjects,
}

impl MappedArrayValue {
    pub fn new(data: MappedArray, objects: ValueObjects) -> Self {
        // one entry per element: kfc refused a stored count the bytes cannot hold before mapping it (eml-array-count)
        let cache = (0..data.len()).map(Entry::Uninitialized).collect();
        Self {
            data,
            values: Rc::new(RefCell::new(cache)),
            dirty: Cell::new(false),
            objects,
        }
    }

    pub fn r#type(&self) -> &TypeHandle {
        self.data.r#type()
    }

    pub fn element_type(&self) -> &TypeHandle {
        self.data.element_type()
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        let values = self.values.borrow();
        let mut result = Vec::with_capacity(values.len());
        for entry in values.iter() {
            match entry {
                Entry::Uninitialized(i) => {
                    let original = self
                        .data
                        .get(*i)?
                        .ok_or_else(|| host_failure("expected a value in the array"))?;
                    result.push(converter.mapped(original, self.data.data().len())?);
                }
                // SF `expect`s here (no path entry): a failure is its panic text, raised
                Entry::Value(lua_value) => match converter.process(lua_value, self.data.element_type()) {
                    Ok(value) => result.push(value),
                    Err(error) => {
                        return Err(host_failure(&format!("failed to convert modified value: {error:?}")).into());
                    }
                },
            }
        }
        Ok(result.into())
    }

    /// A deep copy whose wrappers belong to `objects` (kiln-eml R1: the seam of the value it is copied into).
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        let values = self.values.borrow();
        let mut cloned = Vec::with_capacity(values.len());
        for entry in values.iter() {
            cloned.push(match entry {
                Entry::Uninitialized(i) => Entry::Uninitialized(*i),
                Entry::Value(value) => Entry::Value(try_clone_lua_value(value, lua, objects)?),
            });
        }
        Ok(Self {
            data: self.data.clone(),
            values: Rc::new(RefCell::new(cloned)),
            dirty: Cell::new(self.dirty.get()),
            objects: objects.clone(),
        })
    }

    pub fn len(&self) -> usize {
        self.values.borrow().len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, index: usize, lua: &mlua::Lua) -> mlua::Result<LuaValue> {
        if index == 0 {
            return Ok(LuaValue::Nil);
        }
        Self::get_field(index - 1, &self.data, &mut self.values.borrow_mut(), lua, &self.objects)
    }

    pub fn set(&self, index: usize, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let index = check_range(index, 1, self.len())?;
        let value = validate_and_clone(value, self.element_type(), lua, &self.objects).map_err(LuaError::external)?;
        self.values.borrow_mut()[index] = Entry::Value(value);
        self.dirty.set(true);
        self.objects.wrote();
        Ok(())
    }

    pub fn insert(&mut self, index: usize, value: &LuaValue, lua: &mlua::Lua) -> mlua::Result<()> {
        let index = check_range(index, 1, self.len() + 1)?;
        check_static(self.r#type())?;
        let value = validate_and_clone(value, self.element_type(), lua, &self.objects).map_err(LuaError::external)?;
        self.values.borrow_mut().insert(index, Entry::Value(value));
        self.dirty.set(true);
        self.objects.wrote();
        Ok(())
    }

    pub fn remove(&mut self, index: usize, lua: &mlua::Lua) -> mlua::Result<Option<LuaValue>> {
        if index == 0 {
            return Ok(None);
        }
        let index = check_range(index, 1, self.len() + 1)?;
        check_static(self.r#type())?;
        if index >= self.values.borrow().len() {
            return Ok(None);
        }
        // the element as a mod would read it, so a removed wrapper keeps its identity
        let value = Self::get_field(index, &self.data, &mut self.values.borrow_mut(), lua, &self.objects)?;
        self.values.borrow_mut().remove(index);
        self.dirty.set(true);
        self.objects.wrote();
        Ok(Some(value))
    }

    pub fn clear(&mut self) -> mlua::Result<()> {
        check_static(self.r#type())?;
        self.values.borrow_mut().clear();
        self.dirty.set(true);
        self.objects.wrote();
        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        if self.dirty.get() {
            return Ok(true);
        }
        for entry in self.values.borrow().iter() {
            if let Entry::Value(value) = entry
                && is_dirty_lua_value(value)?
            {
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn get_field(
        key: usize,
        data: &MappedArray,
        values: &mut [Entry],
        lua: &mlua::Lua,
        objects: &ValueObjects,
    ) -> mlua::Result<LuaValue> {
        let Some(entry) = values.get_mut(key) else {
            return Ok(LuaValue::Nil);
        };
        match entry {
            Entry::Uninitialized(index) => {
                let index = *index;
                let decoded = lazily(|| match data.get(index).map_err(LuaError::external)? {
                    Some(value) => value_to_lua(&value, lua, objects).map(Some),
                    None => Ok(None),
                })?;
                let Some(lua_value) = decoded else {
                    return Ok(LuaValue::Nil);
                };
                // every element is cached once read, scalars included
                *entry = Entry::Value(lua_value.clone());
                Ok(lua_value)
            }
            Entry::Value(value) => Ok(value.clone()),
        }
    }
}

impl UserData for MappedArrayValue {
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_function(MetaMethod::Len, |_, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            Ok(this.values.borrow().len() as i32)
        });
        methods.add_meta_function(MetaMethod::Index, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<usize>(0)?;
            this.get(key, lua)
        });
        methods.add_meta_function(MetaMethod::NewIndex, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let key = args.get::<usize>(0)?;
            let value = args.get::<LuaValue>(1)?;
            this.set(key, value, lua)
        });
        methods.add_meta_function(MetaMethod::Pairs, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            MappedArrayValueIter::create(&this, lua)
        });
        methods.add_meta_function(MetaMethod::ToString, |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            let values = this.values.borrow();
            let mut result = String::from('[');
            for (i, entry) in values.iter().enumerate() {
                if i > 0 {
                    result.push(',');
                }
                let decoded;
                let value = match entry {
                    Entry::Uninitialized(index) => {
                        decoded = lazily(|| match this.data.get(*index).map_err(LuaError::external)? {
                            Some(value) => value_to_lua(&value, lua, &this.objects),
                            None => Err(host_failure("expected a valid field in struct")),
                        })?;
                        &decoded
                    }
                    Entry::Value(value) => value,
                };
                push_display(&mut result, value)?;
            }
            result.push(']');
            Ok(LuaValue::String(lua.create_string(result)?))
        });
    }
}

/// Walks the ORIGINAL mapped length (SF), values through the live cache.
struct MappedArrayValueIter {
    data: MappedArray,
    cache: Rc<RefCell<Vec<Entry>>>,
    index: usize,
    objects: ValueObjects,
}

impl MappedArrayValueIter {
    fn create(value: &MappedArrayValue, lua: &mlua::Lua) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            data: value.data.clone(),
            cache: value.values.clone(),
            index: 0,
            objects: value.objects.clone(),
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|lua, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;
            state.step(lua)
        })?;
        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn step(&mut self, lua: &mlua::Lua) -> mlua::Result<(LuaValue, LuaValue)> {
        if self.index >= self.data.len() {
            return Ok((LuaValue::Nil, LuaValue::Nil));
        }
        let key = self.index;
        let value = MappedArrayValue::get_field(key, &self.data, &mut self.cache.borrow_mut(), lua, &self.objects)?;
        self.index += 1;
        Ok((LuaValue::Integer(key as i64 + 1), value))
    }
}

impl UserData for MappedArrayValueIter {}

pub struct MappedVariantValue {
    data: MappedVariant,
    value: RefCell<Option<LuaValue>>,
    dirty: Cell<bool>,
    objects: ValueObjects,
}

impl MappedVariantValue {
    pub fn new(data: MappedVariant, objects: ValueObjects) -> Self {
        Self {
            data,
            value: RefCell::new(None),
            dirty: Cell::new(false),
            objects,
        }
    }

    /// The variant's declared (base) type.
    pub fn r#type(&self) -> &TypeHandle {
        self.data.base_type()
    }

    pub fn variant_type(&self) -> &TypeHandle {
        self.data.variant_type()
    }

    pub fn convert(&self, converter: &mut Converter) -> Result<Value, LuaConversionErrorKind> {
        match self.value.borrow().as_ref() {
            Some(value) => {
                let variant_type = self.variant_type();
                let value = converter
                    .process(value, variant_type)?
                    .into_struct()
                    .ok_or_else(|| host_failure("variant value must be convertible to struct"))?;
                Ok(Variant {
                    type_index: variant_type.index(),
                    value,
                }
                .into())
            }
            None => converter.mapped(MappedValue::Variant(self.data.clone()), self.data.value().data().len()),
        }
    }

    /// A deep copy whose wrappers belong to `objects` (kiln-eml R1: the seam of the value it is copied into).
    pub fn try_clone_lua(&self, lua: &mlua::Lua, objects: &ValueObjects) -> mlua::Result<Self> {
        Ok(Self {
            data: self.data.clone(),
            value: RefCell::new(match self.value.borrow().as_ref() {
                Some(value) => Some(try_clone_lua_value(value, lua, objects)?),
                None => None,
            }),
            dirty: Cell::new(self.dirty.get()),
            objects: objects.clone(),
        })
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        if self.dirty.get() {
            return Ok(true);
        }
        match self.value.borrow().as_ref() {
            Some(value) => is_dirty_lua_value(value),
            None => Ok(false),
        }
    }
}

impl UserData for MappedVariantValue {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("type", |_, this| Ok(this.variant_type().qualified_name.clone()));
        fields.add_field_method_get("value", |lua, this| {
            let mut slot = this.value.borrow_mut();
            match slot.as_ref() {
                Some(value) => Ok(value.clone()),
                None => {
                    let value = value_to_lua(&MappedValue::Struct(this.data.value().clone()), lua, &this.objects)?;
                    *slot = Some(value.clone());
                    Ok(value)
                }
            }
        });
        fields.add_field_method_set("value", |lua, this, value: LuaValue| {
            let value =
                validate_and_clone(&value, this.variant_type(), lua, &this.objects).map_err(LuaError::external)?;
            this.value.replace(Some(value));
            this.dirty.set(true);
            this.objects.wrote();
            Ok(())
        });
    }
}
