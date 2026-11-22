// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs component writes tests.
//! `runtime.ecs.write`, `write_fields`, the compare-exchange core and `runtime.overrides.get_component_claim` /
//! `component` on kiln.world's ECS fake (support/fake_services `EcsWorld`, its compare-exchange steps), the registry
//! join and the VM's typed values (support/fake_values, the fixture's codec), on a scripted registrar host and the
//! production runtime facts (the session write latch, the ownership store); the catalog's write rows and `CanWrite`.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 d8a394bc:
//! modules/mod-runtime/lua/ingame/ecs/{write_component,access_component_fields}.rs, lua/values/{inline_field,
//! expect_inline_fields,validate_runtime_scalar}.rs, lua/ingame/overrides/{claim_runtime_storage.rs,
//! create_override_targets.lua}, game-runtime/bridge/windows/ecs/update_component.rs, and the session write latch's
//! texts (game-runtime/native/ecs/write_entity_components.cpp, engine-context/lock_scene_access.cpp).
#[path = "shared/ecs_fixture.rs"]
mod fixture;

use fixture::*;
use kiln_abi::services::kiln_caps::{KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE};
use kiln_sfi::runtime::ecs::cas::text as cas;
use kiln_sfi::runtime::ecs::fields::text as f;
use kiln_sfi::runtime::ecs::{self, install_into};
use kiln_sfi::runtime::operation_catalog::{Backing, CATALOG, Fact};
use kiln_sfi::runtime::overrides::SESSION_REFUSAL;
use kiln_sfi::runtime::overrides::targets::component::{self, text as claim};
use sfi_core::bindings::runtime::RuntimeOperation as Op;

const UNCERTAIN: &str = "ecs-write-outcome-uncertain-rollback-unverified";
const BUSY: &str = "ecs-scene-frame-busy";

fn health(hp: u32, max: u32) -> (u32, Vec<u8>) {
    (hash(HEALTH), words(&[hp, max]))
}
fn counters(values: [u32; 3], speed: f32) -> (u32, Vec<u8>) {
    let mut bytes = words(&values);
    bytes.extend(speed.to_le_bytes());
    (hash(COUNTERS), bytes)
}
fn world() -> EcsWorld {
    let mut hunger = 0u32.to_le_bytes().to_vec();
    hunger.extend(0.5f32.to_le_bytes());
    EcsWorld::server()
        .with(7, &[health(30, 100), (hash(LOCKED), words(&[1]))])
        .with(
            3,
            &[
                counters([1, 2, 3], 2.5),
                (hash(HUNGER), hunger),
                (hash(TAGGED), vec![0; 28]),
            ],
        )
}
fn stored(s: &Setup, id: u32, name: &str) -> Vec<u8> {
    s.world(|w| w.entities[&id].components[&hash(name)].clone())
}
/// A foreign write (a frame) of `value` at `offset` of entity `id`'s Health before one compare-exchange.
fn frame(id: u32, offset: usize, value: u32) -> CasStep {
    CasStep::Foreign {
        entity: id,
        component: hash(HEALTH),
        offset,
        bytes: value.to_le_bytes().to_vec(),
    }
}
/// `write_fields`'s three results.
fn write_fields(s: &Setup, call: &str) -> (bool, Option<String>, Option<Table>) {
    s.eval(&format!("return ecs.write_fields({call})"))
}

// ---- the catalog ----

#[test]
fn the_catalog_backs_the_writes_on_the_reads_and_kiln_world_s_compare_exchange() {
    assert_eq!(CATALOG.check(), Ok(()));
    let capabilities = |backing: Backing| match backing {
        Backing::Kiln(needs) => needs.iter().map(|n| n.capability).collect::<Vec<_>>(),
        _ => panic!("Kiln needs"),
    };
    let both = ["world.ecs_read", "world.ecs_write"];
    assert_eq!(
        capabilities(CATALOG.row(Op::EcsWrite).unwrap().backing),
        both
    );
    let can_write = CATALOG
        .facts
        .iter()
        .find(|r| r.fact == Fact::CanWrite)
        .unwrap();
    assert_eq!(capabilities(can_write.backing), both);
    let s = setup(world());
    assert_eq!(ecs::probe_writes(&s.services), Ok(()));
    let can = || s.facts.provider_facts().can_write;
    assert!(!can());
    fake_services::set_cap(
        "world.ecs_read",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(!can() && !s.facts.static_support(Op::EcsWrite).is_available());
    fake_services::set_cap(
        "world.ecs_write",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(can() && s.facts.static_support(Op::EcsWrite).is_available());
    // only the server world is written
    s.world(|w| w.world_kind = KILN_WORLD_CLIENT as u32);
    assert!(!can());
    s.world(|w| w.world_kind = SERVER);
    fake_services::set_cap("world.ecs_write", None);
    fake_services::set_cap("world.ecs_read", None);
}

// ---- write_fields ----

#[test]
fn write_fields_writes_the_selected_leaves_under_their_mask_and_confirms_them() {
    let s = setup(world());
    let h = s.handle(7);
    let (ok, reason, confirmed) =
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 42}}"));
    assert_eq!((ok, reason), (true, None));
    assert_eq!(confirmed.unwrap().get::<i64>("hp").unwrap(), 42);
    assert_eq!(stored(&s, 7, HEALTH), words(&[42, 100]));
    // the mask holds the field's leaf only; the encoded value went through the typed values as the field's type
    let mask = s.world(|w| w.masks.last().cloned().flatten()).unwrap();
    assert_eq!(mask, [1, 1, 1, 1, 0, 0, 0, 0]);
    assert!(
        s.codec
            .borrow()
            .exported
            .iter()
            .any(|(_, t)| t == "keen::UInt32")
    );
    // the same value again: nothing to change, no compare-exchange
    let before = s.calls("ecs_compare_exchange");
    let (ok, _, confirmed) = write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 42}}"));
    assert!(ok);
    assert_eq!(confirmed.unwrap().get::<i64>("hp").unwrap(), 42);
    assert_eq!(s.calls("ecs_compare_exchange"), before);
    // several fields, a static array element and a float
    let c = s.handle(3);
    let (ok, reason, _) = write_fields(
        &s,
        &format!("{c}, 'keen::ecs::Counters', {{['values[2]'] = 9, speed = 1.5}}"),
    );
    assert_eq!((ok, reason), (true, None));
    let mut expected = words(&[1, 9, 3]);
    expected.extend(1.5f32.to_le_bytes());
    assert_eq!(stored(&s, 3, COUNTERS), expected);
}

#[test]
fn a_frame_s_change_to_other_bytes_is_compared_again_three_holds_at_most() {
    for conflicts in 1..=3usize {
        let s = setup(world());
        let h = s.handle(7);
        s.world(|w| {
            w.cas = (0..conflicts)
                .map(|i| frame(7, 4, 200 + i as u32))
                .collect()
        });
        let (ok, reason, _) = write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"));
        if conflicts < 3 {
            assert_eq!((ok, reason), (true, None), "{conflicts}");
            // the frame's change is kept, the field written over it
            assert_eq!(
                stored(&s, 7, HEALTH),
                words(&[41, 200 + conflicts as u32 - 1]),
                "{conflicts}"
            );
            assert_eq!(s.calls("ecs_compare_exchange"), conflicts as u64 + 1);
        } else {
            assert_eq!((ok, reason.as_deref()), (false, Some(BUSY)));
            assert_eq!(s.calls("ecs_compare_exchange"), 3);
            assert!(s.world(|w| w.writes.is_empty()));
            // busy never latches
            assert_eq!(s.facts.write_fault(), None);
        }
    }
}

#[test]
fn a_foreign_write_to_a_byte_the_change_depends_on_refuses_at_once() {
    let s = setup(world());
    let h = s.handle(7);
    // another writer changed the field itself
    s.world(|w| w.cas = [frame(7, 0, 77)].into());
    let (ok, reason, confirmed) =
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"));
    assert_eq!((ok, reason.as_deref()), (false, Some(cas::CONFLICT)));
    assert!(confirmed.is_none());
    assert_eq!(s.calls("ecs_compare_exchange"), 1);
    assert_eq!(stored(&s, 7, HEALTH), words(&[77, 100]));
    // a field the write expected: the restore of an override over another writer's change
    s.world(|w| w.cas = [frame(7, 4, 5)].into());
    let (ok, reason, _) = write_fields(
        &s,
        &format!("{h}, 'keen::ecs::Health', {{hp = 30}}, {{max = 100}}"),
    );
    assert_eq!((ok, reason.as_deref()), (false, Some(cas::CONFLICT)));
    assert_eq!(stored(&s, 7, HEALTH), words(&[77, 5]));
    // an expected value that no longer holds is refused before any compare-exchange
    let before = s.calls("ecs_compare_exchange");
    let (ok, reason, _) = write_fields(
        &s,
        &format!("{h}, 'keen::ecs::Health', {{hp = 30}}, {{max = 100}}"),
    );
    assert_eq!(
        (ok, reason.as_deref()),
        (
            false,
            Some("Selected field snapshot changed before update. Read current fields and retry")
        )
    );
    assert_eq!(s.calls("ecs_compare_exchange"), before);
    assert_eq!(
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 1}}, {{}}"))
            .1
            .as_deref(),
        Some("expected fields must be nonempty when provided")
    );
}

#[test]
fn busy_is_never_retried_and_an_uncertain_write_latches_the_session() {
    let s = setup(world());
    let h = s.handle(7);
    s.world(|w| w.cas = [CasStep::Answer(Knob::Busy)].into());
    let (ok, reason, _) = write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"));
    assert_eq!((ok, reason.as_deref()), (false, Some(BUSY)));
    assert_eq!(s.calls("ecs_compare_exchange"), 1);
    assert_eq!(s.facts.write_fault(), None);
    // stale (here: scripted) and the rest: ShroudForge's failure
    s.world(|w| w.cas = [CasStep::Answer(Knob::Stale)].into());
    assert_eq!(
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"))
            .1
            .as_deref(),
        Some(cas::FAILED)
    );
    s.world(|w| w.cas = [CasStep::Answer(Knob::Unavailable)].into());
    assert_eq!(
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"))
            .1
            .as_deref(),
        Some(cas::NO_WRITE_CONTEXT)
    );
    s.world(|w| w.cas = [CasStep::Answer(Knob::Uncertain)].into());
    let (ok, reason, _) = write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 41}}"));
    assert_eq!((ok, reason.as_deref()), (false, Some(UNCERTAIN)));
    // the latch holds for this session: the registrar denies every later ECS write with it
    assert_eq!(s.facts.write_fault().as_deref(), Some(UNCERTAIN));
    *s.host.fault.borrow_mut() = s.facts.write_fault();
    assert_eq!(
        write_fields(&s, &format!("{h}, 'keen::ecs::Health', {{hp = 2}}"))
            .1
            .as_deref(),
        Some(UNCERTAIN)
    );
    assert_eq!(
        s.reason(&format!("ecs.write({h}, 'keen::ecs::Health', 1)")),
        UNCERTAIN
    );
    // reads go on; a new session ends the latch
    let (values, _): (Option<Table>, Option<String>) = s.eval(&format!(
        "return ecs.read_fields({h}, 'keen::ecs::Health', {{'hp'}})"
    ));
    assert!(values.is_some());
    fake_services::set_world_session(s.owner, SERVER, FAKE_SESSION + 1);
    assert_eq!(s.facts.write_fault(), None);
}

#[test]
fn write_fields_refuses_as_shroudforge_does() {
    let s = setup(world());
    let (h, c) = (s.handle(7), s.handle(3));
    let reason = |call: String| {
        write_fields(&s, &call)
            .1
            .unwrap_or_else(|| panic!("{call}: no reason"))
    };
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Health', {{'hp'}}")),
        f::WRITE_KEYS
    );
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Health'")),
        f::WRITE_ARGUMENTS
    );
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Health', {{hp = 1}}, {{}}, 5")),
        f::WRITE_ARGUMENTS
    );
    assert_eq!(
        reason(format!("{c}, 'keen::ecs::Counters', {{speed = 0/0}}")),
        format!("speed: {}", cas::NOT_FINITE)
    );
    // the component's write policy, and an attribute component
    fake_services::set_write_policy(
        s.owner,
        hash(LOCKED),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Locked', {{v = 2}}")),
        cas::NO_WRITE_CONTEXT
    );
    assert_eq!(
        reason(format!("{c}, 'keen::ecs::Hunger', {{value = 1}}")),
        cas::ATTRIBUTES
    );
    assert_eq!(
        reason(format!("{c}, 'keen::ecs::Tagged', {{list = 1}}")),
        "list: keen::DsList has external storage or reference ownership. Select an inline sibling field"
    );
    // the write context and the denial
    s.facts(|f| f.can_write = false);
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Health', {{hp = 1}}")),
        "runtime.ecs.write is currently unavailable"
    );
    s.facts(|f| f.can_write = true);
    s.host.active.set(false);
    assert_eq!(
        reason(format!("{h}, 'keen::ecs::Health', {{hp = 1}}")),
        NOT_ACTIVE
    );
    s.host.active.set(true);
    assert!(s.world(|w| w.writes.is_empty()));
}

// ---- write ----

#[test]
fn write_takes_the_value_read_gave_and_writes_its_changed_ranges() {
    let s = setup(world());
    let h = s.handle(7);
    s.eval::<()>(&format!("value = ecs.read({h}, 'keen::ecs::Health')"));
    let lease = *s.codec.borrow().sources.keys().next().unwrap();
    // what the typed values serialize the (edited) value as
    s.codec
        .borrow_mut()
        .exports
        .insert(lease, words(&[55, 100]));
    let (ok, reason): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write({h}, 'keen::ecs::Health', value)"
    ));
    assert_eq!((ok, reason), (true, None));
    assert_eq!(stored(&s, 7, HEALTH), words(&[55, 100]));
    assert_eq!(
        s.world(|w| w.masks.last().cloned().flatten()).unwrap(),
        [1, 1, 1, 1, 0, 0, 0, 0]
    );
    assert_eq!(
        s.codec.borrow().exported.last().unwrap(),
        &(lease, HEALTH.to_owned())
    );
    // an unchanged value: true, nothing written
    s.codec
        .borrow_mut()
        .exports
        .insert(lease, words(&[30, 100]));
    let before = s.calls("ecs_compare_exchange");
    let (ok, reason): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write({h}, 'keen::ecs::Health', value)"
    ));
    assert_eq!((ok, reason), (true, None));
    assert_eq!(s.calls("ecs_compare_exchange"), before);
    // the value must be read's
    assert_eq!(
        s.reason(&format!("ecs.write({h}, 'keen::ecs::Health', {{hp = 1}})")),
        format!("runtime.ecs.write requires the value returned by runtime.ecs.read for {HEALTH}")
    );
    // a float that is not finite is raised, as ShroudForge raises it
    let c = s.handle(3);
    s.eval::<()>(&format!("counters = ecs.read({c}, 'keen::ecs::Counters')"));
    let lease = *s
        .codec
        .borrow()
        .sources
        .iter()
        .find(|(_, b)| b.len() == 16)
        .unwrap()
        .0;
    let mut nan = words(&[1, 2, 3]);
    nan.extend(f32::NAN.to_le_bytes());
    s.codec.borrow_mut().exports.insert(lease, nan);
    assert_eq!(
        s.raised(&format!("ecs.write({c}, 'keen::ecs::Counters', counters)")),
        cas::NOT_FINITE
    );
    // a type that owns engine storage is no owned value; the readiness and the denial come first
    assert!(
        s.reason(&format!("ecs.write({c}, 'keen::ecs::Tagged', counters)"))
            .contains("requires engine allocation")
    );
    s.facts(|f| f.ready = false);
    assert_eq!(
        s.reason(&format!("ecs.write({h}, 'keen::ecs::Health', value)")),
        NOT_READY
    );
    s.host.active.set(false);
    assert_eq!(s.reason("ecs.write()"), NOT_ACTIVE);
}

// ---- the component claim and override target ----

/// `runtime` for package `id` over the setup's Ecs: its own ecs and overrides tables and a session_id.
fn runtime(s: &Setup, id: &str) -> (Rc<Script>, Table) {
    let lua = s.vm.lua();
    let host = script(id);
    let runtime = lua.create_table().unwrap();
    let ecs = lua.create_table().unwrap();
    install_into(lua, &ecs, host.clone(), s.ecs.clone()).unwrap();
    runtime.raw_set("ecs", ecs).unwrap();
    runtime
        .raw_set("overrides", lua.create_table().unwrap())
        .unwrap();
    let world = lua.create_table().unwrap();
    let session = s.session.clone();
    world
        .raw_set(
            "session_id",
            lua.create_function(move |_, ()| Ok(session.get())).unwrap(),
        )
        .unwrap();
    runtime.raw_set("world", world).unwrap();
    component::install_into(lua, &runtime, host.clone(), s.ecs.clone()).unwrap();
    s.vm.set(&format!("rt_{id}"), runtime.clone());
    (host, runtime)
}

#[test]
fn a_component_claim_is_checked_on_acquire_and_keeps_other_mods_out() {
    let s = setup(world());
    let h = s.handle(7);
    runtime(&s, "alpha");
    runtime(&s, "beta");
    let claim = |id: &str, ty: &str| -> (bool, Option<String>) {
        s.eval(&format!(
            "local c, why = rt_{id}.overrides.get_component_claim({h}, '{ty}'); if not c then return false, why end
             return c:acquire()"
        ))
    };
    assert_eq!(claim("alpha", HEALTH), (true, None));
    // another mod's typed write of that component is refused while alpha holds it
    let (ok, reason, _): (bool, Option<String>, Option<Table>) = s.eval(&format!(
        "return rt_beta.ecs.write_fields({h}, 'keen::ecs::Health', {{hp = 1}})"
    ));
    assert!(!ok);
    assert_eq!(
        reason.unwrap(),
        format!(
            "component {HEALTH} on entity {h} is owned by runtime mod 'alpha'. Disable its setting and restore it before another mod writes this storage"
        )
    );
    let (ok, _, _): (bool, Option<String>, Option<Table>) = s.eval(&format!(
        "return rt_alpha.ecs.write_fields({h}, 'keen::ecs::Health', {{hp = 1}})"
    ));
    assert!(ok);
    // the claim's own refusals
    let reason = |call: &str| -> String {
        let (claim, why): (Value, Option<String>) = s.eval(&format!(
            "return rt_alpha.overrides.get_component_claim({call})"
        ));
        assert!(claim.is_nil());
        why.unwrap()
    };
    assert_eq!(reason(&format!("{h}")), claim::ARGUMENTS);
    assert_eq!(
        reason(&format!("{h}, 'keen::ecs::Unregistered'")),
        claim::LAYOUT
    );
    // acquire: the write policy and the entity, then the session
    fake_services::set_write_policy(
        s.owner,
        hash(LOCKED),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    assert_eq!(
        claim("alpha", LOCKED),
        (false, Some(claim::AUTHORITY.into()))
    );
    s.eval::<()>(&format!(
        "old = rt_alpha.overrides.get_component_claim({h}, 'keen::ecs::Health')"
    ));
    s.session.set(FAKE_SESSION + 1);
    let (ok, why): (bool, Option<String>) = s.eval("return old:acquire()");
    assert_eq!((ok, why.as_deref()), (false, Some(SESSION_REFUSAL)));
}

#[test]
fn the_component_target_reads_writes_and_never_restores_over_a_foreign_write() {
    let s = setup(world());
    let h = s.handle(7);
    runtime(&s, "alpha");
    s.eval::<()>(&format!(
        "target = rt_alpha.overrides.component({h}, 'keen::ecs::Health', 'hp', {{'max'}})"
    ));
    let key: String = s.eval("return target.key");
    assert_eq!(key, format!("component/{FAKE_SESSION}/{h}/{HEALTH}/hp"));
    let (value, max): (i64, i64) =
        s.eval("local snap = target.read(); return snap.value, snap.values.max");
    assert_eq!((value, max), (30, 100));
    // the canonical value is the field codec's
    let canonical: f64 = s.eval("return target.normalize(nil, 12.0)");
    assert_eq!(canonical, 12.0);
    // write: the snapshot's fields are the expectations; the confirmed value comes back twice
    let (ok, actual, why, canonical): (bool, i64, Option<String>, i64) =
        s.eval("local snap = target.read(); return target.write(snap, 12)");
    assert_eq!((ok, actual, why, canonical), (true, 12, None, 12));
    assert_eq!(stored(&s, 7, HEALTH), words(&[12, 100]));
    // another writer changes an expected field between the read and the restore: refused, its value kept
    s.eval::<()>("snap = target.read()");
    s.world(|w| {
        w.entities
            .get_mut(&7)
            .unwrap()
            .components
            .insert(hash(HEALTH), words(&[12, 90]));
    });
    let (ok, actual, why): (bool, Value, Option<String>) = s.eval("return target.write(snap, 30)");
    assert!(!ok && actual.is_nil());
    assert_eq!(
        why.as_deref(),
        Some("Selected field snapshot changed before update. Read current fields and retry")
    );
    assert_eq!(stored(&s, 7, HEALTH), words(&[12, 90]));
    // ... and also when it lands right before the compare-exchange
    s.eval::<()>("snap = target.read()");
    s.world(|w| w.cas = [frame(7, 4, 80)].into());
    let (ok, _, why): (bool, Value, Option<String>) = s.eval("return target.write(snap, 30)");
    assert_eq!((ok, why.as_deref()), (false, Some(cas::CONFLICT)));
    assert_eq!(stored(&s, 7, HEALTH), words(&[12, 80]));
    // a target of another mod's component is not made
    let (target, why): (Value, Option<String>) = s.eval(&format!(
        "return rt_alpha.overrides.component({h}, 'keen::ecs::Unregistered', 'v')"
    ));
    assert!(target.is_nil());
    assert_eq!(why.as_deref(), Some(claim::LAYOUT));
}
