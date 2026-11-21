// SPDX-License-Identifier: GPL-3.0-or-later
//! ShroudForge's own unit tests of the catalog (reflection/build_attribute_catalog.rs, MIT, Copyright (c) 2026
//! bonsaibauer; notice in sf_startup/mod.rs), adapted: the header widths of KFC3 and KFC4 fixtures and the lookup
//! indexes. The attribute catalog adds the decode of a malformed program word, which SF unwraps.
use kfc::reflection::{TypeIndex, TypeRegistry};
use serde_json::json;

use super::*;

fn header_fixture(compact: bool) -> TypeRegistry {
    let header_size = if compact { 8 } else { 36 };
    let mut definitions = Vec::new();
    for (index, (name, kind, size, inner)) in [
        ("uint8", "UINT8", 1, None),
        ("uint16", "UINT16", 2, None),
        ("uint32", "UINT32", 4, None),
        ("HashKey32", "STRUCT", 4, None),
        ("Guid", "GUID", 16, None),
        (
            "AttributeFlags",
            if compact { "BITMASK8" } else { "BITMASK16" },
            if compact { 1 } else { 2 },
            Some(9),
        ),
        ("ecs::AttributeComponent", "STRUCT", header_size, None),
        ("AttributeStorage", "STATIC_ARRAY", 32, Some(2)),
        ("ecs::Stamina", "STRUCT", header_size + 32, Some(6)),
        (
            "AttributeFlagsEnum",
            "ENUM",
            if compact { 1 } else { 2 },
            Some(if compact { 0 } else { 1 }),
        ),
    ]
    .into_iter()
    .enumerate()
    {
        let qualified = format!("keen::{name}");
        definitions.push(
            json!({"index":index,"name":name,"qualifiedName":qualified,"impactName":name,
            "size":size,"alignment":1,"elementAlignment":1,"fieldCount":if index == 7 {8} else {0},
            "primitiveType":kind,"innerType":inner,"flags":"","nameHash":kfc::hash::fnv(name),
            "qualifiedHash":kfc::hash::fnv(&qualified),"impactHash":kfc::hash::fnv(name),"internalHash":index}),
        );
    }
    definitions[3]["structFields"] = json!({"value":{"name":"value","type":2,"dataOffset":0}});
    definitions[6]["structFields"] = if compact {
        json!({"rootId":{"name":"rootId","type":3,"dataOffset":0},
            "storageOffset":{"name":"storageOffset","type":1,"dataOffset":4},
            "flags":{"name":"flags","type":5,"dataOffset":6},
            "storageCount":{"name":"storageCount","type":0,"dataOffset":7}})
    } else {
        json!({"rootId":{"name":"rootId","type":3,"dataOffset":0},
            "storageOffset":{"name":"storageOffset","type":2,"dataOffset":8},
            "storageSize":{"name":"storageSize","type":2,"dataOffset":12},
            "flags":{"name":"flags","type":5,"dataOffset":16},
            "definition":{"name":"definition","type":4,"dataOffset":20}})
    };
    definitions[8]["structFields"] = json!({"dataStorage":{"name":"dataStorage","type":7,"dataOffset":header_size}});
    definitions[9]["enumFields"] = json!({"Initialized":{"name":"Initialized","value":0}});
    serde_json::from_value(json!({"version":"attribute-header-fixture","types":definitions})).unwrap()
}

#[test]
fn attribute_header_uses_reflected_kfc3_and_kfc4_widths() {
    for compact in [false, true] {
        let registry = header_fixture(compact);
        let header = header_layout(&registry, TypeIndex::new(8)).unwrap();
        assert_eq!(header["rootId"]["size"], 4);
        assert_eq!(header["storageOffset"]["size"], if compact { 2 } else { 4 });
        assert_eq!(header["storageCount"]["size"], if compact { 1 } else { 4 });
        assert_eq!(header["flags"]["size"], if compact { 1 } else { 2 });
        assert_eq!(
            header["storageCount"]["name"],
            if compact { "storageCount" } else { "storageSize" }
        );
    }
    let mut invalid = serde_json::to_value(header_fixture(true)).unwrap();
    invalid["types"][6]["structFields"]["storageCount"]["dataOffset"] = json!(6);
    let registry = serde_json::from_value(invalid).unwrap();
    assert!(
        header_layout(&registry, TypeIndex::new(8))
            .unwrap_err()
            .contains("overlap")
    );
}

#[test]
fn lookup_indexes_keep_original_ids_root_order_and_ambiguous_names() {
    let mut catalog = Catalog::default();
    // Attribute-ID order deliberately differs from the engine's root order.
    for (id, name, root, index) in [
        (30, "Stamina", 30, 0),
        (10, "Stamina_Max", 30, 2),
        (20, "Stamina_Min", 30, 1),
        (40, "Repeated", 40, 0),
        (50, "Repeated", 50, 0),
    ] {
        catalog
            .entries
            .insert(id, json!({"name":name,"root_hash":root,"index":index}));
    }
    assert!(std::ptr::eq(
        catalog.by_name("Stamina_Min").unwrap(),
        &catalog.entries[&20]
    ));
    assert!(catalog.by_name("Repeated").unwrap_err().contains("Ambiguous"));
    assert!(catalog.by_name("Missing").unwrap_err().contains("not found"));
    for _ in 0..3 {
        let root = catalog.root_entries(30).unwrap();
        assert_eq!(
            root.iter()
                .map(|entry| entry["name"].as_str().unwrap())
                .collect::<Vec<_>>(),
            ["Stamina", "Stamina_Min", "Stamina_Max"]
        );
    }
    assert!(catalog.root_entries(99).is_err());
}

#[test]
fn lookup_indexes_reject_incomplete_and_duplicate_root_positions() {
    let mut catalog = Catalog::default();
    for (id, root, index) in [(1, 1, 0), (2, 1, 2), (3, 3, 0), (4, 3, 0)] {
        catalog.entries.insert(
            id,
            json!({"name":format!("Attribute{id}"),"root_hash":root,"index":index}),
        );
    }
    for root in [1, 3] {
        assert_eq!(catalog.root_entries(root).unwrap_err(), "incomplete attribute root");
    }
}

#[test]
fn a_program_word_that_is_not_a_u32_is_an_error_not_a_panic() {
    let err = decode_program(&[json!(-1)], &BTreeMap::new(), &[], &[]).unwrap_err();
    assert_eq!(err, "expected u32 attribute data");
    let decoded = decode_program(&[json!(0x50000)], &BTreeMap::from([(5, "Push".to_owned())]), &[], &[]).unwrap();
    assert_eq!(decoded["decoded"], false);
    assert_eq!(decoded["instructions"][0]["reason"], "truncated Push literal");
}

/// One definition record in kfc's compact JSON: IDs from \`root\`, the names, the nodes' links and programs.
fn definition(root: u64, names: &[&str], links: &[(u64, u64, u64)], scalar: u64) -> serde_json::Value {
    json!({
        "ids": (0..names.len()).map(|i| json!({"value": root + i as u64})).collect::<Vec<_>>(),
        "debugNames": names,
        "structure": links.iter().map(|(p, c, s)| json!({"parentIndex": p, "childIndex": c, "siblingIndex": s,
            "calculation": []})).collect::<Vec<_>>(),
        "type": {"value": scalar},
    })
}

fn base(guid: &str, value: serde_json::Value) -> serde_json::Value {
    json!({"id": guid, "type": "keen::BaseAttributeResource", "value": value})
}

// from_resources' refusals of malformed definitions, ShroudForge's texts (build_attribute_catalog.rs:283-461).
#[test]
fn malformed_definitions_are_refused_with_shroudforges_texts() {
    let types = TypeRegistry::default();
    let none = |e: Result<Catalog, String>| e.err().unwrap();
    let g1 = "00000000-0000-0000-0000-000000000001";
    let g2 = "00000000-0000-0000-0000-000000000002";
    let ok = definition(7, &["A", "B"], &[(65535, 1, 65535), (0, 65535, 65535)], 1);
    assert!(Catalog::from_resources(&types, &[base(g1, ok.clone())]).is_ok());
    assert_eq!(
        none(Catalog::from_resources(&types, &[])),
        "no attribute definitions in this KFC container"
    );
    let mut other = ok.clone();
    other["debugNames"][1] = json!("C");
    assert_eq!(
        none(Catalog::from_resources(
            &types,
            &[base(g1, ok.clone()), base(g2, other)]
        )),
        "conflicting container/base definition for 0x00000007"
    );
    let mut short = ok.clone();
    short["debugNames"] = json!(["A"]);
    assert_eq!(
        none(Catalog::from_resources(&types, &[base(g1, short)])),
        "attribute name/ID/structure lengths disagree"
    );
    let bad_link = definition(7, &["A", "B"], &[(65535, 2, 65535), (0, 65535, 65535)], 1);
    assert_eq!(
        none(Catalog::from_resources(&types, &[base(g1, bad_link)])),
        "invalid attribute childIndex"
    );
    let unnamed = definition(7, &["A", ""], &[(65535, 1, 65535), (0, 65535, 65535)], 1);
    assert_eq!(
        none(Catalog::from_resources(&types, &[base(g1, unnamed)])),
        "empty original attribute name"
    );
    // a second root whose second attribute reuses an ID of the first
    let clash = definition(6, &["X", "Y"], &[(65535, 1, 65535), (0, 65535, 65535)], 1);
    assert_eq!(
        none(Catalog::from_resources(
            &types,
            &[base(g1, ok.clone()), base(g2, clash)]
        )),
        "duplicate attribute ID 0x00000007"
    );
    let mut negative = ok.clone();
    negative["ids"][0]["value"] = json!(-1);
    assert_eq!(
        none(Catalog::from_resources(&types, &[base(g1, negative)])),
        "expected u32 attribute data"
    );
    let mut word = ok;
    word["structure"][0]["calculation"] = json!([4294967296u64]);
    assert_eq!(
        none(Catalog::from_resources(&types, &[base(g1, word)])),
        "expected u32 attribute data"
    );
}
