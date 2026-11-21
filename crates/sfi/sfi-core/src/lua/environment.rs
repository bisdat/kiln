// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/create_sandbox.rs:31-125;
// lua/bindings/mod.rs:66-115 and lua/bindings/extend_table_operations.rs:10-114.
use super::{Dispatch, Either, FunctionArgs, TypedTablePort};
use mlua::{AnyUserData, Function, Lua, MetaMethod, Table, Value};
use std::rc::Rc;

pub(crate) fn create(
    lua: &Lua,
    typed_tables: Option<Rc<dyn TypedTablePort>>,
) -> mlua::Result<Table> {
    let env = lua.create_table()?;
    let globals = lua.globals();
    for name in ["utf8", "math", "string", "table", "coroutine"] {
        env.raw_set(name, copy_table(lua, &globals.raw_get::<Table>(name)?)?)?;
    }
    for name in [
        "collectgarbage",
        "type",
        "rawlen",
        "rawget",
        "rawset",
        "rawequal",
        "assert",
        "pcall",
        "xpcall",
        "setmetatable",
        "getmetatable",
        "error",
        "ipairs",
        "pairs",
        "next",
        "select",
        "tostring",
        "tonumber",
    ] {
        env.raw_set(name, globals.raw_get::<Value>(name)?)?;
    }
    env.raw_set("_G", env.clone())?;
    env.raw_set("builtin", lua.create_table()?)?;
    let type_fn: Function = env.raw_get("type")?;
    env.raw_set(
        "typeof",
        lua.create_function(move |_, value: Value| {
            if let Value::UserData(data) = &value {
                let tag = data.metatable()?.get::<Value>(MetaMethod::Type)?;
                if matches!(tag, Value::String(_)) {
                    return Ok(tag);
                }
            }
            type_fn.call::<Value>(value)
        })?,
    )?;
    extend_tables(lua, &env.raw_get::<Table>("table")?, typed_tables)?;
    install_namespace(&env, "buffer", crate::eml::buffer::create(lua)?)?;
    install_namespace(&env, "hasher", crate::eml::hasher::create(lua)?)?;
    crate::eml::guid::install(lua, &env)?;
    crate::eml::image::install(lua, &env)?;
    Ok(env)
}

fn copy_table(lua: &Lua, source: &Table) -> mlua::Result<Table> {
    let result = lua.create_table()?;
    for entry in source.pairs::<Value, Value>() {
        let (key, value) = entry?;
        match value {
            Value::Table(child) => result.raw_set(key, copy_table(lua, &child)?)?,
            value => result.raw_set(key, value)?,
        }
    }
    Ok(result)
}

/// Install a real family-owned API table and its builtin alias, without copying children.
/// This is registration, not a proxy: later slot assignments do not follow one another.
pub fn install_namespace(env: &Table, name: &str, table: Table) -> mlua::Result<()> {
    env.raw_get::<Table>("builtin")?
        .raw_set(name, table.clone())?;
    env.raw_set(name, table)
}

// One source dispatcher; the finite port owns only representation-specific substance.
// Capture the original stock functions, never another package's wrapped closures.
fn extend_tables(
    lua: &Lua,
    table: &Table,
    port: Option<Rc<dyn TypedTablePort>>,
) -> mlua::Result<()> {
    let insert: Function = table.get("insert")?;
    let remove: Function = table.get("remove")?;
    let insert_port = port.clone();
    let remove_port = port.clone();
    table.set(
        "insert",
        lua.create_function(move |lua, args: FunctionArgs| match args.len() {
            2.. => {
                if let Value::UserData(ud) = args.get::<Value>(0)? {
                    // SF converts the position/value BEFORE provider recognition, even
                    // for unsupported userdata. Extra insert args are source-ignored.
                    let handled = if args.len() == 2 {
                        let value = args.get::<Value>(1)?;
                        match &insert_port {
                            Some(port) => port.push(lua, ud.clone(), value.clone())?,
                            None => Dispatch::NotHandled,
                        }
                    } else {
                        let position = args.get::<usize>(1)?;
                        let value = args.get::<Value>(2)?;
                        match &insert_port {
                            Some(port) => port.insert(lua, ud.clone(), position, value.clone())?,
                            None => Dispatch::NotHandled,
                        }
                    };
                    if let Dispatch::Handled(()) = handled {
                        return Ok(Value::Nil);
                    }
                }
                insert.call::<Value>(args)
            }
            _ => Err(mlua::Error::runtime(
                "wrong number of arguments to 'insert'",
            )),
        })?,
    )?;
    table.set(
        "remove",
        lua.create_function(move |lua, args: FunctionArgs| match args.len() {
            1 | 2 => {
                if let Value::UserData(ud) = args.get::<Value>(0)? {
                    let handled = if args.len() == 1 {
                        match &remove_port {
                            Some(port) => port.pop(lua, ud.clone())?,
                            None => Dispatch::NotHandled,
                        }
                    } else {
                        let position = args.get::<usize>(1)?;
                        match &remove_port {
                            Some(port) => port.remove(lua, ud.clone(), position)?,
                            None => Dispatch::NotHandled,
                        }
                    };
                    if let Dispatch::Handled(value) = handled {
                        return Ok(value);
                    }
                }
                remove.call::<Value>(args)
            }
            _ => Err(mlua::Error::runtime(
                "wrong number of arguments to 'remove'",
            )),
        })?,
    )?;
    table.set(
        "clear",
        lua.create_function(move |lua, args: FunctionArgs| {
            match args.get::<Either<AnyUserData, Table>>(0)? {
                Either::A(ud) => {
                    if let Some(port) = &port
                        && let Dispatch::Handled(()) = port.clear(lua, ud.clone())?
                    {
                        return Ok(Value::Nil);
                    }
                    Err(mlua::Error::runtime(format!(
                        "'clear' is not supported for userdata `{:?}`",
                        args.get::<Value>(0)?.type_name()
                    )))
                }
                Either::B(table) => {
                    table.clear()?;
                    Ok(Value::Nil)
                }
            }
        })?,
    )?;
    Ok(())
}
