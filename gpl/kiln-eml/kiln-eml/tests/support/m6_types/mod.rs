// SPDX-License-Identifier: GPL-3.0-or-later
//! The fixture synthetic registry plus types that make each selector rule observable. All values are synthetic and
//! source-derived (ShroudForge 1.7.9 game/types.rs, resolve_type.rs, traverse_reflected_types.rs), not game data.
#![allow(dead_code)]
use kfc::{
    hash::fnv,
    reflection::{
        Attribute, EnumFieldMetadata, PrimitiveType as P, StructFieldMetadata, TypeFlags as F, TypeIndex, TypeMetadata,
        TypeRegistry,
    },
};

use crate::fixture::types as g00;

/// Index of each added type (the fixture's are 0..=12).
pub const ALIASED: usize = 13; // qualified/impact hash fields are the hashes of OTHER names
pub const LAYOUT_A: usize = 14; // shares its internal hash with LAYOUT_B
pub const LAYOUT_B: usize = 15;
pub const INNER_BASE: usize = 16; // short name "Base", like the fixture's m6.Base: same name hash
pub const MODE: usize = 17; // enum with a value above i64::MAX
pub const OVERRIDE: usize = 18; // derives from Base and redeclares its field
pub const BROKEN: usize = 19; // base and a field type the registry does not have
pub const CYCLE_A: usize = 20; // CYCLE_A -> CYCLE_B -> CYCLE_A
pub const CYCLE_B: usize = 21;
pub const ALIAS: usize = 22; // Typedef of UInt32
pub const DECIMAL: usize = 23; // qualified name "4242"
pub const TAGGED: usize = 24; // derives from Child; own attribute only on Child
pub const COUNT: usize = 25;

pub const INTERNAL_SHARED: u32 = 0x77;
pub const GHOST: &str = "m6.Ghost";
pub const PHANTOM: &str = "M6_Phantom";
pub const BIG_ENUM: u64 = 0xffff_ffff_ffff_fff0;

fn metadata(index: usize, qualified: &str, name: &str, primitive_type: P, size: u32) -> TypeMetadata {
    let impact = format!("M6_{}", qualified.replace(['.', ':'], "_"));
    TypeMetadata {
        index: TypeIndex::new(index),
        name: name.into(),
        impact_hash: fnv(&impact),
        impact_name: impact,
        qualified_hash: fnv(qualified),
        internal_hash: 0xb600_0000 + index as u32,
        name_hash: fnv(name),
        qualified_name: qualified.into(),
        namespace: qualified
            .split('.')
            .rev()
            .skip(1)
            .collect::<Vec<_>>()
            .into_iter()
            .rev()
            .map(Into::into)
            .collect(),
        inner_type: None,
        size,
        alignment: 4,
        element_alignment: 4,
        field_count: 0,
        primitive_type,
        flags: F::NONE,
        struct_fields: Default::default(),
        enum_fields: Default::default(),
        default_value: None,
        attributes: Default::default(),
    }
}

fn field(t: &mut TypeMetadata, name: &str, index: usize, data_offset: u64) {
    t.struct_fields.insert(
        name.into(),
        StructFieldMetadata {
            name: name.into(),
            r#type: TypeIndex::new(index),
            data_offset,
            attributes: Default::default(),
        },
    );
    t.field_count = t.struct_fields.len() as u32;
}

pub fn attribute(name: &str, namespace: &[&str], r#type: Option<usize>, value: &str) -> Attribute {
    Attribute {
        name: name.into(),
        namespace: namespace.iter().map(|s| (*s).into()).collect(),
        r#type: r#type.map(TypeIndex::new),
        value: value.into(),
    }
}

/// The types, before the registry indexes them (for a second registry with changes).
pub fn types() -> Vec<TypeMetadata> {
    let mut types = g00::registry().into_inner();
    // Child (2): an own attribute with a namespace and a type, a field attribute, exact default bytes with zeros and
    // bytes that are no UTF-8.
    types[2]
        .attributes
        .insert("tag".into(), attribute("tag", &["m6", "attr"], Some(0), "child-tag"));
    types[2]
        .struct_fields
        .get_mut("extra")
        .unwrap()
        .attributes
        .insert("unit".into(), attribute("unit", &[], None, "cm"));
    types[2].default_value = Some(vec![17, 0, 0xff, 0, 0x80, 23, 0, 0]);
    types[1]
        .attributes
        .insert("base_only".into(), attribute("base_only", &[], Some(999), ""));

    let mut aliased = metadata(ALIASED, "m6.Aliased", "Aliased", P::Struct, 4);
    aliased.qualified_hash = fnv(GHOST);
    aliased.impact_hash = fnv(PHANTOM);
    types.push(aliased);
    for (index, name) in [(LAYOUT_A, "LayoutA"), (LAYOUT_B, "LayoutB")] {
        let mut t = metadata(index, &format!("m6.{name}"), name, P::Struct, 4);
        t.internal_hash = INTERNAL_SHARED;
        types.push(t);
    }
    types.push(metadata(INNER_BASE, "m6.inner.Base", "Base", P::Struct, 4));
    let mut mode = metadata(MODE, "m6.Mode", "Mode", P::Enum, 8);
    for (name, value) in [("Off", 0u64), ("On", 1), ("Big", BIG_ENUM)] {
        mode.enum_fields.insert(
            name.into(),
            EnumFieldMetadata {
                name: name.into(),
                value,
            },
        );
    }
    mode.field_count = 3;
    types.push(mode);
    let mut over = metadata(OVERRIDE, "m6.Override", "Override", P::Struct, 12);
    over.inner_type = Some(TypeIndex::new(1));
    field(&mut over, "late", 0, 4);
    field(&mut over, "base", 0, 8);
    types.push(over);
    let mut broken = metadata(BROKEN, "m6.Broken", "Broken", P::Struct, 8);
    broken.inner_type = Some(TypeIndex::new(999));
    field(&mut broken, "lost", 998, 0);
    types.push(broken);
    for (index, other, name) in [(CYCLE_A, CYCLE_B, "CycleA"), (CYCLE_B, CYCLE_A, "CycleB")] {
        let mut t = metadata(index, &format!("m6.{name}"), name, P::Struct, 4);
        t.inner_type = Some(TypeIndex::new(other));
        types.push(t);
    }
    let mut alias = metadata(ALIAS, "m6.Alias", "Alias", P::Typedef, 4);
    alias.inner_type = Some(TypeIndex::new(0));
    types.push(alias);
    types.push(metadata(DECIMAL, "4242", "4242", P::UInt32, 4));
    let mut tagged = metadata(TAGGED, "m6.Tagged", "Tagged", P::Struct, 8);
    tagged.inner_type = Some(TypeIndex::new(2));
    tagged.flags = F::IS_GPU_UNIFORM | F::HAS_BLOB_STRING;
    types.push(tagged);
    assert_eq!(types.len(), COUNT);
    types
}

pub fn registry_of(version: &str, types: Vec<TypeMetadata>) -> TypeRegistry {
    // TypeRegistry::extend is private; its serde constructor builds the same indexes (as the fixture does).
    serde_json::from_value(serde_json::json!({"version": version, "types": types})).unwrap()
}

pub fn registry() -> TypeRegistry {
    registry_of(g00::VERSION, types())
}
