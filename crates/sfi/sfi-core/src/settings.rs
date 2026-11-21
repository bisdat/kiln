// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/shroudforge/v1/set_mod_settings.rs:2-38;
// create_mod_api.rs:95-115 (1.7.9), :96-116 (1.8.0);
// modules/mod-runtime/ingame/refresh_runtime_configuration.rs:43-75 (1.7.9), :52-84 (1.8.0);
// create_mod_api.rs:273-311 (1.7.9: read_keybind's registry read, read_setting_value).
use crate::package::{
    Manifest,
    manifest::{Capability, SettingDefinition},
};
use mlua::{Lua, Table, Value};
use serde_json::Value as Json;
use std::sync::Arc;

/// Worker-local, owner-bound registry replacement for the lifecycle. Never exposed to Lua.
///
/// It also carries the package's declared settings as the registrar saw them: ShroudForge's input members capture
/// `r#mod.info().settings` when the package's API is made, and a live settings change replaces the values, never
/// those declarations.
#[derive(Clone, Debug)]
pub struct Settings {
    owner: String,
    definitions: Arc<[SettingDefinition]>,
}
impl Settings {
    /// The package the registry belongs to.
    pub fn owner(&self) -> &str {
        &self.owner
    }
    /// The package's declared settings, captured at install.
    pub fn definitions(&self) -> &[SettingDefinition] {
        &self.definitions
    }
    /// The current values: the registry table `settings.get` reads (`shroudforge.settings.<id>`). A live settings
    /// change replaces it whole, so read it at each use rather than keeping it.
    pub fn current(&self, lua: &Lua) -> mlua::Result<Table> {
        lua.named_registry_value(&self.key())
    }
    /// Build before publishing. Returned array tables remain stale references after
    /// replacement, and a conversion error leaves the previous registry untouched.
    pub fn replace(&self, lua: &Lua, values: &Json) -> mlua::Result<()> {
        let values = values
            .as_object()
            .ok_or_else(|| mlua::Error::runtime("mod settings must be an object"))?;
        let table = lua.create_table_with_capacity(0, values.len())?;
        for (key, value) in values {
            table.raw_set(key.as_str(), json_to_lua(lua, value)?)?;
        }
        lua.set_named_registry_value(&self.key(), table)
    }
    /// Reuse the final package-model capabilities, desired setting_values and apply_at /
    /// restart_required declarations. There is no second inference or validation model.
    /// `applied` is the host's startup snapshot, not a Lua-mutated array reference.
    pub fn apply_live(&self, lua: &Lua, manifest: &Manifest, applied: &Json) -> mlua::Result<bool> {
        if manifest.id != self.owner {
            return Err(mlua::Error::runtime(
                "settings manifest owner does not match binding",
            ));
        }
        let Some(next) = Self::values_to_apply_live(manifest, applied) else {
            return Ok(false);
        };
        self.replace(lua, &next)?;
        Ok(true)
    }
    /// The same pure selection used by `apply_live`, exposed for lifecycle's
    /// applied manifest/revision bookkeeping. Never read back Lua-mutated arrays.
    pub fn values_to_apply_live(manifest: &Manifest, applied: &Json) -> Option<Json> {
        if !manifest
            .capabilities
            .iter()
            .any(|capability| capability.requires_runtime())
        {
            return None;
        }
        let mut next = manifest.setting_values.clone();
        if manifest.capabilities.contains(&Capability::Patch) {
            let values = next.as_object_mut()?;
            for setting in &manifest.settings {
                let previous = applied.get(&setting.key);
                if manifest.setting_values.get(&setting.key) != previous
                    && !(setting.apply_at == "live" && !setting.restart_required)
                {
                    if let Some(previous) = previous {
                        values.insert(setting.key.clone(), previous.clone());
                    } else {
                        values.remove(&setting.key);
                    }
                }
            }
        }
        Some(next)
    }
    fn key(&self) -> String {
        format!("shroudforge.settings.{}", self.owner)
    }
}

/// ShroudForge's `read_setting_value` (create_mod_api.rs:303-311, 1.7.9): a current value as the keybind reader sees
/// it. Booleans, integers, numbers and strings keep their value (a number with no JSON form is null); anything else
/// (nil, a table, a function) is null. A string that is not UTF-8 raises mlua's error.
pub fn read_value(current: &Table, key: &str) -> mlua::Result<Json> {
    Ok(match current.raw_get::<Value>(key)? {
        Value::Boolean(value) => Json::Bool(value),
        Value::Integer(value) => Json::from(value),
        Value::Number(value) => Json::from(value),
        Value::String(value) => Json::String(value.to_str()?.to_string()),
        _ => Json::Null,
    })
}

/// `shroudforge.settings` for one package: its registry filled from `values` and `get` in `api`'s `settings`
/// table. `definitions` are the package's declared settings (the manifest's).
pub fn install(
    lua: &Lua,
    api: &Table,
    owner: &str,
    values: &Json,
    definitions: &[SettingDefinition],
) -> mlua::Result<Settings> {
    let settings = Settings {
        owner: owner.to_owned(),
        definitions: definitions.into(),
    };
    settings.replace(lua, values)?;
    let bound = settings.clone();
    let table = crate::bindings::common::child(lua, api, "settings")?;
    // SF actually uses an mlua tuple here, not FunctionArgs. Preserve numeric String
    // conversion, Option<Value> fallback, ignored extra arguments and error conventions.
    table.raw_set(
        "get",
        lua.create_function(move |lua, (key, fallback): (String, Option<Value>)| {
            if !valid_identifier(&key) {
                return Err(mlua::Error::runtime("invalid setting key"));
            }
            let current: Table = lua.named_registry_value(&bound.key())?;
            let value: Value = current.raw_get(key)?;
            Ok(if value.is_nil() {
                fallback.unwrap_or(Value::Nil)
            } else {
                value
            })
        })?,
    )?;
    Ok(settings)
}

// create_mod_api.rs:383-388 (1.7.9), :412-417 (1.8.0).
fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
}
fn json_to_lua(lua: &Lua, value: &Json) -> mlua::Result<Value> {
    Ok(match value {
        Json::Null => Value::Nil,
        Json::Bool(value) => Value::Boolean(*value),
        Json::Number(value) if value.is_i64() => Value::Integer(value.as_i64().unwrap()),
        Json::Number(value) => Value::Number(value.as_f64().unwrap_or_default()),
        Json::String(value) => Value::String(lua.create_string(value)?),
        Json::Array(values) => {
            let table = lua.create_table_with_capacity(values.len(), 0)?;
            for (index, value) in values.iter().enumerate() {
                table.raw_set(index + 1, json_to_lua(lua, value)?)?;
            }
            Value::Table(table)
        }
        _ => {
            return Err(mlua::Error::runtime(
                "setting value must be an array, string, number, boolean or null",
            ));
        }
    })
}
