// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/read_component.rs:3-74 (read, read_bytes),
// lua/ingame/check_ingame_access.rs:347-357 (the unavailable result), game-runtime/bridge/windows/ecs/
// read_component.rs:4-19 (the provider's read: exactly the registered size, or nothing).
//! `runtime.ecs.read` and `read_bytes`.
//!
//! The denial, then the entity (a handle), the component (ShroudForge's type selector, a `keen::ecs::` type the
//! registry join resolved), the archive registry's metadata of it and the size proof, then one stable copy of the
//! component's bytes through kiln.world's `ecs_snapshot` (validated before and after; an unknown or stale handle, a
//! missing component or a copy kiln.world refuses is ShroudForge's "component read failed: <name>").
//! `read_bytes` returns the bytes as a string; `read` decodes them as the component's type with the VM's typed values
//! (Source's `MappedValue::from_bytes`, kiln.eml.values' BYTES origin): a mapped value whose `runtime.ecs.write` source
//! bytes are the bytes read. A decoding error is raised, as ShroudForge raises it.

use super::entities::text::COMPONENT_UNAVAILABLE;
use super::{Ecs, runtime_type_name};
use mlua::{Lua, Value};
use sfi_core::bindings::runtime::{Availability, RuntimeHost, RuntimeOperation as Op};
use sfi_core::lua::FunctionArgs;

type Reply = (Value, Option<String>);

/// `runtime.ecs.read(entity, component)`.
pub fn read(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    read_component(lua, ecs, host, args, false)
}

/// `runtime.ecs.read_bytes(entity, component)`.
pub fn read_bytes(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    read_component(lua, ecs, host, args, true)
}

/// The component's bytes (its resolved hash and size), the type's name, or the refusal.
pub(crate) enum Read {
    Bytes { name: String, bytes: Vec<u8> },
    Refused(String),
}

/// read_component.rs:25-66: everything `read` and `read_bytes` share up to the bytes.
pub(crate) fn component_bytes(
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Read> {
    if let Some(reason) = ecs.denial_reason(host, Op::EcsRead) {
        return Ok(Read::Refused(reason));
    }
    // `unavailable_runtime_result`: only reached with the operation outside the contract, which the denial above
    // already gave
    if let Availability::Unavailable { reason } = host.static_support(Op::EcsRead) {
        return Ok(Read::Refused(reason));
    }
    let entity = args.get::<u32>(0)?;
    let name = runtime_type_name(ecs, args, 1)?;
    let Some(component) = ecs.resolve_component(&name) else {
        return Ok(Read::Refused(format!("{COMPONENT_UNAVAILABLE}{name}")));
    };
    let Some(metadata) = ecs.types().by_qualified_name(&name) else {
        return Ok(Read::Refused(format!("type metadata unavailable: {name}")));
    };
    if metadata.size != component.size {
        return Ok(Read::Refused(format!(
            "component layout mismatch for {name}: parser={}, runtime={}",
            metadata.size, component.size
        )));
    }
    match ecs.snapshot(entity, component.component, component.size) {
        Some(bytes) => Ok(Read::Bytes { name, bytes }),
        None => Ok(Read::Refused(format!("component read failed: {name}"))),
    }
}

fn read_component(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
    raw: bool,
) -> mlua::Result<Reply> {
    let (name, bytes) = match component_bytes(ecs, host, args)? {
        Read::Bytes { name, bytes } => (name, bytes),
        Read::Refused(reason) => return Ok((Value::Nil, Some(reason))),
    };
    if raw {
        return Ok((Value::String(lua.create_string(&bytes)?), None));
    }
    let value = ecs.values()?.open_bytes(lua, &name, &bytes)?;
    Ok((value, None))
}
