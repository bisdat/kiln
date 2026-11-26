use std::any::TypeId;
use kfc::{resource::value::Value, reflection::{PrimitiveType, TypeIndex}};
use mlua::AnyUserData;

use crate::{alias::{MappedValue, TypeHandle}, env::{value::{converter::{value_to_lua, Converter, LuaConversionError}, mapped::{MappedArrayValue, MappedStructValue, MappedVariantValue}, simple::{ArrayValue, StructValue, VariantValue}, validator::{LuaValidationError, Validator}}, writes::Writes}, lua::LuaValue};

mod converter;
mod validator;
mod mapped;
mod simple;
mod util;

pub fn convert_lua_to_value(
    lua_value: &LuaValue,
    r#type: &TypeHandle,
) -> Result<Value, LuaConversionError> {
    Converter::convert(lua_value, r#type)
}

pub fn convert_value_to_lua(
    value: &MappedValue,
    lua: &mlua::Lua,
) -> mlua::Result<LuaValue> {
    value_to_lua(value, lua)
}

pub fn validate_and_clone_lua_value(
    lua_value: &LuaValue,
    r#type: &TypeHandle,
    lua: &mlua::Lua,
) -> Result<LuaValue, LuaValidationError> {
    Validator::validate_and_clone(lua_value, r#type, lua)
}

pub fn is_dirty_lua_value(
    lua_value: &LuaValue,
) -> mlua::Result<bool> {
    match lua_value {
        LuaValue::UserData(ud) => {
            let type_id = match ud.type_id() {
                Some(type_id) => type_id,
                None => panic!("UserData has no type_id"),
            };

            match type_id {
                id if id == TypeId::of::<StructValue>() => {
                    ud.borrow::<StructValue>()?.is_dirty()
                },
                id if id == TypeId::of::<MappedStructValue>() => {
                    ud.borrow::<MappedStructValue>()?.is_dirty()
                },
                id if id == TypeId::of::<ArrayValue>() => {
                    ud.borrow::<ArrayValue>()?.is_dirty()
                }
                id if id == TypeId::of::<MappedArrayValue>() => {
                    ud.borrow::<MappedArrayValue>()?.is_dirty()
                }
                id if id == TypeId::of::<VariantValue>() => {
                    ud.borrow::<VariantValue>()?.is_dirty()
                },
                id if id == TypeId::of::<MappedVariantValue>() => {
                    ud.borrow::<MappedVariantValue>()?.is_dirty()
                },
                _ => Ok(false)
            }
        },
        _ => Ok(false)
    }
}

/// kiln-eml: the writes logged in `lua_value`'s tree, as (writer, member path from `path`) (env/writes.rs). A plain value
/// logs nothing of its own: its writes are its container's. `path` is the walk's one buffer (writes::push_field): it is
/// as it was when the walk returns.
pub(crate) fn walk_writes(
    lua_value: &LuaValue,
    path: &mut String,
    out: &mut Vec<(u32, String)>,
) -> mlua::Result<()> {
    let LuaValue::UserData(ud) = lua_value else { return Ok(()) };
    match ud.type_id() {
        Some(id) if id == TypeId::of::<StructValue>() => ud.borrow::<StructValue>()?.walk_writes(path, out),
        Some(id) if id == TypeId::of::<MappedStructValue>() => ud.borrow::<MappedStructValue>()?.walk_writes(path, out),
        Some(id) if id == TypeId::of::<ArrayValue>() => ud.borrow::<ArrayValue>()?.walk_writes(path, out),
        Some(id) if id == TypeId::of::<MappedArrayValue>() => ud.borrow::<MappedArrayValue>()?.walk_writes(path, out),
        Some(id) if id == TypeId::of::<VariantValue>() => ud.borrow::<VariantValue>()?.walk_writes(path, out),
        Some(id) if id == TypeId::of::<MappedVariantValue>() => ud.borrow::<MappedVariantValue>()?.walk_writes(path, out),
        _ => Ok(()),
    }
}

/// kiln-eml: an array element `value` was put in place (set or inserted): a value with a log of its own is written whole
/// there (its later index is where it ends up), keeping `kept` (the writes logged in the element it replaced,
/// [`replaced_writes`]); a plain value is a write of the list's members.
pub(crate) fn note_put(
    list: &Writes,
    value: &LuaValue,
    kept: &[(u32, String)],
) -> mlua::Result<()> {
    let LuaValue::UserData(ud) = value else {
        list.items();
        return Ok(());
    };
    let put = |writes: &Writes| {
        writes.whole();
        writes.keep(kept);
    };
    match ud.type_id() {
        Some(id) if id == TypeId::of::<StructValue>() => put(ud.borrow::<StructValue>()?.writes()),
        Some(id) if id == TypeId::of::<MappedStructValue>() => put(ud.borrow::<MappedStructValue>()?.writes()),
        Some(id) if id == TypeId::of::<ArrayValue>() => put(ud.borrow::<ArrayValue>()?.writes()),
        Some(id) if id == TypeId::of::<MappedArrayValue>() => put(ud.borrow::<MappedArrayValue>()?.writes()),
        Some(id) if id == TypeId::of::<VariantValue>() => put(ud.borrow::<VariantValue>()?.writes()),
        Some(id) if id == TypeId::of::<MappedVariantValue>() => put(ud.borrow::<MappedVariantValue>()?.writes()),
        _ => list.items(),
    }
    Ok(())
}

/// kiln-eml: the writes logged in `old`'s tree, a value about to be replaced, as (writer, path from `path`): what the
/// value put in its place keeps (env/writes.rs `Member::Kept`). Nothing for a plain value or outside a run.
pub(crate) fn replaced_writes(
    old: Option<&LuaValue>,
    path: &str,
) -> mlua::Result<Vec<(u32, String)>> {
    let mut out = Vec::new();
    if let Some(old @ LuaValue::UserData(_)) = old
        && crate::env::writes::recording()
    {
        walk_writes(old, &mut path.to_owned(), &mut out)?;
    }
    Ok(out)
}

pub fn push(
    ud: &AnyUserData,
    value: &LuaValue,
    lua: &mlua::Lua,
) -> mlua::Result<bool> {
    let type_id = match ud.type_id() {
        Some(type_id) => type_id,
        None => panic!("UserData has no type_id"),
    };

    match type_id {
        id if id == TypeId::of::<ArrayValue>() => {
            let mut array_value = ud.borrow_mut::<ArrayValue>()?;
            let len = array_value.len();

            array_value.insert(len + 1, value, lua)
                .map(|_| true)
        },
        id if id == TypeId::of::<MappedArrayValue>() => {
            let mut mapped_array_value = ud.borrow_mut::<MappedArrayValue>()?;
            let len = mapped_array_value.len();

            mapped_array_value.insert(len + 1, value, lua)
                .map(|_| true)
        },
        _ => Ok(false)
    }
}

pub fn insert(
    ud: &AnyUserData,
    pos: usize,
    value: &LuaValue,
    lua: &mlua::Lua,
) -> mlua::Result<bool> {
    let type_id = match ud.type_id() {
        Some(type_id) => type_id,
        None => panic!("UserData has no type_id"),
    };

    match type_id {
        id if id == TypeId::of::<ArrayValue>() => {
            ud.borrow_mut::<ArrayValue>()?.insert(pos, value, lua)
                .map(|_| true)
        },
        id if id == TypeId::of::<MappedArrayValue>() => {
            ud.borrow_mut::<MappedArrayValue>()?.insert(pos, value, lua)
                .map(|_| true)
        },
        _ => Ok(false)
    }
}

pub fn pop(
    ud: &AnyUserData,
    lua: &mlua::Lua,
) -> mlua::Result<(Option<LuaValue>, bool)> {
    let type_id = match ud.type_id() {
        Some(type_id) => type_id,
        None => panic!("UserData has no type_id"),
    };

    match type_id {
        id if id == TypeId::of::<ArrayValue>() => {
            let mut array_value = ud.borrow_mut::<ArrayValue>()?;
            let len = array_value.len();

            array_value.remove(len)
                .map(|value| (value, true))
        },
        id if id == TypeId::of::<MappedArrayValue>() => {
            let mut mapped_array_value = ud.borrow_mut::<MappedArrayValue>()?;
            let len = mapped_array_value.len();

            mapped_array_value.remove(len, lua)
                .map(|value| (value, true))
        },
        _ => Ok((None, false))
    }
}

pub fn remove(
    ud: &AnyUserData,
    pos: usize,
    lua: &mlua::Lua,
) -> mlua::Result<(Option<LuaValue>, bool)> {
    let type_id = match ud.type_id() {
        Some(type_id) => type_id,
        None => panic!("UserData has no type_id"),
    };

    match type_id {
        id if id == TypeId::of::<ArrayValue>() => {
            ud.borrow_mut::<ArrayValue>()?.remove(pos)
                .map(|value| (value, true))
        },
        id if id == TypeId::of::<MappedArrayValue>() => {
            ud.borrow_mut::<MappedArrayValue>()?.remove(pos, lua)
                .map(|value| (value, true))
        },
        _ => Ok((None, false))
    }
}

pub fn clear(
    ud: &AnyUserData,
) -> mlua::Result<bool> {
    let type_id = match ud.type_id() {
        Some(type_id) => type_id,
        None => panic!("UserData has no type_id"),
    };

    match type_id {
        id if id == TypeId::of::<ArrayValue>() => {
            ud.borrow_mut::<ArrayValue>()?.clear()
                .map(|_| true)
        },
        id if id == TypeId::of::<MappedArrayValue>() => {
            ud.borrow_mut::<MappedArrayValue>()?.clear()
                .map(|_| true)
        },
        _ => Ok(false)
    }
}

pub fn type_of(
    lua_value: &LuaValue,
) -> mlua::Result<Option<TypeIndex>> {
    match lua_value {
        LuaValue::UserData(ud) => {
            let type_id = match ud.type_id() {
                Some(type_id) => type_id,
                None => panic!("UserData has no type_id"),
            };

            match type_id {
                id if id == TypeId::of::<StructValue>() => {
                    let struct_value = ud.borrow::<StructValue>()?;
                    let r#type = struct_value.r#type();

                    Ok(Some(r#type.index()))
                },
                id if id == TypeId::of::<MappedStructValue>() => {
                    let mapped_struct_value = ud.borrow::<MappedStructValue>()?;
                    let r#type = mapped_struct_value.r#type();

                    Ok(Some(r#type.index()))
                },
                id if id == TypeId::of::<ArrayValue>() => {
                    let array_value = ud.borrow::<ArrayValue>()?;
                    let r#type = array_value.r#type();

                    Ok(Some(r#type.index()))
                }
                id if id == TypeId::of::<MappedArrayValue>() => {
                    let mapped_array_value = ud.borrow::<MappedArrayValue>()?;
                    let r#type = mapped_array_value.r#type();

                    Ok(Some(r#type.index()))
                }
                id if id == TypeId::of::<VariantValue>() => {
                    let variant_value = ud.borrow::<VariantValue>()?;
                    let r#type = variant_value.r#type();

                    Ok(Some(r#type.index()))
                },
                id if id == TypeId::of::<MappedVariantValue>() => {
                    let mapped_variant_value = ud.borrow::<MappedVariantValue>()?;
                    let r#type = mapped_variant_value.r#type();

                    Ok(Some(r#type.index()))
                },
                _ => Ok(None)
            }
        },
        _ => Ok(None)
    }
}

fn name_of(
    r#type: &TypeHandle,
) -> String {
    if r#type.primitive_type == PrimitiveType::Typedef {
        name_of(
            &r#type.inner_type()
                .expect("invalid typedef type"),
        )
    } else {
        r#type.qualified_name.clone()
    }
}

#[cfg(test)]
mod tests;
