// SPDX-License-Identifier: GPL-3.0-or-later
//! The attribute catalog members of `runtime.ecs` in the pregame VM: `get_attributes`, `get_attribute` and
//! `evaluate_attributes` have no phase check in ShroudForge; they read the archive's attribute catalog (the VM's one
//! [`AssetSession`], built on first use as `AppState::attribute_catalog` is) and answer what ShroudForge answers before
//! the game starts. A catalog that cannot be built is every call's `nil, reason`, as in ShroudForge.
//! `evaluate_attributes` is a pure, detached evaluation (no game state, no native call). `runtime.overrides.attribute`
//! reaches `get_attribute` first and then `read_attributes`, which refuses in this phase.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! lua/ingame/ecs/attributes/{mod (`Catalog::get`),read_attribute_catalog,read_attribute_descriptor,evaluate_attributes,
//! encode_attribute_scalar,validate_attribute_bits,format_attribute_root}.rs and lua/convert_json.rs (`json_to_lua`).
//! SF's timing spans are left out; its `unwrap` of a descriptor's name (always a string: the catalog built it) is the
//! catalog's own error text here.
use mlua::{IntoLua, Lua, Table, Value as LuaValue};
use serde_json::{Value, json};

use super::PregameRuntime;
use crate::sf_startup::attributes::{Catalog, uint};

/// SF `Catalog::get`: an unsigned attribute ID or an original name.
fn select<'c>(catalog: &'c Catalog, selector: &LuaValue) -> Result<&'c Value, String> {
    match selector {
        LuaValue::Integer(id) => catalog
            .entries
            .get(&u32::try_from(*id).map_err(|_| "attribute ID outside u32")?)
            .ok_or_else(|| "attribute ID not found".into()),
        LuaValue::String(name) => {
            let name = name.to_str().map_err(|e| e.to_string())?;
            catalog.by_name(name.as_ref())
        }
        _ => Err("expected original attribute name or unsigned attribute ID".into()),
    }
}

/// SF `read_attribute_catalog` (`runtime.ecs.get_attributes`): the registry version, the descriptors by ID, the
/// component layouts and the storage-resolution model.
pub(super) fn get_attributes(lua: &Lua, context: &PregameRuntime) -> mlua::Result<(LuaValue, Option<String>)> {
    let catalog = match context.assets.attribute_catalog() {
        Ok(catalog) => catalog,
        Err(reason) => return Ok((LuaValue::Nil, Some(reason))),
    };
    let result = json!({"version":context.types.registry().version,"count":catalog.entries.len(),
        "entries":catalog.entries.values().collect::<Vec<_>>(),"component_layouts":catalog.layouts,
        "storage_resolution":"per-entity-initialized-root-id"});
    Ok((json_to_lua(lua, &result)?, None))
}

/// SF `read_attribute_descriptor` (`runtime.ecs.get_attribute`): one descriptor, without entity binding.
pub(super) fn get_attribute(
    lua: &Lua,
    context: &PregameRuntime,
    selector: LuaValue,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let catalog = context.assets.attribute_catalog();
    match catalog.and_then(|catalog| select(&catalog, &selector).cloned()) {
        Ok(descriptor) => Ok((json_to_lua(lua, &descriptor)?, None)),
        Err(reason) => Ok((LuaValue::Nil, Some(reason))),
    }
}

/// SF `evaluate_attributes`: the selected attribute's whole root evaluated on the caller's values (one per root
/// attribute, by original name), descending root index.
pub(super) fn evaluate_attributes(
    lua: &Lua,
    context: &PregameRuntime,
    selector: LuaValue,
    values: Table,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let execute = || -> Result<LuaValue, String> {
        let catalog = context.assets.attribute_catalog()?;
        let descriptor = select(&catalog, &selector)?;
        let root = uint(&descriptor["root_hash"])?;
        let entries = catalog.root_entries(root)?;
        let scalar = descriptor["calculation_scalar_type"]
            .as_str()
            .ok_or("unknown scalar type")?;
        let mut input = Vec::new();
        for entry in &entries {
            if entry["calculation_encoding_verified"] != true || entry["calculation"]["decoded"] != true {
                return Err("attribute root has an unverified calculation program".into());
            }
            if entry["calculation_scalar_type"].as_str() != Some(scalar) {
                return Err("attribute root has conflicting scalar interpretations".into());
            }
            let name = entry_name(entry)?;
            let value = values.raw_get::<LuaValue>(name).map_err(|e| e.to_string())?;
            input.push(u32::from_le_bytes(
                encode_attribute_scalar(entry, scalar, &value).map_err(|e| format!("{name}: {e}"))?,
            ));
        }
        if values.pairs::<LuaValue, LuaValue>().count() != entries.len() {
            return Err("unexpected attribute names in root values".into());
        }
        let (result, trace) = catalog.recalculate(root, &input)?;
        for (entry, bits) in entries.iter().zip(&result) {
            validate_attribute_bits(entry, scalar, *bits)?;
        }
        json_to_lua(lua, &root_result(&entries, scalar, &result, trace)?).map_err(|e| e.to_string())
    };
    Ok(match execute() {
        Ok(value) => (value, None),
        Err(reason) => (LuaValue::Nil, Some(reason)),
    })
}

/// A descriptor's original name (SF unwraps it: the catalog only builds descriptors with a non-empty name).
fn entry_name(entry: &Value) -> Result<&str, String> {
    entry["name"]
        .as_str()
        .ok_or_else(|| "empty original attribute name".into())
}

/// SF `encode_scalar`: a Lua value as the 4 bytes of an attribute scalar.
fn encode_scalar(kind: &str, value: &LuaValue) -> Result<[u8; 4], String> {
    match (kind, value) {
        ("keen::sint32", LuaValue::Integer(v)) => {
            Ok(i32::try_from(*v).map_err(|_| "value outside sint32")?.to_le_bytes())
        }
        ("keen::uint32", LuaValue::Integer(v)) => {
            Ok(u32::try_from(*v).map_err(|_| "value outside uint32")?.to_le_bytes())
        }
        ("keen::float32", LuaValue::Number(v)) if v.is_finite() && (*v as f32).is_finite() => {
            Ok((*v as f32).to_le_bytes())
        }
        ("keen::float32", LuaValue::Integer(v)) => Ok((*v as f32).to_le_bytes()),
        _ => Err(format!("expected finite scalar of type {kind}")),
    }
}

/// SF `encode_attribute_scalar`: a conflicting-signedness attribute takes only the common integer range.
fn encode_attribute_scalar(descriptor: &Value, kind: &str, value: &LuaValue) -> Result<[u8; 4], String> {
    if descriptor["write_value_domain"] == "integer-common-range-0-to-2147483647"
        && !matches!(value, LuaValue::Integer(v) if (0..=i32::MAX as i64).contains(v))
    {
        return Err("conflicting signedness: only the common integer range 0..2147483647 is validated".into());
    }
    encode_scalar(kind, value)
}

/// SF `validate_attribute_bits`: a result is a value the attribute could have been given.
fn validate_attribute_bits(descriptor: &Value, scalar: &str, bits: u32) -> Result<(), String> {
    let value = match scalar {
        "keen::sint32" => LuaValue::Integer(i64::from(bits as i32)),
        "keen::uint32" => LuaValue::Integer(i64::from(bits)),
        "keen::float32" => LuaValue::Number(f64::from(f32::from_bits(bits))),
        _ => return Err("unsupported attribute scalar".into()),
    };
    encode_attribute_scalar(descriptor, scalar, &value).map(|_| ())
}

/// SF `root_result`: the root's values by original name, the trace, the model and the scalar type.
fn root_result(entries: &[&Value], scalar: &str, values: &[u32], trace: Vec<Value>) -> Result<Value, String> {
    let mut named = serde_json::Map::new();
    for (entry, bits) in entries.iter().zip(values) {
        let value = match scalar {
            "keen::sint32" => json!(*bits as i32),
            "keen::uint32" => json!(*bits),
            "keen::float32" => {
                let value = f32::from_bits(*bits);
                if !value.is_finite() {
                    return Err("non-finite attribute value".into());
                }
                json!(value)
            }
            _ => return Err("unresolved attribute scalar".into()),
        };
        named.insert(entry_name(entry)?.to_owned(), value);
    }
    let root_hash = entries.first().map_or(Value::Null, |entry| entry["root_hash"].clone());
    Ok(json!({"root_hash":root_hash,"values":named,"trace":trace,
        "calculation_model":"attribute-command-v1","scalar_type":scalar}))
}

/// SF `json_to_lua`: detached tables; null is nil, an integer that fits i64 an integer (a larger u64 wraps), any other
/// number a float.
fn json_to_lua(lua: &Lua, value: &Value) -> mlua::Result<LuaValue> {
    match value {
        Value::Null => Ok(LuaValue::Nil),
        Value::Bool(value) => value.into_lua(lua),
        Value::String(value) => value.as_str().into_lua(lua),
        Value::Number(value) => {
            if let Some(value) = value.as_i64() {
                value.into_lua(lua)
            } else if let Some(value) = value.as_u64() {
                Ok(LuaValue::Integer(value as i64))
            } else {
                value.as_f64().into_lua(lua)
            }
        }
        Value::Array(values) => {
            let table = lua.create_table_with_capacity(values.len(), 0)?;
            for (i, value) in values.iter().enumerate() {
                table.raw_set(i + 1, json_to_lua(lua, value)?)?;
            }
            Ok(LuaValue::Table(table))
        }
        Value::Object(values) => {
            let table = lua.create_table_with_capacity(0, values.len())?;
            for (key, value) in values {
                table.raw_set(key.as_str(), json_to_lua(lua, value)?)?;
            }
            Ok(LuaValue::Table(table))
        }
    }
}
