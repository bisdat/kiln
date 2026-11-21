// SPDX-License-Identifier: MIT
// Shared lifecycle bodies: run by kiln-sfi's own tests (tests/lifecycle.rs) and by any
// other test package that includes them.
//! The lifecycle host on the worker and fake pool, with sfi-core and
//! a real Lua VM: interruption on stop, the 10 s watchdog, orderly
//! vs fast leave, the exit order, quarantine and the interpreter
//! stop with its tick-side drain.
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::worker::*;
use serde_json::json;
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::{Cleanup, Monotonic};
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::cell::Cell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use std::sync::{Arc, LazyLock, Mutex};
use std::time::{Duration, Instant};
use support::*;

// ---- scenario plumbing (keyed by owner; the plan runs on the worker thread) ----

#[derive(Clone, Default)]
struct Scenario {
    packages: Vec<(String, String)>,
    fake_clock: bool,
}
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, Scenario>>> = LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
static EXITING: LazyLock<Mutex<HashSet<KilnOwner>>> = LazyLock::new(Default::default);

fn note(owner: KilnOwner, text: String) {
    RECORDS.lock().unwrap().entry(owner).or_default().push(text);
}
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
fn has(owner: KilnOwner, text: &str) -> bool {
    records(owner).iter().any(|r| r == text)
}
fn count(owner: KilnOwner, text: &str) -> usize {
    records(owner).iter().filter(|r| *r == text).count()
}

/// Real monotonic time plus the jumps a package asks for with `advance(ms)`, so the
/// scheduler keeps turning while a test makes one call look long.
struct FakeClock {
    origin: Instant,
    jumps: Cell<Duration>,
}
impl FakeClock {
    fn advance(&self, ms: u64) {
        self.jumps.set(self.jumps.get() + Duration::from_millis(ms));
    }
}
impl Monotonic for FakeClock {
    fn now(&self) -> Duration {
        Duration::from_secs(1) + self.origin.elapsed() + self.jumps.get()
    }
}
struct Source(String);
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(self.0.as_bytes().to_vec())
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct NoCleanup;
impl Cleanup for NoCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}

fn registry(packages: &[(String, String)]) -> Registry {
    let mut registry = Registry::default();
    for (id, text) in packages {
        let source: Rc<dyn PackageSource> = Rc::new(Source(text.clone()));
        let mut parsed = package::Package::parse(
            json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        // `enabled` comes from the extension document (extended.mod.json).
        parsed.manifest.enabled = true;
        registry
            .insert(
                PackageRecord::new(
                    id.clone(),
                    parsed,
                    RuntimePackage::new(id.clone(), source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
    }
    registry
}

struct TestPlan;
impl PlanSource for TestPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        let scenario = SCENARIOS.lock().unwrap()[&owner].clone();
        let fake = Rc::new(FakeClock {
            origin: Instant::now(),
            jumps: Cell::new(Duration::ZERO),
        });
        let clock: Rc<dyn Monotonic> = if scenario.fake_clock {
            fake.clone()
        } else {
            process_clock()
        };
        let ids: Vec<String> = scenario.packages.iter().map(|(id, _)| id.clone()).collect();
        let register: Register = Box::new(move |lua, env, package, ports| {
            let id = package.id().to_owned();
            let advance = |clock: &FakeClock, ms: u64| clock.advance(ms);
            env.set(
                "record",
                lua.create_function(move |_, text: String| {
                    note(owner, format!("{id}:{text}"));
                    Ok(())
                })?,
            )?;
            let (f, w) = (fake.clone(), ports.watchdog.clone());
            env.set(
                "advance",
                lua.create_function(move |_, ms: u64| {
                    let _ = &w;
                    advance(&f, ms);
                    Ok(())
                })?,
            )?;
            // A native call that spent `ms` in ShroudForge's own bounded waits and
            // reports it (source_wait_ns), timed by the guard and subtracted after it
            // returns.
            let (f, w) = (fake.clone(), ports.watchdog.clone());
            env.set(
                "native_wait",
                lua.create_function(move |_, ms: u64| {
                    let mut call = w.native_call();
                    call.call(|| advance(&f, ms));
                    call.exclude(Duration::from_millis(ms));
                    Ok(())
                })?,
            )?;
            // A synchronous wait for a world thread (a hop).
            let (f, w) = (fake.clone(), ports.watchdog.clone());
            env.set(
                "hop_wait",
                lua.create_function(move |_, ms: u64| {
                    let _pause = w.pause();
                    advance(&f, ms);
                    Ok(())
                })?,
            )?;
            // An ordinary engine call that simply takes `ms`: counts as Lua time.
            let f = fake.clone();
            env.set(
                "slow_call",
                lua.create_function(move |_, ms: u64| {
                    advance(&f, ms);
                    Ok(())
                })?,
            )?;
            // A G-W write: refused kiln-gate-closed once the process exit began.
            env.set(
                "gate_write",
                lua.create_function(move |_, ()| Ok(!EXITING.lock().unwrap().contains(&owner)))?,
            )?;
            // A one-shot switch the test flips (GO) and the package polls.
            env.set(
                "flag",
                lua.create_function(move |_, name: String| {
                    Ok(GO.lock().unwrap().contains(&(owner, name)))
                })?,
            )?;
            // Calls a Lua function and records whether its error is the worker's
            // stop, as found through mlua's error chain.
            env.set(
                "classify",
                lua.create_function(move |_, f: mlua::Function| {
                    let result = f.call::<()>(());
                    if let Err(error) = &result {
                        note(owner, format!("is_stop:{}", is_stop(error)));
                    }
                    result
                })?,
            )?;
            // Blocks the worker (outside Lua) until the test flips `name`.
            env.set(
                "hold_until",
                lua.create_function(move |_, name: String| {
                    note(owner, format!("holding:{name}"));
                    while !GO.lock().unwrap().contains(&(owner, name.clone())) {
                        std::thread::sleep(Duration::from_millis(1));
                    }
                    Ok(())
                })?,
            )?;
            env.set(
                "explode",
                lua.create_function(|_, ()| -> mlua::Result<()> {
                    // An unwind that skips the panic hook, so no backtrace is captured
                    // and symbolized before the worker can leave (see WAIT).
                    std::panic::resume_unwind(Box::new("lifecycle test: a Rust binding panics"))
                })?,
            )?;
            Ok(())
        });
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry: registry(&scenario.packages),
            plan: ids.clone(),
            admitted: ids.into_iter().collect(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock,
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: Some(register),
            compose: Default::default(),
        })
    }
}

struct Run {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    worker: &'static Worker,
    shared: Arc<WorkerShared>,
}
impl Run {
    fn finished(&self) -> bool {
        self.pool.task(0).wait_finished(WAIT)
    }
    fn errors(&self) -> Vec<String> {
        logs(self.owner)
            .into_iter()
            .filter(|(level, _)| *level == KILN_LOG_ERROR)
            .map(|(_, t)| t)
            .collect()
    }
    fn warnings(&self) -> Vec<String> {
        logs(self.owner)
            .into_iter()
            .filter(|(level, _)| *level == KILN_LOG_WARN)
            .map(|(_, t)| t)
            .collect()
    }
}
/// A liveness bound, not a latency claim: how long a wait for something that must happen
/// holds before the body reports a hang. Timing claims are asserted on measured values or
/// with their own short negative waits. A passing wait returns as soon as its condition
/// holds, so this sits far above any delay scheduling alone can cause at nice 19.
const WAIT: Duration = Duration::from_secs(300);

fn start(packages: &[(&str, &str)], fake_clock: bool, config: PoolConfig) -> Run {
    let owner = next_owner();
    SCENARIOS.lock().unwrap().insert(
        owner,
        Scenario {
            packages: packages
                .iter()
                .map(|(id, text)| (id.to_string(), text.to_string()))
                .collect(),
            fake_clock,
        },
    );
    let pool = FakePool::new(owner, config);
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<TestPlan>)));
    let api = api(true);
    assert!(worker.bind(unsafe { Host::read(&*api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    Run {
        owner,
        pool,
        worker,
        shared,
    }
}
fn wait_for(run: &Run, text: &str) {
    assert!(
        eventually(WAIT, || has(run.owner, text)),
        "never saw {text}: {:?} / {:?}",
        records(run.owner),
        run.shared.status()
    );
}

// ---- interruption on stop ----

pub fn a_busy_lua_loop_is_interrupted_on_stop_within_one_hook_interval() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() record('loop') while true do end end, \
                      on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:loop");
    let asked = Instant::now();
    run.shared.request_stop();
    // Read right after the request (reading before it counts every check a
    // descheduled test thread misses): only a check in flight and the raising one
    // may follow.
    let (before, _) = run.shared.hook_counts();
    assert!(run.finished());
    let left = asked.elapsed();
    let (after, raises) = run.shared.hook_counts();
    assert_eq!(
        raises, 1,
        "the first check after the stop raised, and only it"
    );
    assert!(
        after - before <= 2,
        "{} hook checks after the stop: more than one interval",
        after - before
    );
    assert!(left < Duration::from_millis(100), "left after {left:?}");
    let status = run.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::StopRequested);
    assert_eq!(status.mods_failed, 0, "the stop is not a mod failure");
    assert!(
        has(run.owner, "a:unload"),
        "the orderly leave still runs on_unload"
    );
    eprintln!(
        "stop -> worker left in {left:?}, {} hook checks",
        after - before
    );
}

pub fn a_pcall_loop_is_re_interrupted() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 record('in') \
                 local ok = pcall(function() while true do end end) \
                 record('caught:' .. tostring(ok)) \
                 while true do end \
               end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    run.shared.request_stop();
    assert!(run.finished(), "the second loop was interrupted too");
    assert!(has(run.owner, "a:caught:false"), "{:?}", records(run.owner));
    assert_eq!(run.shared.status().reason, StopReason::StopRequested);
}

pub fn a_coroutine_cannot_evade_the_hook() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 record('in') \
                 coroutine.wrap(function() while true do end end)() \
               end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    run.shared.request_stop();
    assert!(run.finished());
}

pub fn a_stop_during_runtime_create_interrupts_the_top_level() {
    let run = start(
        &[("a", "record('top') while true do end")],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:top");
    run.shared.request_stop();
    assert!(run.finished());
    assert!(
        !logs(run.owner)
            .iter()
            .any(|(_, t)| t.contains("Runtime initialized")),
        "an interrupted create is not reported initialized"
    );
    assert_eq!(run.shared.status().reason, StopReason::StopRequested);
}

// ---- the 10 s watchdog (fake clock) ----

/// `b` keeps running (each update moves the clock 100 ms) while `a` is judged.
fn watchdog_run(a: &str) -> Run {
    start(
        &[
            ("a", a),
            (
                "b",
                "return { on_update = function() record('b') advance(100) end }",
            ),
        ],
        true,
        PoolConfig::default(),
    )
}
fn stopped_by_watchdog(run: &Run) {
    assert!(
        eventually(WAIT, || has(run.owner, "a:unload")),
        "a was not stopped: {:?}",
        records(run.owner)
    );
    let seen = count(run.owner, "b:b");
    assert!(
        eventually(WAIT, || count(run.owner, "b:b") > seen + 2),
        "b kept running"
    );
    let status = run.shared.status();
    assert_eq!(
        status.phase,
        WorkerPhase::Running,
        "the worker keeps running"
    );
    assert_eq!((status.mods_active, status.mods_failed), (1, 1));
    let warnings = run.warnings();
    assert!(
        warnings
            .iter()
            .any(|w| w.contains("mod 'a' stopped") && w.contains("watchdog")),
        "{warnings:?}"
    );
    run.shared.request_stop();
    assert!(run.finished());
}

pub fn a_call_over_10_s_of_lua_time_stops_that_mod_only() {
    let run = watchdog_run(
        "return { on_update = function() advance(10001) while true do end end, \
                  on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
}

/// The exact boundary (10 s allowed, 10.001 s not) is the watchdog's unit test; here
/// the clock also runs in real time. The first call jumps 9 s, so it stays under
/// 10 s unless its slot is held up for a whole second, and is allowed: the mod is
/// called again. That second call jumps the full 10 s; the real time its loop also
/// takes puts it over, and the watchdog stops it inside the loop (a check at least
/// every 10,000 instructions), so it never records 'over'.
pub fn a_call_just_under_10_s_of_lua_time_is_allowed() {
    let run = watchdog_run(
        "return { on_update = function() \
             if not survived then \
               advance(9000) for i = 1, 50000 do end record('survived') survived = true return \
             end \
             record('again') advance(10000) for i = 1, 50000 do end record('over') \
           end, on_unload = function() record('unload') end }",
    );
    wait_for(&run, "a:survived");
    // stopped_by_watchdog: the second call is stopped, b keeps running, 1 active, 1 failed.
    stopped_by_watchdog(&run);
    let a: Vec<String> = records(run.owner)
        .into_iter()
        .filter(|r| r.starts_with("a:"))
        .collect();
    assert_eq!(
        a,
        ["a:survived", "a:again", "a:unload"],
        "the call under 10 s was allowed and the mod called again; the 10 s call was not"
    );
}

pub fn a_caught_watchdog_error_is_raised_again() {
    let run = watchdog_run(
        "return { on_update = function() \
             advance(10001) \
             pcall(function() while true do end end) \
             record('after') \
             while true do end \
           end, on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
    assert!(
        has(run.owner, "a:after"),
        "pcall caught it once, then it came back"
    );
}

pub fn a_coroutine_does_not_hide_a_call_from_the_watchdog() {
    let run = watchdog_run(
        "return { on_update = function() \
             advance(10001) \
             coroutine.wrap(function() while true do end end)() \
           end, on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
}

pub fn native_waits_and_hop_waits_do_not_count_but_ordinary_calls_do() {
    // 4 s Lua + 30 s source wait + 5 s Lua + 30 s hop wait + 0.9 s Lua = 9.9 s
    // (plus the few real ms of the loop).
    let run = watchdog_run(
        "return { on_update = function() \
             if done then return end \
             advance(4000) native_wait(30000) advance(5000) hop_wait(30000) advance(900) \
             for i = 1, 50000 do end record('ok') done = true \
           end, on_unload = function() record('unload') end }",
    );
    wait_for(&run, "a:ok");
    assert!(!has(run.owner, "a:unload"));
    run.shared.request_stop();
    assert!(run.finished());

    let run = watchdog_run(
        "return { on_update = function() slow_call(11000) for i = 1, 50000 do end record('never') end, \
                  on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
    assert!(!has(run.owner, "a:never"));
}

pub fn each_call_has_its_own_account() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() advance(6000) record('u') end, \
                      on_unload = function() record('unload') end }",
        )],
        true,
        PoolConfig::default(),
    );
    assert!(eventually(WAIT, || count(run.owner, "a:u") >= 4));
    assert!(
        !has(run.owner, "a:unload"),
        "24 s over 4 calls is not one 10 s call"
    );
    run.shared.request_stop();
    assert!(run.finished());
}

pub fn a_module_top_level_over_10_s_fails_that_module_and_create_goes_on() {
    let run = start(
        &[
            ("a", "advance(10001) while true do end"),
            ("b", "return { on_load = function() record('loaded') end }"),
        ],
        true,
        PoolConfig::default(),
    );
    wait_for(&run, "b:loaded");
    assert!(eventually(WAIT, || run.shared.phase() == WorkerPhase::Running));
    assert!(eventually(WAIT, || run.shared.status().mods_failed == 1));
    run.shared.request_stop();
    assert!(run.finished());
}

pub fn a_create_longer_than_15_s_logs_sf_s_line_once() {
    // Two modules of 8 s each: neither trips the 10 s watchdog, create takes 16 s.
    let run = start(
        &[
            ("a", "advance(8000) for i = 1, 20000 do end return {}"),
            ("b", "advance(8000) for i = 1, 20000 do end return {}"),
        ],
        true,
        PoolConfig::default(),
    );
    assert!(eventually(WAIT, || run.shared.phase() == WorkerPhase::Running));
    let lines: Vec<_> = run
        .errors()
        .into_iter()
        .filter(|t| t.contains("has not returned after 15 seconds at runtime-create"))
        .collect();
    assert_eq!(lines.len(), 1, "{:?}", run.errors());
    assert_eq!(run.shared.status().mods_failed, 0);
    run.shared.request_stop();
    assert!(run.finished());

    // 14 s in all, each module under the watchdog's 10 s: no line.
    let quick = start(
        &[
            ("a", "advance(7000) for i = 1, 20000 do end return {}"),
            ("b", "advance(7000) for i = 1, 20000 do end return {}"),
        ],
        true,
        PoolConfig::default(),
    );
    assert!(eventually(WAIT, || quick.shared.phase() == WorkerPhase::Running));
    assert!(quick.errors().is_empty(), "{:?}", quick.errors());
    quick.shared.request_stop();
    assert!(quick.finished());
}

// ---- orderly vs fast leave ----

const GC_PROBE: &str = "keep = setmetatable({}, { __gc = function() record('gc') end }) \
    return { on_update = function() record('u') end, \
             on_unload = function() record('unload') end }";

pub fn an_orderly_stop_runs_sf_s_stop_and_closes_the_state() {
    let run = start(&[("a", GC_PROBE)], false, PoolConfig::default());
    wait_for(&run, "a:u");
    run.shared.request_stop();
    assert!(run.finished());
    let records = records(run.owner);
    let unload = records
        .iter()
        .position(|r| r == "a:unload")
        .expect("on_unload ran");
    let gc = records
        .iter()
        .position(|r| r == "a:gc")
        .expect("the VM closed");
    assert!(
        unload < gc,
        "SF's stop before the state closes: {records:?}"
    );
    assert!(
        logs(run.owner)
            .iter()
            .any(|(_, t)| t == "kiln-sfi: Runtime stopped")
    );
}

pub fn a_fast_stop_runs_no_mod_code() {
    let run = start(&[("a", GC_PROBE)], false, PoolConfig::default());
    wait_for(&run, "a:u");
    run.pool.cancel(run.shared.task());
    assert!(run.finished());
    assert!(!has(run.owner, "a:unload"), "{:?}", records(run.owner));
    assert!(!has(run.owner, "a:gc"), "the __gc probe stays silent");
    assert_eq!(run.shared.status().reason, StopReason::Cancelled);
}

// ---- exit order ----

pub fn at_exit_the_worker_returns_within_100_ms_even_inside_lua() {
    let run = start(
        &[(
            "a",
            "keep = setmetatable({}, { __gc = function() record('gc') end }) \
             return { on_update = function() \
                 record('in') \
                 while true do \
                   if not gate_write() and not noted then record('refused') noted = true end \
                 end \
               end, on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    // Step 3 closes the gate; writes refuse kiln-gate-closed before the park.
    EXITING.lock().unwrap().insert(run.owner);
    wait_for(&run, "a:refused");
    let outcome = run.pool.exit(Duration::from_millis(500));
    assert!(outcome.returned, "the park did not see the body return");
    assert!(
        outcome.waited < Duration::from_millis(100),
        "returned after {:?}",
        outcome.waited
    );
    assert!(
        !has(run.owner, "a:unload") && !has(run.owner, "a:gc"),
        "fast: no mod code"
    );
    assert_eq!(
        run.pool
            .task(0)
            .done_calls
            .load(std::sync::atomic::Ordering::SeqCst),
        0
    );
    assert_eq!(run.shared.status().reason, StopReason::Cancelled);
    eprintln!("exit park waited {:?}", outcome.waited);
}

// ---- quarantine, interpreter stop, drain ----

pub fn a_quarantine_sets_faulted_and_keeps_the_subscriptions_alive() {
    let unsubscribed = unsubscribes();
    let run = start(
        &[("a", "return { on_update = function() record('u') end }")],
        false,
        PoolConfig {
            fault_done: true,
            ..PoolConfig::default()
        },
    );
    wait_for(&run, "a:u");
    assert_eq!(subscriptions(run.owner), (1, 1));
    run.shared.request_stop();
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.phase, WorkerPhase::Faulted);
    assert_eq!(status.reason, StopReason::NativeFault);
    assert!(status.line().contains("native fault"));
    // Both subscriptions still run; neither drains nor restarts anything.
    for _ in 0..3 {
        fire_tick(run.owner);
    }
    assert!(run.pool.drains().is_empty(), "no drain after a quarantine");
    let refused = run.shared.status().inbox.refused[CommandClass::WorldLifecycle as usize];
    fire_world(run.owner, KILN_WORLD_STARTED, KILN_WORLD_SERVER, 1);
    assert_eq!(
        run.shared.status().inbox.refused[CommandClass::WorldLifecycle as usize],
        refused + 1,
        "the world subscriber still ran (and was refused: the worker is gone)"
    );
    assert_eq!(subscriptions(run.owner), (1, 1));
    assert_eq!(unsubscribes(), unsubscribed, "kiln-sfi never unsubscribes");
    assert_eq!(run.pool.submissions().len(), 1);
}

pub fn an_interpreter_stop_leaves_in_order_then_drains_the_owner_from_the_tick() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 if flag('go') then explode() end record('u') \
               end, on_unload = function() record('unload') end, \
               update_interval_ms = 8 }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:u");
    fire_tick(run.owner);
    assert!(
        run.pool.drains().is_empty(),
        "no drain while the worker runs"
    );
    // The world subscriber feeds the inbox while running.
    fire_world(run.owner, KILN_WORLD_STARTED, KILN_WORLD_SERVER, 1);
    assert!(eventually(WAIT, || {
        run.shared.status().inbox.delivered[CommandClass::WorldLifecycle as usize] == 1
    }));
    // Make the next update panic inside a Rust binding: SF's update failure.
    flip(&run, "go");
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::StopRequested);
    assert_eq!(status.detail.as_deref(), Some(UPDATE_FAILED));
    assert!(
        has(run.owner, "a:unload"),
        "an interpreter stop is an orderly leave"
    );
    assert!(run.errors().iter().any(|e| e.contains(UPDATE_FAILED)));
    // Ticks: one sealing drain, then polls until quiet, all with timeout 0.
    fire_tick(run.owner);
    assert!(run.shared.status().drained);
    assert!(run.shared.quiescent(), "body and done had returned");
    fire_tick(run.owner);
    fire_tick(run.owner);
    assert_eq!(
        run.pool.drains(),
        vec![0],
        "no timed drain, no poll after quiescence"
    );
    // No restart in this process.
    assert_eq!(run.worker.start(), Err(StartError::AlreadyStarted));
    assert_eq!(run.pool.submissions().len(), 1);
}

fn flip(run: &Run, name: &str) {
    GO.lock().unwrap().insert((run.owner, name.to_owned()));
}
static GO: LazyLock<Mutex<HashSet<(KilnOwner, String)>>> = LazyLock::new(Default::default);

// ---- review requirements: the lifecycle host ----

pub fn a_hook_stop_is_found_through_mlua_s_error_chain_and_is_no_mod_failure() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 record('in') \
                 classify(function() while true do end end) \
               end, on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    run.shared.request_stop();
    assert!(run.finished());
    assert!(has(run.owner, "is_stop:true"), "{:?}", records(run.owner));
    assert_eq!(run.shared.status().mods_failed, 0);
    assert!(has(run.owner, "a:unload"));
}

pub fn pushes_are_refused_from_the_stop_request_and_the_stopping_phase_on() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() record('u') end, \
                      on_unload = function() hold_until('release') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:u");
    let input = || WorkerCommand::InputSnapshot(InputSnapshot { payload: vec![1] });
    assert!(run.shared.submit(input()).is_ok(), "accepted while running");
    run.shared.request_stop();
    assert_eq!(
        run.shared.submit(input()),
        Err(Refused::Stopped),
        "after the request"
    );
    wait_for(&run, "holding:release");
    assert_eq!(run.shared.phase(), WorkerPhase::Stopping);
    assert_eq!(
        run.shared.submit(input()),
        Err(Refused::Stopped),
        "while Stopping"
    );
    flip(&run, "release");
    assert!(run.finished());
    assert_eq!(
        run.shared.submit(input()),
        Err(Refused::Stopped),
        "after Stopped"
    );
}

pub fn a_stopping_for_a_session_never_seen_starting_is_accepted() {
    let mut worlds = WorldSessions::default();
    let event = |session, event| WorldLifecycle {
        world: KILN_WORLD_SERVER as u32,
        session,
        event,
    };
    worlds.apply(event(3, LifecycleEvent::Stopping));
    assert_eq!(
        worlds.get(0),
        WorldSession {
            live: None,
            ended: 3
        }
    );
    worlds.apply(event(2, LifecycleEvent::Started));
    assert_eq!(
        worlds.get(0).live,
        None,
        "a STARTED of an ended session is stale"
    );
    worlds.apply(event(4, LifecycleEvent::Started));
    worlds.apply(event(6, LifecycleEvent::Stopping));
    assert_eq!(
        worlds.get(0),
        WorldSession {
            live: None,
            ended: 6
        },
        "ends older ones too"
    );

    // Through the worker: session 2 starts and stops between two drains.
    let run = start(
        &[("a", "return { on_update = function() record('u') end }")],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:u");
    fire_world(run.owner, KILN_WORLD_STARTED, KILN_WORLD_SERVER, 1);
    assert!(eventually(WAIT, || run.shared.status().worlds[0] == 1));
    fire_world(run.owner, KILN_WORLD_STOPPING, KILN_WORLD_SERVER, 1);
    fire_world(run.owner, KILN_WORLD_STARTED, KILN_WORLD_SERVER, 2);
    fire_world(run.owner, KILN_WORLD_STOPPING, KILN_WORLD_SERVER, 2);
    assert!(eventually(WAIT, || {
        run.shared.status().inbox.delivered[CommandClass::WorldLifecycle as usize] >= 2
            && run.shared.status().worlds[0] == 0
    }));
    fire_world(run.owner, KILN_WORLD_STARTED, KILN_WORLD_SERVER, 3);
    assert!(eventually(WAIT, || run.shared.status().worlds[0] == 3));
    assert_eq!(run.shared.phase(), WorkerPhase::Running);
    run.shared.request_stop();
    assert!(run.finished());
}

// ---- watchdog enforcement, the leave decision, drain polling, interruption bounds ----

/// The call went over 10 s, so the mod stops, even when it caught the
/// watchdog's error and returned normally.
pub fn a_mod_that_catches_the_watchdog_error_and_returns_is_still_stopped() {
    let run = watchdog_run(
        "return { on_update = function() \
             if done then record('ran again') return end \
             done = true advance(10001) \
             pcall(function() while true do end end) \
             record('returned') \
           end, on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
    assert!(!has(run.owner, "a:ran again"), "{:?}", records(run.owner));
}

/// The same when the whole overrun happened inside one engine call and no hook
/// check came after it: the call's end is a check too.
pub fn a_call_that_overran_inside_a_native_call_is_stopped_when_it_returns() {
    let run = watchdog_run(
        "return { on_update = function() \
             if done then record('ran again') return end \
             done = true slow_call(11000) \
           end, on_unload = function() record('unload') end }",
    );
    stopped_by_watchdog(&run);
    assert!(!has(run.owner, "a:ran again"), "{:?}", records(run.owner));
}

pub fn a_module_top_level_that_catches_the_watchdog_error_is_not_activated() {
    let run = start(
        &[
            (
                "a",
                "advance(10001) pcall(function() while true do end end) \
                 return { on_load = function() record('loaded') end }",
            ),
            ("b", "return { on_load = function() record('loaded') end }"),
        ],
        true,
        PoolConfig::default(),
    );
    wait_for(&run, "b:loaded");
    assert!(eventually(WAIT, || run.shared.phase() == WorkerPhase::Running));
    assert!(eventually(WAIT, || run.shared.status().mods_failed == 1));
    assert!(!has(run.owner, "a:loaded"), "{:?}", records(run.owner));
    run.shared.request_stop();
    assert!(run.finished());
}

/// Once the pool cancelled (the exit's signal during an interpreter stop),
/// no more mod code runs: the leave does not close the state, so no `__gc`.
pub fn a_cancel_during_the_orderly_leave_closes_no_state() {
    let run = start(
        &[(
            "a",
            "keep = setmetatable({}, { __gc = function() record('gc') end }) \
             return { on_update = function() record('u') end, \
                      on_unload = function() hold_until('release') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:u");
    run.shared.request_stop();
    wait_for(&run, "holding:release");
    run.pool.cancel(run.shared.task());
    flip(&run, "release");
    assert!(run.finished());
    assert!(
        !has(run.owner, "a:gc"),
        "mod code after the pool's cancellation: {:?}",
        records(run.owner)
    );
}

/// kiln-sfi's stop and the pool's cancellation together are a fast leave.
pub fn a_stop_and_a_cancel_together_leave_fast() {
    let run = start(
        &[(
            "a",
            "keep = setmetatable({}, { __gc = function() record('gc') end }) \
             return { on_update = function() hold_until('go') end, \
                      on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "holding:go");
    run.shared.request_stop();
    run.pool.cancel(run.shared.task());
    flip(&run, "go");
    assert!(run.finished());
    assert!(
        !has(run.owner, "a:unload") && !has(run.owner, "a:gc"),
        "{:?}",
        records(run.owner)
    );
    assert_eq!(run.shared.status().reason, StopReason::Cancelled);
}

/// The stop is classified by the stop being in force, not by the error the call
/// returns: a mod that turns the sentinel into its own error is no mod failure.
pub fn a_mod_that_catches_the_stop_and_raises_its_own_error_is_no_mod_failure() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 record('in') \
                 local ok = pcall(function() while true do end end) \
                 if not ok then error('my own error') end \
               end, on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    run.shared.request_stop();
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.mods_failed, 0);
    assert_eq!(status.reason, StopReason::StopRequested);
    assert!(has(run.owner, "a:unload"));
    assert!(
        !run.errors().iter().any(|e| e.contains("my own error")),
        "{:?}",
        run.errors()
    );
}

/// Every re-entry of a pcall loop is interrupted at its next check, and a mod
/// that keeps catching holds the orderly leave (the interruption's bound: only the
/// exit park ends it).
pub fn a_pcall_loop_that_keeps_re_entering_is_interrupted_at_every_entry() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 record('in') \
                 while not flag('release') do pcall(function() while true do end end) end \
               end, on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:in");
    let (_, raised) = run.shared.hook_counts();
    run.shared.request_stop();
    assert!(eventually(WAIT, || run.shared.hook_counts().1 >= raised + 50));
    let (c0, r0) = run.shared.hook_counts();
    assert!(
        !run.pool.task(0).wait_finished(Duration::from_millis(100)),
        "the mod kept catching, so the orderly leave waits"
    );
    let (c1, r1) = run.shared.hook_counts();
    assert!(r1 > r0, "still re-interrupted");
    assert!(
        (c1 - c0).abs_diff(r1 - r0) <= 2,
        "every check after the stop raised: {} checks, {} raises",
        c1 - c0,
        r1 - r0
    );
    flip(&run, "release");
    assert!(run.finished());
    assert!(has(run.owner, "a:unload"));
    assert_eq!(run.shared.status().mods_failed, 0);
}

/// The interruption's bound, one more case: Lua 5.4 runs `__gc` finalizers with hooks off
/// (lgc.c GCTM sets `allowhook = 0`), so neither the stop, the watchdog nor the
/// pool's cancellation reaches a finalizer that loops; only the exit park does.
pub fn a_gc_finalizer_runs_without_the_count_hook() {
    let run = start(
        &[(
            "a",
            "return { on_update = function() \
                 if started then return end started = true \
                 setmetatable({}, { __gc = function() \
                     record('gc') \
                     while not flag('release') do for i = 1, 100000 do end end \
                   end }) \
                 collectgarbage() \
               end, on_unload = function() record('unload') end }",
        )],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:gc");
    let (checks, _) = run.shared.hook_counts();
    run.shared.request_stop();
    assert!(
        !run.pool.task(0).wait_finished(Duration::from_millis(200)),
        "the finalizer was interrupted"
    );
    assert_eq!(
        run.shared.hook_counts().0,
        checks,
        "no hook check ran inside the finalizer"
    );
    flip(&run, "release");
    assert!(run.finished());
}

/// After an interpreter stop the tick seals once, then polls with timeout 0
/// until kiln-sfi's owner is quiet (here an IO task is still running), and stops
/// draining once it is.
///
/// "Never blocks" is proven by what the tick did, not by how long it took (a wall-clock
/// bound fails whenever the test thread is descheduled): every drain call it made asked
/// for timeout 0, and it returned while the task owner was still busy. Nothing makes it
/// quiet during those ticks, so a tick that waited for quiet would never return; it runs
/// on its own thread so that shows as a failure after WAIT rather than as a hang.
pub fn the_tick_polls_the_drain_until_the_owner_is_quiet() {
    fn tick_returns(owner: KilnOwner) -> bool {
        let (sent, returned) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            fire_tick(owner);
            let _ = sent.send(());
        });
        returned.recv_timeout(WAIT).is_ok()
    }
    let run = start(
        &[("a", "return { on_update = function() record('u') end }")],
        false,
        PoolConfig::default(),
    );
    wait_for(&run, "a:u");
    run.pool.set_other_running(1);
    run.shared.request_stop();
    assert!(run.finished());
    assert_eq!(run.shared.phase(), WorkerPhase::Stopped);
    for _ in 0..2 {
        assert!(
            tick_returns(run.owner),
            "the tick waited for the task owner to be quiet"
        );
        let status = run.shared.status();
        assert!(status.drained && !status.quiescent, "{status:?}");
    }
    run.pool.set_other_running(0);
    fire_tick(run.owner);
    assert!(run.shared.quiescent());
    fire_tick(run.owner);
    fire_tick(run.owner);
    assert_eq!(
        run.pool.drains(),
        vec![0, 0, 0],
        "polls with timeout 0 until quiet, then none"
    );
    assert_eq!(
        logs(run.owner)
            .iter()
            .filter(|(_, t)| t.contains("its owner is sealed"))
            .count(),
        1,
        "the drain is reported once"
    );
}

// ---- the production mod log sink at ShroudForge's default level ----

/// kiln-sfi's production sink (`lifecycle::mod_log`, the one runtime-create installs) on a
/// production VM and composition runs tests/fixtures/mod_log/default_level.lua with exactly
/// the transcript both pinned ShroudForge builds give at their default INFO level (oracle
/// goldens, both pins): trace/debug convert nothing and hide conversion errors, info, warn,
/// error and print convert. Only the enabled lines reach KilnApi.log, as
/// `[<package>] <text>`; print's DEBUG line does not (write_mod_log.rs:8-20).
pub fn mod_log_honours_shroudforge_s_info_default() {
    struct ZeroClock;
    impl sfi_core::host::Clock for ZeroClock {
        fn seconds(&self) -> f64 {
            0.0
        }
    }
    use sfi_core::bindings::{OwnerBindings, loader::LoaderSnapshot};
    use sfi_core::conformance::{GoldenMatch, compare_golden, run_fixture_in_vm};
    use sfi_core::host::{HostContext, Phase};
    let fixture = include_str!("../fixtures/mod_log/default_level.lua");
    for (version, golden) in [
        (
            SourceVersion::Sf179,
            include_str!("../fixtures/mod_log/default_level.sf179.golden.json"),
        ),
        (
            SourceVersion::Sf180,
            include_str!("../fixtures/mod_log/default_level.sf180.golden.json"),
        ),
    ] {
        let api = api(false);
        let owner = next_owner();
        let mut context = HostContext::new(Phase::Runtime, Side::Server, version);
        // SAFETY: the fake table lives for the whole body.
        context.log = Some(mod_log(unsafe { Host::read(&*api) }, owner));
        context.clock = Some(Rc::new(ZeroClock));
        let vm = sfi_core::lua::LuaVm::new(context).unwrap();
        let manifest = package::Package::parse(
            br#"{"id":"oracle","name":"sfi oracle","version":"1.0.0"}"#,
            None,
            package::ParseOptions::default(),
        )
        .unwrap()
        .manifest;
        let env = vm
            .add_package(
                RuntimePackage::new("oracle", Rc::new(Source(String::new()))),
                |lua, env, package| {
                    kiln_sfi::worker::compose::install(
                        lua,
                        env,
                        package,
                        vm.context(),
                        &kiln_sfi::worker::compose::ComposePlan::default(),
                        OwnerBindings {
                            manifest,
                            locale: Rc::new(NoLocale),
                            loader: LoaderSnapshot {
                                patch: false,
                                export: false,
                                discovered_ids: ["oracle".to_owned()].into(),
                            },
                        },
                        false,
                    )
                    .map(|_| ())
                },
            )
            .unwrap();
        let actual = run_fixture_in_vm(vm.lua(), fixture, |_| Ok(env)).unwrap();
        assert_eq!(
            compare_golden(golden, &actual).unwrap_or_else(|e| panic!("{version:?}: {e}")),
            GoldenMatch::MessageVerifiedV2
        );
        assert_eq!(
            logs(owner),
            vec![
                (KILN_LOG_INFO, "[oracle] counted\tcounted".to_owned()),
                (KILN_LOG_WARN, "[oracle] counted\tcounted".to_owned()),
                (KILN_LOG_ERROR, "[oracle] counted\tcounted".to_owned()),
                (KILN_LOG_INFO, "[oracle] ".to_owned()),
            ],
            "{version:?}"
        );
    }
}
