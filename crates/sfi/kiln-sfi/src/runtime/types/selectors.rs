// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical at both pins, 1.7.9 d8a394bc and 1.8.0 38ef448d):
// modules/mod-runtime/lua/bindings/game/resolve_type.rs:6-44 (`resolve`), :166-215
// (`HashKind`, `by_hash`, `unique_match`);
// modules/mod-runtime/reflection/traverse_reflected_types.rs:6-41 (`inheritance`, `fields`).
//! The registry's selectors and traversals, shared by `game.types` and later
//! families (values, ECS) that name a type.
//!
//! - Hash domains: qualified, internal, name and Impact. A hash that several types share
//!   is ambiguous and never resolves to the first of them.
//! - [`resolve`]: a qualified name (exact text), an unsigned 32-bit qualified hash, a
//!   `{hash, domain}` table or a Type of this registry. A Type made from another
//!   registry resolves only when this registry has the same index, qualified hash and
//!   qualified name.
//! - [`inheritance`]: the type, then its bases through the inner type of a struct or
//!   typedef only (an array's or optional's inner type is its element, not a base); a
//!   cycle or a missing base is an error.
//! - [`fields`]: base-first effective fields; a field a derived type declares again
//!   keeps the base's position and takes the derived declaration.

use super::registry::{Registry, StructField, TypeMeta};
use super::userdata::Type;
use mlua::Value;
use std::collections::HashSet;

/// One of the registry's four hash domains.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HashKind {
    Qualified,
    Internal,
    Name,
    Impact,
}

impl HashKind {
    /// The domain names ShroudForge accepts.
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

    pub fn value(self, ty: &TypeMeta) -> u32 {
        match self {
            Self::Qualified => ty.qualified_hash,
            Self::Internal => ty.internal_hash,
            Self::Name => ty.name_hash,
            Self::Impact => ty.impact_hash,
        }
    }
}

/// The one type with `hash` in `kind`'s domain, none, or the ambiguity error when
/// several share it.
pub fn by_hash(
    registry: &Registry,
    hash: u32,
    kind: HashKind,
) -> Result<Option<&TypeMeta>, String> {
    let mut matches = registry.iter().filter(|ty| kind.value(ty) == hash);
    let first = matches.next();
    if matches.next().is_some() {
        return Err(format!(
            "Ambiguous type hash 0x{hash:08x}. Use a qualified name or qualified hash"
        ));
    }
    Ok(first)
}

/// ShroudForge's type selector: the type's index, or the reason it does not select one.
pub fn resolve(registry: &Registry, selector: &Value) -> Result<usize, String> {
    let hashed = |hash, kind| {
        by_hash(registry, hash, kind)?
            .map(|ty| ty.index)
            .ok_or_else(|| format!("type hash not found: 0x{hash:08x}"))
    };
    match selector {
        Value::String(name) => {
            let name = name.to_str().map_err(|e| e.to_string())?;
            registry
                .iter()
                .find(|ty| ty.qualified_name == name.as_ref())
                .map(|ty| ty.index)
                .ok_or_else(|| format!("type not found: {name}"))
        }
        Value::Integer(hash) if (0..=u32::MAX as i64).contains(hash) => {
            hashed(*hash as u32, HashKind::Qualified)
        }
        Value::Table(value) => {
            let hash = value.get::<u32>("hash").map_err(|e| e.to_string())?;
            let domain = value.get::<String>("domain").map_err(|e| e.to_string())?;
            hashed(hash, HashKind::parse(&domain)?)
        }
        Value::UserData(value) if value.is::<Type>() => {
            let ty = value.borrow::<Type>().map_err(|e| e.to_string())?;
            let theirs = ty.meta();
            registry
                .get(theirs.index)
                .filter(|current| {
                    current.qualified_hash == theirs.qualified_hash
                        && current.qualified_name == theirs.qualified_name
                })
                .map(|current| current.index)
                .ok_or_else(|| "type belongs to another registry".into())
        }
        _ => Err(
            "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}".into(),
        ),
    }
}

/// The type and its bases, nearest first.
pub fn inheritance(registry: &Registry, index: usize) -> Result<Vec<usize>, String> {
    let mut result = Vec::new();
    let mut current = Some(index);
    let mut visited = HashSet::new();
    while let Some(index) = current {
        if !visited.insert(index) {
            return Err("cyclic type inheritance".into());
        }
        let ty = registry.get(index).ok_or("unresolved base type")?;
        result.push(index);
        current = if matches!(ty.primitive_type, "Struct" | "Typedef") {
            ty.inner_type
        } else {
            None
        };
    }
    Ok(result)
}

/// The effective fields, base first, each with the type that declares it.
pub fn fields(registry: &Registry, index: usize) -> Result<Vec<(usize, &StructField)>, String> {
    let mut result: Vec<(usize, &StructField)> = Vec::new();
    for owner in inheritance(registry, index)?.into_iter().rev() {
        let ty = registry.get(owner).ok_or("unresolved declaring type")?;
        for field in &ty.struct_fields {
            match result.iter_mut().find(|(_, f)| f.name == field.name) {
                Some(slot) => *slot = (owner, field),
                None => result.push((owner, field)),
            }
        }
    }
    Ok(result)
}
