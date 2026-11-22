// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs entity queries and component reads tests.
//! `runtime.ecs.query`, `query_bounds`, `resolve`, `read`, `read_bytes`, `read_fields` and `get_template` on
//! kiln.world's ECS fake (support/fake_services `EcsWorld`) through the real service adapter, the registry join and the
//! VM's typed values (support/fake_values, the fixture's codec), on a scripted registrar host; the catalog's rows and the
//! `Ready` fact on the production runtime facts.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 d8a394bc:
//! modules/mod-runtime/lua/ingame/ecs/{query_entities,resolve_entity,read_component,access_component_fields}.rs,
//! lua/values/{parse_field_path,resolve_inline_field,collect_inline_leaves,reject_external_overlap}.rs,
//! game-runtime/bridge/windows/ecs/{query_entities,query_entities_in_bounds}.rs and
//! game-runtime/native/ecs/query_entities_in_bounds.cpp.
#[path = "shared/ecs_fixture.rs"]
mod fixture;

use fixture::*;
use kiln_abi::services::kiln_caps::{KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE};
use kiln_sfi::runtime::ecs::entities::text as q;
use kiln_sfi::runtime::ecs::fields::text as f;
use kiln_sfi::runtime::ecs::template::text as tpl;
use kiln_sfi::runtime::ecs::{self, CLAIMS};
use kiln_sfi::runtime::operation_catalog::{Backing, CATALOG, Fact};
use kiln_sfi::runtime::types;
use kiln_sfi::runtime::unavailable::{self, NO_RESOURCE_READ};
use sfi_core::bindings::runtime::RuntimeOperation as Op;

fn health(hp: u32, max: u32) -> (u32, Vec<u8>) {
    (hash(HEALTH), words(&[hp, max]))
}
fn counters(values: [u32; 3], speed: f32) -> (u32, Vec<u8>) {
    let mut bytes = words(&values);
    bytes.extend(speed.to_le_bytes());
    (hash(COUNTERS), bytes)
}
/// Entities 3 (Health, Counters), 5 (Counters) and 7 (Health).
fn world() -> EcsWorld {
    EcsWorld::server()
        .with(7, &[health(30, 100)])
        .with(3, &[health(10, 50), counters([1, 2, 3], 2.5)])
        .with(5, &[counters([4, 5, 6], 0.5)])
}
fn handles(s: &Setup, call: &str) -> Vec<i64> {
    s.eval(&format!("return ({call})"))
}

// ---- the catalog and readiness ----

#[test]
fn the_catalog_backs_the_queries_and_reads_on_kiln_world_and_ready_follows_its_ecs_context() {
    assert_eq!(CATALOG.check(), Ok(()));
    let capability = |backing: Backing| match backing {
        Backing::Kiln([need]) => need.capability,
        _ => panic!("one Kiln need"),
    };
    for op in [Op::EcsQuery, Op::EcsResolve, Op::EcsRead] {
        assert_eq!(
            capability(CATALOG.row(op).unwrap().backing),
            "world.ecs_read",
            "{op:?}"
        );
    }
    let ready = CATALOG
        .facts
        .iter()
        .find(|r| r.fact == Fact::Ready)
        .unwrap();
    assert_eq!(capability(ready.backing), "world.ecs_read");
    assert!(ready.read.is_none());
    // the members: every ECS read kiln.world v4 has, present in the fake
    let s = setup(world());
    assert_eq!(ecs::probe_reads(&s.services), Ok(()));
    // without the capability row: outside the contract, not ready
    assert!(!s.facts.static_support(Op::EcsQuery).is_available());
    assert!(!s.facts.provider_facts().ready);
    fake_services::set_cap(
        "world.ecs_read",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    let ready_now = || s.facts.provider_facts().ready;
    assert!(s.facts.static_support(Op::EcsRead).is_available());
    assert!(ready_now());
    // kiln.world's context: no exact profile, no live session of the server world, only a client world
    s.world(|w| w.exact_profile = false);
    assert!(!ready_now());
    s.world(|w| {
        w.exact_profile = true;
        w.session = 0;
    });
    assert!(!ready_now());
    s.world(|w| {
        w.session = FAKE_SESSION;
        w.world_kind = KILN_WORLD_CLIENT as u32;
    });
    assert!(!ready_now());
    s.world(|w| w.world_kind = SERVER);
    assert!(ready_now());
    fake_services::set_cap("world.ecs_read", None);
    assert!(!ready_now());
}

#[test]
fn the_family_claims_its_names_and_the_unavailable_family_keeps_the_rest() {
    let wiring = types::wire(Port(archive_types()), |_, _| {});
    let family = ecs::family(wiring.handle.clone(), SERVER);
    assert_eq!(family.claims(), CLAIMS);
    let left: Vec<&str> = unavailable::unclaimed(&[family])
        .iter()
        .map(|r| r.name)
        .collect();
    for name in CLAIMS {
        assert!(!left.contains(&name), "{name}");
    }
    for name in ["runtime.ecs.read_attributes", "runtime.overrides.attribute"] {
        assert!(left.contains(&name), "{name}");
    }
}

// ---- query ----

#[test]
fn a_query_answers_one_handle_per_entity_in_entity_order_and_resolve_agrees() {
    let s = setup(world());
    let found = handles(&s, "ecs.query('keen::ecs::Health')");
    assert_eq!(found.len(), 2);
    assert_eq!(s.handle(3), found[0]);
    assert_eq!(s.handle(7), found[1]);
    assert_ne!(found[0], found[1]);
    // the same entities, the same handles; several components are all required
    assert_eq!(handles(&s, "ecs.query('keen::ecs::Health')"), found);
    assert_eq!(
        handles(&s, "ecs.query('keen::ecs::Health', 'keen::ecs::Counters')"),
        [found[0]]
    );
    // a qualified hash selects the same component; no match is an empty table
    assert_eq!(handles(&s, &format!("ecs.query({})", hash(HEALTH))), found);
    assert!(handles(&s, "ecs.query('keen::ecs::TemplateResource')").is_empty());
    assert_eq!(s.calls("ecs_query"), 5);
    // more matches than the first buffer: one more call at the size kiln.world reported
    let mut many = EcsWorld::server();
    for id in 1..=300 {
        many = many.with(id, &[health(id, 1)]);
    }
    let s = setup(many);
    let found = handles(&s, "ecs.query('keen::ecs::Health')");
    assert_eq!(found.len(), 300);
    assert_eq!(s.calls("ecs_query"), 2);
    assert_eq!(s.handle(300), found[299]);
}

#[test]
fn a_query_refuses_in_shroudforge_s_order() {
    let s = setup(world());
    // the denial first, before the arguments
    s.host.active.set(false);
    assert_eq!(s.reason("ecs.query()"), NOT_ACTIVE);
    s.host.active.set(true);
    assert_eq!(s.raised("ecs.query()"), q::QUERY_NEEDS_TYPE);
    // readiness before the types
    s.facts(|f| f.ready = false);
    assert_eq!(s.reason("ecs.query('keen::ecs::Nope')"), NOT_READY);
    s.facts(|f| f.ready = true);
    assert_eq!(
        s.raised("ecs.query('keen::ecs::Nope')"),
        "type not found: keen::ecs::Nope"
    );
    assert_eq!(
        s.raised("ecs.query('keen::Other')"),
        "runtime ECS operation requires a keen::ecs::* type, got keen::Other"
    );
    assert_eq!(
        s.reason("ecs.query('keen::ecs::Health', 'keen::ecs::Unregistered')"),
        format!("{}{UNREGISTERED}", q::COMPONENT_UNAVAILABLE)
    );
    // a membership change during the walk is read again, three times at most
    s.world(|w| w.query_changed = 2);
    assert_eq!(handles(&s, "ecs.query('keen::ecs::Health')").len(), 2);
    s.world(|w| w.query_changed = 3);
    assert_eq!(
        s.reason("ecs.query('keen::ecs::Health')"),
        q::QUERY_UNSTABLE
    );
    // kiln.world refuses (here: no such world): the provider's failure
    s.world(|w| w.world_kind = KILN_WORLD_CLIENT as u32);
    assert_eq!(s.reason("ecs.query('keen::ecs::Health')"), q::QUERY_FAILED);
    s.world(|w| w.world_kind = SERVER);
    // a kiln.world without ecs_query
    fake_services::set_shape(
        "kiln.world",
        Some(fake_services::Shape::NullAt(std::mem::offset_of!(
            KilnWorldApi,
            ecs_query
        ))),
    );
    assert_eq!(
        s.reason("ecs.query('keen::ecs::Health')"),
        q::PROVIDER_UNAVAILABLE
    );
    fake_services::set_shape("kiln.world", None);
}

// ---- query_bounds ----

fn placed(position: [f64; 3], scale: f32) -> (u32, Vec<u8>) {
    (
        hash(TRANSFORM),
        transform(position, [scale, scale * 0.5, scale]),
    )
}

#[test]
fn query_bounds_keeps_the_matches_whose_transform_lies_in_the_padded_box() {
    let s = setup(
        EcsWorld::server()
            .with(1, &[placed([5.0, 5.0, 5.0], 1.0)])
            .with(2, &[placed([10.0, 5.0, 5.0], 1.0)])
            .with(3, &[placed([0.0, 0.0, 0.0], 1.0), health(1, 1)])
            .with(4, &[placed([11.0, 5.0, 5.0], 2.0)])
            .with(5, &[placed([5.0, 5.0, 5.0], f32::NAN)])
            .with(6, &[health(1, 1)]),
    );
    let call = |padding: &str| {
        handles(
            &s,
            &format!(
                "ecs.query_bounds({{0, 0, 0, 10, 10, 10}}, {padding}, 'keen::ecs::CurrentTransform')"
            ),
        )
    };
    let h = |id: u32| s.handle(id);
    // the minimum is inside, the maximum is not; a scale that is not finite is no match
    assert_eq!(call("0"), [h(1), h(3)]);
    // the margin is the padding times the largest absolute scale
    assert_eq!(call("0.5"), [h(1), h(2), h(3)]);
    assert_eq!(call("1"), [h(1), h(2), h(3), h(4)]);
    // every component is required
    assert_eq!(
        handles(
            &s,
            "ecs.query_bounds({0, 0, 0, 10, 10, 10}, 0, 'keen::ecs::CurrentTransform', 'keen::ecs::Health')"
        ),
        [h(3)]
    );
    // the arguments, in ShroudForge's order
    assert_eq!(
        s.raised("ecs.query_bounds({0, 0, 0, 1, 1, 1}, 0)"),
        q::BOUNDS_ARGUMENTS
    );
    assert_eq!(
        s.reason("ecs.query_bounds({0, 0, 0, 1, 1, 1}, -1, 'keen::ecs::Health')"),
        q::PADDING
    );
    // a padding that is no finite number fails ShroudForge's argument conversion first
    assert_eq!(
        s.raised("ecs.query_bounds({0, 0, 0, 1, 1, 1}, 0/0, 'keen::ecs::Health')"),
        "bad argument #2 (64-bit floating point expected, got number)"
    );
    assert_eq!(
        s.reason("ecs.query_bounds({0, 0, 0/0, 1, 1, 1}, 0, 'keen::ecs::Health')"),
        q::BOUNDS
    );
    assert!(
        s.raised("ecs.query_bounds({0, 0, 0, 1, 1}, 0, 'keen::ecs::Health')")
            .contains("number")
    );
    // the provider's own rules: the transform among the components, every minimum below its maximum
    assert_eq!(
        s.reason("ecs.query_bounds({0, 0, 0, 1, 1, 1}, 0, 'keen::ecs::Health')"),
        q::SPATIAL_FAILED
    );
    assert_eq!(
        s.reason("ecs.query_bounds({0, 0, 0, 0, 1, 1}, 0, 'keen::ecs::CurrentTransform')"),
        q::SPATIAL_FAILED
    );
    s.facts(|f| f.ready = false);
    assert_eq!(
        s.reason("ecs.query_bounds({0, 0, 0, 1, 1, 1}, 0, 'keen::ecs::Health')"),
        NOT_READY
    );
}

// ---- resolve ----

#[test]
fn resolve_opens_the_live_entity_and_refuses_with_its_id() {
    let s = setup(world());
    assert!(s.handle(7) > 0);
    for id in [0, 99] {
        assert_eq!(
            s.reason(&format!("ecs.resolve({id})")),
            format!("{}{id}", q::ENTITY_UNAVAILABLE)
        );
    }
    s.facts(|f| f.ready = false);
    assert_eq!(
        s.reason("ecs.resolve(7)"),
        format!("{}7", q::ENTITY_UNAVAILABLE)
    );
    s.facts(|f| f.ready = true);
    assert_eq!(
        s.raised("ecs.resolve('x')"),
        "bad argument #1 (32-bit unsigned integer expected, got string)"
    );
    s.host.contract.set(false);
    assert_eq!(
        s.reason("ecs.resolve(7)"),
        format!("operation runtime.ecs.resolve has not been verified for game build {BUILD}")
    );
}

// ---- read and read_bytes ----

#[test]
fn read_bytes_gives_the_component_and_read_decodes_it_as_its_type() {
    let s = setup(world());
    let h = s.handle(7);
    let bytes: mlua::String = s.eval(&format!(
        "return (ecs.read_bytes({h}, 'keen::ecs::Health'))"
    ));
    assert_eq!(bytes.as_bytes().to_vec(), words(&[30, 100]));
    let kind: String = s.eval(&format!(
        "local v = ecs.read({h}, 'keen::ecs::Health'); return type(v)"
    ));
    assert_eq!(kind, "userdata");
    let opened = s.codec.borrow().opened.last().cloned().unwrap();
    assert_eq!(
        opened,
        (
            KILN_VALUE_OPEN_BYTES as u32,
            HEALTH.to_owned(),
            words(&[30, 100])
        )
    );
    // one stable copy per read
    assert_eq!(s.calls("ecs_snapshot"), 2);
}

#[test]
fn a_read_refuses_in_shroudforge_s_order() {
    let s = setup(world());
    let (h3, h5) = (s.handle(3), s.handle(5));
    let failed = format!("component read failed: {HEALTH}");
    // an unknown handle, an entity without the component, a copy kiln.world refuses
    assert_eq!(s.reason("ecs.read(424242, 'keen::ecs::Health')"), failed);
    assert_eq!(
        s.reason(&format!("ecs.read_bytes({h5}, 'keen::ecs::Health')")),
        failed
    );
    s.world(|w| w.unreadable = true);
    assert_eq!(
        s.reason(&format!("ecs.read({h3}, 'keen::ecs::Health')")),
        failed
    );
    s.world(|w| w.unreadable = false);
    assert_eq!(
        s.reason(&format!("ecs.read({h3}, 'keen::ecs::Unregistered')")),
        format!("{}{UNREGISTERED}", q::COMPONENT_UNAVAILABLE)
    );
    assert_eq!(
        s.raised(&format!("ecs.read({h3}, 'keen::Other')")),
        "runtime ECS operation requires a keen::ecs::* type, got keen::Other"
    );
    // the denial before anything
    s.host.active.set(false);
    assert_eq!(s.reason("ecs.read('x', 'keen::ecs::Nope')"), NOT_ACTIVE);
    s.host.active.set(true);
    s.host.contract.set(false);
    assert_eq!(
        s.reason("ecs.read_bytes(1, 'keen::ecs::Health')"),
        format!("operation runtime.ecs.read has not been verified for game build {BUILD}")
    );
}

#[test]
fn a_handle_of_a_dead_entity_or_an_old_session_reads_nothing_and_handles_are_never_reused() {
    let s = setup(world());
    let (h3, h7) = (s.handle(3), s.handle(7));
    let read = |h: i64| -> (Value, Option<String>) {
        s.eval(&format!("return ecs.read_bytes({h}, 'keen::ecs::Health')"))
    };
    assert!(read(h7).1.is_none());
    // entity 7's id now names a new entity (a new generation): the old handle is stale, the new one is new
    s.world(|w| w.entities.get_mut(&7).unwrap().generation = 2);
    assert_eq!(
        read(h7).1.as_deref(),
        Some(format!("component read failed: {HEALTH}").as_str())
    );
    let again = s.handle(7);
    assert_ne!(again, h7);
    assert!(read(again).1.is_none());
    // a new world session: every earlier handle is stale, and forgotten once a ref of the new session is seen
    s.world(|w| w.session = 2);
    assert!(read(h3).1.is_some());
    assert!(s.ecs.object(h3 as u32).is_some());
    let fresh = handles(&s, "ecs.query('keen::ecs::Health')");
    assert!(!fresh.contains(&h3) && !fresh.contains(&again));
    assert!(s.ecs.object(h3 as u32).is_none());
    assert!(read(fresh[0]).1.is_none());
}

// ---- read_fields ----

#[test]
fn read_fields_decodes_each_selected_inline_field_from_one_copy() {
    let s = setup(world().with(
        9,
        &[(hash(TAGGED), {
            let mut b = vec![0xee; 24];
            b.extend(12u32.to_le_bytes());
            b
        })],
    ));
    let (h3, h9) = (s.handle(3), s.handle(9));
    let before = s.calls("ecs_snapshot");
    let (hp, max): (i64, i64) = s.eval(&format!(
        "local v = ecs.read_fields({h3}, 'keen::ecs::Health', {{'hp', 'max'}}); return v.hp, v.max"
    ));
    assert_eq!((hp, max), (10, 50));
    assert_eq!(s.calls("ecs_snapshot") - before, 1);
    let (second, speed): (i64, f64) = s.eval(&format!(
        "local v = ecs.read_fields({h3}, 'keen::ecs::Counters', {{'values[2]', 'speed'}}); return v['values[2]'], v.speed"
    ));
    assert_eq!((second, speed), (2, 2.5));
    // a field beside external storage reads; the storage itself does not
    let count: i64 = s.eval(&format!(
        "return ecs.read_fields({h9}, 'keen::ecs::Tagged', {{'count'}}).count"
    ));
    assert_eq!(count, 12);
    // each field decodes its own bytes as its own type
    let opened: Vec<(String, Vec<u8>)> = s
        .codec
        .borrow()
        .opened
        .iter()
        .map(|(_, t, b)| (t.clone(), b.clone()))
        .collect();
    assert!(opened.contains(&("keen::UInt32".to_owned(), 2u32.to_le_bytes().to_vec())));
    assert!(opened.contains(&("keen::Float32".to_owned(), 2.5f32.to_le_bytes().to_vec())));
}

#[test]
fn read_fields_refuses_paths_and_selections_as_shroudforge_does() {
    let s = setup(world().with(
        9,
        &[(hash(TAGGED), vec![0; 28]), (hash(BIG), vec![0; 4400])],
    ));
    let h3 = s.handle(3);
    let h9 = s.handle(9);
    let fields = |component: &str, selection: &str| {
        s.reason(&format!(
            "ecs.read_fields({h3}, '{component}', {selection})"
        ))
    };
    let health = |selection: &str| fields(HEALTH, selection);
    assert_eq!(health("{'hp', 'hp'}"), f::DUPLICATE);
    assert_eq!(health("{}"), f::NONEMPTY);
    assert_eq!(health("{[1] = 'hp', [3] = 'max'}"), f::DENSE);
    assert_eq!(health("{hp = true}"), f::DENSE);
    assert_eq!(health("{1}"), f::STRING_PATHS);
    assert_eq!(
        health("{'hp2'}"),
        format!("hp2: reflected field not found: {HEALTH}.hp2")
    );
    assert_eq!(
        health("{'hp.x'}"),
        "hp.x: keen::UInt32 has no reflected inline fields"
    );
    assert_eq!(
        health("{'hp[1]'}"),
        "hp[1]: keen::UInt32 is not a static array"
    );
    assert_eq!(
        health("{''}"),
        ": expected a nonempty field path of at most 1024 bytes"
    );
    assert_eq!(health("{'hp.'}"), "hp.: expected a field name after '.'");
    assert_eq!(
        health("{'hp]'}"),
        "hp]: expected '.' or an array index in field path"
    );
    let counters = |selection: &str| fields(COUNTERS, selection);
    assert_eq!(
        counters("{'values[0]'}"),
        "values[0]: array indices are one-based"
    );
    assert_eq!(
        counters("{'values[4]'}"),
        "values[4]: static array index or reflected layout is invalid"
    );
    assert_eq!(counters("{'values[x]'}"), "values[x]: invalid array index");
    assert_eq!(
        counters("{'values[1'}"),
        "values[1: unterminated array index"
    );
    assert_eq!(
        s.reason(&format!(
            "ecs.read_fields({h9}, 'keen::ecs::Tagged', {{'list'}})"
        )),
        "list: keen::DsList has external storage or reference ownership. Select an inline sibling field"
    );
    // at most 1,024 paths
    let many: String = (1..=1025).map(|i| format!("'v[{i}]',")).collect();
    assert_eq!(
        s.reason(&format!(
            "ecs.read_fields({h9}, 'keen::ecs::Big', {{{many}}})"
        )),
        f::TOO_MANY
    );
    // the arguments and the availability
    assert_eq!(
        s.reason(&format!("ecs.read_fields({h3}, 'keen::ecs::Health')")),
        f::READ_ARGUMENTS
    );
    assert_eq!(
        s.reason(&format!(
            "ecs.read_fields({h3}, 'keen::ecs::Unregistered', {{'v'}})"
        )),
        format!("{}{UNREGISTERED}", q::COMPONENT_UNAVAILABLE)
    );
    assert_eq!(
        s.reason("ecs.read_fields(424242, 'keen::ecs::Health', {'hp'})"),
        format!("component read failed: {HEALTH}")
    );
    s.facts(|f| f.ready = false);
    assert_eq!(
        health("{'hp'}"),
        "runtime.ecs.read is currently unavailable"
    );
}

#[test]
fn read_fields_reads_a_field_its_base_type_declares() {
    let mut bytes = 7u32.to_le_bytes().to_vec();
    bytes.extend(0.25f32.to_le_bytes());
    let s = setup(world().with(11, &[(hash(HUNGER), bytes)]));
    let h = s.handle(11);
    let (header, value): (i64, f64) = s.eval(&format!(
        "local v = ecs.read_fields({h}, 'keen::ecs::Hunger', {{'header', 'value'}}); return v.header, v.value"
    ));
    assert_eq!((header, value), (7, 0.25));
}

// ---- get_template ----

#[test]
fn get_template_reads_the_template_uuid_and_stops_at_the_loaded_resource_service() {
    let s = setup(world());
    s.world(|w| w.entities.get_mut(&7).unwrap().template = Some([0x5a; 16]));
    let (h3, h7) = (s.handle(3), s.handle(7));
    assert_eq!(
        s.reason(&format!("ecs.get_template({h7})")),
        NO_RESOURCE_READ
    );
    assert_eq!(s.calls("ecs_template_uuid"), 1);
    assert_eq!(s.reason(&format!("ecs.get_template({h3})")), tpl::NO_UUID);
    assert_eq!(s.reason("ecs.get_template(424242)"), tpl::NO_UUID);
    assert_eq!(
        s.reason(&format!("ecs.get_template({h7}, 1)")),
        tpl::ONE_HANDLE
    );
    assert_eq!(
        s.reason("ecs.get_template('x')"),
        "bad argument #1 (32-bit unsigned integer expected, got string)"
    );
    s.session.set(0);
    assert_eq!(
        s.reason(&format!("ecs.get_template({h7})")),
        tpl::NO_SESSION
    );
    s.session.set(FAKE_SESSION);
    s.facts(|f| f.ready = false);
    assert_eq!(
        s.reason(&format!("ecs.get_template({h7})")),
        tpl::READS_UNAVAILABLE
    );
    s.host.active.set(false);
    assert_eq!(s.reason(&format!("ecs.get_template({h7})")), NOT_ACTIVE);
}
