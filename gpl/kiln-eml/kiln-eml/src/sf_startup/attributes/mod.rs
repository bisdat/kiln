// SPDX-License-Identifier: GPL-3.0-or-later
//! The archive's attribute catalog: ShroudForge's attribute definitions and component layouts, read from the
//! VM's reference archive and the session's registry, behind `runtime.ecs.get_attributes`, `get_attribute` and
//! `evaluate_attributes` (and so `runtime.overrides.attribute`).
//!
//! ShroudForge builds the catalog once per session, lazily, from its reference index (`AppState::attribute_catalog`,
//! session/mod.rs:123-136): every `keen::AttributeContainerResource` and `keen::BaseAttributeResource` the index holds
//! is decoded and serialized as kfc's compact JSON, each root's definition is checked, and one descriptor per attribute
//! is built as JSON; a failure is the catalog's answer for the whole session. Here the catalog belongs to the VM's one
//! `AssetSession` (the same reference index and reader) and is built the same way, with these differences:
//! - each resource's bytes are decoded through the value codec (`resource_values::decode_mapped`) and converted by its
//!   bounded walk (`mapped_to_value`: depth 128 and the value budget) instead of kfc's recursive
//!   `Value::from_bytes`, so a hostile definition is an error, never a stack overflow; on sane data the JSON is the
//!   same;
//! - ShroudForge's `unwrap`s stay unreachable from archive data: each one guards data ShroudForge built or checked
//!   itself (`from_resources` checks every program word before `decode_program` reads it); those turned into errors
//!   here anyway answer ShroudForge's own text (`decode_program`'s word is `uint`'s error);
//! - the in-game halves (`read_header`, `bind_storage`: entity storage and the recalculation of live components) are
//!   not ported: no entity exists before the game, and every member that would use them refuses in the pregame phase
//!   first.
//!
//! The JSON keeps ShroudForge's key order: this crate graph builds serde_json with `preserve_order`, as ShroudForge's
//! workspace does.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs), the same at the
//! fork pin 2df1de68: modules/mod-runtime/reflection/build_attribute_catalog.rs (all but `read_header` and
//! `bind_storage`) and reflection/evaluate_attribute_program.rs ([`program`]); inheritance and fields are the type session's port of
//! reflection/traverse_reflected_types.rs.
use std::{cell::OnceCell, collections::BTreeMap, rc::Rc};

use kfc::{
    container::KFCFile,
    guid::{Guid, ResourceId},
    reflection::{LookupKey, PrimitiveType, TypeIndex, TypeRegistry},
};
use serde_json::{Value, json};

use super::{
    format_adapter,
    game::resolve_type::{fields, inheritance},
    resource_values::{self, TypeHandle},
};

pub mod program;

use program::Scalar;

/// SF `attributes::Catalog`: the descriptors by attribute ID and the reflected component layouts.
#[derive(Default)]
pub struct Catalog {
    pub entries: BTreeMap<u32, Value>,
    pub layouts: Vec<Value>,
    // The loaded catalog is immutable: index IDs rather than cloned descriptors.
    indexes: OnceCell<CatalogIndexes>,
}

#[derive(Default)]
struct CatalogIndexes {
    // None keeps a duplicated name ambiguous instead of picking the first ID.
    names: BTreeMap<String, Option<u32>>,
    // None marks a root whose reflected indices are missing or duplicated.
    roots: BTreeMap<u32, Option<Vec<u32>>>,
}

impl CatalogIndexes {
    fn build(entries: &BTreeMap<u32, Value>) -> Self {
        let mut names = BTreeMap::new();
        let mut roots = BTreeMap::<u32, Vec<u32>>::new();
        for (&id, entry) in entries {
            if let Some(name) = entry["name"].as_str() {
                names
                    .entry(name.to_owned())
                    .and_modify(|existing| *existing = None)
                    .or_insert(Some(id));
            }
            if let Ok(root) = uint(&entry["root_hash"]) {
                roots.entry(root).or_default().push(id);
            }
        }
        let roots = roots
            .into_iter()
            .map(|(root, mut ids)| {
                ids.sort_by_key(|id| entries[id]["index"].as_u64());
                let complete = ids.iter().enumerate().all(|(index, id)| entries[id]["index"] == index);
                (root, complete.then_some(ids))
            })
            .collect();
        Self { names, roots }
    }
}

/// SF `header_layout`: an attribute component's header fields (rootId, storageOffset, storageCount or storageSize,
/// flags) with their reflected widths (KFC3 wide, KFC4 compact) and the flags' Initialized bit.
pub fn header_layout(registry: &TypeRegistry, index: TypeIndex) -> Result<Value, String> {
    fn width(registry: &TypeRegistry, index: TypeIndex, depth: usize) -> Result<u32, String> {
        if depth > 16 {
            return Err("cyclic or excessively nested attribute header type".into());
        }
        let ty = registry.get(index).ok_or("unresolved attribute header type")?;
        let expected = match ty.primitive_type {
            PrimitiveType::UInt8 | PrimitiveType::Bitmask8 => 1,
            PrimitiveType::UInt16 | PrimitiveType::Bitmask16 => 2,
            PrimitiveType::UInt32 | PrimitiveType::Bitmask32 => 4,
            PrimitiveType::Typedef | PrimitiveType::Enum => width(
                registry,
                ty.inner_type.ok_or("missing attribute header storage type")?,
                depth + 1,
            )?,
            PrimitiveType::Struct => {
                let fields = fields(registry, index)?;
                let [(_, field)] = fields.as_slice() else {
                    return Err("attribute header wrapper has multiple or missing fields".into());
                };
                if field.data_offset != 0 {
                    return Err("attribute header wrapper does not start at zero".into());
                }
                width(registry, field.r#type, depth + 1)?
            }
            _ => return Err("attribute header is not an unsigned inline integer".into()),
        };
        if ty.size != expected {
            return Err("attribute header type size differs from its integer storage".into());
        }
        Ok(expected)
    }

    let ty = registry.get(index).ok_or("unresolved attribute component")?;
    let fields = fields(registry, index)?;
    let mut result = json!({});
    for (key, names) in [
        ("rootId", &["rootId"][..]),
        ("storageOffset", &["storageOffset"][..]),
        ("storageCount", &["storageCount", "storageSize"][..]),
        ("flags", &["flags"][..]),
    ] {
        let matches: Vec<_> = fields
            .iter()
            .filter(|(_, field)| names.contains(&field.name.as_str()))
            .collect();
        let [(_, field)] = matches.as_slice() else {
            return Err(format!("missing or ambiguous attribute header field {key}"));
        };
        let size = width(registry, field.r#type, 0)?;
        if field
            .data_offset
            .checked_add(u64::from(size))
            .is_none_or(|end| end > u64::from(ty.size))
        {
            return Err(format!("attribute header field {key} exceeds its component"));
        }
        result[key] = json!({"name":field.name,"offset":field.data_offset,"size":size});
        if key == "flags" {
            let flags = registry.get(field.r#type).ok_or("unresolved attribute flags type")?;
            if !matches!(
                flags.primitive_type,
                PrimitiveType::Bitmask8 | PrimitiveType::Bitmask16 | PrimitiveType::Bitmask32
            ) {
                return Err("attribute flags have no reflected bitmask representation".into());
            }
            let initialized = flags
                .inner_type
                .and_then(|index| registry.get(index))
                .and_then(|ty| ty.enum_fields.values().find(|field| field.name == "Initialized"))
                .ok_or("attribute flags have no reflected Initialized bit")?;
            if initialized.value >= u64::from(size) * 8 {
                return Err("attribute Initialized bit exceeds flag storage".into());
            }
            result[key]["initialized_mask"] = json!(1u32 << initialized.value);
        }
    }
    let mut ranges: Vec<(u64, u64)> = header_fields(&result).map(field_range).collect();
    ranges.sort_unstable();
    if ranges.windows(2).any(|pair| pair[0].1 > pair[1].0) {
        return Err("attribute header fields overlap".into());
    }
    Ok(result)
}

/// The field records of a header [`header_layout`] built (always a JSON object of four).
fn header_fields(header: &Value) -> impl Iterator<Item = &Value> {
    header.as_object().into_iter().flat_map(|object| object.values())
}

/// A header field record's byte range (both numbers are set by [`header_layout`]).
fn field_range(field: &Value) -> (u64, u64) {
    let offset = field["offset"].as_u64().unwrap_or_default();
    (offset, offset + field["size"].as_u64().unwrap_or_default())
}

/// SF `component_layouts`: every `keen::ecs::*` type derived from `keen::ecs::AttributeComponent` whose
/// `dataStorage` is a static array of 4-byte scalars that fits the component and misses its header, with the
/// definition GUID from the reflected default when the type has a 16-byte `definition` field.
fn component_layouts(registry: &TypeRegistry) -> Result<Vec<Value>, String> {
    let mut layouts = Vec::new();
    for ty in registry
        .iter()
        .filter(|ty| ty.qualified_name.starts_with("keen::ecs::"))
    {
        if !inheritance(registry, ty.index)?.iter().any(|index| {
            registry
                .get(*index)
                .is_some_and(|base| base.qualified_name == "keen::ecs::AttributeComponent")
        }) {
            continue;
        }
        let fields = fields(registry, ty.index)?;
        let Some((_, storage)) = fields.iter().find(|(_, field)| field.name == "dataStorage") else {
            continue;
        };
        let Some(array) = registry.get(storage.r#type) else {
            continue;
        };
        let Some(element) = array.inner_type.and_then(|index| registry.get(index)) else {
            continue;
        };
        if array.primitive_type != PrimitiveType::StaticArray
            || element.size != 4
            || !matches!(
                element.primitive_type,
                PrimitiveType::SInt32 | PrimitiveType::UInt32 | PrimitiveType::Float32
            )
            || array.field_count.checked_mul(4) != Some(array.size)
            || storage
                .data_offset
                .checked_add(u64::from(array.size))
                .is_none_or(|end| end > u64::from(ty.size))
        {
            continue;
        }
        let Ok(header) = header_layout(registry, ty.index) else {
            continue;
        };
        let storage_end = storage.data_offset + u64::from(array.size);
        if header_fields(&header).any(|field| {
            let (start, end) = field_range(field);
            start < storage_end && end > storage.data_offset
        }) {
            continue;
        }
        let mut layout = json!({"qualified_name":ty.qualified_name,"qualified_hash":ty.qualified_hash,
            "size":ty.size,"storage_offset":storage.data_offset,"storage_size":array.size,"header":header,
            "storage_scalar_type":element.qualified_name,"engine_server_only":ty.attributes.contains_key("server_only"),
            "association":"reflected-attribute-storage"});
        if let Some((_, definition)) = fields.iter().find(|(_, field)| field.name == "definition")
            && registry.get(definition.r#type).is_some_and(|ty| ty.size == 16)
            && let Some(bytes) = ty.default_value.as_ref().and_then(|bytes| {
                let start = usize::try_from(definition.data_offset).ok()?;
                bytes.get(start..start.checked_add(16)?)
            })
            && let Ok(bytes) = <[u8; 16]>::try_from(bytes)
        {
            layout["definition_offset"] = json!(definition.data_offset);
            layout["definition_guid"] = json!(Guid::new(bytes).to_string());
        }
        layouts.push(layout);
    }
    Ok(layouts)
}

impl Catalog {
    fn indexes(&self) -> &CatalogIndexes {
        self.indexes.get_or_init(|| CatalogIndexes::build(&self.entries))
    }

    /// SF `by_name`: a unique original name's descriptor; a duplicated name is refused, never the first match.
    pub fn by_name(&self, name: &str) -> Result<&Value, String> {
        match self.indexes().names.get(name) {
            Some(Some(id)) => self.entries.get(id).ok_or_else(|| "attribute ID not found".into()),
            Some(None) => Err("Ambiguous attribute name. Use its ID".into()),
            None => Err(format!("attribute not found: {name}")),
        }
    }

    /// SF `Catalog::load`: the definitions of the reference index `file` (every AttributeContainerResource, then every
    /// BaseAttributeResource, each in the index's order), `read` giving a resource's bytes (None: missing).
    pub fn load(
        registry: &Rc<TypeRegistry>,
        file: &KFCFile,
        mut read: impl FnMut(&ResourceId) -> Result<Option<Vec<u8>>, String>,
    ) -> Result<Self, String> {
        let mut resources = Vec::new();
        for name in ["keen::AttributeContainerResource", "keen::BaseAttributeResource"] {
            let Some(ty) = registry.get_by_name(LookupKey::Qualified(name)) else {
                continue;
            };
            let handle = TypeHandle::new(registry.clone(), ty.index);
            for id in format_adapter::resources_by_type(file, registry, name) {
                let Some(bytes) = read(&id)? else {
                    return Err(format!("missing attribute resource {id}"));
                };
                let source_len = bytes.len();
                // kfc's compact JSON of the decoded value (SF `Value::from_bytes`), through the codec's bounded walk
                let mapped = resource_values::decode_mapped(Rc::from(bytes), &handle)?;
                let value = resource_values::mapped_to_value(mapped, source_len)?;
                let value = serde_json::to_value(&value).map_err(|e| e.to_string())?;
                resources.push(json!({"id":id.guid().to_string(),"type":name,"value":value}));
            }
        }
        Self::from_resources(registry, &resources)
    }

    /// SF `from_resources`: one record per root (its definition must be the same wherever it appears), then one
    /// descriptor per attribute of each root.
    pub fn from_resources(types: &TypeRegistry, resources: &[Value]) -> Result<Self, String> {
        let mut roots: BTreeMap<u32, (Value, Vec<String>, Vec<String>)> = BTreeMap::new();
        for resource in resources {
            let container = resource["type"] == "keen::AttributeContainerResource";
            let values: Vec<_> = if container {
                resource["value"]["attributes"]
                    .as_array()
                    .ok_or("missing container attributes")?
                    .iter()
                    .collect()
            } else {
                vec![&resource["value"]]
            };
            for value in values {
                let root = uint(&value["ids"][0]["value"])?;
                let record = roots
                    .entry(root)
                    .or_insert_with(|| (value.clone(), Vec::new(), Vec::new()));
                if record.0 != *value {
                    return Err(format!("conflicting container/base definition for {root:#010x}"));
                }
                let ids = if container { &mut record.2 } else { &mut record.1 };
                ids.push(resource["id"].as_str().ok_or("missing resource GUID")?.to_owned());
            }
        }
        if roots.is_empty() {
            return Err("no attribute definitions in this KFC container".into());
        }
        let operations: BTreeMap<u32, String> = types
            .get_by_name(LookupKey::Qualified("keen::AttributeOps"))
            .map(|ty| {
                ty.enum_fields
                    .values()
                    .map(|field| (field.value as u32, field.name.clone()))
                    .collect()
            })
            .unwrap_or_default();
        let mut catalog = Self {
            layouts: component_layouts(types)?,
            ..Self::default()
        };
        let encoding_verified = [
            (1, "Load0"),
            (2, "Load"),
            (3, "LoadRef"),
            (5, "Push"),
            (6, "Add"),
            (7, "Substract"),
            (8, "Multiply"),
            (12, "Clamp"),
            (13, "ScaleToNewMax"),
            (15, "Max"),
        ]
        .iter()
        .all(|(opcode, name)| operations.get(opcode).map(String::as_str) == Some(*name));
        for (root, (value, resource_ids, container_ids)) in roots {
            let ids = value["ids"].as_array().ok_or("missing attribute IDs")?;
            let names = value["debugNames"].as_array().ok_or("missing attribute names")?;
            let structure = value["structure"].as_array().ok_or("missing attribute structure")?;
            if ids.len() != names.len() || ids.len() != structure.len() || ids.len() > 65535 {
                return Err("attribute name/ID/structure lengths disagree".into());
            }
            let scalar_hash = uint(&value["type"]["value"])?;
            let scalar_types: Vec<_> = types
                .iter()
                .filter(|t| {
                    t.internal_hash == scalar_hash
                        && t.size == 4
                        && matches!(
                            t.primitive_type,
                            PrimitiveType::SInt32 | PrimitiveType::UInt32 | PrimitiveType::Float32
                        )
                })
                .collect();
            let scalar = match scalar_types.as_slice() {
                [t] => Some(*t),
                _ => None,
            };
            let mut components = Vec::new();
            for layout in &catalog.layouts {
                let Some(guid) = layout["definition_guid"].as_str() else {
                    continue;
                };
                let guid_bytes = Guid::parse(guid).ok_or("invalid reflected attribute definition GUID")?;
                // The engine initializer hashes the GUID bytes, not a display name. This also resolves roots only
                // present in the container.
                if definition_root(guid_bytes.data()) != root {
                    continue;
                }
                if !resource_ids.is_empty() && !resource_ids.iter().any(|id| id == guid) {
                    return Err(format!("attribute definition GUID/hash collision for {root:#010x}"));
                }
                if layout["storage_size"].as_u64() != Some(ids.len() as u64 * 4) {
                    continue;
                }
                let mut component = layout.clone();
                component["scalar_type_matches"] = json!(
                    scalar.is_some_and(|s| Some(s.qualified_name.as_str()) == layout["storage_scalar_type"].as_str())
                );
                component["integer_representation_compatible"] = json!(
                    scalar.is_some_and(|s| matches!(s.primitive_type, PrimitiveType::SInt32 | PrimitiveType::UInt32))
                        && matches!(
                            layout["storage_scalar_type"].as_str(),
                            Some("keen::sint32" | "keen::uint32")
                        )
                );
                component["association"] = json!("fnv1a-definition-guid-bytes-and-static-array");
                components.push(component);
            }
            let scalar_matches = components.len() == 1 && components[0]["scalar_type_matches"] == true;
            let common_integer = components.len() == 1 && components[0]["integer_representation_compatible"] == true;
            // Kiln's one admitted signedness conflict ([`engine_signed_root`]): calculated as the resource's sint32.
            let engine_signed = components.len() == 1
                && engine_signed_root(
                    root,
                    scalar.map(|t| t.qualified_name.as_str()),
                    components[0]["storage_scalar_type"].as_str(),
                );
            // These instructions operate on the same 32-bit representation for signed/unsigned interpreters,
            // including wrapping arithmetic. Ordering, division and rescaling are intentionally excluded.
            let sign_independent = structure.iter().all(|node| {
                let Some(words) = node["calculation"].as_array() else {
                    return false;
                };
                let mut pc = 0;
                while pc < words.len() {
                    let Ok(word) = uint(&words[pc]) else {
                        return false;
                    };
                    pc += 1;
                    match word >> 16 {
                        1 | 2 | 6 | 7 | 8 => {}
                        5 if pc < words.len() => pc += 1,
                        _ => return false,
                    }
                }
                true
            });
            for (index, id) in ids.iter().enumerate() {
                let hash = uint(&id["value"])?;
                let name = names[index]
                    .as_str()
                    .filter(|n| !n.is_empty())
                    .ok_or("empty original attribute name")?;
                for link in ["parentIndex", "childIndex", "siblingIndex"] {
                    let target = uint(&structure[index][link])?;
                    if target != 65535 && target as usize >= ids.len() {
                        return Err(format!("invalid attribute {link}"));
                    }
                }
                let words = structure[index]["calculation"]
                    .as_array()
                    .ok_or("missing calculation words")?;
                for word in words {
                    uint(word)?;
                }
                let descriptor = json!({"name":name,"hash":hash,"hash_domain":"attribute-id",
                    "name_source":"kfc-debugNames","root_hash":root,"index":index,"resource_ids":resource_ids,
                    "container_ids":container_ids,"scalar_type_hash":scalar_hash,
                    "scalar_type":scalar.map(|t| t.qualified_name.as_str()),"components":components,
                    "calculation_scalar_type":if components.len() == 1 && !engine_signed && (scalar_matches || (common_integer && sign_independent)) {
                        components[0]["storage_scalar_type"].clone()
                    } else {json!(scalar.map(|t| t.qualified_name.as_str()))},
                    "structure":structure[index],"calculation_words":words,
                    "calculation":decode_program(words, &operations, ids, names)?,
                    "calculation_encoding_verified":encoding_verified,
                    "calculation_sign_independent":sign_independent,
                    "storage_resolution":if components.is_empty() {"per-entity-initialized-root-id"} else {"reflected-definition-guid"},
                    "literal_encoding":if scalar.is_some_and(|s| s.primitive_type == PrimitiveType::Float32) {"uint32 converted to float32"} else {"32-bit integer word"},
                    "calculation_execution":"attribute-command-v1, descending root index, guarded updates require exact build validation",
                    "effect_scope":"local process, network authority and publication remain engine-managed",
                    "storage_writable":scalar_matches || common_integer,
                    "write_value_domain":if !scalar_matches && common_integer {"integer-common-range-0-to-2147483647"} else {"declared-scalar"},
                    "calculation_writable":scalar_matches || (common_integer && (sign_independent || engine_signed)),
                    "calculation_reason":if components.is_empty() {Some("calculation storage type requires an initialized entity root match")}
                        else if !scalar_matches && !sign_independent && !engine_signed {Some("signedness-dependent program with conflicting resource/storage scalar types")} else {None},
                    "storage_reason":if components.is_empty() {Some("no proven component/root association. Compact headers without a reflected definition GUID require an independently observed initialized root ID")}
                        else if !scalar_matches && !common_integer {Some("KFC scalar type differs from reflected storage")}
                        else if components.len() != 1 {Some("ambiguous component ownership")} else {None}});
                if catalog.entries.insert(hash, descriptor).is_some() {
                    return Err(format!("duplicate attribute ID {hash:#010x}"));
                }
            }
        }
        Ok(catalog)
    }

    /// SF `root_entries`: a root's descriptors in reflected index order; a root with a missing or repeated index is
    /// incomplete.
    pub fn root_entries(&self, root: u32) -> Result<Vec<&Value>, String> {
        let ids = self
            .indexes()
            .roots
            .get(&root)
            .and_then(Option::as_ref)
            .ok_or("incomplete attribute root")?;
        ids.iter()
            .map(|id| self.entries.get(id).ok_or_else(|| "incomplete attribute root".into()))
            .collect()
    }

    /// SF `recalculate`: the engine's recompute walks the root from its last entry to its first; ScaleToNewMax can
    /// update an earlier referenced value before that entry is evaluated. The values and a trace of every program run.
    pub fn recalculate(&self, root: u32, values: &[u32]) -> Result<(Vec<u32>, Vec<Value>), String> {
        let entries = self.root_entries(root)?;
        // a root in the index has at least one entry (SF indexes entries[0])
        let first = entries.first().ok_or("incomplete attribute root")?;
        if first["calculation_encoding_verified"] != true {
            return Err("engine AttributeOps enum differs from the verified model".into());
        }
        if entries.len() != values.len() {
            return Err("attribute root value count mismatch".into());
        }
        let kind = Scalar::parse(first["calculation_scalar_type"].as_str().ok_or("unknown scalar")?)?;
        let mut result = values.to_vec();
        let mut trace = Vec::new();
        for (i, entry) in entries.iter().enumerate().rev() {
            let words: Vec<u32> =
                serde_json::from_value(entry["calculation_words"].clone()).map_err(|e| e.to_string())?;
            if words.is_empty() {
                continue;
            }
            let before = result.clone();
            if let Some(value) = program::evaluate(kind, &mut result, &words)? {
                result[i] = value;
            }
            trace.push(json!({"name":entry["name"],"hash":entry["hash"],"index":i,
                "before_bits":before,"after_bits":result}));
        }
        Ok((result, trace))
    }
}

/// The roots whose calculation Kiln admits although their resource and storage scalar types differ in signedness and
/// their programs depend on it (ShroudForge refuses them all): BodyHeat (0xC7AEAF1D) alone. The game's attribute
/// resource types the root sint32; its component, keen::ecs::BodyHeat, is a keen::ecs::UInt32Attribute whose storage is
/// keen::uint32; BodyHeat runs Clamp and BodyHeat_Max runs ScaleToNewMax. The engine evaluates this root with its
/// signed interpreter only (client exe 5d9ef2f1, Steam build 26430625; server exe f0d824be): its attribute dispatcher
/// picks the interpreter by the registry's resource type (sint32: the signed one), and the BodyHeat recharge and
/// depletion code and attrAdd recompute the root through the signed recalculation (docs/re/combat.md, "BodyHeat runs").
/// So the descriptor keeps the resource's scalar as its `calculation_scalar_type` (what [`Catalog::recalculate`] runs)
/// and the common-range write domain of every conflicting integer pair. Every other conflicting root stays refused.
/// The same rule as kiln-sfi's `catalog::ENGINE_SIGNED_ROOTS` and the ShroudForge fork's `bind_storage`.
pub const ENGINE_SIGNED_ROOTS: [u32; 1] = [0xC7AE_AF1D];

/// Whether `root` is one of [`ENGINE_SIGNED_ROOTS`] with exactly the pair proven for it: the resource's keen::sint32
/// over keen::uint32 storage. The reverse pair, a float, or another root is not admitted.
pub fn engine_signed_root(root: u32, scalar: Option<&str>, storage: Option<&str>) -> bool {
    scalar == Some("keen::sint32") && storage == Some("keen::uint32") && ENGINE_SIGNED_ROOTS.contains(&root)
}

/// SF `definition_root`: the engine's Guid -> HashKey32. The zero GUID is 0; any other is FNV-1a over its bytes in
/// on-disk order.
pub fn definition_root(bytes: &[u8; 16]) -> u32 {
    if *bytes == [0; 16] {
        return 0;
    }
    bytes
        .iter()
        .fold(0x811c9dc5u32, |h, b| (h ^ *b as u32).wrapping_mul(0x1000193))
}

/// SF `uint`: a JSON number that fits u32.
pub fn uint(value: &Value) -> Result<u32, String> {
    value
        .as_u64()
        .and_then(|v| v.try_into().ok())
        .ok_or_else(|| "expected u32 attribute data".into())
}

/// SF `decode_program`: the attribute-command-v1 words as instructions (opcode in the high 16 bits, operand in the low
/// 16; Push carries its 32-bit literal in the next word). This decodes, never executes. SF unwraps each word's u32 (its
/// caller checked every word first, as `from_resources` does); a bad word is `uint`'s error here.
pub fn decode_program(
    words: &[Value],
    operations: &BTreeMap<u32, String>,
    ids: &[Value],
    names: &[Value],
) -> Result<Value, String> {
    let mut result = Vec::new();
    let mut offset = 0;
    let mut complete = true;
    while offset < words.len() {
        let word = uint(&words[offset])?;
        let opcode = word >> 16;
        let operand = word & 0xffff;
        let name = operations.get(&opcode).map(String::as_str);
        let mut instruction =
            json!({"word_offset":offset,"word":word,"opcode":opcode,"operation":name,"operand":operand});
        offset += 1;
        if name == Some("Push") {
            if let Some(immediate) = words.get(offset) {
                instruction["immediate_bits"] = immediate.clone();
                offset += 1;
            } else {
                instruction["reason"] = json!("truncated Push literal");
                complete = false;
            }
        } else if matches!(name, Some("Load" | "LoadRef" | "Store")) {
            if let (Some(id), Some(name)) = (ids.get(operand as usize), names.get(operand as usize)) {
                instruction["attribute_hash"] = id["value"].clone();
                instruction["attribute_name"] = name.clone();
            } else {
                instruction["reason"] = json!("attribute index outside root definition");
                complete = false;
            }
        }
        if name.is_none() {
            complete = false;
        }
        result.push(instruction);
    }
    Ok(
        json!({"encoding":"attribute-command-v1","decoded":complete,"native_callable":false,
        "evaluation_api":"runtime.ecs.evaluate_attributes","instructions":result}),
    )
}

#[cfg(test)]
mod tests;
