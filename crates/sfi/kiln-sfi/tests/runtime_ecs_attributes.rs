// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs attribute members tests.
//! `runtime.ecs.get_attributes`, `get_attribute`, `evaluate_attributes`, `read_attributes` and `read_attribute` over
//! the fake archive's attribute group (support/fake_attributes.rs: the catalog reply, the recalculation, error tokens)
//! and kiln.world's ECS fake (`ecs_snapshot_many`), through the real service adapter and the registry join, on a
//! scripted registrar host.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 d8a394bc:
//! modules/mod-runtime/lua/ingame/ecs/attributes/*.rs, reflection/build_attribute_catalog.rs (by_name, root_entries,
//! read_header, bind_storage) and game-runtime/bridge/windows/ecs/read_components.rs (read_many).
#[path = "shared/attr_fixture.rs"]
mod fixture;

use fixture::*;
use kiln_sfi::runtime::ecs::attributes::catalog::{
    self as attr_catalog, ATTRIBUTE_ROWS, guid_bytes, untag,
};
use kiln_sfi::runtime::ecs::attributes::storage::batch;
use kiln_sfi::runtime::ecs::attributes::{self, CLAIMS};
use kiln_sfi::runtime::unavailable;
use serde_json::json;

/// Entity 4 holds a Stamina component (100, 0, 80: above its maximum) and a Revive (1.5, 2.0); entity 6 a Health only.
fn world() -> EcsWorld {
    let health = (hash(HEALTH), words(&[30, 100]));
    EcsWorld::server()
        .with(4, &[stamina([100, 0, 80], 1), revive([1.5, 2.0])])
        .with(6, &[health])
}

#[test]
fn the_family_claims_every_attribute_member() {
    assert_eq!(attributes::FAMILY, "ecs-attributes");
    for name in CLAIMS {
        assert!(
            unavailable::ROWS.iter().any(|r| r.name == name),
            "{name} is a ShroudForge member"
        );
    }
    for name in [
        "runtime.ecs.update_attribute",
        "runtime.ecs.write_attribute_storage",
        "runtime.overrides.attribute",
    ] {
        assert!(CLAIMS.contains(&name), "{name}");
    }
}

#[test]
fn guid_text_reads_as_game_order_bytes() {
    assert_eq!(
        guid_bytes("00112233-4455-6677-8899-aabbccddeeff"),
        Some([
            0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
            0xee, 0xff
        ])
    );
    assert_eq!(
        guid_bytes("00112233-4455-6677-8899-AABBCCDDEEFF"),
        guid_bytes("00112233-4455-6677-8899-aabbccddeeff")
    );
    assert_eq!(guid_bytes("00112233-4455-6677-8899"), None);
    assert_eq!(guid_bytes("0011223344-55-6677-8899-aabbccddeeff"), None);
    assert_eq!(guid_bytes("0011223g-4455-6677-8899-aabbccddeeff"), None);
    assert_eq!(guid_bytes(STAMINA_GUID), Some(guid(STAMINA_GUID)));
}

#[test]
fn tagged_trees_come_back_as_plain_json() {
    let value = json!({"b":true,"n":null,"u":7,"i":-3,"f":1.5,"s":"Stamina","a":[1,{"k":"v"}]});
    assert_eq!(untag(&fake_attributes::tagged(&value)).unwrap(), value);
    assert_eq!(
        untag(&json!({"tag":"f32","value":"f32:3fc00000"})).unwrap(),
        json!(1.5)
    );
    for bad in [
        json!({"tag":"guid","value":"guid:00"}),
        json!({"tag":"string","value":"string:ff"}),
        json!({"tag":"u64","value":"7"}),
        json!({"tag":"map","entries":[{"key":{"tag":"u64","value":"u64:0000000000000001"},"value":{"tag":"nil"}}]}),
    ] {
        assert!(untag(&bad).is_err(), "{bad}");
    }
}

// get_attributes: the registry version, the descriptors in ID order, the layouts and the model; the catalog is read once
// per VM (a sizing call, then the read) through a view of the granted generation.
#[test]
fn get_attributes_answers_the_archive_catalog_read_once() {
    let s = setup(world());
    let (count, version, first, layouts, model): (i64, String, String, i64, String) = s.eval(
        "local c = ecs.get_attributes()
         return c.count, c.version, c.entries[1].name, #c.component_layouts, c.storage_resolution",
    );
    let mut ids: Vec<(u32, &str)> = descriptors()
        .iter()
        .map(|d| (d["hash"].as_u64().unwrap() as u32, ""))
        .collect();
    ids.sort();
    let expected_first = descriptors()
        .into_iter()
        .min_by_key(|d| d["hash"].as_u64())
        .unwrap()["name"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        (count, version.as_str(), first, layouts, model.as_str()),
        (
            7,
            "registry rev 303",
            expected_first,
            2,
            "per-entity-initialized-root-id"
        )
    );
    assert_eq!(
        fake_attributes::calls("attribute_catalog"),
        2,
        "a sizing call, then the read"
    );
    assert_eq!(fake_attributes::calls("acquire"), 1);
    let again: i64 = s.eval("return ecs.get_attributes().count");
    assert_eq!(again, 7);
    assert_eq!(
        fake_attributes::calls("attribute_catalog"),
        2,
        "read once per VM"
    );
    let (layout_name, header_mask): (String, i64) =
        s.eval("local c = ecs.get_attributes(); return c.component_layouts[1].qualified_name, c.component_layouts[1].header.flags.initialized_mask");
    assert_eq!((layout_name.as_str(), header_mask), (STAMINA, 1));
}

// get_attribute: by ID and by original name, ShroudForge's refusals.
#[test]
fn get_attribute_selects_by_id_and_name() {
    let s = setup(world());
    let [stamina_id, _, max_id] = stamina_ids();
    let (name, index, root): (String, i64, i64) = s.eval(&format!(
        "local d = ecs.get_attribute({stamina_id}); return d.name, d.index, d.root_hash"
    ));
    assert_eq!(
        (name.as_str(), index, root),
        ("Stamina", 0, i64::from(stamina_root()))
    );
    let (name, component): (String, String) = s.eval(
        "local d = ecs.get_attribute('Stamina_Min'); return d.name, d.components[1].qualified_name",
    );
    assert_eq!(
        (name.as_str(), component.as_str()),
        ("Stamina_Min", STAMINA)
    );
    let by_id: String = s.eval(&format!("return ecs.get_attribute({max_id}).name"));
    assert_eq!(by_id, "Stamina_Max");
    let shared: String = s.eval(&format!("return ecs.get_attribute({SHARED_ROOT}).name"));
    assert_eq!(shared, "Shared");
    assert_eq!(
        s.reason("ecs.get_attribute('Shared')"),
        "Ambiguous attribute name. Use its ID"
    );
    assert_eq!(
        s.reason("ecs.get_attribute('Missing')"),
        "attribute not found: Missing"
    );
    assert_eq!(
        s.reason("ecs.get_attribute(12345)"),
        "attribute ID not found"
    );
    assert_eq!(
        s.reason("ecs.get_attribute(-1)"),
        "attribute ID outside u32"
    );
    assert_eq!(
        s.reason("ecs.get_attribute(4294967296)"),
        "attribute ID outside u32"
    );
    assert_eq!(
        s.reason("ecs.get_attribute(1.5)"),
        "expected original attribute name or unsigned attribute ID"
    );
    assert_eq!(
        s.reason("ecs.get_attribute()"),
        "expected original attribute name or unsigned attribute ID"
    );
}

// evaluate_attributes: the root's values encoded by its scalar, the archive's recalculation, ShroudForge's root result.
#[test]
fn evaluate_attributes_recalculates_on_the_archive() {
    let s = setup(world());
    let (stamina, min, max, model, scalar, steps): (i64, i64, i64, String, String, i64) = s.eval(
        "local r = ecs.evaluate_attributes('Stamina', {Stamina = 150, Stamina_Min = 0, Stamina_Max = 100})
         return r.values.Stamina, r.values.Stamina_Min, r.values.Stamina_Max, r.calculation_model, r.scalar_type, #r.trace",
    );
    assert_eq!(
        (stamina, min, max, model.as_str(), scalar.as_str(), steps),
        (100, 0, 100, "attribute-command-v1", "keen::sint32", 1)
    );
    assert_eq!(
        fake_attributes::calls("attribute_evaluate"),
        2,
        "a sizing call, then the evaluation"
    );
    let (delay, base): (f64, f64) =
        s.eval("local r = ecs.evaluate_attributes('Revive_Delay', {Revive_Delay = 0.5, Revive_Base = 2}); return r.values.Revive_Delay, r.values.Revive_Base");
    assert_eq!((delay, base), (0.5, 2.0));
    assert_eq!(
        s.reason("ecs.evaluate_attributes('Stamina', {Stamina = 1, Stamina_Min = 0, Stamina_Max = 2, Extra = 1})"),
        "unexpected attribute names in root values"
    );
    assert_eq!(
        s.reason(
            "ecs.evaluate_attributes('Stamina', {Stamina = 1.5, Stamina_Min = 0, Stamina_Max = 2})"
        ),
        "Stamina: expected finite scalar of type keen::sint32"
    );
    assert_eq!(
        s.reason("ecs.evaluate_attributes('Stamina', {Stamina = 1, Stamina_Max = 2})"),
        "Stamina_Min: expected finite scalar of type keen::sint32"
    );
    assert_eq!(
        s.reason(
            "ecs.evaluate_attributes('Stamina', {Stamina = 2^31, Stamina_Min = 0, Stamina_Max = 2})"
        ),
        "Stamina: expected finite scalar of type keen::sint32"
    );
    // the values table is converted by mlua, as ShroudForge's (LuaValue, Table) arguments are
    assert!(
        s.raised("ecs.evaluate_attributes('Stamina', 5)")
            .contains("bad argument #2")
    );
    // the archive's refusal keeps its own text
    fake_attributes::with(|f| {
        f.evaluate = Some(Rc::new(|_, _| {
            Err((
                KILN_ERR_INVALID_ARGUMENT,
                "attribute calculation stack underflow".into(),
            ))
        }))
    });
    assert_eq!(
        s.reason(
            "ecs.evaluate_attributes('Stamina', {Stamina = 1, Stamina_Min = 0, Stamina_Max = 2})"
        ),
        "attribute calculation stack underflow"
    );
    assert_eq!(
        fake_attributes::calls("error_release"),
        1,
        "the token is released once"
    );
}

#[test]
fn evaluate_attributes_refuses_unverified_or_mixed_roots() {
    let s = setup(world());
    let mut list = descriptors();
    list[1]["calculation"]["decoded"] = json!(false);
    list[3]["calculation_scalar_type"] = json!("keen::uint32");
    let identity = identity();
    fake_attributes::with(|f| {
        f.catalog = Some(Ok(fake_attributes::catalog_reply(
            &identity,
            &list,
            &[stamina_layout(), revive_layout()],
        )))
    });
    assert_eq!(
        s.reason(
            "ecs.evaluate_attributes('Stamina', {Stamina = 1, Stamina_Min = 0, Stamina_Max = 2})"
        ),
        "attribute root has an unverified calculation program"
    );
    assert_eq!(
        s.reason("ecs.evaluate_attributes('Revive_Base', {Revive_Delay = 1, Revive_Base = 2})"),
        "attribute root has conflicting scalar interpretations"
    );
}

// read_attributes / read_attribute: the entity's component whose initialized header names the root, read in one
// batch with absence proven; ShroudForge's result shape.
#[test]
fn read_attributes_finds_the_component_that_holds_the_root() {
    let s = setup(world());
    let e = s.handle(4);
    let (value, min, max, entity, component, association, applied, scope): (i64, i64, i64, i64, String, String, bool, String) =
        s.eval(&format!(
            "local r = ecs.read_attributes({e}, 'Stamina')
             return r.values.Stamina, r.values.Stamina_Min, r.values.Stamina_Max, r.entity, r.component.qualified_name,
                    r.component.association, r.applied, r.effect_scope"
        ));
    assert_eq!(
        (
            value,
            min,
            max,
            entity,
            component.as_str(),
            association.as_str(),
            applied,
            scope.as_str()
        ),
        (
            100,
            0,
            80,
            e,
            STAMINA,
            "entity-initialized-root-id-and-reflected-static-array",
            false,
            "local process, no automatic client/server RPC"
        )
    );
    assert_eq!(s.calls("ecs_snapshot_many"), 1, "one batch read");
    let (delay, base): (f64, f64) = s.eval(&format!("local r = ecs.read_attributes({e}, 'Revive_Base'); return r.values.Revive_Delay, r.values.Revive_Base"));
    assert_eq!((delay, base), (1.5, 2.0));
    let one: i64 = s.eval(&format!(
        "return (ecs.read_attribute({e}, {}))",
        stamina_ids()[2]
    ));
    assert_eq!(one, 80);
    let float: f64 = s.eval(&format!("return (ecs.read_attribute({e}, 'Revive_Delay'))"));
    assert_eq!(float, 1.5);
    // the same numbers through a fresh read of the bytes
    s.world(|w| {
        w.entities
            .get_mut(&4)
            .unwrap()
            .components
            .insert(fnv(STAMINA), stamina([5, 1, 9], 1).1)
    });
    let now: i64 = s.eval(&format!("return (ecs.read_attribute({e}, 'Stamina'))"));
    assert_eq!(now, 5);
}

#[test]
fn read_attributes_refuses_in_shroudforges_order() {
    let s = setup(world());
    let e = s.handle(4);
    let six = s.handle(6);
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e})")),
        "expected entity, attribute name/ID, and (for updates) scalar value plus optional expected_values"
    );
    assert_eq!(
        s.reason(&format!("ecs.read_attribute({e}, 'Stamina', 1)")),
        "expected entity, attribute name/ID, and (for writes) scalar value"
    );
    assert_eq!(
        s.reason("ecs.read_attributes('4', 'Stamina')"),
        "expected entity integer"
    );
    assert_eq!(
        s.reason("ecs.read_attributes(4.5, 'Stamina')"),
        "expected entity integer"
    );
    assert_eq!(
        s.reason("ecs.read_attributes(-1, 'Stamina')"),
        "entity outside u32"
    );
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e}, 'Shared')")),
        "Ambiguous attribute name. Use its ID"
    );
    let root = stamina_root();
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({six}, 'Stamina')")),
        format!(
            "no initialized component on this entity matches attribute root {root:#010x} (0 compatible component snapshots read)"
        )
    );
    // an uninitialized header is read but not matched
    s.world(|w| {
        w.entities
            .get_mut(&4)
            .unwrap()
            .components
            .insert(fnv(STAMINA), stamina([1, 2, 3], 0).1)
    });
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e}, 'Stamina')")),
        format!(
            "no initialized component on this entity matches attribute root {root:#010x} (1 compatible component snapshots read)"
        )
    );
    // another definition GUID in the bytes: not this root's storage
    s.world(|w| {
        let mut bytes = stamina([1, 2, 3], 1).1;
        bytes[8] ^= 0xff;
        w.entities
            .get_mut(&4)
            .unwrap()
            .components
            .insert(fnv(STAMINA), bytes);
    });
    assert!(
        s.reason(&format!("ecs.read_attributes({e}, 'Stamina')"))
            .contains("(1 compatible component snapshots read)")
    );
    // the gate comes first: the lifecycle, then readiness
    s.host.active.set(false);
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e}, 'Stamina')")),
        NOT_ACTIVE
    );
    s.host.active.set(true);
    s.facts(|f| f.ready = false);
    assert_eq!(
        s.reason(&format!("ecs.read_attribute({e}, 'Stamina')")),
        "runtime.ecs.read unavailable"
    );
}

// Two registered components whose initialized headers both name the root on one entity: refused, never the first; a
// candidate that is absent is skipped (its absence proven by the batch read).
#[test]
fn a_root_in_two_components_is_refused() {
    let twin = "keen::ecs::StaminaTwin";
    let mut bytes = stamina([1, 2, 3], 1).1;
    bytes[8..24].copy_from_slice(&guid(STAMINA_GUID));
    let world = world().with(8, &[stamina([1, 2, 3], 1), (fnv(twin), bytes)]);
    let base = Some(index_of(ATTRIBUTE_BASE) as u64);
    let s = setup_with(
        world,
        vec![T {
            qualified: twin,
            primitive: "Struct",
            size: 36,
            inner: base,
            count: 0,
            fields: Vec::new(),
            flags: 0,
        }],
    );
    let layouts = vec![
        stamina_layout(),
        revive_layout(),
        layout(twin, 3, "keen::sint32", Some(STAMINA_GUID)),
    ];
    let identity = identity();
    fake_attributes::with(|f| {
        f.catalog = Some(Ok(fake_attributes::catalog_reply(
            &identity,
            &descriptors(),
            &layouts,
        )))
    });
    let e = s.handle(8);
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e}, 'Stamina')")),
        "attribute root belongs to multiple initialized components on this entity"
    );
    // entity 4 has no twin: one present, one absent candidate
    let v: i64 = s.eval(&format!(
        "return ecs.read_attributes({}, 'Stamina').values.Stamina",
        s.handle(4)
    ));
    assert_eq!(v, 100);
}

// A handle of an earlier world session: the batch read is refused with ShroudForge's text.
#[test]
fn a_changed_session_refuses_the_batch_read() {
    let s = setup(world());
    let e = s.handle(4);
    s.world(|w| w.session += 1);
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({e}, 'Stamina')")),
        batch::CHANGED
    );
    assert_eq!(
        s.reason("ecs.read_attributes(999999, 'Stamina')"),
        batch::CHANGED
    );
}

// The catalog's failures: the archive's own text, kept for the VM (one attempt); Kiln's text when the group is
// missing; a reply for another generation is refused.
#[test]
fn catalog_failures_are_kept_with_their_text() {
    let s = setup(world());
    fake_attributes::with(|f| {
        f.catalog = Some(Err((
            KILN_ERR_UNAVAILABLE,
            concat!("no attribute definitions in this ", "K", "FC container").into(),
        )))
    });
    let text = concat!("no attribute definitions in this ", "K", "FC container");
    assert_eq!(s.reason("ecs.get_attributes()"), text);
    assert_eq!(s.reason("ecs.get_attribute('Stamina')"), text);
    assert_eq!(s.reason("ecs.evaluate_attributes('Stamina', {})"), text);
    assert_eq!(
        s.reason(&format!("ecs.read_attributes({}, 'Stamina')", s.handle(4))),
        text
    );
    assert_eq!(
        fake_attributes::calls("attribute_catalog"),
        1,
        "the first answer is kept"
    );
    assert_eq!(fake_attributes::calls("error_release"), 1);

    let s = setup(world());
    fake_attributes::with(|f| f.no_slots = true);
    assert_eq!(
        s.reason("ecs.get_attributes()"),
        attr_catalog::unavailable("kiln.eml.archive.attribute_catalog: not provided")
    );
    fake_attributes::with(|f| f.no_slots = false);

    let s = setup(world());
    let mut other = identity();
    other.generation += 1;
    fake_attributes::with(|f| {
        f.catalog = Some(Ok(fake_attributes::catalog_reply(
            &other,
            &descriptors(),
            &[],
        )))
    });
    assert_eq!(
        s.reason("ecs.get_attributes()"),
        attr_catalog::unavailable("attribute_catalog answered for another generation")
    );
    assert_eq!(ATTRIBUTE_ROWS[0], "eml.archive.attributes");
}
