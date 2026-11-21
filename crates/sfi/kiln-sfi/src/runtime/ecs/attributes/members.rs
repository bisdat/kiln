// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/attributes/{read_attribute_catalog,
// read_attribute_descriptor,evaluate_attributes,read_attributes,read_attribute,encode_attribute_scalar,
// validate_attribute_bits,format_attribute_root}.rs, lua/convert_json.rs (`json_to_lua`).
//! The attribute members' Lua bodies, in ShroudForge's order of checks, texts and return shapes.
//!
//! `get_attributes`, `get_attribute` and `evaluate_attributes` have no gate (ShroudForge calls none): they answer from
//! the catalog, `nil, reason` when it cannot be had. `read_attributes` and `read_attribute` are gated by
//! `runtime.ecs.read` (the denial, then its availability: the phase, the contract, the `runtime` capability, the ECS
//! readiness), then check their arguments, then read the entity's storage ([`super::storage::entity_storage`]).
//! Every refusal is `nil, reason`; an argument mlua cannot convert (`evaluate_attributes`' values table) raises as
//! ShroudForge's does.

use super::catalog::Attributes;
use super::storage::{entity_storage, storage_range};
use super::uint;
use crate::runtime::ecs::{Ecs, available};
use mlua::{IntoLua, Lua, MultiValue, Table, Value as LuaValue};
use serde_json::{Value, json};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};

type Reply = (LuaValue, Option<String>);

fn answer(lua: &Lua, outcome: Result<Value, String>) -> mlua::Result<Reply> {
    Ok(match outcome {
        Ok(value) => (json_to_lua(lua, &value)?, None),
        Err(reason) => (LuaValue::Nil, Some(reason)),
    })
}

/// ShroudForge's `read_attribute_catalog` (`runtime.ecs.get_attributes`): the registry version, the descriptors by ID,
/// the component layouts and the storage-resolution model.
pub fn get_attributes(lua: &Lua, attributes: &Attributes, ecs: &Ecs) -> mlua::Result<Reply> {
    let open = match attributes.open() {
        Ok(open) => open,
        Err(reason) => return Ok((LuaValue::Nil, Some(reason))),
    };
    let catalog = &open.catalog;
    let result = json!({"version":ecs.types().registry_version(),"count":catalog.entries.len(),
        "entries":catalog.entries.values().collect::<Vec<_>>(),"component_layouts":catalog.layouts,
        "storage_resolution":"per-entity-initialized-root-id"});
    Ok((json_to_lua(lua, &result)?, None))
}

/// ShroudForge's `read_attribute_descriptor` (`runtime.ecs.get_attribute`): one descriptor, without entity binding.
pub fn get_attribute(
    lua: &Lua,
    attributes: &Attributes,
    selector: LuaValue,
) -> mlua::Result<Reply> {
    let descriptor = attributes
        .open()
        .and_then(|open| open.catalog.get(&selector).cloned());
    answer(lua, descriptor)
}

/// ShroudForge's `evaluate_attributes`: the selected attribute's whole root evaluated on the caller's values (one per
/// root attribute, by original name), descending root index, detached from any entity.
pub fn evaluate_attributes(
    lua: &Lua,
    attributes: &Attributes,
    selector: LuaValue,
    values: Table,
) -> mlua::Result<Reply> {
    let execute = || -> Result<Value, String> {
        let open = attributes.open()?;
        let catalog = &open.catalog;
        let descriptor = catalog.get(&selector)?;
        let root = uint(&descriptor["root_hash"])?;
        let entries = catalog.root_entries(root)?;
        let scalar = descriptor["calculation_scalar_type"]
            .as_str()
            .ok_or("unknown scalar type")?;
        let mut input = Vec::new();
        for entry in &entries {
            if entry["calculation_encoding_verified"] != true
                || entry["calculation"]["decoded"] != true
            {
                return Err("attribute root has an unverified calculation program".into());
            }
            if entry["calculation_scalar_type"].as_str() != Some(scalar) {
                return Err("attribute root has conflicting scalar interpretations".into());
            }
            let name = entry_name(entry)?;
            let value = values
                .raw_get::<LuaValue>(name)
                .map_err(|e| e.to_string())?;
            input.push(u32::from_le_bytes(
                encode_attribute_scalar(entry, scalar, &value)
                    .map_err(|e| format!("{name}: {e}"))?,
            ));
        }
        if values.pairs::<LuaValue, LuaValue>().count() != entries.len() {
            return Err("unexpected attribute names in root values".into());
        }
        let (result, trace) = open.recalculate(root, &input)?;
        for (entry, bits) in entries.iter().zip(&result) {
            validate_attribute_bits(entry, scalar, *bits)?;
        }
        root_result(&entries, scalar, &result, trace)
    };
    answer(lua, execute())
}

/// ShroudForge's `available` gate of `runtime.ecs.read` and `.write`: the denial, then the operation's availability.
pub(crate) fn gate(host: &dyn RuntimeHost, operation: Op) -> Result<(), String> {
    if let Some(reason) = runtime_denial_reason(host, operation) {
        return Err(reason);
    }
    if !available(host, operation) {
        return Err(format!("{} unavailable", operation.as_str()));
    }
    Ok(())
}

/// An entity argument: a Lua integer that fits u32 (ShroudForge's opaque handle).
pub(crate) fn entity(value: &LuaValue) -> Result<u32, String> {
    match value {
        LuaValue::Integer(v) => u32::try_from(*v).map_err(|_| "entity outside u32".into()),
        _ => Err("expected entity integer".into()),
    }
}

/// The root's values in a component's storage, as words.
pub(crate) fn storage_words(
    component: &Value,
    bytes: &[u8],
) -> Result<(usize, usize, Vec<u32>), String> {
    let start = uint(&component["storage_offset"])? as usize;
    let end = start + uint(&component["storage_size"])? as usize;
    let words = bytes
        .get(start..end)
        .ok_or("attribute element outside storage")?
        .as_chunks::<4>()
        .0
        .iter()
        .map(|v| u32::from_le_bytes(*v))
        .collect();
    Ok((start, end, words))
}

/// The scalar type of a resolved component.
pub(crate) fn storage_scalar(component: &Value) -> Result<&str, String> {
    component["storage_scalar_type"]
        .as_str()
        .ok_or_else(|| "unresolved attribute scalar type".into())
}

/// ShroudForge's `read_attributes`: the entity's root values (the descriptor's root, in the component that holds it),
/// with the entity, the component, `applied = false` and the effect scope.
pub fn read_attributes(
    lua: &Lua,
    attributes: &Attributes,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: MultiValue,
) -> mlua::Result<Reply> {
    let execute = || -> Result<Value, String> {
        gate(host, Op::EcsRead)?;
        if args.len() != 2 {
            return Err(
                "expected entity, attribute name/ID, and (for updates) scalar value plus optional expected_values"
                    .into(),
            );
        }
        let entity = entity(&args[0])?;
        let open = attributes.open()?;
        let catalog = &open.catalog;
        let (descriptor, component, before) =
            entity_storage(ecs, catalog, catalog.get(&args[1])?, entity)?;
        let scalar = storage_scalar(&component)?.to_owned();
        let root = uint(&descriptor["root_hash"])?;
        let entries = catalog.root_entries(root)?;
        let (_, _, values) = storage_words(&component, &before)?;
        if entries.len() != values.len() {
            return Err("attribute root storage count changed".into());
        }
        let mut result = root_result(&entries, &scalar, &values, Vec::new())?;
        result["entity"] = json!(entity);
        result["component"] = component;
        result["applied"] = json!(false);
        result["effect_scope"] = json!("local process, no automatic client/server RPC");
        Ok(result)
    };
    answer(lua, execute())
}

/// ShroudForge's `read_attribute`: one attribute's current value on the entity, as its storage scalar.
pub fn read_attribute(
    _: &Lua,
    attributes: &Attributes,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: MultiValue,
) -> mlua::Result<Reply> {
    let execute = || -> Result<LuaValue, String> {
        gate(host, Op::EcsRead)?;
        if args.len() != 2 {
            return Err("expected entity, attribute name/ID, and (for writes) scalar value".into());
        }
        let entity = entity(&args[0])?;
        let open = attributes.open()?;
        let catalog = &open.catalog;
        let (descriptor, component, bytes) =
            entity_storage(ecs, catalog, catalog.get(&args[1])?, entity)?;
        let range = storage_range(&descriptor, &component, &bytes)?;
        let scalar = storage_scalar(&component)?;
        let data = [
            bytes[range.start],
            bytes[range.start + 1],
            bytes[range.start + 2],
            bytes[range.start + 3],
        ];
        match scalar {
            "keen::sint32" => Ok(LuaValue::Integer(i64::from(i32::from_le_bytes(data)))),
            "keen::uint32" => Ok(LuaValue::Integer(i64::from(u32::from_le_bytes(data)))),
            "keen::float32" => Ok(LuaValue::Number(f64::from(f32::from_le_bytes(data)))),
            _ => Err("unsupported attribute scalar".into()),
        }
    };
    Ok(match execute() {
        Ok(value) => (value, None),
        Err(reason) => (LuaValue::Nil, Some(reason)),
    })
}

/// A descriptor's original name (the catalog builds descriptors with a non-empty name only).
pub(crate) fn entry_name(entry: &Value) -> Result<&str, String> {
    entry["name"]
        .as_str()
        .ok_or_else(|| "empty original attribute name".into())
}

/// ShroudForge's `encode_scalar`: a Lua value as the 4 bytes of an attribute scalar.
pub(crate) fn encode_scalar(kind: &str, value: &LuaValue) -> Result<[u8; 4], String> {
    match (kind, value) {
        ("keen::sint32", LuaValue::Integer(v)) => Ok(i32::try_from(*v)
            .map_err(|_| "value outside sint32")?
            .to_le_bytes()),
        ("keen::uint32", LuaValue::Integer(v)) => Ok(u32::try_from(*v)
            .map_err(|_| "value outside uint32")?
            .to_le_bytes()),
        ("keen::float32", LuaValue::Number(v)) if v.is_finite() && (*v as f32).is_finite() => {
            Ok((*v as f32).to_le_bytes())
        }
        ("keen::float32", LuaValue::Integer(v)) => Ok((*v as f32).to_le_bytes()),
        _ => Err(format!("expected finite scalar of type {kind}")),
    }
}

/// ShroudForge's `encode_attribute_scalar`: a conflicting-signedness attribute takes only the common integer range.
pub(crate) fn encode_attribute_scalar(
    descriptor: &Value,
    kind: &str,
    value: &LuaValue,
) -> Result<[u8; 4], String> {
    if descriptor["write_value_domain"] == "integer-common-range-0-to-2147483647"
        && !matches!(value, LuaValue::Integer(v) if (0..=i64::from(i32::MAX)).contains(v))
    {
        return Err(
            "conflicting signedness: only the common integer range 0..2147483647 is validated"
                .into(),
        );
    }
    encode_scalar(kind, value)
}

/// ShroudForge's `validate_attribute_bits`: a result is a value the attribute could have been given.
pub(crate) fn validate_attribute_bits(
    descriptor: &Value,
    scalar: &str,
    bits: u32,
) -> Result<(), String> {
    let value = match scalar {
        "keen::sint32" => LuaValue::Integer(i64::from(bits as i32)),
        "keen::uint32" => LuaValue::Integer(i64::from(bits)),
        "keen::float32" => LuaValue::Number(f64::from(f32::from_bits(bits))),
        _ => return Err("unsupported attribute scalar".into()),
    };
    encode_attribute_scalar(descriptor, scalar, &value).map(|_| ())
}

/// ShroudForge's `root_result`: the root's values by original name, the trace, the model and the scalar type.
pub(crate) fn root_result(
    entries: &[&Value],
    scalar: &str,
    values: &[u32],
    trace: Vec<Value>,
) -> Result<Value, String> {
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
    let root_hash = entries
        .first()
        .map_or(Value::Null, |entry| entry["root_hash"].clone());
    Ok(json!({"root_hash":root_hash,"values":named,"trace":trace,
        "calculation_model":"attribute-command-v1","scalar_type":scalar}))
}

/// ShroudForge's `json_to_lua`: detached tables; null is nil, an integer that fits i64 an integer (a larger u64 wraps),
/// any other number a float.
pub fn json_to_lua(lua: &Lua, value: &Value) -> mlua::Result<LuaValue> {
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
