// SPDX-License-Identifier: MIT
//! The override tracker and its conditional restore. The Lua-level bodies are in
//! shared/runtime_tracker.rs ; here are a seeded stress run over many
//! worlds, foreign writers and busy gates, and the cleanup port through the real worker and
//! lifecycle host (orderly leave, fast leave, a world change, a stop during an active write).
//! Expected rules are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), identical in both
//! pins: modules/mod-runtime/lua/ingame/overrides/track_runtime_overrides.{lua,rs}.
#[path = "shared/runtime_tracker.rs"]
mod bodies;
#[path = "support/fake_services/mod.rs"]
mod fake_services;
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::Families;
use kiln_sfi::runtime::overrides::{OverrideTrackers, session_world_kind};
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::compose::GameTypes;
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use support::*;

#[test]
fn new_trackers_start_with_shroudforge_s_fields() {
    bodies::new_trackers_start_with_shroudforge_s_fields()
}
#[test]
fn fresh_no_op_and_reused_entries() {
    bodies::fresh_no_op_and_reused_entries()
}
#[test]
fn a_desired_callback_that_raises_or_returns_nil() {
    bodies::a_desired_callback_that_raises_or_returns_nil()
}
#[test]
fn an_external_write_blocks_and_a_toggle_rebinds() {
    bodies::an_external_write_blocks_and_a_toggle_rebinds()
}
#[test]
fn uncertain_outcomes_latch_and_refuse_before_touching() {
    bodies::uncertain_outcomes_latch_and_refuse_before_touching()
}
#[test]
fn restores_write_only_over_their_own_last_value() {
    bodies::restores_write_only_over_their_own_last_value()
}
#[test]
fn restore_variants_reset_and_summary_order() {
    bodies::restore_variants_reset_and_summary_order()
}
#[test]
fn one_tracker_per_key_within_a_mod_and_claims_are_required() {
    bodies::one_tracker_per_key_within_a_mod_and_claims_are_required()
}
#[test]
fn a_world_change_never_restores_into_the_new_session() {
    bodies::a_world_change_never_restores_into_the_new_session()
}
#[test]
fn cleanup_is_mod_scoped_retryable_and_answers_shroudforge_s_texts() {
    bodies::cleanup_is_mod_scoped_retryable_and_answers_shroudforge_s_texts()
}
#[test]
fn cleanup_without_a_vm_does_nothing() {
    bodies::cleanup_without_a_vm_does_nothing()
}

#[test]
fn the_session_world_is_the_plan_side_s_world() {
    assert_eq!(session_world_kind(Side::Server), KILN_WORLD_SERVER as u32);
    assert_eq!(session_world_kind(Side::Client), KILN_WORLD_CLIENT as u32);
}

/// A second VM cannot bind the same trackers (the cleanup runs in one VM).
#[test]
fn trackers_bind_one_vm() {
    let trackers = OverrideTrackers::new();
    let a = Lua::new();
    let b = Lua::new();
    let session: Rc<dyn Fn() -> u64> = Rc::new(|| 1);
    trackers
        .install(&a, &a.create_table().unwrap(), "m", session.clone())
        .unwrap();
    let refused = trackers
        .install(&b, &b.create_table().unwrap(), "m", session)
        .unwrap_err();
    assert!(
        refused.to_string().contains("another runtime VM"),
        "{refused}"
    );
}

/// Mod ids may contain dots, so one mod's id can be another's id plus a suffix: each mod keeps its
/// own factory and tracker list in either install order, a rebuilt environment still installs, and
/// each cleanup acts on its own mod only.
#[test]
fn dotted_mod_ids_keep_their_own_trackers() {
    for order in [["x", "x.factory"], ["x.factory", "x"]] {
        let lua = Lua::new();
        let trackers = OverrideTrackers::new();
        let session: Rc<dyn Fn() -> u64> = Rc::new(|| 1);
        for owner in order.iter().chain(order.iter()) {
            let table = lua.create_table().unwrap();
            trackers
                .install(&lua, &table, owner, session.clone())
                .unwrap_or_else(|e| panic!("{order:?} {owner}: {e}"));
            lua.globals().set(owner.replace('.', "_"), table).unwrap();
        }
        lua.load(
            r#"
            store = {x = 1, y = 1}
            local function target(name)
                return {
                    claim = {acquire = function() return true end, release = function() end},
                    read = function() return {value = store[name]} end,
                    write = function(_, v) store[name] = v return true, v end,
                }
            end
            local a = x.new() a:begin_update()
            assert(a:apply("x", "x", target("x"), function() return 2 end))
            local b = x_factory.new() b:begin_update()
            assert(b:apply("y", "y", target("y"), function() return 2 end))
            "#,
        )
        .exec()
        .unwrap();
        assert_eq!(trackers.cleanup("x"), Ok(()), "{order:?}");
        let values: (i64, i64) = lua.load("return store.x, store.y").eval().unwrap();
        assert_eq!(values, (1, 2), "{order:?}: only x's tracker restored");
        assert_eq!(trackers.cleanup("x.factory"), Ok(()), "{order:?}");
        let values: (i64, i64) = lua.load("return store.x, store.y").eval().unwrap();
        assert_eq!(values, (1, 1), "{order:?}");
    }
}

// ---- stress: restores never write over a foreign value or into another world ----

/// Random steps over 4 keys and 2 trackers of one mod: apply with random targets, foreign writes
/// (large jumps, or small values that can equal an applied one), busy gates, world changes (also to
/// 0, where the targets cannot read or write, as a real family's session-bound target cannot), and
/// every restore form. The targets' claims always acquire (a mod's own adapter), so the tracker's
/// own session rule is what is tested. A write made while restoring must land in the nonzero world
/// the entry was bound in (`bound`, recorded at the fresh bind) over the tracker's own last write
/// there (`last`), else it counts as a violation. Removing the session guard is found (seeds 8,
/// 35, 53, ...).
const STRESS: &str = r#"
local seed = ...
local state = seed
local function rand(n) state = (state * 1103515245 + 12345) % 2147483648 return state % n end
worlds, last, bound, violations, restoring, restored, busy = {}, {}, {}, 0, false, 0, false
local function world() worlds[current] = worlds[current] or {} return worlds[current] end
local keys = {"a", "b", "c", "d"}
local function target(name)
    return {
        shared = (name == "d") or nil,
        claim = {acquire = function() return true end, release = function() end},
        read = function()
            if current == 0 then return nil, "no world" end
            local v = world()[name] or 0
            return {value = v}
        end,
        write = function(snapshot, v)
            if busy then return false, nil, "Scene gate is busy" end
            if current == 0 then return false, nil, "no world" end
            local store = world()
            if (store[name] or 0) ~= snapshot.value then return false, nil, "stale" end
            if restoring then
                local home = bound[name]
                if current ~= 0 and home and home ~= 0 and home ~= current then violations = violations + 1 end
                local mine = last[current] and last[current][name]
                if mine ~= snapshot.value then violations = violations + 1 end
                restored = restored + 1
            end
            store[name] = v
            last[current] = last[current] or {}
            last[current][name] = v
            return true, v
        end,
    }
end
local trackers = {overrides.new(), overrides.new()}
for step = 1, 600 do
    local t = trackers[rand(2) + 1]
    local roll = rand(100)
    local k = keys[rand(4) + 1]
    if roll < 35 then
        t:begin_update()
        local _, _, fresh = t:apply(k, k, target(k), function(s, base) return base + rand(3) end)
        if fresh and t.owned[k] then bound[k] = current end
    elseif roll < 50 then
        local store = world()
        -- Large jumps, or small values that can equal a tracker's applied one (in another world too).
        if rand(2) == 0 then store[k] = (store[k] or 0) + 100 + rand(5) else store[k] = rand(3) end
    elseif roll < 58 then
        busy = not busy
    elseif roll < 64 then
        current = (rand(4) == 0) and 0 or (rand(3) + 1)
        set_session(current)
    elseif roll < 72 then
        restoring = true t:restore_all() restoring = false
    elseif roll < 80 then
        restoring = true t:restore_unused() restoring = false
    elseif roll < 88 then
        restoring = true
        t:restore_disabled(function(setting) return setting ~= k end)
        restoring = false
    elseif roll < 92 then
        t:reset()
    else
        t:begin_update()
    end
end
return violations, restored
"#;

#[test]
fn stress_restores_never_land_over_foreign_values_or_in_another_world() {
    let mut total = 0;
    for seed in 1..=100u64 {
        let lua = Lua::new();
        let trackers = OverrideTrackers::new();
        let live = Rc::new(std::cell::Cell::new(1u64));
        let table = lua.create_table().unwrap();
        let session = live.clone();
        trackers
            .install(&lua, &table, "m", Rc::new(move || session.get()))
            .unwrap();
        lua.globals().set("overrides", table).unwrap();
        lua.globals().set("current", 1).unwrap();
        let setter = live.clone();
        lua.globals()
            .set(
                "set_session",
                lua.create_function(move |_, s: u64| {
                    setter.set(s);
                    Ok(())
                })
                .unwrap(),
            )
            .unwrap();
        let (violations, restored): (i64, i64) = lua
            .load(STRESS)
            .call(seed)
            .unwrap_or_else(|e| panic!("seed {seed}: {e}"));
        assert_eq!(violations, 0, "seed {seed}");
        total += restored;
        let _ = trackers.cleanup("m");
    }
    assert!(total > 100, "the stress restored only {total} values");
}

// ---- the cleanup port through the real worker and lifecycle host ----

const WAIT: Duration = Duration::from_secs(300);
const SERVER: u32 = KILN_WORLD_SERVER as u32;

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, &'static str>>> =
    LazyLock::new(Default::default);

fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

/// One package: on_load binds \`hp\` (1 -> 2) through \`runtime.overrides.new\`; \`spin\` mode then
/// starts a second write from on_update that changes the value and never returns.
const PACKAGE: &str = r#"
store = {hp = 1}
local tracker
local function target()
    return {
        claim = {acquire = function() return true end, release = function() end},
        read = function() return {value = store.hp} end,
        write = function(snapshot, v)
            if mode == "spin" and v == 3 then
                store.hp = 3
                record("spin")
                while true do end
            end
            store.hp = v
            record("write " .. v)
            return true, v
        end,
    }
end
return {
    on_load = function()
        tracker = runtime.overrides.new()
        tracker:begin_update()
        local ok, why = tracker:apply("hp", "health", target(), function() return 2 end)
        record("applied " .. tostring(ok) .. " " .. tostring(why) .. " " .. tostring(tracker.session))
    end,
    on_update = function()
        if mode == "spin" then
            tracker:begin_update()
            tracker:apply("hp", "health", target(), function() return 3 end)
        end
    end,
    on_unload = function() record("unload") end,
}
"#;

struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, _: &RuntimePackage) -> mlua::Result<Table> {
        lua.create_table()
    }
}
struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path != "src/mod.lua" {
            return Err(std::io::ErrorKind::NotFound.into());
        }
        Ok(PACKAGE.as_bytes().to_vec())
    }
}

thread_local! {
    static TRACKERS: std::cell::RefCell<Option<Rc<OverrideTrackers>>> = const { std::cell::RefCell::new(None) };
}

/// Like the production plan source: the trackers are the plan's cleanup and the family.
struct TrackerPlan;
impl PlanSource for TrackerPlan {
    fn plan(_: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let source: Rc<dyn PackageSource> = Rc::new(Source);
        let mut parsed = package::Package::parse(
            json!({"id": "alpha", "name": "alpha", "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        let mut registry = Registry::default();
        registry
            .insert(
                PackageRecord::new(
                    "alpha".into(),
                    parsed,
                    RuntimePackage::new("alpha".to_owned(), source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
        let trackers = OverrideTrackers::new();
        TRACKERS.with_borrow_mut(|slot| *slot = Some(trackers.clone()));
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry,
            plan: vec!["alpha".into()],
            admitted: ["alpha".to_owned()].into(),
            cleanup: trackers,
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: kiln_sfi::worker::compose::ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        let mode = SCENARIOS.lock().unwrap().get(&owner).copied().unwrap_or("");
        let trackers = TRACKERS
            .with_borrow_mut(Option::take)
            .expect("plan made the trackers");
        Families::new(vec![
            trackers.family(session_world_kind(Side::Server)),
            kiln_sfi::runtime::Family::new("record", &[], move |lua, env, _, _, _| {
                // The worker thread's owner-less fake members (game_world_session) answer for us.
                fake_services::bind_thread(owner);
                env.raw_set("mode", mode)?;
                env.raw_set(
                    "record",
                    lua.create_function(move |_, text: String| {
                        RECORDS.lock().unwrap().entry(owner).or_default().push(text);
                        Ok(())
                    })?,
                )
            }),
        ])
    }
}

struct Run {
    owner: KilnOwner,
    pool: std::sync::Arc<FakePool>,
    shared: std::sync::Arc<WorkerShared>,
}

fn start(mode: &'static str) -> Run {
    let owner = next_owner();
    SCENARIOS.lock().unwrap().insert(owner, mode);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<TrackerPlan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let run = Run {
        owner,
        pool,
        shared,
    };
    wait_for(&run, "applied true nil 1");
    run
}

fn wait_for(run: &Run, text: &str) {
    assert!(
        eventually(WAIT, || records(run.owner).iter().any(|r| r == text)),
        "never saw {text}: {:?} / {:?}",
        records(run.owner),
        run.shared.status()
    );
}

fn stop(run: &Run) {
    run.shared.request_stop();
    assert!(
        run.pool.task(0).wait_finished(WAIT),
        "{:?}",
        run.shared.status()
    );
}

/// The orderly leave runs on_unload, then the tracker's cleanup puts the baseline back.
#[test]
fn the_orderly_leave_restores_through_the_target() {
    let run = start("");
    stop(&run);
    assert_eq!(
        records(run.owner),
        ["write 2", "applied true nil 1", "unload", "write 1"]
    );
}

/// The fast leave runs no mod code and restores nothing.
#[test]
fn the_fast_leave_restores_nothing() {
    let run = start("");
    run.pool.cancel(run.shared.task());
    assert!(run.pool.task(0).wait_finished(WAIT));
    assert_eq!(records(run.owner), ["write 2", "applied true nil 1"]);
}

/// A world change before the leave: the cleanup's begin_update drops the old world's
/// (non-shared) ownership and writes nothing into the new session.
#[test]
fn a_leave_after_a_world_change_writes_nothing_into_the_new_session() {
    let run = start("");
    fake_services::set_world_session(run.owner, SERVER, 2);
    stop(&run);
    assert_eq!(
        records(run.owner),
        ["write 2", "applied true nil 1", "unload"]
    );
}

/// A stop that interrupts a write in flight (it changed the value, then never returned): the
/// leave never writes the baseline over that unconfirmed value.
#[test]
fn a_stop_during_an_active_write_never_restores_over_it() {
    let run = start("spin");
    wait_for(&run, "spin");
    stop(&run);
    let seen = records(run.owner);
    assert!(seen.contains(&"unload".to_owned()), "{seen:?}");
    assert!(!seen.contains(&"write 1".to_owned()), "{seen:?}");
}
