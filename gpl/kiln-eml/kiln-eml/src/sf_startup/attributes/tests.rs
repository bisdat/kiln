// SPDX-License-Identifier: GPL-3.0-or-later
//! ShroudForge's own unit tests of the catalog (reflection/build_attribute_catalog.rs, MIT, Copyright (c) 2026
//! bonsaibauer; notice in sf_startup/mod.rs), adapted: the header widths of KFC3 and KFC4 fixtures and the lookup
//! indexes. The attribute catalog adds the decode of a malformed program word, which SF unwraps.
use kfc::reflection::{TypeIndex, TypeRegistry};
use serde_json::json;

use super::*;

fn header_fixture(compact: bool) -> TypeRegistry {
    serde_json::from_value(header_fixture_json(compact)).unwrap()
}

fn header_fixture_json(compact: bool) -> serde_json::Value {
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
    json!({"version":"attribute-header-fixture","types":definitions})
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

// Kiln's one admitted signedness conflict (ENGINE_SIGNED_ROOTS): the BodyHeat root, typed sint32 by the resource and
// stored as uint32 by its component, with the game's sign-dependent programs (Clamp, ScaleToNewMax), is calculated as
// the resource's sint32 (the engine's signed interpreter, docs/re/combat.md "BodyHeat runs"); the reverse pair, the
// same pair on any other root and a float storage keep ShroudForge's refusal.
const BODY_HEAT_ROOT: u32 = 0xC7AE_AF1D;
/// Definition GUID bytes whose FNV-1a is [`BODY_HEAT_ROOT`] (a preimage made for this test; the component's own GUID
/// is not needed: the engine joins a definition to its root by this hash).
const BODY_HEAT_GUID: [u8; 16] = [75, 105, 108, 110, 66, 111, 100, 121, 72, 101, 97, 2, 111, 25, 151, 3];

/// The KFC3 header fixture with sint32 (10) and float32 (11) added, its component's storage of `element`'s type and its
/// reflected definition `guid`.
fn body_heat_types(element: u64, guid: [u8; 16]) -> TypeRegistry {
    let mut fixture = header_fixture_json(false);
    let types = fixture["types"].as_array_mut().unwrap();
    for (index, (name, kind)) in [(10, ("sint32", "SINT32")), (11, ("float32", "FLOAT32"))] {
        let qualified = format!("keen::{name}");
        types.push(json!({"index":index,"name":name,"qualifiedName":qualified,"impactName":name,"size":4,
            "alignment":4,"elementAlignment":4,"fieldCount":0,"primitiveType":kind,"flags":"",
            "nameHash":kfc::hash::fnv(name),"qualifiedHash":kfc::hash::fnv(&qualified),"impactHash":kfc::hash::fnv(name),
            "internalHash":index}));
    }
    // the AttributeOps the catalog's model verifies (`encoding_verified`), so the archive's recalculation runs
    let ops = [
        ("Load0", 1),
        ("Load", 2),
        ("LoadRef", 3),
        ("Push", 5),
        ("Add", 6),
        ("Substract", 7),
        ("Multiply", 8),
        ("Clamp", 12),
        ("ScaleToNewMax", 13),
        ("Max", 15),
    ];
    let fields: serde_json::Map<String, serde_json::Value> = ops
        .iter()
        .map(|(name, value)| (name.to_string(), json!({"name":name,"value":value})))
        .collect();
    types.push(json!({"index":12,"name":"AttributeOps","qualifiedName":"keen::AttributeOps",
        "impactName":"AttributeOps","size":4,"alignment":4,"elementAlignment":4,"fieldCount":ops.len(),
        "primitiveType":"ENUM","innerType":2,"flags":"","nameHash":kfc::hash::fnv("AttributeOps"),
        "qualifiedHash":kfc::hash::fnv("keen::AttributeOps"),"impactHash":kfc::hash::fnv("AttributeOps"),
        "internalHash":12,"enumFields":fields}));
    types[7]["innerType"] = json!(element);
    let mut default = vec![0u8; 36 + 32];
    default[20..36].copy_from_slice(&guid);
    types[8]["defaultValue"] = json!(default);
    serde_json::from_value(fixture).unwrap()
}

/// The BodyHeat definition (the game's names and programs, in a container resource) under `root`, typed `scalar`.
fn body_heat_definition(root: u32, scalar: u64) -> serde_json::Value {
    let names = [
        "BodyHeat",
        "BodyHeat_Max",
        "BodyHeat_Max_Adder",
        "BodyHeat_Max_Base",
        "BodyHeat_RechargeRampup",
        "BodyHeat_RechargeTimeout",
        "BodyHeat_Recharge",
        "BodyHeat_Min",
    ];
    let mut links = vec![(65535, 1, 65535)];
    links.extend((1..8).map(|i| (0, 65535, if i == 7 { 65535 } else { i + 1 })));
    let mut value = definition(u64::from(root), &names, &links, scalar);
    value["structure"][0]["calculation"] = json!([0x20000, 0x20007, 0x20001, 0xcffff]);
    value["structure"][1]["calculation"] = json!([0x30000, 0x10000, 0x20001, 0x20003, 0x20002, 0x6ffff, 0xdffff]);
    json!({"id":"8f1fd455-a5bf-4184-a77c-7123955d6e04","type":"keen::AttributeContainerResource",
        "value":{"attributes":[value]}})
}

#[test]
fn body_heat_is_calculated_as_the_resources_sint32() {
    assert_eq!(definition_root(&BODY_HEAT_GUID), BODY_HEAT_ROOT);
    let catalog = Catalog::from_resources(
        &body_heat_types(2, BODY_HEAT_GUID),
        &[body_heat_definition(BODY_HEAT_ROOT, 10)],
    )
    .unwrap();
    let min = catalog.by_name("BodyHeat_Min").unwrap();
    assert_eq!(min["components"].as_array().unwrap().len(), 1);
    assert_eq!(min["components"][0]["storage_scalar_type"], "keen::uint32");
    assert_eq!(min["scalar_type"], "keen::sint32");
    assert_eq!(min["calculation_sign_independent"], false);
    assert_eq!(min["calculation_writable"], true);
    assert_eq!(min["calculation_reason"], serde_json::Value::Null);
    assert_eq!(min["calculation_scalar_type"], "keen::sint32");
    assert_eq!(min["storage_writable"], true);
    assert_eq!(min["write_value_domain"], "integer-common-range-0-to-2147483647");
    // the archive's recalculation runs it signed: Min raised to Max clamps BodyHeat up to it, and a negative Min
    // (as the signed interpreter reads it) is a lower bound, not 4294967295
    let (values, _) = catalog
        .recalculate(BODY_HEAT_ROOT, &[4000, 10000, 0, 10000, 0, 0, 0, 10000])
        .unwrap();
    assert_eq!(values, [10000, 10000, 0, 10000, 0, 0, 0, 10000]);
    let (values, _) = catalog
        .recalculate(BODY_HEAT_ROOT, &[4000, 10000, 0, 10000, 0, 0, 0, u32::MAX])
        .unwrap();
    assert_eq!(values[0], 4000);
    assert!(engine_signed_root(BODY_HEAT_ROOT, Some("keen::sint32"), Some("keen::uint32")));
}

#[test]
fn other_signedness_conflicts_keep_shroudforges_refusal() {
    let refused = |types: TypeRegistry, root: u32, scalar: u64| {
        let catalog = Catalog::from_resources(&types, &[body_heat_definition(root, scalar)]).unwrap();
        let min = catalog.by_name("BodyHeat_Min").unwrap().clone();
        assert_eq!(min["components"].as_array().unwrap().len(), 1);
        assert_eq!(min["calculation_writable"], false);
        assert_eq!(
            min["calculation_reason"],
            "signedness-dependent program with conflicting resource/storage scalar types"
        );
        min
    };
    // the reverse pair on the BodyHeat root: uint32 resource over sint32 storage
    let min = refused(body_heat_types(10, BODY_HEAT_GUID), BODY_HEAT_ROOT, 2);
    assert_eq!(min["calculation_scalar_type"], "keen::uint32");
    assert!(!engine_signed_root(BODY_HEAT_ROOT, Some("keen::uint32"), Some("keen::sint32")));
    // the proven pair on another root
    let mut other = BODY_HEAT_GUID;
    other[15] ^= 1;
    let root = definition_root(&other);
    assert_ne!(root, BODY_HEAT_ROOT);
    refused(body_heat_types(2, other), root, 10);
    assert!(!engine_signed_root(root, Some("keen::sint32"), Some("keen::uint32")));
    // a float storage under the BodyHeat root: no integer pair at all
    let min = refused(body_heat_types(11, BODY_HEAT_GUID), BODY_HEAT_ROOT, 10);
    assert_eq!(min["storage_writable"], false);
    // a sign-independent conflict keeps ShroudForge's storage-typed calculation
    let mut flat = body_heat_definition(root, 10);
    for node in flat["value"]["attributes"][0]["structure"].as_array_mut().unwrap() {
        node["calculation"] = json!([]);
    }
    let catalog = Catalog::from_resources(&body_heat_types(2, other), &[flat]).unwrap();
    let min = catalog.by_name("BodyHeat_Min").unwrap();
    assert_eq!(min["calculation_writable"], true);
    assert_eq!(min["calculation_scalar_type"], "keen::uint32");
}
