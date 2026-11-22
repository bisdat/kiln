// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/attributes/{update_attribute,write_attribute_storage,
// recalculate_bound_component,recalculate_component_update,expect_root_values}.rs; game-runtime/native/profiles/
// read_game_profile.cpp:138-141 and native/loaded-resources/initialize_resource_access.cpp:30-38 (the published
// attribute calculation model).
//! The attribute writes: `runtime.ecs.update_attribute`, `write_attribute_storage`, and the attribute recalculation of
//! the plain component writes.
//!
//! - [`recalculate_bound_component_update`]: ShroudForge's owned-data core of every attribute write. The candidate may
//!   change the root's storage only (the header, the definition identity and the layout are read-only), every root
//!   attribute must be storage- and calculation-writable on that component and verified, the root is recalculated on
//!   the archive (`attribute_evaluate`) and every result must be a value the attribute could hold; then the write mask
//!   is rebuilt to the words the recalculated root changed. It needs the profile's attribute calculation model
//!   ([`attribute_model`]: kiln.engine's `engine.attribute_model` row, ShroudForge's
//!   `functions()["attribute_calculation_model"]`); without "attribute-command-v1" it answers ShroudForge's "attribute
//!   calculations are not validated for this executable profile".
//! - [`recalculate_component_update`]: the plain writes' hook (`runtime.ecs.write`, `write_fields`): an attribute
//!   component's candidate goes through the core above before the compare-exchange; any other component is left as it
//!   is. In a VM without this family it keeps ShroudForge's text for a provider without the function inventory.
//! - The compare-exchange of an attribute write depends on the whole component (its header, identity and every root
//!   value were checked on these bytes): a conflict is refused at once with ShroudForge's text, never retried over
//!   another writer's change.

use super::catalog::{Open, read_header};
use super::members::{
    encode_attribute_scalar, entity, gate, root_result, storage_scalar, storage_words,
    validate_attribute_bits,
};
use super::storage::{entity_storage, storage_range};
use super::{of_vm, uint};
use crate::runtime::ecs::Ecs;
use crate::runtime::ecs::cas::{Writer, is_attribute_component, text as cas_text};
use crate::runtime::overrides::{check_write, component_key};
use crate::runtime::services::ServiceAdapter;
use crate::service_member;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::*;
use mlua::{Lua, MultiValue, Table, Value as LuaValue};
use serde_json::{Value, json};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op};

/// The only attribute calculation model ShroudForge 1.7.9 knows.
pub const MODEL: &str = "attribute-command-v1";
/// kiln.engine's row of the exact profile's published model.
pub const MODEL_ROW: &str = "engine.attribute_model";

/// ShroudForge's texts.
pub mod text {
    /// recalculate_bound_component.rs:21.
    pub const NOT_VALIDATED: &str =
        "attribute calculations are not validated for this executable profile";
    /// expect_root_values.rs:31.
    pub const CHANGED: &str =
        "Attribute snapshot changed before update. Read current values and retry";
}

/// ShroudForge's `functions()["attribute_calculation_model"]`: the exact profile's published model (kiln.engine's
/// [`MODEL_ROW`] row and its text), "" when the row is not AVAILABLE (no exact profile, a profile without the model, a
/// core without the row).
pub fn attribute_model(services: &ServiceAdapter) -> String {
    let Ok(table) = services.bind::<KilnCapsApi>(KILN_CAPS_SERVICE, KILN_CAPS_VERSION) else {
        return String::new();
    };
    let Ok(find) = service_member!(table, KilnCapsApi, find) else {
        return String::new();
    };
    // SAFETY: an all-zero row is valid (integers and empty strings).
    let mut row: KilnCapRow = unsafe { std::mem::zeroed() };
    row.size = std::mem::size_of::<KilnCapRow>() as u32;
    let name = KilnStr {
        ptr: MODEL_ROW.as_ptr().cast(),
        len: MODEL_ROW.len(),
    };
    // SAFETY: a counted name and a sized caller row (kiln_caps.h).
    let result = unsafe { services.call(find, (name, &raw mut row)) };
    if result != KILN_OK || row.state != KILN_CAP_AVAILABLE as u32 || row.text.ptr.is_null() {
        return String::new();
    }
    // SAFETY: find's strings stay valid until this thread's next find (kiln_caps.h); copied at once.
    let bytes = unsafe { std::slice::from_raw_parts(row.text.ptr.cast::<u8>(), row.text.len) };
    String::from_utf8_lossy(bytes).into_owned()
}

/// ShroudForge's `recalculate_bound_component_update` (recalculate_bound_component.rs): the core of every attribute
/// write (module docs). A failure leaves `after` and `mask` as they were.
pub fn recalculate_bound_component_update(
    open: &Open,
    name: &str,
    before: &[u8],
    after: &mut [u8],
    mask: &mut [u8],
    calculation_model: &str,
) -> Result<Vec<Value>, String> {
    if calculation_model != MODEL {
        return Err(text::NOT_VALIDATED.into());
    }
    if before.len() != after.len()
        || before.len() != mask.len()
        || mask.iter().any(|byte| *byte > 1)
    {
        return Err("attribute component candidate or mask size is invalid".into());
    }
    let catalog = &open.catalog;
    let mut layouts = catalog
        .layouts
        .iter()
        .filter(|layout| layout["qualified_name"] == name);
    let component = layouts
        .next()
        .ok_or("attribute component has no verified inline storage layout")?;
    if layouts.next().is_some() {
        return Err("attribute component has multiple reflected storage layouts".into());
    }
    let root = read_header(&component["header"], "rootId", before)?;
    let entries = catalog.root_entries(root)?;
    let first = entries.first().ok_or("incomplete attribute root")?;
    storage_range(first, component, before)?;
    let start = uint(&component["storage_offset"])? as usize;
    let length = uint(&component["storage_size"])? as usize;
    let end = start
        .checked_add(length)
        .ok_or("attribute storage range overflow")?;
    if length
        != entries
            .len()
            .checked_mul(4)
            .ok_or("attribute storage count overflow")?
        || end > before.len()
    {
        return Err("attribute root storage count changed".into());
    }
    for index in 0..before.len() {
        if !(start..end).contains(&index) && (before[index] != after[index] || mask[index] != 0) {
            return Err(
                "attribute headers, definition identity and storage layout are read-only".into(),
            );
        }
        if before[index] != after[index] && mask[index] == 0 {
            return Err("attribute candidate changes bytes outside its selected write mask".into());
        }
    }
    let scalar = component["storage_scalar_type"]
        .as_str()
        .ok_or("unresolved attribute scalar type")?;
    let values: Vec<u32> = after[start..end]
        .as_chunks::<4>()
        .0
        .iter()
        .map(|bytes| u32::from_le_bytes(*bytes))
        .collect();
    let mut bound = Vec::with_capacity(entries.len());
    for (entry, bits) in entries.iter().zip(&values) {
        let descriptor = catalog.bind_storage(entry, component)?;
        if descriptor["storage_writable"] != true || descriptor["calculation_writable"] != true {
            return Err(descriptor["calculation_reason"]
                .as_str()
                .or_else(|| descriptor["storage_reason"].as_str())
                .unwrap_or("attribute storage or calculation is not writable")
                .into());
        }
        if descriptor["calculation_encoding_verified"] != true
            || descriptor["calculation"]["decoded"] != true
        {
            return Err("attribute root has an unverified calculation program".into());
        }
        validate_attribute_bits(&descriptor, scalar, *bits)?;
        bound.push(descriptor);
    }
    let (recalculated, trace) = open.recalculate(root, &values)?;
    if recalculated.len() != values.len() {
        return Err("attribute root value count mismatch".into());
    }
    for (descriptor, bits) in bound.iter().zip(&recalculated) {
        validate_attribute_bits(descriptor, scalar, *bits)?;
    }
    mask.fill(0);
    for (index, value) in recalculated.iter().enumerate() {
        let range = start + index * 4..start + (index + 1) * 4;
        after[range.clone()].copy_from_slice(&value.to_le_bytes());
        if after[range.clone()] != before[range.clone()] {
            mask[range].fill(1);
        }
    }
    Ok(trace)
}

/// ShroudForge's `recalculate_component_update` (recalculate_component_update.rs), the plain writes' hook: an attribute
/// component's candidate recalculated as the core does it (true), any other component left alone (false).
pub fn recalculate_component_update(
    lua: &Lua,
    ecs: &Ecs,
    name: &str,
    before: &[u8],
    after: &mut [u8],
    mask: &mut [u8],
) -> Result<bool, String> {
    if !is_attribute_component(ecs.types(), name)? {
        return Ok(false);
    }
    // a VM without this family has no attribute catalog or model: ShroudForge's provider without the inventory
    let attributes = of_vm(lua).ok_or(cas_text::ATTRIBUTES)?;
    recalculate_attribute_component(&attributes, ecs, name, before, after, mask)?;
    Ok(true)
}

/// The core over the VM's catalog and the profile's model, for an attribute component.
fn recalculate_attribute_component(
    attributes: &super::catalog::Attributes,
    ecs: &Ecs,
    name: &str,
    before: &[u8],
    after: &mut [u8],
    mask: &mut [u8],
) -> Result<(), String> {
    let open = attributes.open()?;
    let model = attribute_model(ecs.services());
    recalculate_bound_component_update(&open, name, before, after, mask, &model).map(|_| ())
}

/// ShroudForge's `expect_root_values` (expect_root_values.rs): an earlier read's decisions bound to this fresh root
/// snapshot (original names only, at least one, all of this root).
fn expect_root_values(
    catalog: &super::catalog::Catalog,
    root: u32,
    scalar: &str,
    values: &[u32],
    expected: &Table,
) -> Result<(), String> {
    let mut count = 0;
    for pair in expected.clone().pairs::<LuaValue, LuaValue>() {
        let (selector, value) = pair.map_err(|e| e.to_string())?;
        if !matches!(selector, LuaValue::String(_)) {
            return Err("expected_values keys must be original attribute names".into());
        }
        let descriptor = catalog.get(&selector)?;
        if uint(&descriptor["root_hash"])? != root {
            return Err("expected attribute belongs to a different root".into());
        }
        let index = uint(&descriptor["index"])? as usize;
        let expected_bits =
            u32::from_le_bytes(encode_attribute_scalar(descriptor, scalar, &value)?);
        if values.get(index) != Some(&expected_bits) {
            return Err(text::CHANGED.into());
        }
        count += 1;
        if count > values.len() {
            return Err("too many expected root attributes".into());
        }
    }
    if count == 0 {
        return Err("expected_values must contain at least one attribute".into());
    }
    Ok(())
}

/// ShroudForge's `check_write` for an attribute component, then the compare-exchange of the whole component.
fn exchange(
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    handle: u32,
    name: &str,
    before: &[u8],
    mask: &[u8],
    after: &[u8],
) -> Result<(), String> {
    let writer = Writer {
        host,
        session: ecs.world_session(),
    };
    if let Some(runtime) = ecs.runtime() {
        let key = component_key(&writer, handle, name);
        check_write(runtime.facts.ownership(), &writer, &key)?;
    }
    let component = ecs.resolve_component(name).ok_or_else(|| {
        format!(
            "{}{name}",
            crate::runtime::ecs::entities::text::COMPONENT_UNAVAILABLE
        )
    })?;
    // the header, the identity and every root value were checked on these bytes: all of them are depended on
    let depends = vec![1u8; before.len()];
    ecs.compare_exchange(handle, component.component, before, mask, after, &depends)
}

/// ShroudForge's `update_attribute` (update_attribute.rs): one attribute set on the entity, its whole root recalculated
/// on the archive and written in one compare-exchange; the root's values after it, `applied = true`.
pub fn update_attribute(
    lua: &Lua,
    attributes: &super::catalog::Attributes,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: MultiValue,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let execute = || -> Result<Value, String> {
        gate(host, Op::EcsWrite)?;
        if args.len() != 3 && args.len() != 4 {
            return Err(
                "expected entity, attribute name/ID, and (for updates) scalar value plus optional expected_values"
                    .into(),
            );
        }
        let handle = entity(&args[0])?;
        let open = attributes.open()?;
        let catalog = &open.catalog;
        let (descriptor, component, before) =
            entity_storage(ecs, catalog, catalog.get(&args[1])?, handle)?;
        let name = component["qualified_name"]
            .as_str()
            .ok_or("unresolved attribute component")?
            .to_owned();
        let scalar = storage_scalar(&component)?.to_owned();
        let root = uint(&descriptor["root_hash"])?;
        let entries = catalog.root_entries(root)?;
        let (start, end, values) = storage_words(&component, &before)?;
        if entries.len() != values.len() {
            return Err("attribute root storage count changed".into());
        }
        if args.len() == 4 {
            let LuaValue::Table(expected) = &args[3] else {
                return Err(
                    "expected_values must be a table of original attribute names and values".into(),
                );
            };
            expect_root_values(catalog, root, &scalar, &values, expected)?;
        }
        let mut after = before.clone();
        let mut mask = vec![0u8; before.len()];
        let range = storage_range(&descriptor, &component, &before)?;
        after[range.clone()].copy_from_slice(&encode_attribute_scalar(
            &descriptor,
            &scalar,
            &args[2],
        )?);
        mask[range].fill(1);
        let model = attribute_model(ecs.services());
        let trace = recalculate_bound_component_update(
            &open, &name, &before, &mut after, &mut mask, &model,
        )?;
        let values: Vec<u32> = after[start..end]
            .as_chunks::<4>()
            .0
            .iter()
            .map(|v| u32::from_le_bytes(*v))
            .collect();
        exchange(ecs, host, handle, &name, &before, &mask, &after)?;
        let mut result = root_result(&entries, &scalar, &values, trace)?;
        result["entity"] = json!(handle);
        result["component"] = component;
        result["applied"] = json!(true);
        result["effect_scope"] = json!("local process, no automatic client/server RPC");
        Ok(result)
    };
    Ok(match execute() {
        Ok(value) => (super::members::json_to_lua(lua, &value)?, None),
        Err(reason) => (LuaValue::Nil, Some(reason)),
    })
}

/// ShroudForge's `write_attribute_storage` (write_attribute_storage.rs): one attribute's storage set as given (the
/// component's attribute recalculation still runs): `true`, or `false, reason`.
pub fn write_attribute_storage(
    _: &Lua,
    attributes: &super::catalog::Attributes,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: MultiValue,
) -> mlua::Result<(bool, Option<String>)> {
    let execute = || -> Result<(), String> {
        gate(host, Op::EcsWrite)?;
        if args.len() != 3 {
            return Err("expected entity, attribute name/ID, and (for writes) scalar value".into());
        }
        let handle = entity(&args[0])?;
        let open = attributes.open()?;
        let catalog = &open.catalog;
        let (descriptor, component, mut bytes) =
            entity_storage(ecs, catalog, catalog.get(&args[1])?, handle)?;
        let name = component["qualified_name"]
            .as_str()
            .ok_or("unresolved attribute component")?
            .to_owned();
        let range = storage_range(&descriptor, &component, &bytes)?;
        let scalar = storage_scalar(&component)?.to_owned();
        if descriptor["storage_writable"] != true {
            return Err(descriptor["storage_reason"]
                .as_str()
                .unwrap_or("unverified attribute write layout")
                .into());
        }
        let encoded = encode_attribute_scalar(&descriptor, &scalar, &args[2])?;
        let before = bytes.clone();
        let mut mask = vec![0u8; bytes.len()];
        mask[range.clone()].fill(1);
        bytes[range].copy_from_slice(&encoded);
        // ShroudForge's recalculate_component_update: this is an attribute component (entity_storage resolved it)
        if is_attribute_component(ecs.types(), &name)? {
            recalculate_attribute_component(
                attributes, ecs, &name, &before, &mut bytes, &mut mask,
            )?;
        }
        exchange(ecs, host, handle, &name, &before, &mask, &bytes)
    };
    Ok(match execute() {
        Ok(()) => (true, None),
        Err(reason) => (false, Some(reason)),
    })
}
