// SPDX-License-Identifier: GPL-3.0-or-later
//! Registry identity and traversal: selectors, hash domains, inheritance and effective fields.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/bindings/game/resolve_type.rs:6-44 (`resolve`) and 166-215 (`HashKind`, `by_hash`,
//!   `unique_match`);
//! - modules/mod-runtime/reflection/traverse_reflected_types.rs:6-41 (`inheritance`, `fields`).
//!
//! Exact selectors only: a name is compared in full against the metadata, and a hash that more than one type shares is
//! refused instead of picking one. The texts are SF's: callers return them as `nil, reason` or raise them.
use std::collections::{HashMap, HashSet};

use kfc::reflection::{PrimitiveType, StructFieldMetadata, TypeIndex, TypeMetadata, TypeRegistry};
use sfi_core::lua::LuaValue;

use super::Type;

/// The four hash domains a mod can name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HashKind {
    Qualified,
    Internal,
    Name,
    Impact,
}

impl HashKind {
    pub fn parse(name: &str) -> Result<Self, String> {
        match name {
            "qualified" => Ok(Self::Qualified),
            "internal" => Ok(Self::Internal),
            "name" => Ok(Self::Name),
            "impact" => Ok(Self::Impact),
            _ => Err(format!(
                "Unknown hash domain '{name}'. Expected qualified, internal, name or impact"
            )),
        }
    }

    pub fn value(self, ty: &TypeMetadata) -> u32 {
        match self {
            Self::Qualified => ty.qualified_hash,
            Self::Internal => ty.internal_hash,
            Self::Name => ty.name_hash,
            Self::Impact => ty.impact_hash,
        }
    }
}

/// The one type with `hash` in `kind`, `None` for none. Internal/layout hashes are frequently shared: never the first
/// of a collision.
pub fn by_hash(registry: &TypeRegistry, hash: u32, kind: HashKind) -> Result<Option<&TypeMetadata>, String> {
    unique_match(registry.iter().filter(|ty| kind.value(ty) == hash))
        .map_err(|()| format!("Ambiguous type hash 0x{hash:08x}. Use a qualified name or qualified hash"))
}

fn unique_match<T>(mut values: impl Iterator<Item = T>) -> Result<Option<T>, ()> {
    let first = values.next();
    if values.next().is_some() { Err(()) } else { Ok(first) }
}

/// A type selector: an exact qualified name, an integer qualified hash in `[0, u32::MAX]`, a `{hash, domain}` table, or
/// a Type whose index and qualified identity match this registry.
pub fn resolve(registry: &TypeRegistry, selector: &LuaValue) -> Result<TypeIndex, String> {
    let by_hash = |hash, kind| {
        by_hash(registry, hash, kind)?
            .map(|ty| ty.index)
            .ok_or_else(|| format!("type hash not found: 0x{hash:08x}"))
    };
    match selector {
        LuaValue::String(name) => {
            let name = name.to_str().map_err(|e| e.to_string())?;
            registry
                .iter()
                .find(|ty| ty.qualified_name == name.as_ref())
                .map(|ty| ty.index)
                .ok_or_else(|| format!("type not found: {name}"))
        }
        LuaValue::Integer(hash) if (0..=u32::MAX as i64).contains(hash) => by_hash(*hash as u32, HashKind::Qualified),
        LuaValue::Table(value) => {
            let hash = value.get::<u32>("hash").map_err(|e| e.to_string())?;
            let domain = value.get::<String>("domain").map_err(|e| e.to_string())?;
            by_hash(hash, HashKind::parse(&domain)?)
        }
        LuaValue::UserData(value) if value.is::<Type>() => {
            let ty = value.borrow::<Type>().map_err(|e| e.to_string())?;
            registry
                .get(ty.index)
                .filter(|current| {
                    current.qualified_hash == ty.qualified_hash && current.qualified_name == ty.qualified_name
                })
                .map(|current| current.index)
                .ok_or_else(|| "type belongs to another registry".into())
        }
        _ => Err("expected a qualified name, unsigned qualified hash, Type, or {hash, domain}".into()),
    }
}

/// `index` and its bases, the type itself first. Only Struct and Typedef inner links are bases; an array's or optional's
/// inner type is its element, not a base.
pub fn inheritance(registry: &TypeRegistry, index: TypeIndex) -> Result<Vec<TypeIndex>, String> {
    let mut result = Vec::new();
    let mut current = Some(index);
    let mut visited = HashSet::new();
    while let Some(index) = current {
        if !visited.insert(index) {
            return Err("cyclic type inheritance".into());
        }
        let ty = registry.get(index).ok_or("unresolved base type")?;
        result.push(index);
        current = if matches!(ty.primitive_type, PrimitiveType::Struct | PrimitiveType::Typedef) {
            ty.inner_type
        } else {
            None
        };
    }
    Ok(result)
}

/// The effective fields, base first, each with its declaring type. A derived field of a base field's name takes the
/// base's slot (IndexMap::insert order), offsets as the metadata says.
pub fn fields(registry: &TypeRegistry, index: TypeIndex) -> Result<Vec<(TypeIndex, &StructFieldMetadata)>, String> {
    let mut order: Vec<(TypeIndex, &StructFieldMetadata)> = Vec::new();
    let mut slots: HashMap<&str, usize> = HashMap::new();
    for owner in inheritance(registry, index)?.into_iter().rev() {
        let ty = registry.get(owner).ok_or("unresolved declaring type")?;
        for field in ty.struct_fields.values() {
            match slots.get(field.name.as_str()) {
                Some(&slot) => order[slot] = (owner, field),
                None => {
                    slots.insert(field.name.as_str(), order.len());
                    order.push((owner, field));
                }
            }
        }
    }
    Ok(order)
}
