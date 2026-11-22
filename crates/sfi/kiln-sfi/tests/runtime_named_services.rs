// SPDX-License-Identifier: MIT
// the interpreter's runtime.services tests.
//! `runtime.services.get`, its views (`read_fields`, `write_field`, the fields) and `runtime.overrides.get_service_claim` /
//! `service` on kiln.world's named-service fake (support/fake_services [`NamedServices`]: services, views bound to
//! their session, the compare-exchange's steps), the archive registry and the VM's typed values of the ECS fixture,
//! a scripted registrar host and the production runtime facts (the session write latch, the ownership store); the
//! production registrar's family list. The service keys are the fixture's `keen::ecs::` types (ecs::Health and
//! ecs::Counters stand for ecs::GameSettings). Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026
//! bonsaibauer), 1.7.9 d8a394bc: modules/mod-runtime/lua/ingame/access_named_service.rs, lua/ingame/overrides/
//! {claim_runtime_storage.rs,create_override_targets.lua}, game-runtime/bridge/windows/named_services/
//! {read_service.rs,update_service.rs}.
#[path = "shared/ecs_fixture.rs"]
mod fixture;

use fake_services::{NamedServices, NamedStep, Shape};
use fixture::*;
use kiln_sfi::runtime::install::Family;
use kiln_sfi::runtime::named_services::{self, text as t};
use kiln_sfi::runtime::overrides::targets::service::text as claim;
use kiln_sfi::runtime::overrides::{OverrideTrackers, SESSION_REFUSAL};
use kiln_sfi::runtime::registrar::{self, Parts};

thread_local! {
    static TRACKERS: Rc<OverrideTrackers> = OverrideTrackers::new();
}

const HEALTH_KEY: &str = "ecs::Health";
const COUNTERS_KEY: &str = "ecs::Counters";

/// A setup whose kiln.world has ecs::Health (hp 30, max 100) and ecs::Counters as named services.
fn services_setup() -> Setup {
    let s = setup(EcsWorld::server());
    let mut counters = words(&[1, 2, 3]);
    counters.extend(2.5f32.to_le_bytes());
    fake_services::set_named_services(
        s.owner,
        Some(
            NamedServices::new()
                .with(HEALTH_KEY, words(&[30, 100]))
                .with(COUNTERS_KEY, counters),
        ),
    );
    s
}
fn named<R>(s: &Setup, f: impl FnOnce(&mut NamedServices) -> R) -> R {
    fake_services::named_services(s.owner, f).expect("named services")
}
fn stored(s: &Setup, key: &str) -> Vec<u8> {
    named(s, |n| n.services[key].clone())
}
/// `runtime` for package `id` over the setup's Ecs: its services and overrides, installed as the family does.
fn runtime(s: &Setup, id: &str) -> Rc<Script> {
    let lua = s.vm.lua();
    let host = script(id);
    let runtime = lua.create_table().unwrap();
    let overrides = lua.create_table().unwrap();
    let live = s.session.clone();
    TRACKERS
        .with(|t| t.install(lua, &overrides, id, Rc::new(move || live.get())))
        .unwrap();
    runtime.raw_set("overrides", overrides).unwrap();
    named_services::install_into(lua, &runtime, host.clone(), s.ecs.clone()).unwrap();
    s.vm.set(&format!("rt_{id}"), runtime);
    host
}
/// `get`'s refusal for package alpha.
fn get_reason(s: &Setup, args: &str) -> String {
    s.reason(&format!("rt_alpha.services.get({args})"))
}
fn needs_contract(key: &str, reason: &str) -> String {
    format!("{key}{}{reason}", t::NEEDS_CONTRACT)
}

// ---- the registrar ----

fn names(parts: Parts) -> Vec<&'static str> {
    registrar::production(parts)
        .unwrap()
        .iter()
        .map(Family::name)
        .collect()
}
fn parts(ecs: Option<Family>) -> Parts {
    Parts {
        types: None,
        ecs,
        attributes: None,
        assets: None,
        network: Family::new("network", &["runtime.network.status"], |_, _, _, _, _| {
            Ok(())
        }),
        ui: Family::new("ui", &["shroudforge.ui"], |_, _, _, _, _| Ok(())),
        trackers: None,
    }
}

#[test]
fn the_registrar_adds_the_named_services_right_after_the_ecs_family() {
    let ecs = Family::new("ecs-registry", &["runtime.ecs.query"], |_, _, _, _, _| {
        Ok(())
    });
    let with = names(parts(Some(ecs)));
    let at = with.iter().position(|n| *n == "ecs-registry").unwrap();
    assert_eq!(with[at + 1], named_services::FAMILY);
    // without the ECS family (no types wiring) the unavailable family answers the three names
    let without = names(parts(None));
    assert!(!without.contains(&named_services::FAMILY));
    assert_eq!(
        named_services::CLAIMS,
        [
            "runtime.services.get",
            "runtime.overrides.get_service_claim",
            "runtime.overrides.service"
        ]
    );
}

// ---- get ----

#[test]
fn get_checks_access_the_name_the_reflected_type_and_its_inline_fields_in_shroudforge_s_order() {
    let s = services_setup();
    let alpha = runtime(&s, "alpha");
    assert_eq!(get_reason(&s, ""), t::GET_ARGUMENTS);
    assert_eq!(get_reason(&s, "'a', 'b'"), t::GET_ARGUMENTS);
    for bad in [
        "''",
        "'keen::ecs::Health'",
        &format!("'{}'", "x".repeat(128)),
    ] {
        assert_eq!(get_reason(&s, bad), t::EXACT_NAME, "{bad}");
    }
    assert_eq!(
        get_reason(&s, "'ecs::Nope'"),
        format!("{}keen::ecs::Nope", t::NO_TYPE)
    );
    // a static array is not a plain-data struct
    assert_eq!(get_reason(&s, "'Float32x4'"), t::NOT_PLAIN);
    // a field with dynamic storage refuses the whole service
    let tagged = get_reason(&s, "'ecs::Tagged'");
    assert!(
        tagged.contains("has external storage or reference ownership"),
        "{tagged}"
    );
    // a type kiln.world names no contract for
    assert_eq!(
        get_reason(&s, "'ecs::Locked'"),
        needs_contract("ecs::Locked", t::GUARDED_READ)
    );
    // no live session
    s.session.set(0);
    assert_eq!(get_reason(&s, &format!("'{HEALTH_KEY}'")), t::VIEW_SESSION);
    s.session.set(FAKE_SESSION);
    // the denial and the availability of runtime.ecs.read come first
    alpha.facts.borrow_mut().ready = false;
    assert_eq!(
        get_reason(&s, "'x'"),
        "runtime.ecs.read is currently unavailable"
    );
    alpha.facts.borrow_mut().ready = true;
    alpha.contract.set(false);
    assert_eq!(
        get_reason(&s, "'x'"),
        format!("operation runtime.ecs.read has not been verified for game build {BUILD}")
    );
    alpha.contract.set(true);
    alpha.active.set(false);
    assert_eq!(get_reason(&s, "'x'"), NOT_ACTIVE);
    // nothing reached kiln.world for any of these but the one contract it was asked for
    assert_eq!(s.calls("named_service_open"), 1);
}

#[test]
fn a_view_names_its_service_and_reads_through_one_kiln_world_view_per_key() {
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "view = assert(rt_alpha.services.get('{HEALTH_KEY}'))"
    ));
    let (key, scope, session, writable, ty): (String, String, u64, bool, Value) =
        s.eval("return view.key, view.scope, view.session_id, view.write_supported, view.type");
    assert_eq!(
        (key.as_str(), scope.as_str(), session, writable),
        (HEALTH_KEY, "host-world-shared", FAKE_SESSION, true)
    );
    assert!(matches!(ty, Value::UserData(_)), "{ty:?}");
    let (hp, max): (i64, i64) =
        s.eval("local f = assert(view:read_fields({'hp', 'max'})); return f.hp, f.max");
    assert_eq!((hp, max), (30, 100));
    // a second get and every read use the same kiln.world view
    s.eval::<()>(&format!(
        "again = assert(rt_alpha.services.get('{HEALTH_KEY}'))"
    ));
    let hp: i64 = s.eval("return again:read_fields({'hp'}).hp");
    assert_eq!(hp, 30);
    assert_eq!(
        (s.calls("named_service_open"), named(&s, |n| n.views.len())),
        (1, 1)
    );
    // read_fields' own checks
    let read = |call: &str| -> String { s.reason(&format!("view:read_fields({call})")) };
    assert_eq!(read(""), t::READ_ARGUMENTS);
    assert_eq!(read("'hp'"), t::READ_TABLE);
    assert_eq!(read("{hp = 1}"), t::DENSE);
    assert_eq!(read("{1}"), t::STRINGS);
    assert_eq!(read("{'hp', 'hp'}"), t::DUPLICATE);
    assert_eq!(read("{}"), t::NONEMPTY);
    assert!(read("{'nope'}").contains("reflected field not found"));
    // kiln.world's session moved on (the server world started again): the view is opened again
    named(&s, |n| n.session += 1);
    let hp: i64 = s.eval("return view:read_fields({'hp'}).hp");
    assert_eq!(hp, 30);
    assert_eq!(s.calls("named_service_open"), 2);
    assert_eq!(named(&s, |n| (n.views.len(), n.closed.len())), (1, 1));
    // ShroudForge's session marker moved on: the view is of another session
    s.session.set(FAKE_SESSION + 1);
    assert_eq!(read("{'hp'}"), t::VIEW_SESSION);
    // a refused snapshot: ShroudForge's provider text
    s.session.set(FAKE_SESSION);
    named(&s, |n| {
        n.steps.push_back(NamedStep::Answer(KILN_ECS_ERR_BUSY))
    });
    assert_eq!(read("{'hp'}"), t::GUARDED_READ);
    named(&s, |n| n.session = 0);
    assert_eq!(read("{'hp'}"), t::GUARDED_READ);
}

#[test]
fn an_older_kiln_world_without_named_services_says_the_provider_lacks_them() {
    let s = services_setup();
    fake_services::set_shape("kiln.world", Some(Shape::Short(616)));
    runtime(&s, "alpha");
    assert_eq!(
        get_reason(&s, &format!("'{HEALTH_KEY}'")),
        needs_contract(HEALTH_KEY, t::NO_SNAPSHOTS)
    );
    fake_services::set_shape("kiln.world", None);
}

#[test]
fn the_views_close_with_the_vm() {
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "assert(rt_alpha.services.get('{HEALTH_KEY}')); assert(rt_alpha.services.get('{COUNTERS_KEY}'))"
    ));
    let owner = s.owner;
    assert_eq!(
        fake_services::named_services(owner, |n| n.views.len()),
        Some(2)
    );
    drop(s);
    assert_eq!(
        fake_services::named_services(owner, |n| (n.views.len(), n.closed.len())),
        Some((0, 2))
    );
}

// ---- write_field ----

#[test]
fn write_field_compares_the_whole_service_and_writes_only_the_field_under_its_mask() {
    let s = services_setup();
    let alpha = runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "view = assert(rt_alpha.services.get('{HEALTH_KEY}'))"
    ));
    let (ok, why): (bool, Option<String>) = s.eval("return view:write_field('hp', 42, 30)");
    assert_eq!((ok, why), (true, None));
    assert_eq!(stored(&s, HEALTH_KEY), words(&[42, 100]));
    assert_eq!(
        named(&s, |n| n.masks.last().cloned().flatten()),
        Some(vec![1, 1, 1, 1, 0, 0, 0, 0])
    );
    // the expected current value is checked against a fresh snapshot first
    let write = |call: &str| -> (bool, Option<String>) {
        s.eval(&format!("return view:write_field({call})"))
    };
    assert_eq!(write("'hp', 1, 30"), (false, Some(t::FIELD_CHANGED.into())));
    // the expected fields too
    assert_eq!(write("'hp', 43, 42, {max = 100}"), (true, None));
    assert_eq!(
        write("'hp', 44, 43, {max = 5}"),
        (
            false,
            Some(
                "Selected field snapshot changed before update. Read current fields and retry"
                    .into()
            )
        )
    );
    assert_eq!(
        write("'hp', 44, 43, 7"),
        (false, Some(t::EXPECTED_FIELDS.into()))
    );
    assert_eq!(write("'hp', 44"), (false, Some(t::WRITE_ARGUMENTS.into())));
    assert_eq!(write("1, 44, 43"), (false, Some(t::WRITE_PATH.into())));
    // another writer changed the service between the snapshot and the compare-exchange: refused, its change kept
    named(&s, |n| {
        n.steps.push_back(NamedStep::Plain); // the snapshot
        n.steps.push_back(NamedStep::Foreign {
            key: HEALTH_KEY.into(),
            offset: 4,
            bytes: 77u32.to_le_bytes().to_vec(),
        });
    });
    assert_eq!(write("'hp', 44, 43"), (false, Some(t::CONFLICT.into())));
    assert_eq!(stored(&s, HEALTH_KEY), words(&[43, 77]));
    // busy (a frame held the gate) and any other refusal: ShroudForge's failed text, nothing latched
    named(&s, |n| {
        n.steps
            .extend([NamedStep::Plain, NamedStep::Answer(KILN_ECS_ERR_BUSY)])
    });
    assert_eq!(write("'hp', 44, 43"), (false, Some(t::FAILED.into())));
    assert_eq!(s.facts.write_fault(), None);
    // the access checks of runtime.ecs.write
    alpha.facts.borrow_mut().can_write = false;
    assert_eq!(
        write("'hp', 44, 43"),
        (
            false,
            Some("runtime.ecs.write is currently unavailable".into())
        )
    );
    alpha.facts.borrow_mut().can_write = true;
    assert_eq!(stored(&s, HEALTH_KEY), words(&[43, 77]));
}

#[test]
fn an_uncertain_write_latches_the_session_and_the_view() {
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "view = assert(rt_alpha.services.get('{HEALTH_KEY}'))"
    ));
    named(&s, |n| {
        n.steps
            .extend([NamedStep::Plain, NamedStep::Answer(KILN_ECS_ERR_UNCERTAIN)])
    });
    let (ok, why): (bool, Option<String>) = s.eval("return view:write_field('hp', 42, 30)");
    assert_eq!((ok, why.as_deref()), (false, Some(t::UNCERTAIN)));
    let fault = s.facts.write_fault();
    assert_eq!(fault.as_deref(), Some(t::UNCERTAIN));
    // the view answers it at once from now on, without kiln.world
    let calls = s.calls("named_service_snapshot");
    let (ok, why): (bool, Option<String>) = s.eval("return view:write_field('hp', 42, 30)");
    assert_eq!((ok, why.as_deref()), (false, Some(t::UNCERTAIN)));
    assert_eq!(s.calls("named_service_snapshot"), calls);
    assert_eq!(stored(&s, HEALTH_KEY), words(&[30, 100]));
}

// ---- the claim and the service target ----

#[test]
fn a_service_claim_belongs_to_its_package_and_keeps_other_mods_out() {
    let s = services_setup();
    runtime(&s, "alpha");
    runtime(&s, "beta");
    s.eval::<()>(&format!(
        "va = assert(rt_alpha.services.get('{HEALTH_KEY}')); vb = assert(rt_beta.services.get('{HEALTH_KEY}'))"
    ));
    let claim_reason = |call: &str| -> String {
        s.reason(&format!("rt_alpha.overrides.get_service_claim({call})"))
    };
    assert_eq!(claim_reason(""), claim::ARGUMENTS);
    assert_eq!(claim_reason("vb"), t::OTHER_MOD);
    assert!(
        claim_reason("{}").contains("RuntimeService"),
        "{}",
        claim_reason("{}")
    );
    let (ok, why): (bool, Option<String>) =
        s.eval("ca = rt_alpha.overrides.get_service_claim(va); return ca:acquire()");
    assert_eq!((ok, why), (true, None));
    // beta's typed write of the service is refused while alpha holds it
    let (ok, why): (bool, Option<String>) = s.eval("return vb:write_field('hp', 1, 30)");
    assert!(!ok);
    assert_eq!(
        why.unwrap(),
        format!(
            "named service {HEALTH_KEY} is owned by runtime mod 'alpha'. Disable its setting and restore it before another mod writes this storage"
        )
    );
    let (ok, _): (bool, Option<String>) = s.eval("return va:write_field('hp', 1, 30)");
    assert!(ok);
    // a claim of an ended session
    s.eval::<()>("old = rt_alpha.overrides.get_service_claim(va)");
    s.session.set(FAKE_SESSION + 1);
    let (ok, why): (bool, Option<String>) = s.eval("return old:acquire()");
    assert_eq!((ok, why.as_deref()), (false, Some(SESSION_REFUSAL)));
}

#[test]
fn the_service_target_reads_writes_and_never_restores_over_a_foreign_write() {
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "view = assert(rt_alpha.services.get('{HEALTH_KEY}')); target = assert(rt_alpha.overrides.service(view, 'hp', {{'max'}}))"
    ));
    let (key, shared): (String, bool) = s.eval("return target.key, target.shared");
    assert_eq!(
        (key, shared),
        (format!("view/{FAKE_SESSION}/{HEALTH_KEY}/hp"), true)
    );
    let (value, max): (i64, i64) =
        s.eval("local snap = assert(target.read()); return snap.value, snap.values.max");
    assert_eq!((value, max), (30, 100));
    let canonical: f64 = s.eval("return target.normalize(nil, 12.0)");
    assert_eq!(canonical, 12.0);
    let (ok, actual, why): (bool, i64, Option<String>) =
        s.eval("local snap = target.read(); return target.write(snap, 12)");
    assert_eq!((ok, actual, why), (true, 12, None));
    assert_eq!(stored(&s, HEALTH_KEY), words(&[12, 100]));
    // another writer changes an expected field between the read and the restore: refused, its value kept
    s.eval::<()>("snap = target.read()");
    named(&s, |n| {
        n.services.insert(HEALTH_KEY.into(), words(&[12, 90]))
    });
    let (ok, actual, why): (bool, Value, Option<String>) = s.eval("return target.write(snap, 30)");
    assert!(!ok && actual.is_nil());
    assert_eq!(
        why.as_deref(),
        Some("Selected field snapshot changed before update. Read current fields and retry")
    );
    assert_eq!(stored(&s, HEALTH_KEY), words(&[12, 90]));
    // a field that is not a finite scalar is no target value
    s.eval::<()>("bad = assert(rt_alpha.overrides.service(view, 'nope'))");
    let (snap, why): (Value, Option<String>) = s.eval("return bad.read()");
    assert!(snap.is_nil() && why.unwrap().contains("reflected field not found"));
}

#[test]
fn an_override_tracker_applies_the_service_setting_and_puts_the_world_s_value_back() {
    // SF Survival's Infinite Weapon Durability, on the fixture's service: apply while the setting is on, restore when
    // it is switched off.
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "o = rt_alpha.overrides.new()
         on = true
         function step()
             o:begin_update()
             o:restore_disabled(function() return on end)
             if not on then return true end
             local view, why = rt_alpha.services.get('{HEALTH_KEY}')
             if not view then return false, why end
             local target, failure = rt_alpha.overrides.service(view, 'hp')
             if not target then return false, failure end
             return o:apply(target.key, 'setting', target, function() return 0 end)
         end"
    ));
    let (applied, why): (bool, Option<String>) = s.eval("return step()");
    assert_eq!((applied, why), (true, None));
    assert_eq!(stored(&s, HEALTH_KEY), words(&[0, 100]));
    // the next update keeps it, writing nothing
    let writes = named(&s, |n| n.writes.len());
    let (applied, _): (bool, Option<String>) = s.eval("return step()");
    assert!(applied);
    assert_eq!(named(&s, |n| n.writes.len()), writes);
    // switched off: the world's own value comes back
    let (done, _): (bool, Option<String>) = s.eval("on = false; return step()");
    assert!(done);
    assert_eq!(stored(&s, HEALTH_KEY), words(&[30, 100]));
    let owns: bool = s.eval("return o:has_ownership()");
    assert!(!owns);
}

#[test]
fn a_float_field_of_another_service_is_written_through_the_same_view_kind() {
    let s = services_setup();
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "c = assert(rt_alpha.services.get('{COUNTERS_KEY}'))"
    ));
    let speed: f64 = s.eval("return c:read_fields({'speed'}).speed");
    assert_eq!(speed, 2.5);
    let (ok, why): (bool, Option<String>) = s.eval("return c:write_field('values[2]', 9, 2)");
    assert_eq!((ok, why), (true, None));
    let mut expected = words(&[1, 9, 3]);
    expected.extend(2.5f32.to_le_bytes());
    assert_eq!(stored(&s, COUNTERS_KEY), expected);
}
