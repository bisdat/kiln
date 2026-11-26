use std::{cell::RefCell, collections::HashMap, ops::Deref, rc::Rc};

use indexmap::IndexMap;
use kfc::{resource::value::{Value, Variant}, reflection::{PrimitiveType, TypeMetadata}};
use mlua::{AnyUserData, Function, MetaMethod, UserData};
use ouroboros::self_referencing;

use crate::{alias::TypeHandle, env::{game::value::{Member, graft, note_put, walk_writes}, value::{converter::{Converter, LuaConversionErrorKind}, name_of, validate_and_clone_lua_value, validator::try_clone_lua_value}, writes::{Writes, push_field, push_index}}, lua::{FunctionArgs, LuaError, LuaValue, MethodArgs}};

pub struct StructValue {
    r#type: TypeHandle,
    fields: Rc<RefCell<HashMap<String, LuaValue>>>,
    // kiln-eml: the writes made through this value, for the change journal (env/writes.rs)
    writes: Writes,
}

impl StructValue {

    /// Creates a new `StructValue` with the given type and a pre-populated map.
    ///
    /// # Contract
    ///
    /// The provided `data` must adhere to the type's structure,
    /// otherwise it will lead to unexpected behavior.
    #[inline]
    pub fn new_with_data(
        r#type: TypeHandle,
        map: HashMap<String, LuaValue>,
    ) -> mlua::Result<Self> {
        Ok(Self {
            r#type,
            fields: Rc::new(RefCell::new(map)),
            writes: Writes::default(),
        })
    }

    #[inline]
    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    pub fn convert(
        &self,
        converter: &mut Converter,
    ) -> Result<Value, LuaConversionErrorKind> {
        let fields = self.fields.borrow();
        let mut result = IndexMap::with_capacity(fields.len());

        for (key, value) in fields.iter() {
            converter.path.push(key);

            let field_type = self.r#type.get_field_type(key)
                .expect("field type must be present");
            let converted_value = converter.process(
                value,
                &field_type,
            )?;

            result.insert(key.clone(), converted_value);

            converter.path.pop();
        }

        Ok(result.into())
    }

    pub fn try_clone_lua(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<Self> {
        let table = self.fields.borrow();
        let mut fields = HashMap::with_capacity(table.len());

        for (key, value) in table.iter() {
            fields.insert(
                key.clone(),
                try_clone_lua_value(value, lua)?
            );
        }

        Ok(Self {
            r#type: self.r#type.clone(),
            fields: Rc::new(RefCell::new(fields)),
            writes: Writes::default(), // kiln-eml
        })
    }

    pub fn is(
        &self,
        r#type: &TypeMetadata,
    ) -> bool {
        self.r#type.deref() == r#type
    }

    pub fn get(
        &self,
        key: String,
    ) -> mlua::Result<LuaValue> {
        Ok(self.fields.borrow().get(&key).cloned().unwrap_or(LuaValue::Nil))
    }

    pub fn set(
        &self,
        key: String,
        value: &LuaValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<()> {
        let field_type = match self.r#type.get_field_type(&key) {
            Some(field_type) if self.fields.borrow().contains_key(&key) => field_type,
            _ => {
                return Err(LuaError::generic(format!(
                    "attempt to set invalid property '{key}' on {}",
                    name_of(&self.r#type)
                )));
            }
        };

        // the field's type, as MappedStructValue::set checks it (not the struct's own type)
        let value = validate_and_clone_lua_value(value, &field_type, lua)
            .map_err(LuaError::external)?;

        // kiln-eml: the writes logged in the value replaced move onto the one put in its place (env/writes.rs)
        let kept = graft(self.fields.borrow().get(&key), &value, &key)?;
        self.writes.field(&key); // kiln-eml
        self.writes.keep(&kept); // kiln-eml
        self.fields.borrow_mut().insert(key, value);

        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }

    /// kiln-eml: this value was put in place whole (note_put).
    pub(crate) fn writes(&self) -> &Writes {
        &self.writes
    }

    /// kiln-eml: the fields that are wrappers, and one of them (graft).
    pub(crate) fn members(&self) -> Vec<(Member, LuaValue)> {
        self.fields.borrow().iter().filter(|(_, v)| v.is_userdata()).map(|(k, v)| (Member::Key(k.clone()), v.clone())).collect()
    }
    pub(crate) fn member(&self, at: &Member) -> Option<LuaValue> {
        match at {
            Member::Key(key) => self.fields.borrow().get(key).cloned(),
            _ => None,
        }
    }

    /// kiln-eml: this value's logged writes and its fields' (env/writes.rs), `path` being its own path.
    pub(crate) fn walk_writes(&self, path: &mut String, out: &mut Vec<(u32, String)>) -> mlua::Result<()> {
        self.writes.emit(path, out);
        for (key, value) in self.fields.borrow().iter() {
            if let LuaValue::UserData(_) = value {
                let len = push_field(path, key);
                let walked = walk_writes(value, path, out);
                path.truncate(len);
                walked?;
            }
        }
        Ok(())
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
            let mut result = String::new();

            result.push('{');

            for (i, (key, value)) in values.iter().enumerate() {
                if i > 0 {
                    result.push(',');
                }

                result.push('"');
                result.push_str(key);
                result.push_str("\":");
                // Add quotes to value if it is a string
                if value.is_string() {
                    result.push('"');
                    (result).push_str(&value.to_string()?);
                    result.push('"');
                }
                else {
                    (result).push_str(&value.to_string()?);
                }
            }

            result.push('}');

            Ok(LuaValue::String(lua.create_string(result)?))
        });
    }

}

#[self_referencing]
struct StructValueIter {
    r#type: TypeHandle,
    fields: Rc<RefCell<HashMap<String, LuaValue>>>,
    #[borrows(r#type)]
    #[not_covariant]
    keys: RefCell<Box<dyn Iterator<Item = &'this str> + 'this>>,
}

impl StructValueIter {

    fn create(
        value: &StructValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self::new(
            value.r#type.clone(),
            value.fields.clone(),
            |r#type| {
                let iter = r#type.iter_fields()
                    .map(|field| field.name.as_str());

                RefCell::new(Box::new(iter))
            }
        );
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|lua, args: FunctionArgs| {
            let state = args.get::<&Self>(0)?;

            state.next(lua)
        })?;

        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn next(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<(LuaValue, LuaValue)> {
        let key = self.with_keys(|keys| {
            keys.borrow_mut().next()
        });

        let key = match key {
            Some(k) => k,
            None => return Ok((LuaValue::Nil, LuaValue::Nil)),
        };

        let value = self.with_fields(|fields| {
            fields.borrow()
                .get(key)
                .cloned()
                .unwrap_or(LuaValue::Nil)
        });

        Ok((LuaValue::String(lua.create_string(key)?), value))
    }

}

impl UserData for StructValueIter {}

pub struct ArrayValue {
    r#type: TypeHandle,
    inner_type: TypeHandle,
    values: Rc<RefCell<Vec<LuaValue>>>,
    // kiln-eml: the writes made through this value, for the change journal (env/writes.rs)
    writes: Writes,
}

impl ArrayValue {

    /// Creates a new `ArrayValue` with the given type and a pre-populated array.
    ///
    /// # Contract
    ///
    /// The provided `data` must adhere to the type's structure,
    /// otherwise it will lead to unexpected behavior.
    #[inline]
    pub fn new_with_data(
        r#type: TypeHandle,
        data: Vec<LuaValue>,
    ) -> mlua::Result<Self> {
        Ok(Self {
            inner_type: r#type.inner_type()
                .expect("array type must have an inner type"),
            r#type,
            values: Rc::new(RefCell::new(data)),
            writes: Writes::default(),
        })
    }

    #[inline]
    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    pub fn convert(
        &self,
        converter: &mut Converter,
    ) -> Result<Value, LuaConversionErrorKind> {
        let values = self.values.borrow();
        let mut result = Vec::with_capacity(values.len());

        let field_type = self.r#type.inner_type()
            .expect("array type must have an inner type");

        for (i, value) in values.iter().enumerate() {
            converter.path.push_index(i);

            let converted_value = converter.process(
                value,
                &field_type,
            )?;
            result.push(converted_value);

            converter.path.pop();
        }

        Ok(result.into())
    }

    pub fn try_clone_lua(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<Self> {
        let values = self.values.borrow();
        let mut result = Vec::with_capacity(values.len());

        for value in values.iter() {
            result.push(try_clone_lua_value(value, lua)?);
        }

        Ok(Self {
            r#type: self.r#type.clone(),
            inner_type: self.inner_type.clone(),
            values: Rc::new(RefCell::new(result)),
            writes: Writes::default(), // kiln-eml
        })
    }

    pub fn is(
        &self,
        r#type: &TypeMetadata,
    ) -> bool {
        self.r#type.deref() == r#type
    }

    pub fn len(&self) -> usize {
        self.values.borrow().len()
    }

    pub fn get(
        &self,
        index: usize,
    ) -> mlua::Result<LuaValue> {
        if index == 0 {
            return Ok(LuaValue::Nil);
        }

        let value = self.values.borrow().get(index - 1)
            .cloned()
            .unwrap_or(LuaValue::Nil);

        Ok(value)
    }

    pub fn set(
        &self,
        index: usize,
        value: &LuaValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<()> {
        let index = self.check_range(index, 1, self.len())?;

        // the element type, as MappedArrayValue::set checks it (not the array's own type)
        let value = validate_and_clone_lua_value(value, &self.inner_type, lua)
            .map_err(LuaError::external)?;

        // kiln-eml: the writes logged in the element replaced move onto the one put in its place (env/writes.rs)
        let kept = graft(self.values.borrow().get(index), &value, &format!("[{index}]"))?;
        self.writes.keep(&kept);
        note_put(&self.writes, &value)?; // kiln-eml
        self.values.borrow_mut()[index] = value;

        Ok(())
    }

    pub fn insert(
        &mut self,
        index: usize,
        value: &LuaValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<()> {
        let index = self.check_range(index, 1, self.len() + 1)?;
        self.check_static()?;

        let value = validate_and_clone_lua_value(value, &self.inner_type, lua)
            .map_err(LuaError::external)?;

        note_put(&self.writes, &value)?; // kiln-eml
        self.writes.items();
        self.values.borrow_mut().insert(index, value);

        Ok(())
    }

    pub fn remove(
        &mut self,
        index: usize,
    ) -> mlua::Result<Option<LuaValue>> {
        if index == 0 {
            return Ok(None);
        }

        let index = self.check_range(index, 1, self.len() + 1)?;
        self.check_static()?;

        if index >= self.values.borrow().len() {
            return Ok(None);
        }

        self.writes.items(); // kiln-eml
        Ok(Some(self.values.borrow_mut().remove(index)))
    }

    pub fn clear(&mut self) -> mlua::Result<()> {
        self.check_static()?;

        self.values.borrow_mut().clear();
        self.writes.items(); // kiln-eml

        Ok(())
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }

    /// kiln-eml: this value was put in place whole (note_put).
    pub(crate) fn writes(&self) -> &Writes {
        &self.writes
    }

    /// kiln-eml: the elements that are wrappers, and one of them (graft).
    pub(crate) fn members(&self) -> Vec<(Member, LuaValue)> {
        self.values.borrow().iter().enumerate().filter(|(_, v)| v.is_userdata()).map(|(i, v)| (Member::Index(i), v.clone())).collect()
    }
    pub(crate) fn member(&self, at: &Member) -> Option<LuaValue> {
        match at {
            Member::Index(i) => self.values.borrow().get(*i).cloned(),
            _ => None,
        }
    }

    /// kiln-eml: this value's logged writes and its elements' (env/writes.rs), at their current indices.
    pub(crate) fn walk_writes(&self, path: &mut String, out: &mut Vec<(u32, String)>) -> mlua::Result<()> {
        self.writes.emit(path, out);
        for (i, value) in self.values.borrow().iter().enumerate() {
            if let LuaValue::UserData(_) = value {
                let len = push_index(path, i);
                let walked = walk_writes(value, path, out);
                path.truncate(len);
                walked?;
            }
        }
        Ok(())
    }

    fn check_range(
        &self,
        index: usize,
        start: usize,
        end: usize,
    ) -> mlua::Result<usize> {
        if index == 0 || index > end {
            Err(LuaError::position_out_of_bounds(
                index,
                start,
                end,
            ))
        } else {
            Ok(index - 1)
        }
    }

    fn check_static(
        &self,
    ) -> mlua::Result<()> {
        if self.r#type.primitive_type == PrimitiveType::StaticArray {
            Err(LuaError::generic("attempt to modify static array"))
        } else {
            Ok(())
        }
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
            let mut result = String::new();

            result.push('[');

            for (i, value) in values.iter().enumerate() {
                if i > 0 {
                    result.push(',');
                }

                // Add quotes to value if it is a string
                if value.is_string() {
                    result.push('"');
                    (result).push_str(&value.to_string()?);
                    result.push('"');
                }
                else {
                    (result).push_str(&value.to_string()?);
                }
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

    fn create(
        value: &ArrayValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<(Function, AnyUserData, LuaValue)> {
        let state = Self {
            values: value.values.clone(),
            index: 0,
        };
        let ud = lua.create_userdata(state)?;
        let iter_fn = lua.create_function(|_, args: FunctionArgs| {
            let mut state = args.get::<&mut Self>(0)?;

            state.next()
        })?;

        Ok((iter_fn, ud, LuaValue::Nil))
    }

    fn next(
        &mut self,
    ) -> mlua::Result<(LuaValue, LuaValue)> {
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
    // kiln-eml: the writes made through this value, for the change journal (env/writes.rs)
    writes: Writes,
}

impl VariantValue {

    /// Creates a new `VariantValue` with the given types and a value.
    ///
    /// # Contract
    ///
    /// The provided `data` must adhere to the type's structure,
    /// otherwise it will lead to unexpected behavior.
    #[inline]
    pub fn new_with_data(
        r#type: TypeHandle,
        variant_type: TypeHandle,
        value: LuaValue,
    ) -> mlua::Result<Self> {
        Ok(Self {
            r#type,
            variant_type,
            value,
            writes: Writes::default(),
        })
    }

    #[inline]
    pub fn r#type(&self) -> &TypeHandle {
        &self.r#type
    }

    #[inline]
    pub fn variant_type(&self) -> &TypeHandle {
        &self.variant_type
    }

    pub fn convert(
        &self,
        converter: &mut Converter,
    ) -> Result<Value, LuaConversionErrorKind> {
        let value = converter.process(
            &self.value,
            &self.variant_type,
        )?
            .into_struct()
            .expect("variant value must be convertible to struct");

        Ok(Variant {
            type_index: self.variant_type.index,
            value,
        }.into())
    }

    pub fn try_clone_lua(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<Self> {
        Ok(Self {
            r#type: self.r#type.clone(),
            variant_type: self.variant_type.clone(),
            value: try_clone_lua_value(&self.value, lua)?,
            writes: Writes::default(), // kiln-eml
        })
    }

    pub fn is(
        &self,
        r#type: &TypeMetadata,
    ) -> bool {
        self.r#type.deref() == r#type
    }

    pub fn is_dirty(&self) -> mlua::Result<bool> {
        Ok(true)
    }

    /// kiln-eml: this value was put in place whole (note_put).
    pub(crate) fn writes(&self) -> &Writes {
        &self.writes
    }

    /// kiln-eml: its value when a wrapper, at its own path (graft).
    pub(crate) fn members(&self) -> Vec<(Member, LuaValue)> {
        if self.value.is_userdata() { vec![(Member::Same, self.value.clone())] } else { Vec::new() }
    }
    pub(crate) fn member(&self, at: &Member) -> Option<LuaValue> {
        matches!(at, Member::Same).then(|| self.value.clone())
    }

    /// kiln-eml: this value's logged writes and its value's (env/writes.rs): a variant's fields are at its own path.
    pub(crate) fn walk_writes(&self, path: &mut String, out: &mut Vec<(u32, String)>) -> mlua::Result<()> {
        self.writes.emit(path, out);
        walk_writes(&self.value, path, out)
    }

}

impl UserData for VariantValue {

    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("type", |_, this| Ok(this.variant_type().qualified_name.clone()));
        fields.add_field_method_get("value", |_, this| Ok(this.value.clone()));

        fields.add_field_method_set("value", |lua, this, value: LuaValue| {
            let value = validate_and_clone_lua_value(&value, &this.variant_type, lua)
                .map_err(LuaError::external)?;
            // kiln-eml: the writes logged in the value replaced move onto the new one (its fields are at the variant's path)
            let kept = graft(Some(&this.value), &value, "")?;
            this.value = value;
            this.writes.whole(); // kiln-eml
            this.writes.keep(&kept); // kiln-eml

            Ok(())
        });
    }

}
