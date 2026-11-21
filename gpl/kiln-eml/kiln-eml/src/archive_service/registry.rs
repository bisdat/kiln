// SPDX-License-Identifier: GPL-3.0-or-later
//! The immutable registry snapshot behind `registry_metadata`.
//!
//! Built once on the worker that owns the registry, from the registry itself (not from Lua or the Type userdata
//! cache), then shared read-only by any thread: each type's metadata JSON (schema/sfi/eml-archive/common.schema.json
//! `type`) is serialized here, in registry order, so a page is only a slice of owned bytes pinned by its generation.
//! A type whose effective fields cannot be listed (an unresolved or cyclic base) keeps its original cause instead, and
//! a page that covers it fails with that cause.
//!
//! The same facts the type session's `game.types` exposes (both SF pins: lua/bindings/game/types.rs:42-44,245-322,374-505), with
//! nothing collapsed: the original index, all four hash domains, default bytes exactly (absent is null), u64 enum
//! values, flags, struct fields in declaration order and the effective fields with their declaring types.
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

use kfc::reflection::{Attribute, TypeMetadata, TypeRegistry};
use sha2::{Digest, Sha256};

use crate::sf_startup::game::{resolve_type, types::primitive_type_to_string};

static NEXT_REGISTRY: AtomicU64 = AtomicU64::new(1);

/// One registry's metadata, frozen. `id` is process-unique and nonzero. `sha256` (the identity's registry_sha256, a
/// Kiln definition) covers every byte a page can return except its pin: SHA-256 over the domain text
/// `kiln.eml.archive registry v1\0`, then the registry version (tag 0) and the game version (tag 3), then each type
/// in registry order, its JSON (tag 1) or its failure text (tag 2); each item is its tag byte, its length as u64 little
/// endian and its bytes. Equal snapshots have equal digests; any difference a page could show changes it.
pub struct RegistrySnapshot {
    id: u64,
    sha256: [u8; 32],
    game_version: String,
    registry_version: String,
    types: Vec<Result<String, Arc<[u8]>>>,
}

impl std::fmt::Debug for RegistrySnapshot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RegistrySnapshot")
            .field("id", &self.id)
            .field("types", &self.types.len())
            .finish()
    }
}

/// The registry digest's writer: the domain text, then tagged, length-prefixed items (the layout above).
struct Digester(Sha256);

impl Digester {
    fn new(registry: &TypeRegistry, game_version: &str) -> Self {
        let mut digest = Self(Sha256::new());
        digest.0.update(b"kiln.eml.archive registry v1\0");
        digest.field(0, registry.version.as_bytes());
        digest.field(3, game_version.as_bytes());
        digest
    }
    fn field(&mut self, tag: u8, bytes: &[u8]) {
        self.0.update([tag]);
        self.0.update((bytes.len() as u64).to_le_bytes());
        self.0.update(bytes);
    }
    fn of_type(&mut self, ty: &Result<String, Arc<[u8]>>) {
        match ty {
            Ok(json) => self.field(1, json.as_bytes()),
            Err(cause) => self.field(2, cause),
        }
    }
    fn finish(self) -> [u8; 32] {
        self.0.finalize().into()
    }
}

/// The `registry_sha256` a snapshot of `registry` with `game_version` has, computed type by type without keeping the
/// pages (kiln.eml.values' `arena_create` checks its own registry copy against a view's pins with it, the C table).
pub fn digest(registry: &TypeRegistry, game_version: &str) -> [u8; 32] {
    let mut digest = Digester::new(registry, game_version);
    for ty in registry.iter() {
        digest.of_type(&type_json(registry, ty));
    }
    digest.finish()
}

impl RegistrySnapshot {
    /// `game_version` is the selected archive index's data version (`KFCFile::game_version`), kept as text.
    pub fn new(registry: &TypeRegistry, game_version: impl Into<String>) -> Arc<Self> {
        let game_version = game_version.into();
        let types: Vec<_> = registry.iter().map(|ty| type_json(registry, ty)).collect();
        let mut digest = Digester::new(registry, &game_version);
        for ty in &types {
            digest.of_type(ty);
        }
        Arc::new(Self {
            id: NEXT_REGISTRY.fetch_add(1, Ordering::Relaxed),
            sha256: digest.finish(),
            game_version,
            registry_version: registry.version.clone(),
            types,
        })
    }

    pub fn id(&self) -> u64 {
        self.id
    }
    pub fn sha256(&self) -> &[u8; 32] {
        &self.sha256
    }
    /// The number of types (`count`).
    pub fn len(&self) -> u64 {
        self.types.len() as u64
    }
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }
    pub fn game_version(&self) -> &str {
        &self.game_version
    }
    pub fn registry_version(&self) -> &str {
        &self.registry_version
    }

    /// The registry.schema.json reply for ordinals `first..end` (already checked against `len`), with `pin` as the
    /// serialized pin object. The first type without metadata fails the page with its original cause.
    pub(super) fn page(&self, pin: &str, first: u64, end: u64) -> Result<String, Arc<[u8]>> {
        let mut out = String::new();
        out.push_str("{\"schema_version\":1,\"pin\":");
        out.push_str(pin);
        out.push_str(",\"game_version\":");
        out.push_str(&string(&self.game_version));
        out.push_str(",\"registry_version\":");
        out.push_str(&string(&self.registry_version));
        out.push_str(&format!(
            ",\"count\":\"{}\",\"first\":\"{}\",\"types\":[",
            u64s(self.len()),
            u64s(first)
        ));
        for (n, ty) in self.types[first as usize..end as usize].iter().enumerate() {
            if n > 0 {
                out.push(',');
            }
            out.push_str(ty.as_ref().map_err(Arc::clone)?);
        }
        out.push_str("]}");
        Ok(out)
    }
}

pub(super) fn u64s(value: u64) -> String {
    format!("u64:{value:016x}")
}

pub(super) fn string(text: &str) -> String {
    serde_json::to_string(text).expect("a str serializes")
}

fn strings(items: &[String]) -> String {
    format!("[{}]", items.iter().map(|s| string(s)).collect::<Vec<_>>().join(","))
}

fn attributes<'a>(map: impl IntoIterator<Item = (&'a String, &'a Attribute)>) -> String {
    let entries: Vec<String> = map
        .into_iter()
        .map(|(key, a)| {
            format!(
                "{{\"key\":{},\"name\":{},\"namespace\":{},\"type_index\":{},\"value\":{}}}",
                string(key),
                string(&a.name),
                strings(&a.namespace),
                a.r#type
                    .map_or("null".into(), |t| format!("\"{}\"", u64s(t.as_usize() as u64))),
                string(&a.value)
            )
        })
        .collect();
    format!("[{}]", entries.join(","))
}

fn type_json(registry: &TypeRegistry, ty: &TypeMetadata) -> Result<String, Arc<[u8]>> {
    let effective = resolve_type::fields(registry, ty.index).map_err(|cause| {
        Arc::<[u8]>::from(format!("type {} (index {}): {cause}", ty.qualified_name, ty.index.as_usize()).into_bytes())
    })?;
    let index = |i: kfc::reflection::TypeIndex| format!("\"{}\"", u64s(i.as_usize() as u64));
    let struct_fields: Vec<String> = ty
        .struct_fields
        .values()
        .map(|f| {
            format!(
                "{{\"name\":{},\"type_index\":{},\"data_offset\":\"{}\",\"attributes\":{}}}",
                string(&f.name),
                index(f.r#type),
                u64s(f.data_offset),
                attributes(&f.attributes)
            )
        })
        .collect();
    let fields: Vec<String> = effective
        .iter()
        .map(|(owner, f)| {
            format!(
                "{{\"name\":{},\"type_index\":{},\"data_offset\":\"{}\",\"attributes\":{},\"declaring_type\":{}}}",
                string(&f.name),
                index(f.r#type),
                u64s(f.data_offset),
                attributes(&f.attributes),
                index(*owner)
            )
        })
        .collect();
    let enum_fields: Vec<String> = ty
        .enum_fields
        .values()
        .map(|f| {
            format!(
                "{{\"name\":{},\"value\":\"{}\",\"value_hex\":\"0x{:016x}\"}}",
                string(&f.name),
                u64s(f.value),
                f.value
            )
        })
        .collect();
    let flags: Vec<String> = ty.flags.iter_names().map(|(name, _)| name.to_owned()).collect();
    let default = match &ty.default_value {
        Some(bytes) => format!(
            "\"bytes:{}\"",
            bytes.iter().map(|b| format!("{b:02x}")).collect::<String>()
        ),
        None => "null".into(),
    };
    Ok(format!(
        concat!(
            "{{\"index\":{},\"name\":{},\"qualified_name\":{},\"impact_name\":{},\"namespace\":{},",
            "\"hashes\":{{\"qualified\":{},\"internal\":{},\"name\":{},\"impact\":{}}},\"inner_type\":{},",
            "\"size\":{},\"alignment\":{},\"element_alignment\":{},\"field_count\":{},\"primitive_type\":{},",
            "\"flags_bits\":{},\"flags\":{},\"has_ds\":{},\"default_bytes\":{},\"struct_fields\":[{}],",
            "\"fields\":[{}],\"enum_fields\":[{}],\"attributes\":{}}}"
        ),
        index(ty.index),
        string(&ty.name),
        string(&ty.qualified_name),
        string(&ty.impact_name),
        strings(&ty.namespace),
        ty.qualified_hash,
        ty.internal_hash,
        ty.name_hash,
        ty.impact_hash,
        ty.inner_type.map_or("null".into(), index),
        ty.size,
        ty.alignment,
        ty.element_alignment,
        ty.field_count,
        string(primitive_type_to_string(ty.primitive_type)),
        ty.flags.bits(),
        strings(&flags),
        ty.flags.contains(kfc::reflection::TypeFlags::HAS_DS),
        default,
        struct_fields.join(","),
        fields.join(","),
        enum_fields.join(","),
        attributes(&ty.attributes),
    ))
}
