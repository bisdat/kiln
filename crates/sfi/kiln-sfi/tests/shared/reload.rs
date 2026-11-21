// SPDX-License-Identifier: MIT
// the interpreter's live reload/unload transport, shared test bodies.
// Run by kiln-sfi's own tests (tests/reload.rs) and by any other test package that includes them.
// Behaviour checked against ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), both pins
// (identical files): modules/mod-runtime/ingame/process_runtime_reload_request.rs:5-46,
// unload_runtime_packages.rs:5-25, reload_runtime_packages.rs:6-95, restore_runtime_packages.rs:4-22,
// modules/loader-config/write_json_atomically.rs:4-25.
//! The live reload on the real worker, lifecycle host and IO thread, with a fake pool and a real Lua VM: the request file read
//! by kiln-sfi's IO thread, the inbox hand-off, the turn-start dispatch through sfi-core's `ReloadProtocol` and
//! `LifecycleRuntime::{unload, reload}`, and the result file ShroudForge writes.
// Another test package may load support through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;
// kiln.eml.archive and kiln.eml.values as far as a VM's host drives them (its arena over a granted view).
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_archive.rs"]
mod fake_archive;

use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use kiln_abi::services::kiln_eml_values::KILN_VALUE_RUNTIME;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::WriteOutcome;
use kiln_sfi::runtime::overrides::{ResourceKey, StorageKey};
use kiln_sfi::runtime::reload::{self, REQUEST_FILE, RESULT_FILE, Transport};
use kiln_sfi::runtime::ui;
use kiln_sfi::state::{self, StateHub};
use kiln_sfi::worker::*;
use serde_json::{Value, json};
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::{Cleanup, Monotonic};
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use sfi_policy::native_selection::Withdrawn;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::{Arc, LazyLock, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);

// ---- scenario plumbing (keyed by owner; the plan runs on the worker thread) ----

#[derive(Clone)]
struct Scenario {
    /// Every package in the registry, in plan order.
    packages: Vec<String>,
    /// What runtime-create admits.
    admitted: Vec<String>,
    /// What a rebuild withdraws (id, reason): as the production rebuild withdraws a changed folder.
    rebuild_withdraws: Vec<(String, String)>,
    /// Packages only a restart reloads (id, reason).
    restart_only: Vec<(String, String)>,
    /// A rebuild that fails (the plan source's error).
    rebuild_fails: Option<String>,
    /// Packages a rebuild's registry no longer has (the new VM cannot be built).
    rebuild_drops: Vec<String>,
    /// What a rebuild's plan admits, when not the start's: the lifecycle host must never admit more.
    rebuild_admits: Option<Vec<String>>,
    /// Every VM gets typed values (an arena over a view of the fixture generation granted to the task owner).
    values: bool,
    /// Runs with the run's owner before the worker starts (the fake provider's knobs).
    prepare: Option<fn(KilnOwner)>,
    /// A kiln.caps row of kiln.eml.archive withheld (pending) on the worker thread before each VM's typed values.
    withhold: Option<&'static str>,
    dir: PathBuf,
    hub: Arc<StateHub>,
}
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, Scenario>>> = LazyLock::new(Default::default);
/// Each package's current Lua, read when its VM requires it (a rebuild reads it again).
static SCRIPTS: LazyLock<Mutex<HashMap<(KilnOwner, String), String>>> =
    LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
static GO: LazyLock<Mutex<HashSet<(KilnOwner, String)>>> = LazyLock::new(Default::default);
/// The jumps a package asks for with `advance(ms)` (each run's clock is the process clock plus these), so a test can
/// make one call look long to the watchdog.
static JUMPS: LazyLock<Mutex<HashMap<KilnOwner, Duration>>> = LazyLock::new(Default::default);
struct JumpClock {
    owner: KilnOwner,
    base: Rc<dyn Monotonic>,
}
impl Monotonic for JumpClock {
    fn now(&self) -> Duration {
        let jumps = JUMPS
            .lock()
            .unwrap()
            .get(&self.owner)
            .copied()
            .unwrap_or_default();
        self.base.now() + jumps
    }
}
/// Each run's worker, for the `host_stop()` test hook (kiln-sfi's own orderly stop, raised from mod code).
static WORKERS: LazyLock<Mutex<HashMap<KilnOwner, std::sync::Weak<WorkerShared>>>> =
    LazyLock::new(Default::default);

fn scenario(owner: KilnOwner) -> Scenario {
    SCENARIOS.lock().unwrap()[&owner].clone()
}
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
fn count(owner: KilnOwner, text: &str) -> usize {
    records(owner).iter().filter(|r| *r == text).count()
}
fn set_script(owner: KilnOwner, id: &str, text: &str) {
    SCRIPTS
        .lock()
        .unwrap()
        .insert((owner, id.to_owned()), text.to_owned());
}

struct Source(KilnOwner, String);
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path != "src/mod.lua" {
            return Err(std::io::ErrorKind::NotFound.into());
        }
        SCRIPTS
            .lock()
            .unwrap()
            .get(&(self.0, self.1.clone()))
            .map(|text| text.as_bytes().to_vec())
            .ok_or_else(|| std::io::ErrorKind::NotFound.into())
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
/// The lifecycle's cleanup port (the override trackers in production): it records each call.
struct Recording(KilnOwner);
impl Cleanup for Recording {
    fn restore(&self, owner: &str) -> Result<(), String> {
        note(self.0, format!("cleanup:{owner}"));
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}

fn registry(owner: KilnOwner, packages: &[String]) -> Registry {
    let mut registry = Registry::default();
    for id in packages {
        let source: Rc<dyn PackageSource> = Rc::new(Source(owner, id.clone()));
        let mut parsed = package::Package::parse(
            json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
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

/// The storage `claim()` claims.
fn leaked_storage() -> StorageKey {
    StorageKey::Resource(ResourceKey {
        guid: "rl1-leaked".into(),
        type_hash: 1,
        part_index: 0,
    })
}

fn runtime_plan(cx: &WorkerContext<'_>, boot: bool) -> RuntimePlan {
    let owner = cx.owner();
    let scenario = scenario(owner);
    note(owner, if boot { "plan" } else { "rebuild" }.to_owned());
    let register: Register = Box::new(move |lua, env, package, ports| {
        let id = package.id().to_owned();
        // This VM's runtime facts: `latch()` latches an uncertain write in session 9, `claim()` leaves a claim
        // nothing releases, `facts()` says what this VM's facts hold.
        let facts = ports.runtime.facts().clone();
        env.set(
            "latch",
            lua.create_function({
                let facts = facts.clone();
                move |_, ()| {
                    facts.write_faults().report(
                        KILN_WORLD_SERVER as u32,
                        9,
                        WriteOutcome::EcsWriteUncertain,
                    );
                    Ok(())
                }
            })?,
        )?;
        env.set(
            "claim",
            lua.create_function({
                let (facts, id) = (facts.clone(), id.clone());
                move |_, ()| {
                    let store = facts.ownership();
                    let token = store.token().map_err(mlua::Error::runtime)?;
                    store
                        .acquire(&id, &leaked_storage(), token)
                        .map_err(mlua::Error::runtime)
                }
            })?,
        )?;
        env.set(
            "facts",
            lua.create_function(move |_, ()| {
                let latched = facts.write_faults().fault().is_some();
                let claimed = facts.ownership().owner_of(&leaked_storage()).is_some();
                Ok(format!(
                    "{}+{}",
                    if latched { "latched" } else { "clear" },
                    if claimed { "claimed" } else { "free" }
                ))
            })?,
        )?;
        env.set(
            "record",
            lua.create_function(move |_, text: String| {
                note(owner, format!("{id}:{text}"));
                Ok(())
            })?,
        )?;
        // The test hook for kiln-sfi's own stop (what `interpreter_stop` requests): asked from mod code, so it
        // arrives at a chosen point of a rebuild.
        env.set(
            "host_stop",
            lua.create_function(move |_, ()| {
                let worker = WORKERS
                    .lock()
                    .unwrap()
                    .get(&owner)
                    .and_then(|w| w.upgrade());
                worker.expect("the run's worker").request_stop();
                Ok(())
            })?,
        )?;
        // An unwind out of a Rust binding that skips the panic hook (no backtrace to symbolize): ShroudForge's
        // update failure when an on_update calls it.
        env.set(
            "explode",
            lua.create_function(|_, ()| -> mlua::Result<()> {
                std::panic::resume_unwind(Box::new("reload test: a Rust binding panics"))
            })?,
        )?;
        // Moves this run's clock (the watchdog's too) forward by `ms`.
        env.set(
            "advance",
            lua.create_function(move |_, ms: u64| {
                *JUMPS.lock().unwrap().entry(owner).or_default() += Duration::from_millis(ms);
                Ok(())
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
        Ok(())
    });
    let values = scenario.values;
    let register: Register = Box::new(move |lua, env, package, ports| {
        // The families see this VM's arena before any package code runs; a package named "unregistrable"
        // fails its VM's setup after the arena was made
        if values {
            let id = package.id();
            note(
                owner,
                format!("{id}:register values={}", ports.runtime.values().is_some()),
            );
            if id == "unregistrable" {
                return Err(mlua::Error::runtime("this package cannot be registered"));
            }
            // `open_typed(t)`: a typed value opened from a Lua table through this VM's arena (as a family does)
            if let Some(values) = ports.runtime.values() {
                env.set(
                    "open_typed",
                    lua.create_function(move |lua, value: mlua::Value| {
                        values.open_lua(lua, "fixture.Typed", value)
                    })?,
                )?;
            }
        }
        register(lua, env, package, ports)
    });
    let mut packages = scenario.packages.clone();
    if !boot {
        packages.retain(|id| !scenario.rebuild_drops.contains(id));
    }
    let withdrawn = if boot {
        Vec::new()
    } else {
        scenario
            .rebuild_withdraws
            .iter()
            .map(|(id, reason)| Withdrawn {
                mod_id: id.clone(),
                reason: reason.clone(),
                caused_by: Vec::new(),
            })
            .collect()
    };
    RuntimePlan {
        side: Side::Server,
        version: SourceVersion::Sf179,
        registry: registry(owner, &packages),
        plan: scenario.packages.clone(),
        admitted: match (&scenario.rebuild_admits, boot) {
            (Some(admits), false) => admits.iter().cloned().collect(),
            _ => scenario.admitted.iter().cloned().collect(),
        },
        cleanup: Rc::new(Recording(owner)),
        locale: Rc::new(NoLocale),
        clock: Rc::new(JumpClock {
            owner,
            base: process_clock(),
        }),
        loader_patch: false,
        loader_export: false,
        attachments: None,
        register: Some(register),
        compose: kiln_sfi::worker::compose::ComposePlan {
            withdrawn,
            ..Default::default()
        },
    }
}

struct TestPlan;
impl PlanSource for TestPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        Ok(runtime_plan(cx, true))
    }
    fn rebuild(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        match scenario(cx.owner()).rebuild_fails {
            Some(error) => Err(error),
            None => Ok(runtime_plan(cx, false)),
        }
    }
    fn restart_only(cx: &WorkerContext<'_>, id: &str) -> Option<String> {
        scenario(cx.owner())
            .restart_only
            .iter()
            .find(|(r, _)| r == id)
            .map(|(_, reason)| reason.clone())
    }
    fn reload_transport(cx: &WorkerContext<'_>) -> Option<Transport> {
        let scenario = scenario(cx.owner());
        Some(Transport {
            dir: scenario.dir,
            io: scenario.hub,
        })
    }
    // The UI family in every VM and its action drain, over the scenario's folder
    fn families(cx: &WorkerContext<'_>) -> Result<kiln_sfi::runtime::Families, String> {
        kiln_sfi::runtime::Families::new(vec![ui::family(Some(ui_files(cx.owner())))])
    }
    fn ui_transport(cx: &WorkerContext<'_>) -> Option<ui::transport::Transport> {
        Some(ui_files(cx.owner()))
    }
    fn values(cx: &WorkerContext<'_>) -> Option<KilnEmlArchiveIdentityV1> {
        let scenario = scenario(cx.owner());
        // the fake's rows are per thread: this is the worker's
        for row in fake_archive::ROWS {
            fake_archive::set_row(row, None);
        }
        if let Some(row) = scenario.withhold {
            fake_archive::set_row(
                row,
                Some(fake_archive::Row::State(
                    kiln_abi::services::kiln_caps::KILN_CAP_PENDING,
                    "held for the test",
                )),
            );
        }
        scenario.values.then(fake_archive::fixture_identity)
    }
}

/// A run's UI files (Kiln.Ui's actions, the notices) below its scenario folder.
fn ui_files(owner: KilnOwner) -> ui::transport::Transport {
    let dir = scenario(owner).dir;
    ui::transport::Transport {
        game: dir.join("game"),
        config: Some(dir.join("config")),
        launch: ui::transport::Launch {
            launch_id: 0x3f2b_8c1e_5a7d_4e2f,
            plan_hash: [0xaa; 32],
        },
    }
}
/// An action request as Kiln.Ui writes it (staged, then renamed into actions/).
fn ui_request(owner: KilnOwner, name: &str, mod_id: &str, epoch: u64, value: &str) {
    let files = ui_files(owner);
    let actions = files.actions_dir();
    let staging = actions.parent().unwrap().join("staging");
    std::fs::create_dir_all(&actions).unwrap();
    std::fs::create_dir_all(&staging).unwrap();
    let record = ui::transport::ActionRecord {
        request_id: name.into(),
        sequence: 0,
        launch_id: files.launch.launch_id_hex(),
        plan_hash: files.launch.plan_hash_hex(),
        owner_epoch: epoch,
        mod_id: mod_id.into(),
        action: "ping".into(),
        value: Some(value.into()),
        created_ms: 0,
    };
    std::fs::write(staging.join(name), record.to_bytes()).unwrap();
    std::fs::rename(staging.join(name), actions.join(name)).unwrap();
}

// ---- a run ----

struct Run {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    shared: Arc<WorkerShared>,
    dir: PathBuf,
}
impl Run {
    fn finished(&self) -> bool {
        self.pool.task(0).wait_finished(WAIT)
    }
    fn stop(&self) {
        self.shared.request_stop();
        assert!(self.finished());
    }
    fn wait_for(&self, text: &str) {
        assert!(
            eventually(WAIT, || records(self.owner).iter().any(|r| r == text)),
            "never saw {text}: {:?} / {:?}",
            records(self.owner),
            self.shared.status()
        );
    }
    fn flip(&self, name: &str) {
        GO.lock().unwrap().insert((self.owner, name.to_owned()));
    }
    /// Writes the request as a requester does: a whole new file renamed over the old one.
    fn request(&self, document: &Value) {
        self.request_bytes(&serde_json::to_vec(document).unwrap());
    }
    fn request_bytes(&self, bytes: &[u8]) {
        let temp = self.dir.join("request.tmp");
        std::fs::write(&temp, bytes).unwrap();
        std::fs::rename(&temp, self.dir.join(REQUEST_FILE)).unwrap();
    }
    fn result_path(&self) -> PathBuf {
        self.dir.join(RESULT_FILE)
    }
    /// The result file once it answers `id`.
    fn result(&self, id: &str) -> Vec<u8> {
        let mut bytes = Vec::new();
        assert!(
            eventually(WAIT, || {
                bytes = std::fs::read(self.result_path()).unwrap_or_default();
                serde_json::from_slice::<Value>(&bytes).is_ok_and(|v| v["requestId"] == id)
            }),
            "no result for {id}: {:?}",
            records(self.owner)
        );
        bytes
    }
    fn answer(&self, id: &str) -> Value {
        serde_json::from_slice(&self.result(id)).unwrap()
    }
    fn delivered(&self) -> u64 {
        self.shared.status().inbox.delivered[CommandClass::ReloadRequest as usize]
    }
    /// Waits until the worker took the `n`th request from the inbox and turned twice after it.
    fn settle_after_delivery(&self, n: u64) {
        assert!(eventually(WAIT, || self.delivered() >= n));
        let turns = self.shared.status().turns;
        assert!(eventually(WAIT, || self.shared.status().turns >= turns + 2));
    }
    /// Waits long enough for the IO thread to have read the request file several times.
    fn settle_polls(&self) {
        let turns = self.shared.status().turns;
        std::thread::sleep(reload::POLL * 5);
        assert!(eventually(WAIT, || self.shared.status().turns >= turns + 2));
    }
}

fn start_with(scenario: impl FnOnce(&mut Scenario), scripts: &[(&str, &str)]) -> Run {
    let owner = next_owner();
    let dir = fresh_dir("rl1-runtime");
    let hub = Arc::new(StateHub::new());
    let mut s = Scenario {
        packages: scripts.iter().map(|(id, _)| id.to_string()).collect(),
        admitted: scripts.iter().map(|(id, _)| id.to_string()).collect(),
        rebuild_withdraws: Vec::new(),
        restart_only: Vec::new(),
        rebuild_fails: None,
        rebuild_drops: Vec::new(),
        rebuild_admits: None,
        values: false,
        prepare: None,
        withhold: None,
        dir: dir.clone(),
        hub: hub.clone(),
    };
    scenario(&mut s);
    // The fixture generation is granted to this owner; its arena steps join the run's record
    fake_archive::grant(
        owner,
        fake_archive::Grant::new(
            fake_archive::fixture_identity(),
            Arc::new(|_, _| Vec::new()),
        ),
    );
    fake_archive::observe_values(Some(note));
    if let Some(prepare) = s.prepare {
        prepare(owner);
    }
    for (id, text) in scripts {
        set_script(owner, id, text);
    }
    SCENARIOS.lock().unwrap().insert(owner, s);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<TestPlan>)));
    worker.report_status_to(hub);
    let mut api = api(true);
    fake_archive::wrap(&mut api);
    assert!(worker.bind(unsafe { Host::read(&*api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    WORKERS
        .lock()
        .unwrap()
        .insert(owner, Arc::downgrade(&shared));
    Run {
        owner,
        pool,
        shared,
        dir,
    }
}
fn start(scripts: &[(&str, &str)]) -> Run {
    start_with(|_| {}, scripts)
}

/// A package that records its top level, on_load, on_update (once) and on_unload, each with `tag`.
fn recorder(tag: &str) -> String {
    format!(
        "record('top{tag}') local seen = false \
         return {{ on_load = function() record('load{tag}') end, \
                   on_update = function() if not seen then seen = true record('update{tag}') end end, \
                   on_unload = function() record('unload{tag}') end }}"
    )
}

/// ShroudForge's result file for one answer: serde_json's pretty form with its key order.
fn shroudforge_result(id: &str, status: &str, message: &str) -> Vec<u8> {
    format!(
        "{{\n  \"requestId\": \"{id}\",\n  \"status\": \"{status}\",\n  \"message\": \"{message}\"\n}}"
    )
    .into_bytes()
}

// ---- the documents ----

/// The result file holds ShroudForge's exact bytes for each status; the operation defaults to reload; an unknown
/// one answers ShroudForge's text and runs nothing.
pub fn the_result_file_holds_shroudforge_s_documents() {
    let run = start(&[("a", &recorder(""))]);
    run.wait_for("a:update");
    run.request(&json!({"requestId": "r1", "operation": "reload", "modIds": ["a"]}));
    assert_eq!(
        run.result("r1"),
        shroudforge_result("r1", "reloaded", "Runtime mod packages updated")
    );
    run.request(&json!({"requestId": "r2", "operation": "unload", "modIds": ["a"]}));
    assert_eq!(
        run.result("r2"),
        shroudforge_result("r2", "unloaded", "Runtime mod packages updated")
    );
    let unloads = count(run.owner, "a:unload");
    run.request(&json!({"requestId": "r3", "operation": "restart", "modIds": ["a"]}));
    assert_eq!(
        run.result("r3"),
        shroudforge_result("r3", "error", "unknown runtime mod operation: restart")
    );
    assert_eq!(
        count(run.owner, "a:unload"),
        unloads,
        "an unknown operation runs nothing"
    );
    // no operation: reload (process_runtime_reload_request.rs:25)
    run.request(&json!({"requestId": "r4"}));
    assert_eq!(
        run.result("r4"),
        shroudforge_result("r4", "reloaded", "Runtime mod packages updated")
    );
    // the same bytes sfi-core's protocol hands over
    assert_eq!(
        reload::result_bytes(
            &json!({"requestId":"r4","status":"reloaded","message":"Runtime mod packages updated"})
        ),
        shroudforge_result("r4", "reloaded", "Runtime mod packages updated")
    );
    run.stop();
}

/// A file that cannot be read as a request is ignored without a result: not JSON, no string requestId, larger
/// than 64 KiB. The next good one is answered.
pub fn requests_that_cannot_be_read_are_ignored_silently() {
    let run = start(&[("a", &recorder(""))]);
    run.wait_for("a:update");
    let big = json!({"requestId": "big", "pad": "x".repeat(reload::REQUEST_MAX as usize)});
    for bytes in [
        b"{ not json".to_vec(),
        serde_json::to_vec(&json!({"operation": "unload", "modIds": ["a"]})).unwrap(),
        serde_json::to_vec(&json!({"requestId": 7, "operation": "unload", "modIds": ["a"]}))
            .unwrap(),
        serde_json::to_vec(&big).unwrap(),
    ] {
        run.request_bytes(&bytes);
        run.settle_polls();
        assert!(!run.result_path().exists(), "{:?}", records(run.owner));
    }
    assert_eq!(count(run.owner, "a:unload"), 0);
    run.request(&json!({"requestId": "ok", "operation": "unload", "modIds": ["a"]}));
    assert_eq!(run.answer("ok")["status"], "unloaded");
    run.stop();
}

// ---- reload and unload ----

/// A reload unloads every active package in reverse order, reads and runs their Lua again in plan order, and
/// answers once per requestId: the same id again is not run (even after the runtime was replaced), another id is,
/// and the first id again after that is (only the last id counts).
pub fn a_reload_runs_the_lua_again_and_dedupes_on_the_request_id() {
    let run = start(&[("a", &recorder("")), ("b", &recorder(""))]);
    run.wait_for("b:update");
    // the Lua on disk changed: a reload reads it again
    set_script(run.owner, "a", &recorder("2"));
    run.request(&json!({"requestId": "one", "modIds": ["a"]}));
    assert_eq!(run.answer("one")["status"], "reloaded");
    run.wait_for("a:update2");
    let seen = records(run.owner);
    let at = |text: &str| {
        seen.iter()
            .position(|r| r == text)
            .unwrap_or_else(|| panic!("{text}: {seen:?}"))
    };
    assert!(at("b:unload") < at("a:unload"), "reverse order: {seen:?}");
    // The rebuild's plan is made first (Kiln's refusals are decided on it before anything is unloaded)
    assert!(
        at("rebuild") < at("b:unload"),
        "the plan before the unload: {seen:?}"
    );
    assert!(
        at("a:unload") < at("a:top2"),
        "unload before the new VM: {seen:?}"
    );
    assert_eq!(
        seen[at("a:top2")..at("a:top2") + 4],
        ["a:top2", "a:load2", "b:top", "b:load"],
        "plan order on the new VM: {seen:?}"
    );
    assert_eq!(count(run.owner, "rebuild"), 1);
    // the same id again (other bytes, so the IO thread hands it over): not run
    run.request_bytes(br#"{ "requestId": "one", "modIds": ["a"] }"#);
    run.settle_after_delivery(2);
    assert_eq!(count(run.owner, "rebuild"), 1, "{:?}", records(run.owner));
    run.request(&json!({"requestId": "two"}));
    run.result("two");
    assert_eq!(count(run.owner, "rebuild"), 2);
    run.request(&json!({"requestId": "one"}));
    run.result("one");
    assert_eq!(count(run.owner, "rebuild"), 3);
    run.stop();
}

/// An unload runs on_unload for the named active packages in reverse plan order and nothing else; the others keep
/// running. A reload brings the whole set back.
pub fn an_unload_runs_on_unload_in_reverse_and_keeps_the_others() {
    let run = start(&[
        ("a", &recorder("")),
        ("b", "return { on_update = function() record('tick') end }"),
        ("c", &recorder("")),
    ]);
    run.wait_for("c:update");
    run.request(&json!({"requestId": "u", "operation": "unload", "modIds": ["a", "c", "nobody"]}));
    assert_eq!(run.answer("u")["status"], "unloaded");
    let seen = records(run.owner);
    let c = seen.iter().position(|r| r == "c:unload").unwrap();
    let a = seen.iter().position(|r| r == "a:unload").unwrap();
    assert!(c < a, "{seen:?}");
    assert_eq!(count(run.owner, "rebuild"), 0);
    let ticks = count(run.owner, "b:tick");
    assert!(eventually(WAIT, || count(run.owner, "b:tick") > ticks + 2));
    assert!(eventually(WAIT, || run.shared.status().mods_active == 1));
    run.request(&json!({"requestId": "r"}));
    assert_eq!(run.answer("r")["status"], "reloaded");
    assert!(eventually(WAIT, || run.shared.status().mods_active == 3));
    run.stop();
}

/// A package the start did not admit is refused with ShroudForge's failed-to-activate text, its Lua never runs,
/// and the running packages are left alone: the refusal is decided before anything is unloaded (no on_unload,
/// no cleanup, no on_load again).
pub fn a_package_not_admitted_at_the_start_is_refused_and_the_runtime_restored() {
    let run = start_with(
        |s| {
            s.admitted = vec!["a".into()];
            // a rebuild that would admit more than the start did
            s.rebuild_admits = Some(vec!["a".into(), "late".into()]);
        },
        &[("a", &recorder("")), ("late", &recorder(""))],
    );
    run.wait_for("a:update");
    run.request(&json!({"requestId": "r", "modIds": ["late"]}));
    let answer = run.answer("r");
    assert_eq!(answer["status"], "error");
    assert_eq!(
        answer["message"],
        reload::failed_to_activate("late", reload::NOT_ADMITTED)
    );
    assert_eq!(count(run.owner, "late:top"), 0, "{:?}", records(run.owner));
    assert_eq!(
        (
            count(run.owner, "a:unload"),
            count(run.owner, "cleanup:a"),
            count(run.owner, "a:load")
        ),
        (0, 0, 1),
        "left alone: {:?}",
        records(run.owner)
    );
    assert_eq!(
        count(run.owner, "a:top"),
        1,
        "the old VM is kept, not rebuilt"
    );
    assert!(eventually(WAIT, || run.shared.status().mods_active == 1));
    // an id no package has is no refusal (ShroudForge checks enabled runtime packages only)
    run.request(&json!({"requestId": "s", "modIds": ["nobody"]}));
    assert_eq!(run.answer("s")["status"], "reloaded");
    // a successful reload never admits what the start did not
    run.wait_for("a:load");
    assert_eq!(count(run.owner, "a:top"), 2);
    assert_eq!(count(run.owner, "late:top"), 0, "{:?}", records(run.owner));
    run.stop();
}

/// A package the rebuild withdrew (its folder no longer has the plan's digest) is refused with the rebuild's
/// reason, requested or not (a running package is never dropped silently), before anything is unloaded: the
/// running packages are left alone.
pub fn a_changed_package_is_refused_with_the_rebuild_s_reason() {
    let reason = "its files changed since the launcher wrote this start's launch plan";
    let run = start_with(
        |s| s.rebuild_withdraws = vec![("b".into(), reason.into())],
        &[("a", &recorder("")), ("b", &recorder(""))],
    );
    run.wait_for("b:update");
    for (id, requested) in [("one", json!(["b"])), ("two", json!(["a"]))] {
        run.request(&json!({"requestId": id, "modIds": requested}));
        let answer = run.answer(id);
        assert_eq!(answer["status"], "error");
        assert_eq!(answer["message"], reload::failed_to_activate("b", reason));
    }
    assert_eq!(count(run.owner, "a:top"), 1, "{:?}", records(run.owner));
    for text in ["a:unload", "b:unload", "cleanup:a", "cleanup:b"] {
        assert_eq!(
            count(run.owner, text),
            0,
            "{text}: {:?}",
            records(run.owner)
        );
    }
    assert_eq!(
        (count(run.owner, "a:load"), count(run.owner, "b:load")),
        (1, 1),
        "nothing re-run: {:?}",
        records(run.owner)
    );
    assert!(eventually(WAIT, || run.shared.status().mods_active == 2));
    run.stop();
}

/// A package only a restart reloads (startup data, DLLs) is refused by a reload before anything is unloaded;
/// an unload still runs.
pub fn a_restart_only_package_refuses_a_reload_but_not_an_unload() {
    let run = start_with(
        |s| s.restart_only = vec![("a".into(), reload::RESTART_NATIVE.into())],
        &[("a", &recorder(""))],
    );
    run.wait_for("a:update");
    run.request(&json!({"requestId": "r", "modIds": []}));
    assert_eq!(
        run.answer("r")["message"],
        reload::failed_to_activate("a", reload::RESTART_NATIVE)
    );
    assert_eq!(count(run.owner, "a:top"), 1);
    assert_eq!(
        (count(run.owner, "a:unload"), count(run.owner, "a:load")),
        (0, 1),
        "{:?}",
        records(run.owner)
    );
    run.request(&json!({"requestId": "u", "operation": "unload", "modIds": ["a"]}));
    assert_eq!(run.answer("u")["status"], "unloaded");
    assert_eq!(count(run.owner, "a:unload"), 1);
    run.stop();
}

// ---- threads and leaves ----

/// The pool's cancellation while the new VM is being built: the worker leaves fast. No more mod code runs (no
/// on_load of the new VM, no restore of the old one, no __gc) and no result is written.
pub fn a_cancel_during_the_rebuild_leaves_fast_with_no_mod_code() {
    let probe = "keep = setmetatable({}, { __gc = function() record('gc') end }) ";
    let run = start(&[("a", &format!("{probe}{}", recorder("")))]);
    run.wait_for("a:update");
    set_script(
        run.owner,
        "a",
        &format!(
            "{probe}record('top2') hold_until('go') return {{ on_load = function() record('load2') end }}"
        ),
    );
    run.request(&json!({"requestId": "r"}));
    run.wait_for("holding:go");
    run.pool.cancel(run.shared.task());
    run.flip("go");
    assert!(run.finished());
    for text in ["a:load2", "a:gc"] {
        assert_eq!(
            count(run.owner, text),
            0,
            "{text}: {:?}",
            records(run.owner)
        );
    }
    assert_eq!(
        count(run.owner, "a:load"),
        1,
        "no restore: {:?}",
        records(run.owner)
    );
    // the IO thread would have written a result by now
    std::thread::sleep(reload::POLL * 5);
    assert!(!run.result_path().exists());
    assert_eq!(run.shared.status().reason, StopReason::Cancelled);
}

/// kiln-sfi's own orderly stop while the new VM is being built (raised from the new VM's code through the
/// `host_stop()` test hook, after a's on_load ran there): the worker leaves in order. The new VM is not forgotten:
/// its package whose on_load ran gets its on_unload and the VM closes (its `__gc` runs). The old runtime is not
/// restored (no on_load) and its packages, which the reload already unloaded, are not unloaded again. No result
/// is written.
pub fn a_stop_during_the_rebuild_leaves_in_order_and_unloads_nothing_twice() {
    let probe = |tag: &str| {
        format!("keep = setmetatable({{}}, {{ __gc = function() record('gc{tag}') end }}) ")
    };
    let run = start(&[
        ("a", &format!("{}{}", probe(""), recorder(""))),
        ("b", &recorder("")),
    ]);
    run.wait_for("b:update");
    set_script(run.owner, "a", &format!("{}{}", probe("2"), recorder("2")));
    set_script(
        run.owner,
        "b",
        "record('stop2') host_stop() return { on_load = function() record('load2') end, \
         on_unload = function() record('unload2') end }",
    );
    run.request(&json!({"requestId": "r"}));
    assert!(run.finished());
    let seen = records(run.owner);
    for (text, n) in [
        // the reload's unload of the old runtime, once; no restore
        ("a:unload", 1),
        ("b:unload", 1),
        ("a:load", 1),
        ("b:load", 1),
        // the new VM: a ran and is unloaded by the orderly leave; b's top level raised the stop
        ("a:load2", 1),
        ("b:stop2", 1),
        ("b:load2", 0),
        ("b:unload2", 0),
        ("a:unload2", 1),
        ("a:gc2", 1),
    ] {
        assert_eq!(count(run.owner, text), n, "{text}: {seen:?}");
    }
    let at = |text: &str| {
        seen.iter()
            .position(|r| r == text)
            .unwrap_or_else(|| panic!("{text}: {seen:?}"))
    };
    assert!(
        at("a:unload2") < at("a:gc2"),
        "on_unload, then the VM closes: {seen:?}"
    );
    // the IO thread would have written a result by now
    std::thread::sleep(reload::POLL * 5);
    assert!(!run.result_path().exists());
    assert_eq!(run.shared.status().reason, StopReason::StopRequested);
}

/// The pool's cancellation during the orderly leave that stops a reload's abandoned new VM (kiln-sfi's own stop
/// came while it was built): the rest of the leave is fast. The new VM's on_unload that was running
/// finishes, but the VM is never closed (no `__gc`), as the old runtime's VM is never closed either.
pub fn a_cancel_during_the_abandoned_vm_s_leave_closes_no_vm() {
    let probe = |tag: &str| {
        format!("keep = setmetatable({{}}, {{ __gc = function() record('gc{tag}') end }}) ")
    };
    let run = start(&[
        ("a", &format!("{}{}", probe(""), recorder(""))),
        ("b", &recorder("")),
    ]);
    run.wait_for("b:update");
    set_script(
        run.owner,
        "a",
        &format!(
            "{}record('top2') return {{ on_load = function() record('load2') end, \
             on_unload = function() record('unload2') hold_until('leave') end }}",
            probe("2")
        ),
    );
    set_script(run.owner, "b", "record('stop2') host_stop() return {}");
    run.request(&json!({"requestId": "r"}));
    run.wait_for("holding:leave");
    run.pool.cancel(run.shared.task());
    run.flip("leave");
    assert!(run.finished());
    let seen = records(run.owner);
    for (text, n) in [
        ("a:load2", 1),
        ("a:unload2", 1),
        ("a:gc2", 0),
        ("a:gc", 0),
        ("a:load", 1),
    ] {
        assert_eq!(count(run.owner, text), n, "{text}: {seen:?}");
    }
    std::thread::sleep(reload::POLL * 5);
    assert!(!run.result_path().exists());
}

/// Requests written while the worker is busy in a long turn coalesce: the inbox keeps only the latest, and only
/// that one is run and answered.
pub fn requests_during_a_long_turn_coalesce_to_the_latest() {
    let run = start(&[(
        "a",
        "record('top') return { on_load = function() record('load') end, \
         on_update = function() hold_until('go') end, on_unload = function() record('unload') end }",
    )]);
    run.wait_for("holding:go");
    let reloads = || run.shared.status().inbox;
    run.request(&json!({"requestId": "first"}));
    assert!(eventually(WAIT, || reloads().accepted
        [CommandClass::ReloadRequest as usize]
        == 1));
    run.request(&json!({"requestId": "second"}));
    assert!(eventually(WAIT, || reloads().coalesced
        [CommandClass::ReloadRequest as usize]
        == 1));
    run.flip("go");
    run.result("second");
    assert_eq!(count(run.owner, "a:unload"), 1, "{:?}", records(run.owner));
    assert_eq!(count(run.owner, "rebuild"), 1);
    assert_eq!(reloads().delivered[CommandClass::ReloadRequest as usize], 1);
    run.stop();
}

/// sfi.json follows a reload and a restore: a package that failed at the start and runs after a reload is
/// `active`, and after a refused reload the restored packages are still active and counted.
pub fn sfi_json_is_updated_after_a_reload_and_a_restore() {
    let run = start_with(
        |s| {
            s.hub.begin(identity(), state::Phase::Starting, None);
            for id in ["a", "b"] {
                let mut init = state::PackageInit::selected(id, Some("1.0.0"), None);
                init.lua = state::Lua::Admitted;
                s.hub.update(|r| r.add_package(init));
            }
        },
        &[
            ("a", &recorder("")),
            ("b", "return { on_load = function() error('not yet') end }"),
        ],
    );
    let hub = scenario(run.owner).hub;
    let lua = |id: &str| hub.record().unwrap().package(id).map(|p| p.lua);
    // The `ownerEpoch` of each package's live runtime generation (UI actions carry it)
    let epoch = |id: &str| {
        hub.record()
            .unwrap()
            .package(id)
            .and_then(|p| p.owner_epoch)
    };
    assert!(eventually(WAIT, || lua("b") == Some(state::Lua::Failed)));
    assert_eq!(lua("a"), Some(state::Lua::Active));
    assert!(eventually(WAIT, || epoch("a").is_some()));
    let first = epoch("a");
    assert_eq!(epoch("b"), None, "a package that never ran has no epoch");
    set_script(run.owner, "b", &recorder(""));
    // After the reload, a registers a UI action
    set_script(
        run.owner,
        "a",
        &format!(
            "shroudforge.ui.on_action('ping', function(v) record('ui:' .. tostring(v)) end) {}",
            recorder("")
        ),
    );
    run.request(&json!({"requestId": "fixed"}));
    assert_eq!(run.answer("fixed")["status"], "reloaded");
    assert!(
        eventually(WAIT, || lua("b") == Some(state::Lua::Active)),
        "{:?}",
        lua("b")
    );
    assert!(eventually(WAIT, || run.shared.status().mods_active == 2));
    // the reload's new generation: one new epoch for both
    assert!(eventually(WAIT, || epoch("b").is_some() && epoch("a") == epoch("b")));
    let second = epoch("a");
    assert_ne!(second, first);
    // The turn's drain calls a's callback for a request at this generation's epoch, and drops one made for the
    // first generation without calling it
    ui_request(run.owner, "old.json", "a", first.unwrap(), "old");
    ui_request(run.owner, "now.json", "a", second.unwrap(), "hello");
    run.wait_for("a:ui:hello");
    let actions = ui_files(run.owner).actions_dir();
    assert!(eventually(WAIT, || std::fs::read_dir(&actions)
        .unwrap()
        .count()
        == 0));
    assert_eq!(count(run.owner, "a:ui:old"), 0);
    assert_eq!(count(run.owner, "a:ui:hello"), 1);
    // a refused reload (ShroudForge's own refusal, after the unload): both restored, still active and counted
    SCENARIOS
        .lock()
        .unwrap()
        .get_mut(&run.owner)
        .unwrap()
        .rebuild_fails = Some("no mods folder".into());
    run.request(&json!({"requestId": "refused"}));
    assert_eq!(run.answer("refused")["status"], "error");
    assert!(eventually(WAIT, || run.shared.status().mods_active == 2));
    assert_eq!(
        (lua("a"), lua("b")),
        (Some(state::Lua::Active), Some(state::Lua::Active))
    );
    // restored after an unload: a new generation again, never an earlier epoch
    assert!(eventually(WAIT, || epoch("a").is_some()
        && epoch("a") == epoch("b")
        && epoch("a") != second
        && epoch("a") != first));
    run.stop();
}

/// Each unload runs the package's cleanup through the lifecycle's cleanup port (the override trackers) after its
/// on_unload. A restore whose on_load fails again is cleaned up the same way and logged with ShroudForge's text.
pub fn unload_and_a_failed_restore_clean_up_through_the_cleanup_port() {
    let run = start_with(
        |s| s.rebuild_fails = Some("no mods folder".into()),
        &[
            ("a", &recorder("")),
            (
                "b",
                "local loads = 0 return { on_load = function() loads = loads + 1 \
                 if loads > 1 then error('not again') end end, on_unload = function() record('unload') end }",
            ),
        ],
    );
    run.wait_for("a:update");
    run.request(&json!({"requestId": "u", "operation": "unload", "modIds": ["a"]}));
    run.result("u");
    let seen = records(run.owner);
    let at = |text: &str| {
        seen.iter()
            .position(|r| r == text)
            .unwrap_or_else(|| panic!("{text}: {seen:?}"))
    };
    assert!(at("a:unload") < at("cleanup:a"), "{seen:?}");
    // the rediscovery fails after b was unloaded (ShroudForge's order); b's restored on_load fails on the old VM, so
    // it is cleaned up and stays down
    run.request(&json!({"requestId": "r", "modIds": ["b"]}));
    assert_eq!(
        run.answer("r")["message"],
        "updated mod package discovery failed: no mods folder"
    );
    assert!(
        eventually(WAIT, || count(run.owner, "cleanup:b") == 2),
        "{:?}",
        records(run.owner)
    );
    assert!(eventually(WAIT, || logs(run.owner).iter().any(
        |(level, t)| *level == KILN_LOG_ERROR
            && t.contains("mod 'b': Could not restore runtime mod after refresh rollback: ")
    )));
    assert!(eventually(WAIT, || run.shared.status().mods_active == 0));
    run.stop();
}

/// The plan source's rebuild failing and the new VM failing to build answer ShroudForge's texts
/// (reload_runtime_packages.rs:40,48) and restore the runtime.
pub fn rebuild_failures_answer_shroudforge_s_texts_and_restore() {
    let run = start(&[("a", &recorder("")), ("b", &recorder(""))]);
    run.wait_for("b:update");
    SCENARIOS
        .lock()
        .unwrap()
        .get_mut(&run.owner)
        .unwrap()
        .rebuild_fails = Some("no mods folder".into());
    run.request(&json!({"requestId": "d"}));
    assert_eq!(
        run.answer("d")["message"],
        "updated mod package discovery failed: no mods folder"
    );
    {
        let mut scenarios = SCENARIOS.lock().unwrap();
        let s = scenarios.get_mut(&run.owner).unwrap();
        s.rebuild_fails = None;
        s.rebuild_drops = vec!["b".into()];
    }
    run.request(&json!({"requestId": "i"}));
    assert_eq!(
        run.answer("i")["message"],
        "updated runtime initialization failed: planned package is not discovered: b"
    );
    assert_eq!(
        count(run.owner, "a:top"),
        1,
        "no new Lua ran: {:?}",
        records(run.owner)
    );
    assert_eq!(
        (count(run.owner, "a:load"), count(run.owner, "b:load")),
        (3, 3)
    );
    assert!(eventually(WAIT, || run.shared.status().mods_active == 2));
    run.stop();
}

/// A reload's VM starts with runtime facts of its own, as ShroudForge's reload builds a new `AppState`
/// (reload_runtime_packages.rs:44, start_ingame_runtime.rs:38-58): the write fault the old runtime latched and the
/// claim one of its mods left behind do not reach the new VM.
pub fn a_reloaded_vm_starts_with_runtime_facts_of_its_own() {
    let run = start(&[(
        "a",
        "latch() claim() return { on_load = function() record('facts:' .. facts()) end }",
    )]);
    run.wait_for("a:facts:latched+claimed");
    set_script(
        run.owner,
        "a",
        "return { on_load = function() record('facts:' .. facts()) end }",
    );
    run.request(&json!({"requestId": "r"}));
    assert_eq!(run.answer("r")["status"], "reloaded");
    let seen = || {
        records(run.owner)
            .into_iter()
            .filter(|r| r.starts_with("a:facts:"))
            .collect::<Vec<_>>()
    };
    assert!(
        eventually(WAIT, || seen().len() == 2),
        "{:?}",
        records(run.owner)
    );
    assert_eq!(seen(), ["a:facts:latched+claimed", "a:facts:clear+free"]);
    run.stop();
}

/// A mod error the old runtime logged is logged again when the reloaded runtime fails the same way: each runtime's
/// failures are logged once, as ShroudForge logs each start's.
pub fn a_reloaded_runtime_logs_its_mod_errors_again() {
    let run = start(&[
        ("a", &recorder("")),
        ("b", "return { on_load = function() error('broken') end }"),
    ]);
    let failed = || {
        logs(run.owner)
            .iter()
            .filter(|(level, t)| {
                *level == KILN_LOG_ERROR
                    && t.starts_with("kiln-sfi: mod 'b': ")
                    && t.contains("broken")
            })
            .count()
    };
    run.wait_for("a:update");
    assert!(eventually(WAIT, || failed() == 1), "{:?}", logs(run.owner));
    run.request(&json!({"requestId": "r"}));
    assert_eq!(run.answer("r")["status"], "reloaded");
    // the new VM's first turn ran (and published at its end); two more turns, then count
    assert!(eventually(WAIT, || count(run.owner, "a:update") == 2));
    let turns = run.shared.status().turns;
    assert!(eventually(WAIT, || run.shared.status().turns >= turns + 2));
    assert_eq!(failed(), 2, "{:?}", logs(run.owner));
    run.stop();
}

// ---- each VM's typed values (kiln.eml.values) ----

/// A package's `__gc` probe: records `gc<tag>` when its VM closes.
fn gc_probe(tag: &str) -> String {
    format!("keep = setmetatable({{}}, {{ __gc = function() record('gc{tag}') end }}) ")
}

/// Each text occurs in `seen`, the first occurrences in this order.
fn in_order(seen: &[String], texts: &[&str]) {
    let mut previous: Option<(&str, usize)> = None;
    for text in texts {
        let at = seen
            .iter()
            .position(|r| r == text)
            .unwrap_or_else(|| panic!("never saw {text}: {seen:?}"));
        if let Some((before, was)) = previous {
            assert!(at > was, "{before} before {text}: {seen:?}");
        }
        previous = Some((text, at));
    }
}

/// No view was released before the arena made over it drained (the fake records each one).
fn no_violation(owner: KilnOwner) {
    let seen = records(owner);
    assert!(
        !seen.iter().any(|r| r.ends_with(":VIOLATION")),
        "a view released before its arena drained: {seen:?}"
    );
}

fn with_values(scripts: &[(&str, &str)]) -> Run {
    start_with(|s| s.values = true, scripts)
}

/// Runtime-create's VM gets its arena over its own view of the granted generation before any package is registered
/// (the families see it), with the typed table port; a wake from another thread is collected at the end of a turn
/// with the control token; the orderly leave closes and drains it after every on_unload, while the VM lives, and the
/// view is released only after the drain.
pub fn a_vm_s_arena_is_made_before_its_packages_collected_at_the_turn_end_and_drained_at_the_stop()
{
    let run = with_values(&[("a", &format!("{}{}", gc_probe(""), recorder("")))]);
    run.wait_for("a:update");
    in_order(
        &records(run.owner),
        &["arena1:create", "a:register values=true", "a:top", "a:load"],
    );
    let arenas = fake_archive::arenas(run.owner);
    assert_eq!(arenas.len(), 1, "{arenas:?}");
    assert_eq!(arenas[0].phase, KILN_VALUE_RUNTIME as u32);
    assert_eq!(fake_archive::seen(run.owner).views, [arenas[0].view]);
    assert!(!arenas[0].closed);
    // a close off the worker wakes the arena; the next turn's end collects it on the worker
    assert_eq!(count(run.owner, "arena1:collect"), 0);
    assert!(fake_archive::wake(run.owner, 1));
    run.wait_for("arena1:collect");
    run.stop();
    let seen = records(run.owner);
    in_order(
        &seen,
        &[
            "a:unload",
            "arena1:close",
            "arena1:drained",
            "arena1:view-released",
            "a:gc",
        ],
    );
    assert_eq!(count(run.owner, "arena1:close"), 1, "{seen:?}");
    no_violation(run.owner);
}

/// A reload's new VM gets a fresh arena over a fresh view before its packages are registered; after the swap the
/// old VM's arena is closed and drained while the old VM still lives, then the old VM closes (its `__gc`), and only
/// then is its view released. The new arena stays open (and collects its wakes) until the stop drains it.
pub fn a_reload_s_new_vm_gets_a_fresh_arena_and_the_old_one_drains_before_its_vm_goes() {
    let run = with_values(&[("a", &format!("{}{}", gc_probe(""), recorder("")))]);
    run.wait_for("a:update");
    set_script(
        run.owner,
        "a",
        &format!("{}{}", gc_probe("2"), recorder("2")),
    );
    run.request(&json!({"requestId": "r"}));
    assert_eq!(run.answer("r")["status"], "reloaded");
    run.wait_for("a:update2");
    let seen = records(run.owner);
    in_order(
        &seen,
        &[
            "a:unload",
            "arena2:create",
            "a:top2",
            "a:load2",
            "arena1:close",
            "arena1:drained",
            "a:gc",
        ],
    );
    in_order(&seen, &["arena1:drained", "arena1:view-released"]);
    assert_eq!(count(run.owner, "a:register values=true"), 2, "{seen:?}");
    assert_eq!(count(run.owner, "arena2:close"), 0, "{seen:?}");
    let arenas = fake_archive::arenas(run.owner);
    assert_eq!(arenas.len(), 2, "{arenas:?}");
    assert_ne!(arenas[0].control, arenas[1].control);
    assert_ne!(arenas[0].view, arenas[1].view, "a view of its own");
    assert!(!fake_archive::wake(run.owner, 1), "the old arena is closed");
    assert!(fake_archive::wake(run.owner, 2));
    run.wait_for("arena2:collect");
    run.stop();
    let seen = records(run.owner);
    in_order(
        &seen,
        &[
            "a:unload2",
            "arena2:close",
            "arena2:drained",
            "arena2:view-released",
            "a:gc2",
        ],
    );
    no_violation(run.owner);
}

/// Kiln's refusal of a reload (decided before anything is unloaded) makes no arena; a ShroudForge failure after the
/// new VM's arena was made (its setup fails, or a requested package does not become active in it) closes and drains
/// that arena and releases its view; the running VM's arena stays open throughout, until the stop.
pub fn a_refused_reload_makes_no_arena_and_a_failed_one_drains_the_new_vm_s() {
    let run = start_with(
        |s| {
            s.values = true;
            s.restart_only = vec![("b".into(), reload::RESTART_NATIVE.into())];
        },
        &[("a", &recorder("")), ("b", &recorder(""))],
    );
    run.wait_for("b:update");
    run.request(&json!({"requestId": "kiln", "modIds": ["a"]}));
    assert_eq!(
        run.answer("kiln")["message"],
        reload::failed_to_activate("b", reload::RESTART_NATIVE)
    );
    assert_eq!(
        count(run.owner, "arena2:create"),
        0,
        "{:?}",
        records(run.owner)
    );
    // the new VM's setup fails after its arena was made
    {
        let mut scenarios = SCENARIOS.lock().unwrap();
        let s = scenarios.get_mut(&run.owner).unwrap();
        s.restart_only.clear();
        s.rebuild_drops = vec!["b".into()];
    }
    run.request(&json!({"requestId": "setup"}));
    assert_eq!(
        run.answer("setup")["message"],
        "updated runtime initialization failed: planned package is not discovered: b"
    );
    in_order(
        &records(run.owner),
        &[
            "arena2:create",
            "arena2:close",
            "arena2:drained",
            "arena2:view-released",
        ],
    );
    // a requested package does not become active in the new VM: sfi-core stops it, its arena drains
    SCENARIOS
        .lock()
        .unwrap()
        .get_mut(&run.owner)
        .unwrap()
        .rebuild_drops
        .clear();
    set_script(
        run.owner,
        "b",
        "record('top3') return { on_load = function() error('not now') end }",
    );
    run.request(&json!({"requestId": "active", "modIds": ["b"]}));
    let answer = run.answer("active");
    assert_eq!(answer["status"], "error");
    assert!(
        answer["message"]
            .as_str()
            .unwrap()
            .starts_with("updated runtime mod 'b' failed to activate: "),
        "{answer}"
    );
    let seen = records(run.owner);
    in_order(
        &seen,
        &[
            "arena3:create",
            "b:top3",
            "arena3:close",
            "arena3:drained",
            "arena3:view-released",
        ],
    );
    assert_eq!(count(run.owner, "arena1:close"), 0, "{seen:?}");
    assert!(eventually(WAIT, || run.shared.status().mods_active == 2));
    run.stop();
    in_order(&records(run.owner), &["arena1:close", "arena1:drained"]);
    no_violation(run.owner);
}

/// kiln-sfi's own stop during the rebuild: the abandoned new VM's arena is closed and drained by the orderly leave
/// after that VM's on_unload and before it closes; the old runtime's arena is drained at the end of the leave.
pub fn a_stop_during_the_rebuild_drains_the_abandoned_vm_s_arena_after_its_on_unload() {
    let run = with_values(&[
        ("a", &format!("{}{}", gc_probe(""), recorder(""))),
        ("b", &recorder("")),
    ]);
    run.wait_for("b:update");
    set_script(
        run.owner,
        "a",
        &format!("{}{}", gc_probe("2"), recorder("2")),
    );
    set_script(run.owner, "b", "record('stop2') host_stop() return {}");
    run.request(&json!({"requestId": "r"}));
    assert!(run.finished());
    let seen = records(run.owner);
    in_order(
        &seen,
        &[
            "arena2:create",
            "a:load2",
            "a:unload2",
            "arena2:close",
            "arena2:drained",
            "arena1:close",
            "arena1:drained",
        ],
    );
    in_order(&seen, &["arena2:drained", "a:gc2"]);
    in_order(&seen, &["arena1:drained", "a:gc"]);
    assert_eq!(run.shared.status().reason, StopReason::StopRequested);
    no_violation(run.owner);
}

/// The fast leaves forget every arena with its VM: the pool's cancellation while the worker runs, during a
/// rebuild (the new VM's arena too) and during the orderly leave that stops an abandoned VM. No arena is closed or
/// collected, no view is released, no `__gc` runs.
pub fn a_fast_leave_closes_no_arena_and_releases_no_view() {
    // while running
    let run = with_values(&[("a", &format!("{}{}", gc_probe(""), recorder("")))]);
    run.wait_for("a:update");
    run.pool.cancel(run.shared.task());
    assert!(run.finished());
    let forgotten = |run: &Run, arenas: usize| {
        let seen = records(run.owner);
        for text in [":close", ":collect", ":drained", ":view-released"] {
            assert!(
                !seen
                    .iter()
                    .any(|r| r.starts_with("arena") && r.ends_with(text)),
                "{text}: {seen:?}"
            );
        }
        assert!(!seen.iter().any(|r| r.starts_with("a:gc")), "{seen:?}");
        assert_eq!(fake_archive::arenas(run.owner).len(), arenas);
        assert!(fake_archive::seen(run.owner).released.is_empty());
    };
    forgotten(&run, 1);
    // during a rebuild: the new VM was built with its arena, then the cancellation
    let run = with_values(&[("a", &format!("{}{}", gc_probe(""), recorder("")))]);
    run.wait_for("a:update");
    set_script(
        run.owner,
        "a",
        &format!(
            "{}record('top2') hold_until('go') return {{ on_load = function() record('load2') end }}",
            gc_probe("2")
        ),
    );
    run.request(&json!({"requestId": "r"}));
    run.wait_for("holding:go");
    run.pool.cancel(run.shared.task());
    run.flip("go");
    assert!(run.finished());
    in_order(&records(run.owner), &["arena2:create", "a:top2"]);
    forgotten(&run, 2);
    // during the orderly leave that stops the abandoned VM
    let run = with_values(&[
        ("a", &format!("{}{}", gc_probe(""), recorder(""))),
        ("b", &recorder("")),
    ]);
    run.wait_for("b:update");
    set_script(
        run.owner,
        "a",
        &format!(
            "{}record('top2') return {{ on_load = function() record('load2') end, \
             on_unload = function() record('unload2') hold_until('leave') end }}",
            gc_probe("2")
        ),
    );
    set_script(run.owner, "b", "record('stop2') host_stop() return {}");
    run.request(&json!({"requestId": "r"}));
    run.wait_for("holding:leave");
    run.pool.cancel(run.shared.task());
    run.flip("leave");
    assert!(run.finished());
    assert_eq!(count(run.owner, "a:unload2"), 1);
    forgotten(&run, 2);
}

/// An interpreter stop (ShroudForge's update failure: a panic out of the turn) leaves in order: the arena drains after
/// the on_unloads, as at any orderly stop.
pub fn an_interpreter_stop_drains_the_arena_after_the_on_unloads() {
    let run = with_values(&[(
        "a",
        &format!(
            "{}local once = false return {{ on_update = function() \
               if once then explode() end once = true record('update') end, \
               on_unload = function() record('unload') end }}",
            gc_probe("")
        ),
    )]);
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.detail.as_deref(), Some(UPDATE_FAILED), "{status:?}");
    in_order(
        &records(run.owner),
        &[
            "a:update",
            "a:unload",
            "arena1:close",
            "arena1:drained",
            "arena1:view-released",
            "a:gc",
        ],
    );
    no_violation(run.owner);
}

/// A VM whose typed values cannot be set up is not made: an arena the provider refuses fails runtime-create with
/// the provider's answer before any package code (the view acquired for it is released); a setup that fails after
/// the arena was made closes and drains it before the VM goes.
pub fn a_vm_whose_typed_values_cannot_be_set_up_fails_its_setup() {
    let run = start_with(
        |s| {
            s.values = true;
            s.prepare =
                Some(|owner| fake_archive::values_create_answer(owner, Some(KILN_ERR_UNAVAILABLE)));
        },
        &[("a", &recorder(""))],
    );
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
    let detail = status.detail.unwrap_or_default();
    assert!(
        detail.contains(&format!(
            "typed values (kiln.eml.values): kiln.eml.values.arena_create refused (KilnResult {KILN_ERR_UNAVAILABLE})"
        )),
        "{detail}"
    );
    assert_eq!(count(run.owner, "a:top"), 0);
    assert_eq!(count(run.owner, "arena1:create-refused"), 1);
    let views = fake_archive::seen(run.owner);
    assert_eq!(
        views.released, views.views,
        "the view goes with the refusal"
    );
    // the setup fails after the arena was made: it drains before the VM goes, no package code ran
    let run = with_values(&[("a", &recorder("")), ("unregistrable", "")]);
    assert!(run.finished());
    assert_eq!(run.shared.status().reason, StopReason::CreateFailed);
    in_order(
        &records(run.owner),
        &[
            "arena1:create",
            "arena1:close",
            "arena1:drained",
            "arena1:view-released",
        ],
    );
    assert_eq!(count(run.owner, "a:top"), 0);
    no_violation(run.owner);
}

/// An arena that never drains (the provider keeps a host token) is logged at the stop, and its view is never
/// released (the provider may still use it), as its reader is never freed.
pub fn an_arena_that_does_not_drain_is_logged_and_keeps_its_view() {
    let run = start_with(
        |s| {
            s.values = true;
            s.prepare = Some(|owner| fake_archive::values_never_drain(owner, true));
        },
        &[("a", &recorder(""))],
    );
    run.wait_for("a:update");
    run.stop();
    let seen = records(run.owner);
    assert_eq!(count(run.owner, "arena1:close"), 1, "{seen:?}");
    assert_eq!(count(run.owner, "arena1:drained"), 0, "{seen:?}");
    assert_eq!(count(run.owner, "arena1:view-released"), 0, "{seen:?}");
    let lines = logs(run.owner);
    for text in [
        "kiln-sfi: a VM's typed values did not drain; their reader and archive view are kept for good",
        "kiln-sfi: typed values: kiln.eml.values: arena still has 1 pending after close",
    ] {
        assert!(
            lines
                .iter()
                .any(|(level, t)| *level == KILN_LOG_ERROR && t == text),
            "{text}: {lines:?}"
        );
    }
}

/// The view a VM's arena is made over is acquired only when kiln.caps reports `eml.archive.metadata` and its member
/// rows AVAILABLE (the header: a consumer checks the group and member rows before each use): a withheld row fails the
/// VM's setup before any view is acquired or arena made, and no package code runs.
pub fn a_withheld_archive_row_refuses_the_arena_s_view_before_it_is_acquired() {
    let run = start_with(
        |s| {
            s.values = true;
            s.withhold = Some("eml.archive.metadata.acquire");
        },
        &[("a", &recorder(""))],
    );
    assert!(eventually(WAIT, || {
        run.pool.task(0).body_returned() || count(run.owner, "a:top") > 0
    }));
    assert_eq!(count(run.owner, "a:top"), 0, "{:?}", records(run.owner));
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
    let detail = status.detail.unwrap_or_default();
    assert!(
        detail.contains(
            "typed values (kiln.eml.values): the archive view: capability eml.archive.metadata.acquire is not available"
        ),
        "{detail}"
    );
    assert!(
        fake_archive::seen(run.owner).views.is_empty(),
        "no view acquired"
    );
    assert!(fake_archive::arenas(run.owner).is_empty(), "no arena made");
}

/// An arena the provider admitted without a control token (a provider fault) can never be closed or drained: its
/// reader is kept for good, and so is the view it was made over (never released under a live reader). The VM's setup
/// fails before any package code.
pub fn an_arena_admitted_without_a_control_keeps_its_view() {
    let run = start_with(
        |s| {
            s.values = true;
            s.prepare = Some(|owner| fake_archive::values_create_no_control(owner, true));
        },
        &[("a", &recorder(""))],
    );
    assert!(run.finished());
    let status = run.shared.status();
    assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
    let detail = status.detail.unwrap_or_default();
    assert!(
        detail.contains("arena_create returned no control token"),
        "{detail}"
    );
    assert_eq!(count(run.owner, "arena1:create-no-control"), 1);
    assert_eq!(count(run.owner, "a:top"), 0);
    assert!(
        fake_archive::seen(run.owner).released.is_empty(),
        "the view stays with the admitted arena: {:?}",
        records(run.owner)
    );
    no_violation(run.owner);
}

/// A review's warning: the count hook can raise the watchdog's error inside a reader callback, where the
/// provider may swallow it (Source's struct get turns a failed field read into MissingField). Here the provider reads
/// a field whose `__index` runs past the 10 s budget, gets the reader's error object, and answers a plain nil: the
/// call returns normally, and its end still fails it (the watchdog's own account, not the error value), so the mod is
/// stopped and never runs again.
pub fn the_watchdog_stops_a_call_that_overran_inside_a_reader_callback_whose_error_was_swallowed() {
    let run = start_with(
        |s| {
            s.values = true;
            s.prepare = Some(|owner| fake_archive::values_open_reads(owner, Some("x")));
        },
        &[(
            "a",
            "local done = false \
             return { on_update = function() \
                 if done then record('ran again') return end \
                 done = true \
                 local v = open_typed(setmetatable({}, { __index = function() advance(10001) while true do end end })) \
                 record('returned ' .. tostring(v)) \
               end, on_unload = function() record('unload') end }",
        )],
    );
    run.wait_for("a:returned nil");
    assert!(
        eventually(Duration::from_secs(20), || count(run.owner, "a:unload")
            == 1),
        "a was not stopped: {:?}",
        records(run.owner)
    );
    in_order(
        &records(run.owner),
        &["arena1:read:error", "a:returned nil", "a:unload"],
    );
    let warned = logs(run.owner).iter().any(|(level, text)| {
        *level == KILN_LOG_WARN && text.contains("mod 'a' stopped") && text.contains("watchdog")
    });
    assert!(warned, "{:?}", logs(run.owner));
    let status = run.shared.status();
    assert_eq!(
        (status.mods_active, status.mods_failed),
        (0, 1),
        "{status:?}"
    );
    assert_eq!(
        status.phase,
        WorkerPhase::Running,
        "the worker keeps running"
    );
    run.stop();
    assert_eq!(
        count(run.owner, "a:ran again"),
        0,
        "{:?}",
        records(run.owner)
    );
    no_violation(run.owner);
}

fn identity() -> state::Identity {
    state::Identity {
        launch_id: Some(7),
        plan_hash: Some([0xaa; 32]),
        pid: std::process::id(),
        process_start_ms: 1,
        side: state::Side::Server,
        profile: None,
        backend: state::Backend::Kiln,
        reason_code: "plan".into(),
        reason_text: None,
        frozen_at_ms: Some(1),
    }
}

/// The result write is ShroudForge's atomic replace: the old file is replaced whole and no temp file is left
/// behind. (A reader holding the old file, natively and under Wine, is tests/reload_windows.rs.)
pub fn the_result_replaces_the_file_atomically() {
    let dir = fresh_dir("rl1-result");
    std::fs::write(dir.join(RESULT_FILE), b"old").unwrap();
    reload::write_result(&dir, b"new").unwrap();
    assert_eq!(std::fs::read(dir.join(RESULT_FILE)).unwrap(), b"new");
    let names: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    assert_eq!(names, [RESULT_FILE]);
    // a folder that is not there yet is made (write_json_atomically.rs:6)
    let nested = dir.join("kiln").join("sfi").join("runtime");
    reload::write_result(&nested, b"x").unwrap();
    assert_eq!(std::fs::read(nested.join(RESULT_FILE)).unwrap(), b"x");
    assert_eq!(
        reload::runtime_dir(Path::new("g")),
        Path::new("g").join("kiln").join("sfi").join("runtime")
    );
}
