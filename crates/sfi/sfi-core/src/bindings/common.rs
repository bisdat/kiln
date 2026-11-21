// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:19-115,208-230
// (1.7.9), :19-116,236-258 (1.8.0); lua/bindings/mod.rs:88-110 (both pins).
use super::{
    clock,
    loader::{self, LoaderSnapshot},
    logging,
};
use crate::{
    host::{HostContext, Package},
    locales::{self, LocaleSource},
    lua::install_namespace,
    package::Manifest,
    settings::{self, Settings},
};
use mlua::{Lua, Table, Value};
use std::rc::Rc;

/// Captured package model plus explicit worker-local ports. Parser version never grants
/// activation of a 1.8-only feature. The registry adapter is supplied by discovery, not the VM.
pub struct OwnerBindings {
    pub manifest: Manifest,
    pub locale: Rc<dyn LocaleSource>,
    pub loader: LoaderSnapshot,
}

/// Extension points for other family owners. No successful dummy native/UI APIs.
pub struct BindingTables {
    pub shroudforge: Table,
    pub loader: Table,
    pub loader_features: Table,
    pub settings: Settings,
}

/// Registrar for LuaVm::add_package, before executing any package. Missing ports and
/// mismatched model identity are setup errors; no fallback clock or logging sink exists.
pub fn install(
    lua: &Lua,
    env: &Table,
    package: &Package,
    context: &HostContext,
    input: OwnerBindings,
) -> mlua::Result<BindingTables> {
    if package.id() != input.manifest.id {
        return Err(mlua::Error::runtime(
            "binding manifest owner does not match package",
        ));
    }
    let clock = clock::create(lua, context)?;
    let sink = context
        .log
        .clone()
        .ok_or_else(|| mlua::Error::runtime("logging host port is required"))?;
    let api = namespace(lua, env, "shroudforge")?;
    api.raw_set("version", "1.0.0")?;
    api.raw_set("mod_id", package.id())?;
    // Docs say 'lua'; neither pinned implementation registers mod_kind.
    logging::install(lua, env, &api, package.id(), sink)?;
    api.raw_set("clock", clock)?;
    let settings = settings::install(
        lua,
        &api,
        package.id(),
        &input.manifest.setting_values,
        &input.manifest.settings,
    )?;
    locales::install(lua, &api, input.locale, input.manifest.locales)?;
    let loader = namespace(lua, env, "loader")?;
    let loader_features = loader::install(
        lua,
        &loader,
        context.side,
        &input.manifest.capabilities,
        input.loader,
    )?;
    Ok(BindingTables {
        shroudforge: api,
        loader,
        loader_features,
        settings,
    })
}

/// Extend existing family tables rather than discarding another registrar's members.
pub(crate) fn namespace(lua: &Lua, env: &Table, name: &str) -> mlua::Result<Table> {
    let table = match env.raw_get::<Value>(name)? {
        Value::Nil => lua.create_table()?,
        Value::Table(table) => table,
        _ => {
            return Err(mlua::Error::runtime(format!(
                "namespace {name} must be a table"
            )));
        }
    };
    install_namespace(env, name, table.clone())?;
    Ok(table)
}

pub(crate) fn child(lua: &Lua, parent: &Table, name: &str) -> mlua::Result<Table> {
    let table = match parent.raw_get::<Value>(name)? {
        Value::Nil => lua.create_table()?,
        Value::Table(table) => table,
        _ => {
            return Err(mlua::Error::runtime(format!(
                "namespace {name} must be a table"
            )));
        }
    };
    parent.raw_set(name, table.clone())?;
    Ok(table)
}
