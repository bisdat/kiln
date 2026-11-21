// SPDX-License-Identifier: GPL-3.0-or-later
//! Hand-authored registry using GPL metadata/serialization APIs; all values are synthetic.
use kfc::{
    hash::fnv,
    reflection::{
        PrimitiveType as P, StructFieldMetadata, TypeFlags as F, TypeIndex, TypeMetadata,
        TypeRegistry,
    },
    resource::value::{Value, Variant},
};

pub const VERSION: &str = "m6-synthetic-v1";
pub const ROOT: &str = "m6.Root";
pub const CHILD: &str = "m6.Child";

fn metadata(index: usize, name: &str, primitive_type: P, size: u32) -> TypeMetadata {
    let qualified_name = format!("m6.{name}");
    TypeMetadata {
        index: TypeIndex::new(index),
        name: name.into(),
        impact_name: format!("M6_{name}"),
        qualified_hash: fnv(&qualified_name),
        internal_hash: 0xa600_0000 + index as u32,
        name_hash: fnv(name),
        impact_hash: fnv(&format!("M6_{name}")),
        qualified_name,
        namespace: vec!["m6".into()],
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

fn fields(t: &mut TypeMetadata, specs: &[(&str, usize, u64)]) {
    for &(name, index, data_offset) in specs {
        t.struct_fields.insert(
            name.into(),
            StructFieldMetadata {
                name: name.into(),
                r#type: TypeIndex::new(index),
                data_offset,
                attributes: Default::default(),
            },
        );
    }
    t.field_count = specs.len() as u32;
}

pub fn registry() -> TypeRegistry {
    let mut types = vec![
        metadata(0, "UInt32", P::UInt32, 4),
        metadata(1, "Base", P::Struct, 4),
        metadata(2, "Child", P::Struct, 8),
        metadata(3, "Fixed", P::StaticArray, 8),
        metadata(4, "Children", P::BlobArray, 8),
        metadata(5, "MaybeChild", P::BlobOptional, 4),
        metadata(6, "Choice", P::BlobVariant, 12),
        metadata(7, "Text", P::BlobString, 8),
        metadata(8, "Root", P::Struct, 48),
        // DS is reflection-only: the existing archive Value writer deliberately has no DS encoder.
        metadata(9, "DsChildren", P::DsArray, 24),
        metadata(10, "DsMaybeChild", P::DsOptional, 8),
        metadata(11, "DsChoice", P::DsVariant, 24),
        metadata(12, "DsText", P::DsString, 16),
    ];
    fields(&mut types[1], &[("base", 0, 0)]);
    fields(&mut types[2], &[("extra", 0, 4)]);
    types[2].inner_type = Some(TypeIndex::new(1));
    types[3].inner_type = Some(TypeIndex::new(0));
    types[3].field_count = 2;
    types[4].inner_type = Some(TypeIndex::new(2));
    types[4].flags = F::HAS_BLOB_ARRAY;
    types[5].inner_type = Some(TypeIndex::new(2));
    types[5].flags = F::HAS_BLOB_OPTIONAL;
    types[6].inner_type = Some(TypeIndex::new(1));
    types[6].flags = F::HAS_BLOB_VARIANT;
    types[7].flags = F::HAS_BLOB_STRING;
    fields(
        &mut types[8],
        &[
            ("child", 2, 0),
            ("fixed", 3, 8),
            ("children", 4, 16),
            ("maybe", 5, 24),
            ("choice", 6, 28),
            ("text", 7, 40),
        ],
    );
    types[8].flags =
        F::HAS_BLOB_ARRAY | F::HAS_BLOB_OPTIONAL | F::HAS_BLOB_VARIANT | F::HAS_BLOB_STRING;
    for (i, inner) in [(9, 2), (10, 2), (11, 1)] {
        types[i].inner_type = Some(TypeIndex::new(inner));
    }
    for t in &mut types[9..] {
        t.flags = F::HAS_DS;
    }
    // TypeRegistry::extend is intentionally private; its public serde constructor builds the same indexes.
    serde_json::from_value(serde_json::json!({"version": VERSION, "types": types})).unwrap()
}

pub fn structure(fields: impl IntoIterator<Item = (&'static str, Value)>) -> Value {
    Value::Struct(Box::new(
        fields.into_iter().map(|(k, v)| (k.into(), v)).collect(),
    ))
}

pub fn child(base: u64, extra: u64) -> Value {
    structure([("base", Value::UInt(base)), ("extra", Value::UInt(extra))])
}

pub fn root_value() -> Value {
    structure([
        ("child", child(7, 11)),
        ("fixed", Value::Array(vec![Value::UInt(2), Value::UInt(3)])),
        ("children", Value::Array(vec![child(13, 17), child(19, 23)])),
        ("maybe", child(29, 31)),
        (
            "choice",
            Value::Variant(Box::new(Variant {
                type_index: TypeIndex::new(2),
                value: child(37, 41).into_struct().unwrap(),
            })),
        ),
        ("text", Value::String("synthetic".into())),
    ])
}
