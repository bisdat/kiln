// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs attribute writes tests.
//! `runtime.ecs.update_attribute`, `write_attribute_storage`, the attribute recalculation of the plain component writes
//! and `runtime.overrides.attribute` with the override tracker, over the fake archive's attribute group and
//! kiln.engine's model row (support/fake_attributes.rs) and kiln.world's ECS fake with its compare-exchange, through the
//! real service adapter, the registry join and the production ownership store, on scripted registrar hosts.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 d8a394bc:
//! modules/mod-runtime/lua/ingame/ecs/attributes/{update_attribute,write_attribute_storage,recalculate_bound_component,
//! recalculate_component_update,expect_root_values}.rs, lua/ingame/overrides/{create_override_targets,
//! track_runtime_overrides}.lua and claim_runtime_storage.rs.
#[path = "shared/attr_fixture.rs"]
mod fixture;

use fixture::*;
use kiln_sfi::runtime::ecs::attributes::writes::text as w;
use kiln_sfi::runtime::ecs::cas::text as cas;
use serde_json::{Value as Json, json};

/// Entity 4: Stamina (70, 0, 80) and Revive (1.5, 2.0); entity 6: a Health only.
fn world() -> EcsWorld {
    EcsWorld::server()
        .with(4, &[stamina([70, 0, 80], 1), revive([1.5, 2.0])])
        .with(6, &[(hash(HEALTH), words(&[30, 100]))])
}

/// The Stamina root's values as the world holds them.
fn values(s: &AttrSetup, id: u32) -> [i32; 3] {
    s.world(|w| {
        let bytes = &w.entities[&id].components[&fnv(STAMINA)];
        let word = |i: usize| i32::from_le_bytes(bytes[24 + 4 * i..28 + 4 * i].try_into().unwrap());
        [word(0), word(1), word(2)]
    })
}

// update_attribute: the value encoded, the whole root recalculated on the archive (Stamina clamped to its maximum),
// one compare-exchange of the words that changed; ShroudForge's result with applied = true.
#[test]
fn update_attribute_recalculates_and_writes_the_root() {
    let s = setup(world());
    let e = s.handle(4);
    let (stamina, max, applied, steps, entity): (i64, i64, bool, i64, i64) = s.eval(&format!(
        "local r = ecs.update_attribute({e}, 'Stamina', 50)
         return r.values.Stamina, r.values.Stamina_Max, r.applied, #r.trace, r.entity"
    ));
    assert_eq!((stamina, max, applied, steps, entity), (50, 80, true, 1, e));
    assert_eq!(values(&s, 4), [50, 0, 80]);
    let mask = s.world(|w| w.masks.last().cloned().flatten()).unwrap();
    let expected: Vec<u8> = (0..36).map(|i| u8::from((24..28).contains(&i))).collect();
    assert_eq!(mask, expected, "only the word that changed is written");
    // above the maximum: the recalculation clamps it, and the clamped value is what is written and answered
    let clamped: i64 = s.eval(&format!(
        "return ecs.update_attribute({e}, 'Stamina', 120).values.Stamina"
    ));
    assert_eq!(clamped, 80);
    assert_eq!(values(&s, 4), [80, 0, 80]);
    // a value the recalculation keeps as it was: nothing to change, nothing written
    let writes = s.world(|w| w.writes.len());
    let same: i64 = s.eval(&format!(
        "return ecs.update_attribute({e}, 'Stamina', 90).values.Stamina"
    ));
    assert_eq!(same, 80);
    assert_eq!(s.world(|w| w.writes.len()), writes);
}

#[test]
fn update_attribute_checks_expected_values_against_the_fresh_root() {
    let s = setup(world());
    let e = s.handle(4);
    let ok: i64 = s.eval(&format!(
        "return ecs.update_attribute({e}, 'Stamina', 60, {{Stamina = 70, Stamina_Min = 0, Stamina_Max = 80}}).values.Stamina"
    ));
    assert_eq!(ok, 60);
    assert_eq!(
        s.reason(&format!(
            "ecs.update_attribute({e}, 'Stamina', 61, {{Stamina = 70}})"
        )),
        w::CHANGED
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 61, 5)")),
        "expected_values must be a table of original attribute names and values"
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 61, {{}})")),
        "expected_values must contain at least one attribute"
    );
    assert_eq!(
        s.reason(&format!(
            "ecs.update_attribute({e}, 'Stamina', 61, {{Revive_Delay = 1.5}})"
        )),
        "expected attribute belongs to a different root"
    );
    assert_eq!(
        s.reason(&format!(
            "ecs.update_attribute({e}, 'Stamina', 61, {{[1] = 60}})"
        )),
        "expected_values keys must be original attribute names"
    );
    assert_eq!(values(&s, 4), [60, 0, 80], "no refused update wrote");
}

#[test]
fn update_attribute_refuses_in_shroudforges_order() {
    let s = setup(world());
    let e = s.handle(4);
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina')")),
        "expected entity, attribute name/ID, and (for updates) scalar value plus optional expected_values"
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1.5)")),
        "expected finite scalar of type keen::sint32"
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 2^31)")),
        "expected finite scalar of type keen::sint32"
    );
    // the session write fault comes with the denial, before anything
    *s.host.fault.borrow_mut() = Some("ecs-write-outcome-uncertain-rollback-unverified".into());
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1)")),
        "ecs-write-outcome-uncertain-rollback-unverified"
    );
    *s.host.fault.borrow_mut() = None;
    s.facts(|f| f.can_write = false);
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1)")),
        "runtime.ecs.write unavailable"
    );
    assert_eq!(
        s.reason(&format!("ecs.write_attribute_storage({e}, 'Stamina', 1)")),
        "runtime.ecs.write unavailable"
    );
    s.facts(|f| f.can_write = true);
    // the profile's calculation model: none published, or a core without the row
    fake_attributes::with(|f| {
        f.model = Some(Err(
            "the profile publishes no attribute calculation model".into()
        ))
    });
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1)")),
        w::NOT_VALIDATED
    );
    fake_attributes::with(|f| f.model = None);
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1)")),
        w::NOT_VALIDATED
    );
    fake_attributes::with(|f| f.model = Some(Ok("attribute-command-v1".into())));
    // a component kiln.world may not write from a task (tracked by the game): its write-context text
    fake_services::set_write_policy(
        s.owner,
        fnv(STAMINA),
        Some((KILN_ECS_NOT_WRITABLE as u32, 5)),
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 1)")),
        cas::NO_WRITE_CONTEXT
    );
    fake_services::set_write_policy(s.owner, fnv(STAMINA), None);
    assert_eq!(values(&s, 4), [70, 0, 80], "nothing was written");
}

// The compare-exchange of an attribute write depends on the whole component: a frame's change of any of its bytes is
// ShroudForge's conflict at once, never retried over it.
#[test]
fn an_attribute_write_never_lands_over_another_writer() {
    let s = setup(world());
    let e = s.handle(4);
    s.world(|w| {
        w.cas = [CasStep::Foreign {
            entity: 4,
            component: fnv(STAMINA),
            offset: 28,
            bytes: 5u32.to_le_bytes().to_vec(),
        }]
        .into()
    });
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'Stamina', 50)")),
        cas::CONFLICT
    );
    assert_eq!(s.calls("ecs_compare_exchange"), 1, "one hold, no retry");
    assert_eq!(
        values(&s, 4),
        [70, 5, 80],
        "the other writer's change stays"
    );
}

// write_attribute_storage: the value set as given, the component's recalculation still run; `true` or `false, reason`.
#[test]
fn write_attribute_storage_writes_through_the_recalculation() {
    let s = setup(world());
    let e = s.handle(4);
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write_attribute_storage({e}, 'Stamina', 40)"
    ));
    assert_eq!((ok, why), (true, None));
    assert_eq!(values(&s, 4), [40, 0, 80]);
    let _: bool = s.eval(&format!(
        "return ecs.write_attribute_storage({e}, 'Stamina', 99)"
    ));
    assert_eq!(values(&s, 4), [80, 0, 80], "clamped by the recalculation");
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write_attribute_storage({e}, 'Stamina')"
    ));
    assert_eq!(
        (ok, why.as_deref()),
        (
            false,
            Some("expected entity, attribute name/ID, and (for writes) scalar value")
        )
    );
    // a descriptor whose storage is not writable on its component
    let mut list = descriptors();
    let mut float = stamina_layout();
    float["storage_scalar_type"] = json!("keen::float32");
    for d in list.iter_mut().take(3) {
        d["components"] = json!([float.clone()]);
    }
    let identity = identity();
    fake_attributes::with(|f| {
        f.catalog = Some(Ok(fake_attributes::catalog_reply(
            &identity,
            &list,
            &[float.clone(), revive_layout()],
        )))
    });
    let s = setup_reusing_catalog(world());
    let e = s.handle(4);
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write_attribute_storage({e}, 'Stamina', 1)"
    ));
    assert!(!ok);
    assert_eq!(
        why.as_deref(),
        Some(concat!(
            "K",
            "FC scalar type differs from reflected storage"
        ))
    );
}

/// A setup that keeps the catalog this thread's fake archive serves now.
fn setup_reusing_catalog(world: EcsWorld) -> AttrSetup {
    let catalog = fake_attributes::with(|f| f.catalog.clone());
    let s = setup(world);
    fake_attributes::with(|f| f.catalog = catalog);
    s
}

// The plain component writes recalculate an attribute component (ShroudForge's recalculate_component_update): the
// storage through the archive's recalculation, the header read-only.
#[test]
fn plain_field_writes_recalculate_attribute_components() {
    let s = setup(world());
    let e = s.handle(4);
    let (ok, why, confirmed): (bool, Option<String>, i64) = s.eval(&format!(
        "local ok, why, c = ecs.write_fields({e}, '{STAMINA}', {{['dataStorage[1]'] = 120}})
         return ok, why, c and c['dataStorage[1]']"
    ));
    assert_eq!(
        (ok, why, confirmed),
        (true, None, 80),
        "clamped by the recalculation and confirmed so"
    );
    assert_eq!(values(&s, 4), [80, 0, 80]);
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "return ecs.write_fields({e}, '{STAMINA}', {{rootId = 7}})"
    ));
    assert_eq!(
        (ok, why.as_deref()),
        (
            false,
            Some("attribute headers, definition identity and storage layout are read-only")
        )
    );
    fake_attributes::with(|f| f.model = None);
    assert_eq!(
        s.reason(&format!(
            "ecs.write_fields({e}, '{STAMINA}', {{['dataStorage[1]'] = 1}})"
        )),
        w::NOT_VALIDATED
    );
}

// runtime.overrides.attribute with ShroudForge's tracker: apply, restore when the mod stops, another mod refused while
// the claim is held, an external change preserved, and a world session change.
#[test]
fn the_attribute_override_applies_and_restores_when_the_mod_stops() {
    let s = setup(world());
    let e = s.handle(4);
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "tracker = runtime.overrides.new()
         tracker:begin_update()
         target = assert(runtime.overrides.attribute({e}, 'Stamina'))
         return tracker:apply('stamina', 'Stamina', target, function() return 60 end)"
    ));
    assert_eq!((ok, why), (true, None));
    assert_eq!(values(&s, 4), [60, 0, 80]);
    let key: String = s.eval("return target.key");
    assert_eq!(key, format!("attribute/{FAKE_SESSION}/{e}/Stamina"));
    // another mod: its own target's claim is refused while alpha holds the component, and so is its direct update
    let refused: String = s.eval(&format!(
        "local t = beta.overrides.new(); t:begin_update()
         local other = assert(beta.overrides.attribute({e}, 'Stamina'))
         local ok, why = t:apply('stamina', 'Stamina', other, function() return 10 end)
         return why"
    ));
    assert!(
        refused.contains("is owned by runtime mod 'alpha'"),
        "{refused}"
    );
    let direct = s.reason(&format!("beta.ecs.update_attribute({e}, 'Stamina', 10)"));
    assert!(
        direct.contains("is owned by runtime mod 'alpha'"),
        "{direct}"
    );
    assert_eq!(values(&s, 4), [60, 0, 80]);
    // the mod stops: its tracker restores the baseline and releases the claim
    assert_eq!(s.trackers.cleanup("alpha"), Ok(()));
    assert_eq!(values(&s, 4), [70, 0, 80]);
    assert!(s.facts.ownership().is_empty(), "the claim is released");
    let now: i64 = s.eval(&format!(
        "return beta.ecs.update_attribute({e}, 'Stamina', 10).values.Stamina"
    ));
    assert_eq!(now, 10, "beta may write once alpha let go");
}

#[test]
fn a_restore_keeps_an_external_change_and_ends_with_its_world() {
    let apply = |e: i64| {
        format!(
            "tracker = runtime.overrides.new()
             tracker:begin_update()
             local target = assert(runtime.overrides.attribute({e}, 'Stamina'))
             return tracker:apply('stamina', 'Stamina', target, function() return 60 end)"
        )
    };
    let s = setup(world());
    let (ok, _): (bool, Option<String>) = s.eval(&apply(s.handle(4)));
    assert!(ok);
    // another writer changes the value: the restore preserves it (ShroudForge's rule)
    s.world(|w| {
        w.entities
            .get_mut(&4)
            .unwrap()
            .components
            .insert(fnv(STAMINA), stamina([65, 0, 80], 1).1)
    });
    let writes = s.world(|w| w.writes.len());
    assert_eq!(s.trackers.cleanup("alpha"), Ok(()));
    assert_eq!(values(&s, 4), [65, 0, 80]);
    assert_eq!(s.world(|w| w.writes.len()), writes, "no restore write");

    // a new world session: the entry is dropped without a write into the new session
    let s = setup(world());
    let (ok, _): (bool, Option<String>) = s.eval(&apply(s.handle(4)));
    assert!(ok);
    s.session.set(FAKE_SESSION + 1);
    s.world(|w| w.session = FAKE_SESSION + 1);
    let writes = s.world(|w| w.writes.len());
    assert_eq!(s.trackers.cleanup("alpha"), Ok(()));
    assert_eq!(
        s.world(|w| w.writes.len()),
        writes,
        "nothing written into another session"
    );
    assert_eq!(values(&s, 4), [60, 0, 80]);
}

// The target's own refusals: a descriptor that cannot be resolved, an entity without the root, and normalize through
// the archive's recalculation.
#[test]
fn the_attribute_target_refuses_and_normalizes() {
    let s = setup(world());
    let e = s.handle(4);
    let six = s.handle(6);
    assert_eq!(
        s.reason("runtime.overrides.attribute(1, 'Missing')"),
        "attribute not found: Missing"
    );
    let root = stamina_root();
    assert_eq!(
        s.reason(&format!("runtime.overrides.attribute({six}, 'Stamina')")),
        format!(
            "no initialized component on this entity matches attribute root {root:#010x} (0 compatible component snapshots read)"
        )
    );
    let (normalized, read): (i64, i64) = s.eval(&format!(
        "local t = assert(runtime.overrides.attribute({e}, 'Stamina'))
         local snapshot = assert(t.read())
         return t.normalize(snapshot, 500), snapshot.value"
    ));
    assert_eq!((normalized, read), (80, 70));
}

// BodyHeat: Kiln's one admitted signedness conflict (catalog::ENGINE_SIGNED_ROOTS). The game's resource types the root
// keen::sint32, its component (a keen::ecs::UInt32Attribute) stores keen::uint32 and its programs are sign-dependent
// (Clamp, ScaleToNewMax), so ShroudForge refuses every calculation of it; the engine runs it on its signed interpreter
// only (docs/re/combat.md, "BodyHeat runs"). Kiln calculates exactly that root, as keen::sint32, with every word in the
// common range; the reverse pair, another root and a word outside the range stay refused.
const BODY_HEAT: &str = "keen::ecs::BodyHeat";
const BODY_HEAT_ROOT: u32 = 0xC7AE_AF1D;
/// The game's BodyHeat root in slot order (attribute resource 8f1fd455-a5bf-4184-a77c-7123955d6e04).
const BODY_HEAT_IDS: [(&str, u32); 8] = [
    ("BodyHeat", 0xC7AE_AF1D),
    ("BodyHeat_Max", 0x783E_C3B6),
    ("BodyHeat_Max_Adder", 0x3EAA_6DA6),
    ("BodyHeat_Max_Base", 0xA0DD_FAE8),
    ("BodyHeat_RechargeRampup", 0xC717_AFE7),
    ("BodyHeat_RechargeTimeout", 0x66B9_3DDF),
    ("BodyHeat_Recharge", 0xF8A7_A735),
    ("BodyHeat_Min", 0x227C_527E),
];
const SIGNEDNESS: &str =
    "signedness-dependent program with conflicting resource/storage scalar types";
const COMMON_RANGE: &str =
    "conflicting signedness: only the common integer range 0..2147483647 is validated";

/// The BodyHeat root as kiln-eml's archive describes it for today's game (no definition GUID: bound per entity), with
/// the game's programs: BodyHeat = Clamp(BodyHeat, BodyHeat_Min, BodyHeat_Max) and BodyHeat_Max =
/// ScaleToNewMax(LoadRef BodyHeat, Load0, BodyHeat_Max, BodyHeat_Max_Base + BodyHeat_Max_Adder). `root` moves it to
/// another root ID, `resource` is the resource's scalar type.
fn body_heat_descriptors(root: u32, resource: &str) -> Vec<Json> {
    BODY_HEAT_IDS
        .iter()
        .enumerate()
        .map(|(index, (name, id))| {
            let hash = if root == BODY_HEAT_ROOT {
                *id
            } else {
                root.wrapping_add(index as u32)
            };
            let mut d = descriptor(name, hash, root, index, resource, None);
            let words = match index {
                0 => json!([0x20000, 0x20007, 0x20001, 0xcffff]),
                1 => json!([
                    0x30000, 0x10000, 0x20001, 0x20003, 0x20002, 0x6ffff, 0xdffff
                ]),
                _ => json!([]),
            };
            d["structure"]["calculation"] = words.clone();
            d["calculation_words"] = words;
            d["calculation_sign_independent"] = json!(false);
            d["calculation_writable"] = json!(false);
            d["calculation_reason"] =
                json!("calculation storage type requires an initialized entity root match");
            d
        })
        .collect()
}

/// The fake archive's attribute_evaluate for an 8-word root: the two BodyHeat programs on the signed interpreter (the
/// ScaleToNewMax branch included, as the engine's); any other root as the fixture's.
fn body_heat_evaluate(root: u32, input: &[u32]) -> Result<(Vec<u32>, Vec<Json>), (i32, String)> {
    if input.len() != 8 {
        return recalculate(root, input);
    }
    let mut v: Vec<i32> = input.iter().map(|w| *w as i32).collect();
    let new_max = v[3].wrapping_add(v[2]);
    if new_max != v[1] && v[1] != 0 {
        v[0] = (v[0] as f32 / v[1] as f32 * new_max as f32) as i32;
    }
    v[1] = new_max;
    v[0] = v[0].clamp(v[7].min(v[1]), v[7].max(v[1]));
    let out: Vec<u32> = v.iter().map(|x| *x as u32).collect();
    let trace =
        vec![json!({"name":"BodyHeat","hash":root,"index":0,"before_bits":input,"after_bits":out})];
    Ok((out, trace))
}

/// A server VM whose entity 9 carries a BodyHeat component of `words` under `root`, with the catalog above
/// (`resource` scalar) and a reflected layout of `storage` storage.
fn body_heat_setup(words: [u32; 8], root: u32, resource: &str, storage: &str) -> AttrSetup {
    let (_, bytes) = component(root, STAMINA_GUID, 1, &words);
    let world = EcsWorld::server().with(9, &[(fnv(BODY_HEAT), bytes)]);
    let extra = vec![T {
        qualified: BODY_HEAT,
        primitive: "Struct",
        size: 24 + 32,
        inner: Some(index_of(ATTRIBUTE_BASE) as u64),
        count: 0,
        fields: Vec::new(),
        flags: 0,
    }];
    let s = setup_with(world, extra);
    let mut list = descriptors();
    list.extend(body_heat_descriptors(root, resource));
    let identity = identity();
    fake_attributes::with(|f| {
        f.catalog = Some(Ok(fake_attributes::catalog_reply(
            &identity,
            &list,
            &[
                stamina_layout(),
                revive_layout(),
                layout(BODY_HEAT, 8, storage, None),
            ],
        )));
        f.evaluate = Some(std::rc::Rc::new(body_heat_evaluate));
    });
    s
}

/// Entity 9's BodyHeat words as the world holds them.
fn body_heat(s: &AttrSetup) -> [u32; 8] {
    s.world(|w| {
        let bytes = &w.entities[&9].components[&fnv(BODY_HEAT)];
        std::array::from_fn(|i| {
            u32::from_le_bytes(bytes[24 + 4 * i..28 + 4 * i].try_into().unwrap())
        })
    })
}

// SF Survival's Infinite Frost Timer as it runs: its tracker sets BodyHeat_Min to BodyHeat_Max through the attribute
// target, the root's signed calculation clamps BodyHeat up to it, and the tracker restores BodyHeat_Min when the mod
// stops.
#[test]
fn body_heat_min_takes_its_maximum_through_the_attribute_target() {
    let s = body_heat_setup(
        [4000, 10000, 0, 10000, 0, 0, 0, 0],
        BODY_HEAT_ROOT,
        "keen::sint32",
        "keen::uint32",
    );
    let e = s.handle(9);
    let (ok, why): (bool, Option<String>) = s.eval(&format!(
        "tracker = runtime.overrides.new()
         tracker:begin_update()
         target = assert(runtime.overrides.attribute({e}, 'BodyHeat_Min'))
         return tracker:apply(target.key, 'infiniteFrostTimer', target, function(snapshot)
           return snapshot.values.BodyHeat_Max
         end)"
    ));
    assert_eq!((ok, why), (true, None));
    assert_eq!(body_heat(&s), [10000, 10000, 0, 10000, 0, 0, 0, 10000]);
    assert_eq!(s.trackers.cleanup("alpha"), Ok(()));
    assert_eq!(
        body_heat(&s),
        [10000, 10000, 0, 10000, 0, 0, 0, 0],
        "BodyHeat_Min restored; BodyHeat stays inside its bounds"
    );
    // the direct update answers the root as the storage's scalar, the calculation applied
    let (heat, min, applied): (i64, i64, bool) = s.eval(&format!(
        "local r = ecs.update_attribute({e}, 'BodyHeat_Min', 2500)
         return r.values.BodyHeat, r.values.BodyHeat_Min, r.applied"
    ));
    assert_eq!((heat, min, applied), (10000, 2500, true));
}

// The common range holds for every word of the root, before and after the calculation, not only the one written: a
// word the two readings disagree on refuses the write, and nothing is written.
#[test]
fn body_heat_writes_stay_in_the_common_range() {
    let s = body_heat_setup(
        [4000, 10000, 0x8000_0000, 10000, 0, 0, 0, 0],
        BODY_HEAT_ROOT,
        "keen::sint32",
        "keen::uint32",
    );
    let e = s.handle(9);
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'BodyHeat_Min', 10000)")),
        COMMON_RANGE
    );
    assert_eq!(
        s.reason(&format!("ecs.update_attribute({e}, 'BodyHeat_Min', -1)")),
        COMMON_RANGE
    );
    assert_eq!(body_heat(&s), [4000, 10000, 0x8000_0000, 10000, 0, 0, 0, 0]);
    // a calculation whose result leaves the range is refused too: Base + Adder wraps to a negative maximum
    let s = body_heat_setup(
        [0, 0, 0, 0x7FFF_FFFF, 0, 0, 0, 0],
        BODY_HEAT_ROOT,
        "keen::sint32",
        "keen::uint32",
    );
    let e = s.handle(9);
    assert_eq!(
        s.reason(&format!(
            "ecs.update_attribute({e}, 'BodyHeat_Max_Adder', 1)"
        )),
        COMMON_RANGE
    );
    assert_eq!(body_heat(&s), [0, 0, 0, 0x7FFF_FFFF, 0, 0, 0, 0]);
}

// Every other signedness conflict stays ShroudForge's refusal: the reverse pair on the BodyHeat root, the proven pair on
// any other root, and a float storage; nothing is written.
#[test]
fn other_signedness_conflicts_stay_refused() {
    let words = [4000, 10000, 0, 10000, 0, 0, 0, 0];
    for (root, resource, storage) in [
        (BODY_HEAT_ROOT, "keen::uint32", "keen::sint32"),
        (0x5EED_0100, "keen::sint32", "keen::uint32"),
        (
            BODY_HEAT_ROOT.wrapping_add(1),
            "keen::sint32",
            "keen::uint32",
        ),
        (BODY_HEAT_ROOT, "keen::sint32", "keen::float32"),
    ] {
        let s = body_heat_setup(words, root, resource, storage);
        let e = s.handle(9);
        assert_eq!(
            s.reason(&format!("ecs.update_attribute({e}, 'BodyHeat_Min', 10000)")),
            SIGNEDNESS,
            "{root:#010x} {resource} over {storage}"
        );
        if storage == "keen::float32" {
            continue;
        }
        // SF Survival's path: the target resolves, and its write answers the same refusal
        let why: String = s.eval(&format!(
            "local tracker = runtime.overrides.new()
             tracker:begin_update()
             local target = assert(runtime.overrides.attribute({e}, 'BodyHeat_Min'))
             local ok, why = tracker:apply(target.key, 'infiniteFrostTimer', target, function(snapshot)
               return snapshot.values.BodyHeat_Max
             end)
             assert(not ok)
             return why"
        ));
        assert!(why.contains(SIGNEDNESS), "{why}");
        assert_eq!(body_heat(&s), words, "nothing written");
    }
}

// The bound descriptor (catalog::Catalog::bind_storage): the admitted root is calculation-writable as the resource's
// keen::sint32 with the common-range domain; the same descriptors under the reverse pair or on another root are not.
#[test]
fn body_heat_binds_as_a_signed_calculation() {
    use kiln_sfi::runtime::ecs::attributes::catalog::{
        Catalog, ENGINE_SIGNED_ROOTS, engine_signed_root,
    };
    assert_eq!(ENGINE_SIGNED_ROOTS, [BODY_HEAT_ROOT]);
    let bind = |root: u32, resource: &str, storage: &str| {
        let list = body_heat_descriptors(root, resource);
        let entries = list
            .iter()
            .map(|d| (d["hash"].as_u64().unwrap() as u32, d.clone()))
            .collect();
        let component = layout(BODY_HEAT, 8, storage, None);
        let catalog = Catalog::new(entries, vec![component.clone()]);
        catalog.bind_storage(&list[7], &component).unwrap()
    };
    let bound = bind(BODY_HEAT_ROOT, "keen::sint32", "keen::uint32");
    assert_eq!(bound["calculation_writable"], true);
    assert_eq!(bound["calculation_reason"], Json::Null);
    assert_eq!(bound["calculation_scalar_type"], "keen::sint32");
    assert_eq!(bound["storage_writable"], true);
    assert_eq!(
        bound["write_value_domain"],
        "integer-common-range-0-to-2147483647"
    );
    for (root, resource, storage) in [
        (BODY_HEAT_ROOT, "keen::uint32", "keen::sint32"),
        (0x5EED_0100, "keen::sint32", "keen::uint32"),
        (BODY_HEAT_ROOT, "keen::sint32", "keen::float32"),
    ] {
        let bound = bind(root, resource, storage);
        assert_eq!(
            bound["calculation_writable"], false,
            "{root:#010x} {resource} over {storage}"
        );
        assert_eq!(bound["calculation_reason"], SIGNEDNESS);
        assert!(!engine_signed_root(root, Some(resource), Some(storage)));
    }
    // a sign-independent conflict keeps ShroudForge's storage-typed calculation
    let mut list = body_heat_descriptors(0x5EED_0100, "keen::uint32");
    for d in &mut list {
        d["calculation_sign_independent"] = json!(true);
    }
    let entries = list
        .iter()
        .map(|d| (d["hash"].as_u64().unwrap() as u32, d.clone()))
        .collect();
    let component = layout(BODY_HEAT, 8, "keen::sint32", None);
    let bound = Catalog::new(entries, vec![component.clone()])
        .bind_storage(&list[0], &component)
        .unwrap();
    assert_eq!(bound["calculation_writable"], true);
    assert_eq!(bound["calculation_scalar_type"], "keen::sint32");
}
