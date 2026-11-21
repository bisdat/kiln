// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/mod-runtime/lua/bindings/write_mod_log.rs:8-51 (both pins);
// lua/shroudforge/v1/create_mod_api.rs:25-65 (1.7.9), :26-66 (1.8.0).
//! One lua-env-lifecycle logging implementation. The sink owns framing, CR/LF
//! escaping and file/stdout delivery. No payload byte (including NUL) is rewritten here.
use crate::host::{LogLevel, LogSink};
use mlua::{Function, Lua, Table, Value, Variadic};
use std::rc::Rc;

#[derive(Clone, Copy)]
enum Format {
    Print,
    Log,
}

pub(super) fn install(
    lua: &Lua,
    env: &Table,
    api: &Table,
    owner: &str,
    sink: Rc<dyn LogSink>,
) -> mlua::Result<()> {
    for (name, level) in [("print", LogLevel::Debug), ("warn", LogLevel::Warn)] {
        env.raw_set(
            name,
            function(lua, owner, level, Format::Print, sink.clone())?,
        )?;
    }
    let log = super::common::child(lua, api, "log")?;
    for (name, level) in [
        ("trace", LogLevel::Trace),
        ("debug", LogLevel::Debug),
        ("info", LogLevel::Info),
        ("warn", LogLevel::Warn),
        ("error", LogLevel::Error),
    ] {
        log.raw_set(
            name,
            function(lua, owner, level, Format::Log, sink.clone())?,
        )?;
    }
    Ok(())
}

fn function(
    lua: &Lua,
    owner: &str,
    level: LogLevel,
    format: Format,
    sink: Rc<dyn LogSink>,
) -> mlua::Result<Function> {
    let owner = owner.to_owned();
    lua.create_function(move |_, args: Variadic<Value>| {
        if matches!(format, Format::Log)
            && matches!(level, LogLevel::Trace | LogLevel::Debug)
            && !sink.enabled(level)
        {
            return Ok(());
        }
        let mut output = String::new();
        for (index, value) in args.into_iter().enumerate() {
            if match format {
                Format::Print => !output.is_empty(),
                Format::Log => index != 0,
            } {
                output.push('\t');
            }
            // Use the source's mlua convention, not Lua's global tostring (which mods
            // can replace), Display formatting, or the FunctionArgs conversion layer.
            output.push_str(&value.to_string()?);
        }
        if sink.enabled(level) {
            sink.write(&owner, level, output.as_bytes());
        }
        Ok(())
    })
}
