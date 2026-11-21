// SPDX-License-Identifier: GPL-3.0-or-later
//! The pinned parser's name/type lookup helpers, on EML's existing kfc APIs.
//!
//! ShroudForge's kfc-parser adds `container/resource_lookup.rs` to the same parser EML ships: read-only helpers over
//! `TypeRegistry` and `KFCFile` that change no parsing or format. Every API they call exists unchanged in eml/kfc-base
//! (the reflection modules and `KFCFile::{resource_types, resources_by_type}` are identical at both pins), so they are
//! ported here instead of upgrading the parser. The asset session uses them for resources; the type session owns them.
//!
//! Adapted from crates/kfc-base/src/container/resource_lookup.rs:9-72 of bonsaibauer's kfc-parser fork, ShroudForge's
//! `kfc-parser` submodule at both pins (https://github.com/bonsaibauer/kfc-parser, commit
//! db1580a98dcd679cb9f309b8e26fac97f0734de4). That file is part of kfc-parser, so it is GPL-3.0-or-later like the
//! rest of kfc-parser, not ShroudForge's MIT code.
use kfc::{
    container::KFCFile,
    guid::{Guid, ResourceId},
    reflection::{LookupKey, TypeIndex, TypeMetadata, TypeRegistry},
};

/// Resolves a qualified type name and combines its hash with a GUID and part index.
pub fn resource_id(registry: &TypeRegistry, qualified_name: &str, guid: Guid, part: u32) -> Option<ResourceId> {
    registry
        .get_by_name(LookupKey::Qualified(qualified_name))
        .map(|metadata| ResourceId::new(guid, metadata.qualified_hash, part))
}

/// The same identifier from a registry type index.
pub fn resource_id_for_type(
    registry: &TypeRegistry,
    type_index: TypeIndex,
    guid: Guid,
    part: u32,
) -> Option<ResourceId> {
    registry
        .get(type_index)
        .and_then(|metadata| resource_id(registry, &metadata.qualified_name, guid, part))
}

/// The reflected type of an existing resource identifier.
pub fn type_for_resource<'a>(registry: &'a TypeRegistry, resource: &ResourceId) -> Option<&'a TypeMetadata> {
    registry.get_by_hash(LookupKey::Qualified(resource.type_hash()))
}

/// Whether two resource identifiers have the same type hash.
pub fn same_type(left: &ResourceId, right: &ResourceId) -> bool {
    left.type_hash() == right.type_hash()
}

/// The resources of a named reflected type in an already parsed index.
pub fn resources_by_type(file: &KFCFile, registry: &TypeRegistry, qualified_name: &str) -> Vec<ResourceId> {
    let Some(metadata) = registry.get_by_name(LookupKey::Qualified(qualified_name)) else {
        return Vec::new();
    };
    file.resources_by_type(metadata.qualified_hash).copied().collect()
}

/// The registry indices of the index's resource bundle types that the registry knows.
pub fn resource_type_indices(file: &KFCFile, registry: &TypeRegistry) -> Vec<TypeIndex> {
    file.resource_types()
        .filter_map(|identifier| registry.get_by_hash(LookupKey::Qualified(identifier)))
        .map(|metadata| metadata.index)
        .collect()
}
