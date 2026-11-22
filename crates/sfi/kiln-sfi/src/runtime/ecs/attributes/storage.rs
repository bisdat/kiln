// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/attributes/{read_attribute_storage_range,
// resolve_attribute_storage,read_entity_attribute_storage}.rs; game-runtime/bridge/windows/ecs/read_components.rs:4-73
// (`read_many`: the checked batch read, absence proven).
//! An entity's attribute storage: which of its components holds an attribute's root, and where in it.
//!
//! - [`storage_range`]: on one owned snapshot of a component, the attribute's 4 bytes, after the snapshot's size, the
//!   initialized header's root ID, the definition GUID (when the layout has one), the header's storage offset and count
//!   and its Initialized bit agree with the reflected layout. Nothing is read from a pointer the bytes hold.
//! - [`entity_storage`]: the candidates (each reflected layout of the root's storage size that kiln.types' join resolves
//!   to a registered component of the same size), one batch read of them on the entity ([`read_many`]), then
//!   [`resolve_storage`]: exactly one readable candidate whose initialized header names the root, the descriptor bound
//!   to it (`bind_storage`).
//! - [`read_many`]: kiln.world's `ecs_snapshot_many` of the candidates, every one validated before and after its copy,
//!   each spec's presence proven (present or absent; anything else fails the whole batch), at most 64 components and
//!   1 MiB, the names unique, in ShroudForge's texts.

use super::catalog::{Catalog, guid_bytes, read_header, text};
use super::uint;
use crate::runtime::ecs::Ecs;
use crate::runtime::ecs::registry::Resolved;
use crate::service_member;
use kiln_abi::KILN_OK;
use kiln_abi::services::kiln_world::{
    KILN_ECS_ABSENT, KILN_ECS_OK, KilnEcsSnapshotSpec, KilnWorldApi,
};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::ops::Range;

/// ShroudForge's texts of the batch read (read_components.rs).
pub mod batch {
    pub const COUNT: &str = "component batch expects 1 to 64 selections";
    pub const SIZE: &str = "component batch size exceeds 1 MiB";
    pub const NAMES: &str = "component batch names must be unique";
    pub const CHANGED: &str = "component batch snapshot is unavailable or changed";
    pub const NO_BATCH: &str = "native provider lacks checked component batch reads. Install the matching runtime provider";
    pub const PRESENCE: &str = "component batch returned an invalid presence marker";
}

/// SF `storage_range` (read_attribute_storage_range.rs): the attribute's 4 bytes in one owned snapshot of `component`.
/// Relocated or external storage and a definition that differs are refused, never followed.
pub fn storage_range(
    descriptor: &Value,
    component: &Value,
    bytes: &[u8],
) -> Result<Range<usize>, String> {
    if bytes.len() != uint(&component["size"])? as usize {
        return Err("attribute component size changed".into());
    }
    let header = &component["header"];
    if read_header(header, "rootId", bytes)? != uint(&descriptor["root_hash"])? {
        return Err(text::ROOT_DIFFERS.into());
    }
    if !component["definition_offset"].is_null() {
        let definition = uint(&component["definition_offset"])? as usize;
        let expected = guid_bytes(
            component["definition_guid"]
                .as_str()
                .ok_or("missing GUID")?,
        )
        .ok_or("invalid GUID")?;
        if bytes.get(definition..definition + 16) != Some(expected.as_slice()) {
            return Err(text::DEFINITION_DIFFERS.into());
        }
    }
    let offset = read_header(header, "storageOffset", bytes)? as usize;
    // Both storageSize and storageCount count elements.
    let count = read_header(header, "storageCount", bytes)? as usize;
    let flags = read_header(header, "flags", bytes)?;
    let initialized = uint(&header["flags"]["initialized_mask"])?;
    if initialized == 0
        || flags & initialized == 0
        || offset != uint(&component["storage_offset"])? as usize
        || count.checked_mul(4) != Some(uint(&component["storage_size"])? as usize)
    {
        return Err("attribute is uninitialized or uses an unverified storage layout".into());
    }
    let start = offset
        .checked_add(uint(&descriptor["index"])? as usize * 4)
        .ok_or("attribute offset overflow")?;
    if start + 4 > bytes.len() || start + 4 > offset + count * 4 {
        return Err("attribute element outside storage".into());
    }
    Ok(start..start + 4)
}

/// SF `resolve_storage` (resolve_attribute_storage.rs): the one component `read` gives a snapshot of whose initialized
/// header names the descriptor's root: the bound descriptor, the component (its association made the entity's) and its
/// bytes. Type names select registered storage; they never decide which root a component holds.
pub fn resolve_storage<S: AsRef<[u8]>>(
    catalog: &Catalog,
    descriptor: &Value,
    mut read: impl FnMut(&Value) -> Option<S>,
) -> Result<(Value, Value, Vec<u8>), String> {
    let count = catalog.root_entries(uint(&descriptor["root_hash"])?)?.len();
    let mut found = None;
    let mut readable = 0;
    for component in &catalog.layouts {
        if component["storage_size"].as_u64() != Some(count as u64 * 4) {
            continue;
        }
        let Some(bytes) = read(component) else {
            continue;
        };
        readable += 1;
        if storage_range(descriptor, component, bytes.as_ref()).is_err() {
            continue;
        }
        if found.is_some() {
            return Err(
                "attribute root belongs to multiple initialized components on this entity".into(),
            );
        }
        let mut component = component.clone();
        component["association"] = json!("entity-initialized-root-id-and-reflected-static-array");
        let bound = catalog.bind_storage(descriptor, &component)?;
        found = Some((bound, component, bytes.as_ref().to_vec()));
    }
    found.ok_or_else(|| {
        format!(
            "no initialized component on this entity matches attribute root {:#010x} ({readable} compatible component snapshots read)",
            uint(&descriptor["root_hash"]).unwrap_or_default()
        )
    })
}

/// SF `read_many` (read_components.rs): the components `selections` name (qualified name, registered component) of
/// the entity `handle` names, by name; an absent one is not in the map. kiln.world's `ecs_snapshot_many`, all or
/// nothing.
pub fn read_many(
    ecs: &Ecs,
    handle: u32,
    selections: &[(String, Resolved)],
) -> Result<HashMap<String, Vec<u8>>, String> {
    if selections.is_empty() {
        return Ok(HashMap::new());
    }
    let many = ecs
        .world()
        .and_then(|table| service_member!(table, KilnWorldApi, ecs_snapshot_many))
        .map_err(|_| batch::NO_BATCH)?;
    if selections.len() > 64 {
        return Err(batch::COUNT.into());
    }
    let total = selections
        .iter()
        .try_fold(0usize, |size, (_, layout)| {
            if layout.size == 0 {
                return None;
            }
            size.checked_add(layout.size as usize)
        })
        .filter(|total| *total <= 1024 * 1024)
        .ok_or(batch::SIZE)?;
    if selections
        .iter()
        .any(|(name, _)| selections.iter().filter(|(other, _)| name == other).count() != 1)
    {
        return Err(batch::NAMES.into());
    }
    let object = ecs.object(handle).ok_or(batch::CHANGED)?;
    let specs: Vec<KilnEcsSnapshotSpec> = selections
        .iter()
        .map(|(_, layout)| KilnEcsSnapshotSpec {
            component: layout.component,
            flags: 0,
        })
        .collect();
    let mut buffer = vec![0u8; total];
    let mut presence = vec![u32::MAX; selections.len()];
    let mut required = 0u32;
    // SAFETY: kiln_world.h ecs_snapshot_many: the caller's ref, `specs.len()` specs, a buffer of `total` bytes (at most
    // 1 MiB, so it fits u32), an out count and one presence word per spec.
    let result = unsafe {
        ecs.services().call(
            many,
            (
                ecs.owner(),
                &raw const object,
                specs.as_ptr(),
                specs.len() as u32,
                buffer.as_mut_ptr().cast::<core::ffi::c_void>(),
                total as u32,
                &raw mut required,
                presence.as_mut_ptr(),
            ),
        )
    };
    if result != KILN_OK {
        return Err(batch::CHANGED.into());
    }
    let mut result = HashMap::new();
    let mut start = 0usize;
    for ((name, layout), state) in selections.iter().zip(presence) {
        match state as i32 {
            KILN_ECS_ABSENT => {}
            KILN_ECS_OK => {
                let end = start + layout.size as usize;
                let bytes = buffer.get(start..end).ok_or(batch::CHANGED)?;
                result.insert(name.clone(), bytes.to_vec());
                start = end;
            }
            _ => return Err(batch::PRESENCE.into()),
        }
    }
    if start != required as usize {
        return Err(batch::CHANGED.into());
    }
    Ok(result)
}

/// SF `entity_storage` (read_entity_attribute_storage.rs): the descriptor's storage on the entity `handle` names: the
/// bound descriptor, its component and the component's bytes.
pub fn entity_storage(
    ecs: &Ecs,
    catalog: &Catalog,
    descriptor: &Value,
    handle: u32,
) -> Result<(Value, Value, Vec<u8>), String> {
    let count = catalog.root_entries(uint(&descriptor["root_hash"])?)?.len();
    let candidates: Vec<(String, Resolved)> = catalog
        .layouts
        .iter()
        .filter_map(|component| {
            if component["storage_size"].as_u64() != Some(count as u64 * 4) {
                return None;
            }
            let name = component["qualified_name"].as_str()?;
            let layout = ecs.resolve_component(name)?;
            (Some(u64::from(layout.size)) == component["size"].as_u64())
                .then(|| (name.to_owned(), layout))
        })
        .collect();
    let snapshots = read_many(ecs, handle, &candidates)?;
    // Every compatible initialized root is still checked.
    resolve_storage(catalog, descriptor, |component| {
        snapshots
            .get(component["qualified_name"].as_str()?)
            .map(Vec::as_slice)
    })
}
