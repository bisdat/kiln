// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.ecs` in the pregame VM (lua/ingame/ecs/mod.rs:19-39). Live operations refuse with the denial in their own
//! result shapes; `get_catalog` lists the registry's `keen::ecs::*` types with every live fact false (no provider);
//! `get_registry` and `get_component` answer the provider's missing registry; the attribute catalog members read the
//! archive's catalog (the attribute catalog, [`super::attributes`]). Adapted from ShroudForge 1.7.9 (MIT,
//! Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs): ecs/{read_component_registry,read_component_catalog,
//! query_entities,resolve_entity,read_component,access_component_fields,write_component,access_template_component}.rs
//! and ecs/attributes/{register_attribute_api,read_attributes,update_attribute,read_attribute,write_attribute_storage,
//! read_attribute_catalog,read_attribute_descriptor,evaluate_attributes}.rs.
use mlua::{Lua, MultiValue, Table};
use sfi_core::lua::{FunctionArgs, LuaValue};

use super::{
    PregameRuntime,
    access::{REGISTRY_MISSING, add, denial, past_denial},
    attributes,
    values::validate_owned_type,
};
use crate::sf_startup::game::resolve_type::resolve;

type Reason = (LuaValue, Option<String>);

/// A live operation whose source returns `nil, reason` on its denial, checked before any argument.
fn nil_reason(
    lua: &Lua,
    table: &Table,
    context: &PregameRuntime,
    name: &str,
    operation: &'static str,
) -> mlua::Result<()> {
    let host = context.host.clone();
    let path = format!("runtime.ecs.{name}");
    add(lua, table, name, move |_, _: FunctionArgs| -> mlua::Result<Reason> {
        match denial(host.as_ref(), operation) {
            Some(reason) => Ok((LuaValue::Nil, Some(reason))),
            None => Err(past_denial(&path)),
        }
    })
}

/// The attribute bindings take raw multi-values (register_attribute_api.rs:10-51); their denial comes first. Their
/// failure value is `nil`, except `write_attribute_storage`'s `false` (write_attribute_storage.rs:75-78).
fn attribute_reason(
    lua: &Lua,
    table: &Table,
    context: &PregameRuntime,
    name: &str,
    operation: &'static str,
    failure: LuaValue,
) -> mlua::Result<()> {
    let host = context.host.clone();
    let path = format!("runtime.ecs.{name}");
    table.raw_set(
        name,
        lua.create_function(move |_, _: MultiValue| -> mlua::Result<Reason> {
            match denial(host.as_ref(), operation) {
                Some(reason) => Ok((failure.clone(), Some(reason))),
                None => Err(past_denial(&path)),
            }
        })?,
    )
}

pub(super) fn create(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    // read_component_registry.rs:3-13: the query denial, then the provider's readiness
    nil_reason(lua, &table, context, "get_components", "runtime.ecs.query")?;
    let c = context.clone();
    add(lua, &table, "get_catalog", move |lua, _| catalog(lua, &c))?;
    // read_component_registry.rs:109-127: no phase check; the provider's registry is missing
    add(lua, &table, "get_registry", |_, _| -> mlua::Result<Reason> {
        Ok((LuaValue::Nil, Some(REGISTRY_MISSING.into())))
    })?;
    // read_component_registry.rs:153-170: the selector resolves first, then the provider's registry
    let c = context.clone();
    add(lua, &table, "get_component", move |_, args| -> mlua::Result<Reason> {
        match resolve(c.types.registry(), args.get::<LuaValue>(0)?) {
            Ok(_) => Ok((LuaValue::Nil, Some(REGISTRY_MISSING.into()))),
            Err(reason) => Ok((LuaValue::Nil, Some(reason))),
        }
    })?;
    nil_reason(lua, &table, context, "query", "runtime.ecs.query")?;
    nil_reason(lua, &table, context, "query_bounds", "runtime.ecs.query")?;
    nil_reason(lua, &table, context, "resolve", "runtime.ecs.resolve")?;
    nil_reason(lua, &table, context, "read", "runtime.ecs.read")?;
    nil_reason(lua, &table, context, "read_bytes", "runtime.ecs.read")?;
    nil_reason(lua, &table, context, "read_fields", "runtime.ecs.read")?;
    // resolve_entity.rs:22-60 and access_template_component.rs:221-233: (nil, reason) after the read denial
    nil_reason(lua, &table, context, "get_template", "runtime.ecs.read")?;
    nil_reason(lua, &table, context, "get_template_component", "runtime.ecs.read")?;
    // access_component_fields.rs:13-22: (false, reason, nil)
    let host = context.host.clone();
    add(
        lua,
        &table,
        "write_fields",
        move |_, _| -> mlua::Result<(bool, Option<String>, LuaValue)> {
            match denial(host.as_ref(), "runtime.ecs.write") {
                Some(reason) => Ok((false, Some(reason), LuaValue::Nil)),
                None => Err(past_denial("runtime.ecs.write_fields")),
            }
        },
    )?;
    // write_component.rs:2-10: (false, reason)
    let host = context.host.clone();
    add(
        lua,
        &table,
        "write",
        move |_, _| -> mlua::Result<(bool, Option<String>)> {
            match denial(host.as_ref(), "runtime.ecs.write") {
                Some(reason) => Ok((false, Some(reason))),
                None => Err(past_denial("runtime.ecs.write")),
            }
        },
    )?;
    // attributes/register_attribute_api.rs:10-51
    let c = context.clone();
    table.raw_set(
        "evaluate_attributes",
        lua.create_function(move |lua, (selector, values): (LuaValue, Table)| {
            attributes::evaluate_attributes(lua, &c, selector, values)
        })?,
    )?;
    attribute_reason(
        lua,
        &table,
        context,
        "read_attributes",
        "runtime.ecs.read",
        LuaValue::Nil,
    )?;
    attribute_reason(
        lua,
        &table,
        context,
        "update_attribute",
        "runtime.ecs.write",
        LuaValue::Nil,
    )?;
    let c = context.clone();
    table.raw_set(
        "get_attributes",
        lua.create_function(move |lua, (): ()| attributes::get_attributes(lua, &c))?,
    )?;
    let c = context.clone();
    table.raw_set(
        "get_attribute",
        lua.create_function(move |lua, selector: LuaValue| attributes::get_attribute(lua, &c, selector))?,
    )?;
    attribute_reason(
        lua,
        &table,
        context,
        "read_attribute",
        "runtime.ecs.read",
        LuaValue::Nil,
    )?;
    attribute_reason(
        lua,
        &table,
        context,
        "write_attribute_storage",
        "runtime.ecs.write",
        LuaValue::Boolean(false),
    )?;
    Ok(table)
}

/// SF `lua_ecs_get_catalog` (read_component_catalog.rs:2-193) without a provider: no registration report, nothing
/// resolved, not ready, no write context; the read and write denials are the phase's.
fn catalog(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let registry = context.types.registry();
    let result = lua.create_table()?;
    result.raw_set("version", registry.version.clone())?;
    let entries = lua.create_table()?;
    let read_denial = denial(context.host.as_ref(), "runtime.ecs.read");
    let write_denial = denial(context.host.as_ref(), "runtime.ecs.write");
    if read_denial.is_none() || write_denial.is_none() {
        return Err(past_denial("runtime.ecs.get_catalog"));
    }
    // can_write() is false without the provider (bridge/windows/ecs/check_ecs_write_access.rs)
    let type_write_reason = Some("runtime entity write context is not ready".to_owned());
    for metadata in registry
        .iter()
        .filter(|ty| ty.qualified_name.starts_with("keen::ecs::"))
    {
        let entry = lua.create_table()?;
        entry.raw_set("type", context.types.get_type(lua, metadata.index)?)?;
        entry.raw_set("qualified_name", metadata.qualified_name.clone())?;
        entry.raw_set("size", metadata.size)?;
        entry.raw_set("registered", false)?;
        entry.raw_set("runtime_resolved", false)?;
        entry.raw_set("registration_state", "reflected-only")?;
        entry.raw_set("storage", "unregistered")?;
        entry.raw_set("registrations", lua.create_table()?)?;
        entry.raw_set("resolved", false)?;
        let codec_reason = validate_owned_type(registry, metadata.index).err();
        entry.raw_set("partial_value", codec_reason.is_some())?;
        entry.raw_set("value_reason", codec_reason.clone())?;
        entry.raw_set("read_available", false)?;
        entry.raw_set("write_available", false)?;
        entry.raw_set("read_fields_available", false)?;
        entry.raw_set("write_fields_available", false)?;
        entry.raw_set("read_reason", read_denial.clone())?;
        entry.raw_set(
            "write_fields_reason",
            write_denial.clone().or_else(|| type_write_reason.clone()),
        )?;
        entry.raw_set(
            "write_reason",
            write_denial.clone().or(codec_reason).or(type_write_reason.clone()),
        )?;
        entry.raw_set("reason", "reflected type has no engine component registration")?;
        entries.push(entry)?;
    }
    result.raw_set("registry_available", false)?;
    result.raw_set("registered_type_count", 0)?;
    result.raw_set("runtime_type_count", 0)?;
    result.raw_set("template_type_count", 0)?;
    result.raw_set("resolved_count", 0)?;
    result.raw_set("reflected_only_count", entries.raw_len())?;
    result.raw_set("entity_access_ready", false)?;
    result.raw_set("entity_writes_ready", false)?;
    result.raw_set("entries", entries)?;
    Ok(result)
}
