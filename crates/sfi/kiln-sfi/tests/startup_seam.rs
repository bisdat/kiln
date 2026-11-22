// SPDX-License-Identifier: MIT
//! kiln-sfi's side of the kiln.eml.startup v1 handshake with kiln-eml, against a fake seam built from kiln-abi's
//! types (support/fake_startup.rs; the provider is kiln-eml's). The early phase writes sfi.json's `starting` first, then
//! attaches with kiln.launch's key and the native plan's DLL count, which wires the native run's witness to the seam
//! (one `witness_wait` of 2 s); the run's terminal goes back once it is known; Kiln's plugin step reads the outcome once,
//! cross-checks it, grants the archive generation and hands the verdict to the worker, whose runtime-create is refused
//! without a grant.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use fake_startup::{Config, OutcomeAnswer, RecheckAnswer, WitnessAnswer, committed, left_out};
use kiln_abi::services::kiln_eml_startup::*;
use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::native_manager::arming::{self, LoaderBody, TerminalPush, WitnessPort};
use kiln_sfi::native_manager::executor::{
    NativeRun, NativeTerminal, Outcome, TerminalRecord, Witness, WitnessToken,
};
use kiln_sfi::native_manager::plan::NativePlan;
use kiln_sfi::native_manager::selection::{Mode, NativePermission, NativePolicy, Withdrawn};
use kiln_sfi::plan_source::{self, KilnPlanSource, StartupVerdict};
use kiln_sfi::startup_seam::{self, Seam, SeamWitness};
use kiln_sfi::state::{Data, Lua, Part, Phase as RecordPhase, Presence};
use kiln_sfi::worker::{Worker, WorkerPhase};
use kiln_sfi::{Boundary, InactiveReason, NativeHooks, Phase, lifecycle};
use serde_json::{Value, json};
use sfi_policy::digest::package_content_sha256;
use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound, not a latency claim (300 s): a passing wait returns as soon as its condition holds.
const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
/// The launch id the fake core's kiln.launch freezes.
const LAUNCH_ID: u64 = 0x0123_4567_89ab_cdef;
const ALWAYS: u32 = KILN_LAUNCH_NATIVE_POLICY_ALWAYS as u32;
const NEVER: u32 = KILN_LAUNCH_NATIVE_POLICY_NEVER as u32;

// ---- packages and plans ----

fn put_package(game: &Path, folder: &str, id: &str, lua: &str) -> [u8; 32] {
    let dir = game.join("mods").join(folder);
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(
        dir.join("mod.json"),
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]}).to_string(),
    )
    .unwrap();
    std::fs::write(dir.join("src").join("mod.lua"), lua).unwrap();
    package_content_sha256(&dir).unwrap()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn candidate(id: &str, folder: &str, content: [u8; 32]) -> Value {
    json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{folder}"),
      "content_sha256":hex(&content),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client","server"],
      "capabilities":["runtime"],"dependencies":[],"requiredApis":[],"services":[],"requires180":false,
      "saveSensitive":false,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]})
}
fn depends_on(mut package: Value, on: &[&str]) -> Value {
    package["dependencies"] = on
        .iter()
        .map(|id| json!({"id":id,"range":"^1.0","optional":false,"included":true}))
        .collect();
    package
}
fn with_data(mut package: Value) -> Value {
    package["capabilities"] = json!(["runtime", "patch"]);
    package["data"] = json!({"required":true,"admission":"admit"});
    package
}
fn with_dll(mut package: Value) -> Value {
    package["dlls"] = json!([{"path":"native/x.dll","sha256":"e".repeat(64),"mode":"library","source":"ini",
        "required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]);
    package
}
fn plan(packages: Vec<Value>, order: &[&str], dialect: &str) -> Value {
    let mut plan = plan_document("client");
    plan["packages"] = Value::Array(packages);
    plan["loadOrder"] = json!(order);
    plan["startupDialect"] = json!(dialect);
    plan
}
fn use_plan(document: &Value) {
    set_plan(Some(serde_json::to_vec(document).unwrap()));
}
/// One Lua package `alpha` that logs when it runs.
fn alpha_plan() -> Value {
    let content = put_package(
        &game_dir(),
        "a",
        "alpha",
        "shroudforge.log.info('alpha:ran')",
    );
    let document = plan(vec![candidate("alpha", "a", content)], &["alpha"], "none");
    use_plan(&document);
    document
}

// ---- a start ----

thread_local! {
    static SPAWNS: Cell<usize> = const { Cell::new(0) };
    /// Whether sfi.json said `starting` and the seam was attached when the loader thread was asked for.
    static AT_SPAWN: Cell<Option<(bool, bool)>> = const { Cell::new(None) };
    static STATE: std::cell::RefCell<Option<PathBuf>> = const { std::cell::RefCell::new(None) };
}
/// A spawn that notes what was true when it was asked, and runs the body on a thread of its own.
fn noting_spawn(body: LoaderBody) -> Result<(), String> {
    SPAWNS.set(SPAWNS.get() + 1);
    let starting = STATE.with_borrow(|dir| {
        dir.as_ref()
            .and_then(|d| std::fs::read_to_string(d.join("sfi.json")).ok())
            .is_some_and(|t| t.contains("\"starting\""))
    });
    AT_SPAWN.set(Some((starting, fake_startup::seen().is_some())));
    std::thread::spawn(body);
    Ok(())
}
fn abandoning_loader(
    run: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    Box::new(move || run.abandon("the test loader loads nothing"))
}
fn panicking_loader(
    _: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    Box::new(|| std::panic::resume_unwind(Box::new("the test loader fails")))
}
const NOTED: NativeHooks = NativeHooks {
    witness: arming::not_wired,
    spawn: noting_spawn,
    loader: abandoning_loader,
    ..NativeHooks::PRODUCTION
};

struct Boot {
    boundary: &'static Boundary,
    worker: &'static Worker,
    early: KilnOwner,
    normal: KilnOwner,
    pool: Arc<FakePool>,
    info: KilnPluginInfo,
    state: PathBuf,
    /// sfi.json right after the early phase.
    after_early: Option<Value>,
    /// What the seam saw right after Kiln's plugin step (before on_ready).
    after_main: Option<fake_startup::Seen>,
}
impl Boot {
    fn lines(&self) -> Vec<(i32, String)> {
        logs(self.normal)
    }
    fn has(&self, text: &str) -> bool {
        self.lines().iter().any(|(_, l)| l.contains(text))
    }
    fn running(&self) -> bool {
        eventually(WAIT, || {
            self.worker
                .shared()
                .is_some_and(|s| s.phase() == WorkerPhase::Running)
        })
    }
    fn create_failed(&self) -> bool {
        eventually(WAIT, || {
            self.worker
                .shared()
                .is_some_and(|s| matches!(s.phase(), WorkerPhase::Stopping | WorkerPhase::Stopped))
        })
    }
    fn stop(&self) {
        if let Some(shared) = self.worker.shared() {
            shared.request_stop();
            assert!(self.pool.task(0).wait_finished(WAIT));
        }
    }
    fn record(&self) -> kiln_sfi::state::Reducer {
        self.boundary.status().record().expect("a record")
    }
    fn file(&self) -> Value {
        serde_json::from_slice(&std::fs::read(self.state.join("sfi.json")).unwrap()).unwrap()
    }
    fn seen(&self) -> fake_startup::Seen {
        fake_startup::seen().expect("an attach on this thread")
    }
}

struct Case {
    /// The start kiln-eml has (None: nothing pending, an attach is UNAVAILABLE).
    config: Option<Config>,
    resolve: kiln_sfi::startup_seam::Resolve,
    hooks: NativeHooks,
    /// The record's native policy and build_verified, build_info's verified.
    native: (u32, u32, bool),
    skip_early: bool,
    /// The state folder (default: a fresh one).
    state: Option<PathBuf>,
    /// Runs between Kiln's plugin step and its on_ready.
    before_ready: Option<fn()>,
    /// Grant fake_archive what the seam served (as kiln-eml's archive would).
    archive: bool,
}
impl Default for Case {
    fn default() -> Self {
        Self {
            config: None,
            resolve: fake_startup::resolve,
            hooks: NativeHooks::PRODUCTION,
            native: (ALWAYS, 1, true),
            skip_early: false,
            state: None,
            before_ready: None,
            archive: true,
        }
    }
}
/// A start whose kiln-eml grants the plan this thread serves.
fn granted() -> Case {
    Case {
        config: Some(Config::granting_current()),
        ..Case::default()
    }
}

fn boot(case: Case) -> Boot {
    set_launch(Some(KILN));
    set_native(case.native.0, case.native.1, case.native.2);
    let state = case
        .state
        .clone()
        .unwrap_or_else(|| fresh_dir("g14c-state"));
    set_state_dir(state.clone());
    STATE.set(Some(state.clone()));
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_native(case.hooks)
            .with_startup(case.resolve),
    ));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let api = Box::leak(api(true));
    fake_archive::wrap(api);
    match case.config {
        Some(config) => fake_startup::install(config),
        None => fake_startup::uninstall(),
    }
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    let mut after_early = None;
    if !case.skip_early {
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
            KILN_OK
        );
        after_early = std::fs::read(state.join("sfi.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok());
    }
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut info) },
        KILN_OK
    );
    let after_main = fake_startup::seen();
    if case.archive {
        fake_startup::grant_archive(normal);
    }
    if let Some(before) = case.before_ready {
        before();
    }
    if let Some(ready) = info.on_ready {
        unsafe { ready(info.user) };
    }
    Boot {
        boundary,
        worker,
        early,
        normal,
        pool,
        info,
        state,
        after_early,
        after_main,
    }
}

/// The runtime-create was refused with `code` (and `said`): the loud warning, a refused record, no Lua, no view.
fn create_refused(b: &Boot, code: &str, said: &str) {
    assert!(
        b.create_failed(),
        "{code}: {:?}",
        b.worker.shared().map(|s| s.status())
    );
    assert!(
        b.lines().iter().any(|(level, l)| *level == KILN_LOG_ERROR
            && l.starts_with("kiln-sfi: WARNING: Kiln's interpreter does not run this start")
            && l.contains(said)),
        "{code}: {:?}",
        b.lines()
    );
    assert!(
        eventually(WAIT, || b.file()["phase"] == "refused"),
        "{code}: {}",
        b.file()
    );
    let v = b.file();
    assert_eq!(v["reason"]["code"], code, "{v}");
    assert!(v["reason"]["text"].as_str().unwrap().contains(said), "{v}");
    assert!(!b.has(":ran"), "{code}: no Lua: {:?}", b.lines());
    assert!(
        fake_archive::seen(b.normal).calls.is_empty(),
        "{code}: no view acquired: {:?}",
        fake_archive::seen(b.normal).calls
    );
}

// ---- 1. the early phase: starting first, then the attach with kiln.launch's key, then the loader ----

#[test]
fn the_early_phase_writes_starting_then_attaches_with_kiln_launchs_key_before_it_arms() {
    let c = [1u8; 32];
    let document = plan(
        vec![
            with_dll(candidate("dll", "dll", c)),
            candidate("lua", "lua", c),
        ],
        &["dll", "lua"],
        "none",
    );
    use_plan(&document);
    SPAWNS.set(0);
    AT_SPAWN.set(None);
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        hooks: NOTED,
        ..Case::default()
    });
    // sfi.json said starting when the early phase returned
    assert_eq!(
        b.after_early.as_ref().map(|v| v["phase"].clone()),
        Some(json!("starting"))
    );
    let seen = b.seen();
    // attached once, with kiln.launch's launch id and plan hash and the plan's one DLL
    assert_eq!(seen.calls[0], "attach(1)");
    assert_eq!(seen.count("attach"), 1);
    assert_eq!(seen.launch_id, LAUNCH_ID);
    assert_eq!(
        seen.plan_sha256,
        sfi_policy::digest::sha256(&serde_json::to_vec(&document).unwrap())
    );
    assert!(b.boundary.startup_attached());
    // the loader was armed (the seam wires the witness) after starting was on disk and the attach was made
    assert_eq!(SPAWNS.get(), 1);
    assert_eq!(AT_SPAWN.get(), Some((true, true)));
    let run = b.boundary.native_run().unwrap().clone();
    assert!(
        run.wait(WAIT).not_armed.is_none(),
        "armed through the seam's witness"
    );
    b.stop();
}

#[test]
fn a_start_whose_early_phase_cannot_write_sfi_json_arms_and_attaches_nothing() {
    let document = alpha_plan();
    // the state folder is a file: no record can be written
    let blocked = fresh_dir("g14c-blocked").join("state");
    std::fs::write(&blocked, b"not a folder").unwrap();
    SPAWNS.set(0);
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        hooks: NOTED,
        state: Some(blocked),
        ..Case::default()
    });
    assert!(fake_startup::seen().is_none(), "no attach");
    assert!(!b.boundary.startup_attached());
    assert!(b.boundary.native_run().is_none(), "no run made, none armed");
    assert_eq!(SPAWNS.get(), 0);
    assert!(
        logs(b.early)
            .iter()
            .any(|(level, l)| *level == KILN_LOG_ERROR
                && l.contains("cannot be written in kiln-sfi's early phase")),
        "{:?}",
        logs(b.early)
    );
    // on_ready refuses the start: nothing submitted
    assert!(b.pool.submissions().is_empty());
    assert!(!b.boundary.worker_started());
    assert_eq!(b.record().phase(), RecordPhase::Refused);
    fake_startup::uninstall();
}

// ---- 2. the witness: one wait of 2 s, its answer decides the loads ----

fn bound(config: Config) -> Arc<Seam> {
    fake_startup::install(config);
    let seam = unsafe { Seam::bind(fake_startup::getter, LAUNCH_ID, [9; 32], [5; 16]) }.unwrap();
    assert!(seam.attach(2).is_ok());
    Arc::new(seam)
}
fn base_config() -> Config {
    Config {
        attach: KILN_OK,
        plan_id: [5; 16],
        witness: WitnessAnswer::Ready(41),
        recheck: RecheckAnswer::Unchanged,
        terminal: KILN_OK,
        outcome: OutcomeAnswer::Unavailable,
    }
}

#[test]
fn the_witness_is_one_wait_of_two_seconds_whose_answer_decides() {
    let long = "x".repeat(5000);
    for (answer, expect, waits) in [
        (WitnessAnswer::Ready(41), Ok(([5u8; 16], 41)), vec![2000]),
        (
            WitnessAnswer::ReadyFor([6; 16], 3),
            Err("names another launch plan".to_owned()),
            vec![2000],
        ),
        (
            WitnessAnswer::Refused("lock-busy: another game".into()),
            Err("lock-busy: another game".to_owned()),
            vec![2000],
        ),
        // a reason longer than the first buffer: asked again at once (timeout 0), whole
        (
            WitnessAnswer::Refused(long.clone()),
            Err(long.clone()),
            vec![2000, 0],
        ),
        (
            WitnessAnswer::Pending,
            Err("not ready within 2000 ms".to_owned()),
            vec![2000],
        ),
        (
            WitnessAnswer::Fail(KILN_ERR_UNAVAILABLE),
            Err("UNAVAILABLE".to_owned()),
            vec![2000],
        ),
    ] {
        let what = format!("{answer:?}");
        let seam = bound(Config {
            witness: answer,
            ..base_config()
        });
        let witness = SeamWitness::new(Arc::clone(&seam));
        let got = witness.ready();
        match (&got, &expect) {
            (Ok(token), Ok((plan_id, revision))) => {
                assert_eq!(
                    (token.plan_id, token.revision),
                    (*plan_id, *revision),
                    "{what}"
                );
                assert!(witness.issued() && !witness.refused(), "{what}");
            }
            (Err(text), Err(said)) => {
                assert!(text.contains(said.as_str()), "{what}: {text}");
                assert!(!witness.issued() && witness.refused(), "{what}");
            }
            _ => panic!("{what}: {got:?}"),
        }
        let seen = fake_startup::seen().unwrap();
        let asked: Vec<u32> = seen
            .calls
            .iter()
            .filter_map(|c| {
                c.strip_prefix("witness_wait(")?
                    .strip_suffix(')')?
                    .parse()
                    .ok()
            })
            .collect();
        assert_eq!(
            asked, waits,
            "{what}: never longer than 2 s, never more than one wait"
        );
    }
}

#[test]
fn the_recheck_says_unchanged_or_why_not() {
    let long = "y".repeat(6000);
    for (answer, expect) in [
        (RecheckAnswer::Unchanged, Ok(())),
        (
            RecheckAnswer::Changed("startup-input-changed: archive_001.dat".into()),
            Err("startup-input-changed: archive_001.dat".to_owned()),
        ),
        (RecheckAnswer::Changed(long.clone()), Err(long.clone())),
        // kiln-eml's wait for the terminal had ended: nothing confirms the input after the DLLs any more (fail closed;
        // a run that loaded no DLL is still coherent, plan_source::incoherent)
        (
            RecheckAnswer::Fail(KILN_ERR_UNAVAILABLE),
            Err("no longer answers".to_owned()),
        ),
        (
            RecheckAnswer::Fail(KILN_ERR_INTERNAL),
            Err("could not be asked (INTERNAL)".to_owned()),
        ),
    ] {
        let what = format!("{answer:?}");
        let seam = bound(Config {
            recheck: answer,
            ..base_config()
        });
        let witness = SeamWitness::new(seam);
        let token = witness.ready().unwrap();
        match (witness.recheck(&token), expect) {
            (Ok(()), Ok(())) => {}
            (Err(text), Err(said)) => assert!(text.contains(&said), "{what}: {text}"),
            (got, _) => panic!("{what}: {got:?}"),
        }
    }
    // a witness this session was never given is not rechecked (the provider refuses it)
    let seam = bound(base_config());
    let witness = SeamWitness::new(seam);
    let _ = witness.ready().unwrap();
    let other = WitnessToken {
        plan_id: [5; 16],
        revision: 42,
    };
    assert!(
        witness
            .recheck(&other)
            .unwrap_err()
            .contains("INVALID_ARGUMENT")
    );
}

#[test]
fn a_table_without_a_member_or_of_an_older_version_is_not_bound() {
    unsafe extern "C" fn none(_: u32) -> *const KilnEmlStartupApiV1 {
        std::ptr::null()
    }
    static SHORT: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
        header: KilnServiceHeader {
            size: 40,
            version: 1,
        },
        ..fake_startup::TABLE_V1
    };
    unsafe extern "C" fn short(_: u32) -> *const KilnEmlStartupApiV1 {
        &raw const SHORT
    }
    static NULL: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
        witness_recheck: None,
        ..fake_startup::TABLE_V1
    };
    unsafe extern "C" fn null(_: u32) -> *const KilnEmlStartupApiV1 {
        &raw const NULL
    }
    static OLD: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
        header: KilnServiceHeader {
            size: 48,
            version: 0,
        },
        ..fake_startup::TABLE_V1
    };
    unsafe extern "C" fn old(_: u32) -> *const KilnEmlStartupApiV1 {
        &raw const OLD
    }
    for (getter, said) in [
        (none as startup_seam::Getter, "does not serve version 1"),
        (short, "no outcome member"),
        (null, "no witness_recheck member"),
        (old, "version 0"),
    ] {
        let error = unsafe { Seam::bind(getter, LAUNCH_ID, [9; 32], [5; 16]) }
            .err()
            .unwrap();
        assert!(error.contains(said), "{error}");
    }
    // the getter refuses a version above 1
    assert!(unsafe { fake_startup::getter(2) }.is_null());
    // A kiln-eml from before published_identity was appended (a 48-byte table) has every member kiln-sfi binds
    static PRE_G14E: KilnEmlStartupApiV1 = KilnEmlStartupApiV1 {
        header: KilnServiceHeader {
            size: 48,
            version: 1,
        },
        ..fake_startup::TABLE_V1
    };
    unsafe extern "C" fn pre_g14e(_: u32) -> *const KilnEmlStartupApiV1 {
        &raw const PRE_G14E
    }
    assert!(unsafe { Seam::bind(pre_g14e, LAUNCH_ID, [9; 32], [5; 16]) }.is_ok());
}

// ---- 3. the native terminal goes back once it is known ----

fn record(mod_id: &str, path: &str, outcome: Outcome) -> TerminalRecord {
    TerminalRecord {
        status_id: format!("dll:{mod_id}:{path}"),
        mod_id: mod_id.into(),
        package_directory: mod_id.into(),
        path: path.into(),
        planned_sha256: "e".repeat(64),
        actual_sha256: None,
        mode: Mode::Library,
        source: "ini".into(),
        outcome,
    }
}
fn withdrawn(id: &str, reason: &str, chain: &[&str]) -> Withdrawn {
    Withdrawn {
        mod_id: id.into(),
        reason: reason.into(),
        caused_by: chain.iter().map(|s| s.to_string()).collect(),
    }
}

#[test]
fn the_terminal_json_has_the_schema_fields_in_order_and_fixed_codes() {
    let terminal = NativeTerminal {
        records: vec![
            record("ok", "a.dll", Outcome::Loaded { module: 1 }),
            record(
                "faults",
                "f.dll",
                Outcome::Faulted("DllMain returned FALSE".into()),
            ),
            record("slow", "s.dll", Outcome::TimedOut("did not finish".into())),
            record(
                "bad",
                "b.dll",
                Outcome::Refused("b.dll changed since the launch plan pinned it".into()),
            ),
            record(
                "bad",
                "c.dll",
                Outcome::Withdrawn("b.dll did not load".into()),
            ),
        ],
        withdrawn: vec![
            withdrawn("faults", "native DLL f.dll did not load", &[]),
            withdrawn("slow", "native DLL s.dll did not load", &[]),
            withdrawn("bad", "native DLL b.dll did not load", &[]),
            withdrawn("over", "depends on withdrawn package bad", &["bad"]),
        ],
        coherent: Err("archive_001.dat changed".into()),
        deadline_passed: true,
        not_armed: None,
    };
    let text = startup_seam::terminal_json([0x0a; 16], &terminal, None, false);
    let keys: Vec<&str> = [
        "\"format\"",
        "\"planId\"",
        "\"notArmed\"",
        "\"deadlinePassed\"",
        "\"coherent\"",
        "\"withdrawn\"",
        "\"dlls\"",
    ]
    .into_iter()
    .collect();
    let at: Vec<usize> = keys.iter().map(|k| text.find(k).unwrap()).collect();
    assert!(
        at.windows(2).all(|w| w[0] < w[1]),
        "fields in the schema's order: {text}"
    );
    let v: Value = serde_json::from_str(&text).unwrap();
    assert_eq!(v["format"], 1);
    assert_eq!(v["planId"], "0a0a0a0a-0a0a-0a0a-0a0a-0a0a0a0a0a0a");
    assert_eq!(
        (v["notArmed"].clone(), v["deadlinePassed"].clone()),
        (json!(false), json!(true))
    );
    assert_eq!(
        v["coherent"],
        json!({"ok": false, "text": "archive_001.dat changed"})
    );
    let codes: Vec<(&str, &str)> = v["withdrawn"]
        .as_array()
        .unwrap()
        .iter()
        .map(|w| (w["id"].as_str().unwrap(), w["code"].as_str().unwrap()))
        .collect();
    assert_eq!(
        codes,
        [
            ("faults", "dll-faulted"),
            ("slow", "dll-timed-out"),
            ("bad", "dll-refused"),
            ("over", "dependency-withdrawn")
        ]
    );
    for code in codes.iter().map(|(_, c)| *c) {
        assert!(startup_seam::WITHDRAWN_CODES.contains(&code), "{code}");
    }
    assert_eq!(v["withdrawn"][3]["chain"], json!(["bad"]), "nearest first");
    assert!(
        v["withdrawn"][2]["text"]
            .as_str()
            .unwrap()
            .contains("changed since the launch plan pinned it")
    );
    let outcomes: Vec<&str> = v["dlls"]
        .as_array()
        .unwrap()
        .iter()
        .map(|d| d["outcome"].as_str().unwrap())
        .collect();
    assert_eq!(
        outcomes,
        ["loaded", "faulted", "timed-out", "refused", "withdrawn"]
    );
    assert_eq!(
        v["dlls"][0],
        json!({"statusId": "dll:ok:a.dll", "modId": "ok", "outcome": "loaded"})
    );
    // closed objects: nothing else
    assert_eq!(v.as_object().unwrap().len(), 7);
    // a policy refusal, another reason not to arm, and a witness that never came
    let not_armed = |reason: &str| NativeTerminal {
        records: vec![record("dll", "x.dll", Outcome::Refused(reason.into()))],
        withdrawn: vec![withdrawn("dll", reason, &[])],
        coherent: Ok(()),
        deadline_passed: false,
        not_armed: Some(reason.into()),
    };
    let policy = r#"kiln.json native.policy is "never""#;
    let v: Value = serde_json::from_str(&startup_seam::terminal_json(
        [1; 16],
        &not_armed(policy),
        Some(policy),
        false,
    ))
    .unwrap();
    assert_eq!(
        (v["notArmed"].clone(), v["withdrawn"][0]["code"].clone()),
        (json!(true), json!("native-policy-refused"))
    );
    assert_eq!(v["coherent"], json!({"ok": true}));
    let v: Value = serde_json::from_str(&startup_seam::terminal_json(
        [1; 16],
        &not_armed("the thread could not start"),
        None,
        false,
    ))
    .unwrap();
    assert_eq!(v["withdrawn"][0]["code"], "native-not-armed");
    let mut unwitnessed = not_armed("x");
    unwitnessed.not_armed = None;
    unwitnessed.coherent = Err("no startup witness was issued".into());
    let v: Value = serde_json::from_str(&startup_seam::terminal_json(
        [1; 16],
        &unwitnessed,
        None,
        true,
    ))
    .unwrap();
    assert_eq!(v["withdrawn"][0]["code"], "witness-refused");
    // the same run whose loader stopped before it asked for the witness: the DLL's own outcome names it
    let v: Value = serde_json::from_str(&startup_seam::terminal_json(
        [1; 16],
        &unwitnessed,
        None,
        false,
    ))
    .unwrap();
    assert_eq!(v["withdrawn"][0]["code"], "dll-refused");
    // free text is clamped at 4096 characters on a character boundary
    let mut long = not_armed(&"é".repeat(5000));
    long.not_armed = Some("y".into());
    let v: Value =
        serde_json::from_str(&startup_seam::terminal_json([1; 16], &long, None, false)).unwrap();
    assert_eq!(
        v["withdrawn"][0]["text"].as_str().unwrap().chars().count(),
        4096
    );
}

#[test]
fn a_run_that_is_not_armed_pushes_its_terminal_from_the_early_phase() {
    let c = [1u8; 32];
    let document = plan(
        vec![
            with_dll(candidate("dll", "dll", c)),
            depends_on(candidate("over", "over", c), &["dll"]),
        ],
        &["dll", "over"],
        "none",
    );
    use_plan(&document);
    SPAWNS.set(0);
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        hooks: NOTED,
        native: (NEVER, 1, true),
        ..Case::default()
    });
    assert_eq!(SPAWNS.get(), 0, "never armed");
    let seen = b.after_main.clone().unwrap();
    // pushed in the early phase, before Kiln's plugin step read the outcome
    let terminal_at = seen
        .calls
        .iter()
        .position(|c| c == "native_terminal")
        .expect("a terminal");
    let outcome_at = seen
        .calls
        .iter()
        .position(|c| c.starts_with("outcome"))
        .expect("an outcome read");
    assert!(terminal_at < outcome_at, "{:?}", seen.calls);
    let v = seen.terminal_json().unwrap();
    assert_eq!(v["notArmed"], true);
    assert_eq!(
        v["dlls"],
        json!([{"statusId": "dll:dll:native/x.dll", "modId": "dll", "outcome": "refused"}])
    );
    assert_eq!(v["withdrawn"][0]["code"], "native-policy-refused");
    assert_eq!(
        (
            v["withdrawn"][1]["id"].clone(),
            v["withdrawn"][1]["chain"].clone()
        ),
        (json!("over"), json!(["dll"]))
    );
    assert_eq!(seen.count("native_terminal"), 1);
    b.stop();
}

/// A package that registers DLLs from its Lua (`runtime-register-dll`).
fn registers(mut package: Value) -> Value {
    package["capabilities"] = json!(["runtime", "runtime-register-dll"]);
    package
}

#[test]
fn a_run_that_is_not_armed_withdraws_every_package_admission_refuses_for_native_code() {
    let c = [1u8; 32];
    let game = game_dir();
    let lua = |folder: &str| put_package(&game, folder, folder, "shroudforge.log.info('ran')");
    for (native, packages, order, expect) in [
        // the policy refuses: the planned DLL's package, the package that registers DLLs from its Lua, and what depends
        // on them; an independent Lua package stays
        (
            (NEVER, 1, true),
            vec![
                with_dll(candidate("dll", "dll", lua("dll"))),
                registers(candidate("reg", "reg", lua("reg"))),
                depends_on(candidate("over", "over", lua("over")), &["reg"]),
                candidate("free", "free", lua("free")),
            ],
            vec!["dll", "reg", "over", "free"],
            vec![
                ("dll", "native-policy-refused", vec![]),
                ("reg", "native-policy-refused", vec![]),
                ("over", "dependency-withdrawn", vec!["reg"]),
            ],
        ),
        // no planned DLL: never armed, so a package that registers DLLs from its Lua cannot run either
        (
            (ALWAYS, 1, true),
            vec![
                registers(candidate("reg", "reg", lua("reg"))),
                depends_on(candidate("over", "over", lua("over")), &["reg"]),
                candidate("free", "free", c),
            ],
            vec!["reg", "over", "free"],
            vec![
                ("reg", "native-not-armed", vec![]),
                ("over", "dependency-withdrawn", vec!["reg"]),
            ],
        ),
    ] {
        let document = plan(packages, &order, "none");
        use_plan(&document);
        let b = boot(Case {
            config: Some(Config::granting(&document)),
            hooks: NOTED,
            native,
            ..Case::default()
        });
        let v = b
            .after_main
            .clone()
            .unwrap()
            .terminal_json()
            .expect("a terminal");
        assert_eq!(v["notArmed"], true);
        let got: Vec<(String, String, Vec<String>)> = v["withdrawn"]
            .as_array()
            .unwrap()
            .iter()
            .map(|w| {
                (
                    w["id"].as_str().unwrap().to_owned(),
                    w["code"].as_str().unwrap().to_owned(),
                    serde_json::from_value(w["chain"].clone()).unwrap(),
                )
            })
            .collect();
        let want: Vec<(String, String, Vec<String>)> = expect
            .iter()
            .map(|(id, code, chain)| {
                (
                    id.to_string(),
                    code.to_string(),
                    chain.iter().map(|s| s.to_string()).collect(),
                )
            })
            .collect();
        assert_eq!(got, want, "{v}");
        let reg = v["withdrawn"]
            .as_array()
            .unwrap()
            .iter()
            .find(|w| w["id"] == "reg")
            .unwrap();
        assert!(
            reg["text"]
                .as_str()
                .unwrap()
                .contains("registers DLLs from its Lua"),
            "{reg}"
        );
        // kiln-eml learns exactly the packages kiln-sfi's own admission removes for native code
        assert!(b.running());
        b.stop();
        let record = b.record();
        let removed: Vec<String> = order
            .iter()
            .filter(|id| {
                record.package(id).is_some_and(|row| {
                    row.skip.as_ref().is_some_and(|s| {
                        ["native-refused", "dependency-skipped"].contains(&s.code.as_str())
                    })
                })
            })
            .map(|id| id.to_string())
            .collect();
        assert_eq!(
            removed,
            got.iter().map(|(id, _, _)| id.clone()).collect::<Vec<_>>(),
            "{v}"
        );
    }
}

#[test]
fn an_armed_run_pushes_its_terminal_after_the_loader_even_when_the_loader_fails() {
    for (loader, said) in [
        (
            abandoning_loader
                as fn(
                    Arc<NativeRun>,
                    unsafe extern "C" fn(KilnOwner, i32, KilnStr),
                    KilnOwner,
                ) -> LoaderBody,
            "the test loader loads nothing",
        ),
        (panicking_loader, arming::LOADER_STOPPED),
    ] {
        let c = [1u8; 32];
        let document = plan(vec![with_dll(candidate("dll", "dll", c))], &["dll"], "none");
        use_plan(&document);
        let b = boot(Case {
            config: Some(Config::granting(&document)),
            hooks: NativeHooks { loader, ..NOTED },
            ..Case::default()
        });
        let run = b.boundary.native_run().unwrap().clone();
        let terminal = run.wait(WAIT);
        assert!(terminal.not_armed.is_none(), "{said}");
        assert!(eventually(WAIT, || b.seen().terminal.is_some()), "{said}");
        let v = b.seen().terminal_json().unwrap();
        assert_eq!(v["notArmed"], false, "{said}");
        assert_eq!(v["dlls"][0]["outcome"], "refused", "{said}");
        // the loader never asked the witness (it loads nothing): not coherent, but no witness was refused either, so the
        // code follows the DLL's own outcome
        assert_eq!(v["coherent"]["ok"], false, "{said}");
        assert_eq!(v["withdrawn"][0]["code"], "dll-refused", "{said}");
        assert!(
            v["withdrawn"][0]["text"].as_str().unwrap().contains(said),
            "{said}: {v}"
        );
        assert_eq!(b.seen().count("native_terminal"), 1);
        b.stop();
    }
}

/// The gate the hanging loader waits at (only this test's loader uses it).
static HUNG: (Mutex<bool>, std::sync::Condvar) = (Mutex::new(false), std::sync::Condvar::new());
fn hanging_loader(
    run: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    Box::new(move || {
        let (open, changed) = &HUNG;
        let mut open = open.lock().unwrap();
        while !*open {
            open = changed.wait(open).unwrap();
        }
        drop(open);
        run.abandon("the test loader loads nothing");
    })
}

/// The startup deadline through the boundary: an armed run whose loader hangs gets its terminal from the production deadline waiter
/// ("kiln-sfi native deadline") at the hooks' deadline, while the loader thread is still held: every DLL `timed-out`,
/// the DLL package `dll-timed-out` and its Lua-only dependent `dependency-withdrawn`, pushed once. Production's
/// deadline is ShroudForge's 15 s.
#[test]
fn an_armed_run_whose_loader_hangs_gets_its_terminal_at_the_deadline() {
    assert_eq!(NativeHooks::PRODUCTION.deadline, Duration::from_secs(15));
    let c = [1u8; 32];
    let document = plan(
        vec![
            with_dll(candidate("dll", "dll", c)),
            depends_on(candidate("lua", "lua", c), &["dll"]),
        ],
        &["dll", "lua"],
        "none",
    );
    use_plan(&document);
    let deadline = Duration::from_millis(200);
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        hooks: NativeHooks {
            loader: hanging_loader,
            deadline,
            ..NOTED
        },
        ..Case::default()
    });
    assert!(eventually(WAIT, || b.seen().terminal.is_some()));
    let seen = b.seen();
    assert_eq!(seen.pushed_on.as_deref(), Some(arming::DEADLINE_THREAD));
    let after = seen.pushed_at.unwrap() - seen.attached_at.unwrap();
    // the hooks' deadline, not production's 15 s
    assert!(
        after >= deadline && after < Duration::from_secs(14),
        "pushed {after:?} after the attach"
    );
    let v = seen.terminal_json().unwrap();
    assert_eq!(v["notArmed"], false, "{v}");
    assert_eq!(v["deadlinePassed"], true, "{v}");
    assert_eq!(v["dlls"][0]["outcome"], "timed-out", "{v}");
    let withdrawn: Vec<(String, String)> = v["withdrawn"]
        .as_array()
        .unwrap()
        .iter()
        .map(|w| {
            (
                w["id"].as_str().unwrap().to_owned(),
                w["code"].as_str().unwrap().to_owned(),
            )
        })
        .collect();
    assert_eq!(
        withdrawn,
        [
            ("dll".to_owned(), "dll-timed-out".to_owned()),
            ("lua".to_owned(), "dependency-withdrawn".to_owned())
        ],
        "{v}"
    );
    // the loader thread is still held; when it lets go, nothing is pushed again
    let run = b.boundary.native_run().unwrap().clone();
    {
        let (open, changed) = &HUNG;
        *open.lock().unwrap() = true;
        changed.notify_all();
    }
    assert_eq!(run.wait(WAIT), run.wait(Duration::ZERO));
    std::thread::sleep(Duration::from_millis(50));
    assert_eq!(b.seen().count("native_terminal"), 1);
    b.stop();
}

#[test]
fn a_second_push_never_reaches_kiln_eml() {
    let seam = bound(base_config());
    assert_eq!(seam.push_terminal(r#"{"dlls":[{},{}]}"#), KILN_OK);
    assert_eq!(
        seam.push_terminal(r#"{"dlls":[{},{}]}"#),
        KILN_ERR_UNAVAILABLE
    );
    assert_eq!(fake_startup::seen().unwrap().count("native_terminal"), 1);
}

// ---- 4. the outcome: read once at the plugin step, cross-checked, granted ----

#[test]
fn a_granted_outcome_is_read_once_before_the_worker_and_its_identity_acquires_the_view() {
    let document = alpha_plan();
    let b = boot(granted());
    // read at Kiln's plugin step, before on_ready submitted the worker: a sizing call and one read
    let at_main = b.after_main.clone().unwrap();
    let reads: Vec<&String> = at_main
        .calls
        .iter()
        .filter(|c| c.starts_with("outcome"))
        .collect();
    assert_eq!(reads.len(), 2, "{:?}", at_main.calls);
    assert_eq!(reads[0], "outcome(0)");
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[alpha] alpha:ran")),
        "{:?}",
        b.lines()
    );
    // never again (on_ready, runtime-create)
    assert_eq!(b.seen().count("outcome"), 2);
    // the identity granted is the one served; the view was acquired with it
    let served = fake_startup::served().unwrap();
    let mut launch = [0u8; 16];
    launch[8..].copy_from_slice(&LAUNCH_ID.to_be_bytes());
    assert_eq!(served.launch_id, launch);
    assert_eq!(served.plan_id, fake_startup::plan_id(&document));
    let archive = fake_archive::seen(b.normal);
    assert_eq!(
        &archive.calls[..2],
        ["acquire", "identity"],
        "{:?}",
        archive.calls
    );
    assert!(b.boundary.startup_outcome().unwrap().grant.is_some());
    let record = b.record();
    assert_eq!(record.phase(), RecordPhase::Running);
    b.stop();
}

#[test]
fn without_a_grant_the_runtime_create_is_refused_before_any_lua() {
    // no seam (an older kiln-eml)
    alpha_plan();
    let b = boot(Case {
        resolve: fake_startup::none,
        ..Case::default()
    });
    assert!(!b.boundary.startup_attached());
    assert!(
        b.info.on_ready.is_some(),
        "the plugin step does not refuse; the runtime-create does"
    );
    create_refused(&b, "startup-grant-missing", "kiln-eml has no startup seam");
    // the native run had no witness: never armed
    assert_eq!(
        b.boundary
            .native_run()
            .unwrap()
            .wait(Duration::ZERO)
            .not_armed
            .as_deref()
            .map(|r| r == arming::WITNESS_NOT_WIRED || r == arming::NO_DLLS),
        Some(true)
    );
    // kiln-eml refused the attach
    alpha_plan();
    let mut config = Config::granting_current();
    config.attach = KILN_ERR_UNAVAILABLE;
    let b = boot(Case {
        config: Some(config),
        ..Case::default()
    });
    create_refused(
        &b,
        "startup-attach-refused",
        "kiln-eml refused kiln-sfi's attach",
    );
    // the outcome is not there (kiln-eml's barrier did not finish)
    alpha_plan();
    let b = boot(Case {
        config: Some(Config::granting_current().with_outcome(OutcomeAnswer::Unavailable)),
        ..Case::default()
    });
    create_refused(&b, "startup-outcome-missing", "is not available");
    // the startup pass proved no live generation
    alpha_plan();
    let b = boot(Case {
        config: Some(
            Config::granting_current().with_outcome(OutcomeAnswer::Record {
                state: "failed",
                dialect: "none".into(),
                rows: vec![],
                edit: Some(|v, _| {
                    v["attempt"] = json!("none");
                }),
            }),
        ),
        ..Case::default()
    });
    create_refused(
        &b,
        "startup-grant-missing",
        "granted no archive generation this start (state failed)",
    );
    // the game's own data could not be put back: that warning's text (it says what the player does) is the reason
    // the launcher shows, over any other warning
    alpha_plan();
    let b = boot(Case {
        config: Some(
            Config::granting_current().with_outcome(OutcomeAnswer::Record {
                state: "failed",
                dialect: "none".into(),
                rows: vec![],
                edit: Some(|v, _| {
                    v["attempt"] = json!("none");
                    v["warnings"] = json!([
                        {"code": "base-not-restored", "text": "Kiln could not put the game's own data back: left as it is. Verify the game files in Steam\nsecond line"},
                        {"code": "later-note", "text": "a later note"},
                    ]);
                }),
            }),
        ),
        ..Case::default()
    });
    create_refused(
        &b,
        "startup-grant-missing",
        "(state failed: Kiln could not put the game's own data back: left as it is. Verify the game files in Steam), so",
    );
    // a refused witness (no attempt, no error): the warning that ended the start is the reason
    alpha_plan();
    let b = boot(Case {
        config: Some(
            Config::granting_current().with_outcome(OutcomeAnswer::Record {
                state: "refused",
                dialect: "none".into(),
                rows: vec![],
                edit: Some(|v, _| {
                    v["attempt"] = json!("none");
                    v["warnings"] = json!([
                        {"code": "baseline-shroudforge-output", "text": "the live enshrouded.kfc is what Kiln's bundled ShroudForge prepared. Verify the game files in Steam"},
                    ]);
                }),
            }),
        ),
        ..Case::default()
    });
    create_refused(
        &b,
        "startup-grant-missing",
        "(state refused: the live enshrouded.kfc is what Kiln's bundled ShroudForge prepared. Verify the game files in Steam)",
    );
    // the early phase never ran: no attach
    alpha_plan();
    let b = boot(Case {
        config: Some(Config::granting_current()),
        skip_early: true,
        ..Case::default()
    });
    create_refused(&b, "startup-attach-missing", "did not attach");
    fake_startup::uninstall();
}

#[test]
fn a_worker_without_a_grant_refuses_its_runtime_create() {
    // The production plan source never runs Lua without a granted archive generation,
    // whoever started the worker
    alpha_plan();
    set_launch(Some(KILN));
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = Box::leak(api(true));
    let record = unsafe { kiln_sfi::read_launch_record(api.service.unwrap()) };
    let frozen = match unsafe { plan_source::read_frozen(&*api, &record) } {
        plan_source::PlanRead::Frozen(frozen) => frozen,
        other => panic!("{other:?}"),
    };
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    assert!(worker.bind(unsafe { kiln_sfi::worker::Host::read(&*api) }, owner));
    worker.follow_plan(frozen);
    worker.start().unwrap();
    let shared = worker.shared().unwrap().clone();
    assert!(eventually(WAIT, || matches!(
        shared.phase(),
        WorkerPhase::Stopping | WorkerPhase::Stopped
    )));
    assert!(
        logs(owner)
            .iter()
            .any(|(level, l)| *level == KILN_LOG_ERROR && l.contains(plan_source::NO_GRANT)),
        "{:?}",
        logs(owner)
    );
    assert!(
        shared
            .status()
            .detail
            .unwrap()
            .starts_with("startup-grant-missing")
    );
    assert!(!logs(owner).iter().any(|(_, l)| l.contains("alpha:ran")));
    assert!(pool.task(0).wait_finished(WAIT));
}

#[test]
fn an_outcome_that_does_not_match_this_start_grants_nothing() {
    let edits: [(fake_startup::Edit, &str); 8] = [
        (|_, i| i.launch_id[15] ^= 1, "another launch id"),
        (|_, i| i.plan_id[0] ^= 1, "another plan id"),
        (|_, i| i.plan_sha256[3] ^= 1, "another plan hash"),
        (|_, i| i.phase = 1, "is not PUBLISHED"),
        (|_, i| i.archive_sha256[0] ^= 1, "another archive digest"),
        (
            |v, _| v["launchId"] = json!("0000000000000001"),
            "it names launch 0000000000000001",
        ),
        (
            |v, _| v["planId"] = json!("00000000-0000-4000-8000-000000000000"),
            "it names plan",
        ),
        (
            |v, _| v["planSha256"] = json!("0".repeat(64)),
            "its plan hash is not the one kiln.launch froze",
        ),
    ];
    for (edit, said) in edits {
        alpha_plan();
        let b = boot(Case {
            config: Some(
                Config::granting_current().with_outcome(OutcomeAnswer::Record {
                    state: "unchanged",
                    dialect: "none".into(),
                    rows: vec![],
                    edit: Some(edit),
                }),
            ),
            ..Case::default()
        });
        assert!(
            b.boundary.startup_outcome().unwrap().grant.is_none(),
            "{said}"
        );
        create_refused(&b, "startup-grant-mismatch", said);
    }
    // a grant whose identity is not zero when there is none, and a row for a package the plan does not list
    alpha_plan();
    let b = boot(Case {
        config: Some(
            Config::granting_current().with_outcome(OutcomeAnswer::Record {
                state: "refused",
                dialect: "none".into(),
                rows: vec![],
                edit: Some(|v, i| {
                    v["attempt"] = json!("none");
                    i.generation = 9;
                }),
            }),
        ),
        ..Case::default()
    });
    create_refused(
        &b,
        "startup-grant-mismatch",
        "is not zero for an outcome without a grant",
    );
}

#[test]
fn an_outcome_the_schema_refuses_grants_nothing() {
    let edits: [(fake_startup::Edit, &str); 6] = [
        (|v, _| v["extra"] = json!(1), "unknown member"),
        (|v, _| v["format"] = json!(2), "format is not 1"),
        (
            |v, _| v["grant"] = json!(false),
            "grant is false for state unchanged",
        ),
        (
            |v, _| v["native"] = json!({"received": false, "coherent": true}),
            "no terminal is never coherent",
        ),
        (
            |v, _| v["attempt"] = json!("retry"),
            "a retry is ShroudForge's",
        ),
        (
            |v, _| v["warnings"] = json!([{"code": "Bad Code", "text": "x"}]),
            "not a code word",
        ),
    ];
    for (edit, said) in edits {
        alpha_plan();
        let b = boot(Case {
            config: Some(
                Config::granting_current().with_outcome(OutcomeAnswer::Record {
                    state: "unchanged",
                    dialect: "none".into(),
                    rows: vec![],
                    edit: Some(edit),
                }),
            ),
            ..Case::default()
        });
        create_refused(&b, "startup-outcome-invalid", said);
    }
}

// ---- 5. the outcome's verdict: admission and sfi.json's rows ----

fn data_plan() -> Value {
    let game = game_dir();
    let mut packages = Vec::new();
    for (id, lua) in [
        ("base", true),
        ("addon", true),
        ("solo", true),
        ("over-addon", true),
    ] {
        let content = put_package(&game, id, id, &format!("shroudforge.log.info('{id}:ran')"));
        let mut p = candidate(id, id, content);
        if id != "over-addon" {
            p = with_data(p);
        }
        if !lua {
            p["lua"] = json!({"required":false,"admission":"not-needed"});
        }
        packages.push(p);
    }
    packages[1] = depends_on(packages[1].clone(), &["base"]);
    packages[3] = depends_on(packages[3].clone(), &["addon"]);
    let document = plan(
        packages,
        &["base", "addon", "solo", "over-addon"],
        "shroudforge",
    );
    use_plan(&document);
    document
}

#[test]
fn admission_and_sfi_json_follow_the_outcomes_rows() {
    let document = data_plan();
    // base failed in its own entrypoint, the retry dropped addon (needing base), solo committed
    let config = Config::granting(&document).with_outcome(OutcomeAnswer::Record {
        state: "committed",
        dialect: "shroudforge".into(),
        rows: vec![
            left_out(
                "base",
                "shroudforge",
                "failed",
                json!(false),
                "startup-failed",
                "base/init.lua:3: attempt to index nil",
                &[],
            ),
            left_out(
                "addon",
                "shroudforge",
                "excluded",
                json!(false),
                "dependency-skipped",
                "needs base, which failed in the pregame pass",
                &["base"],
            ),
            committed("solo", "shroudforge"),
        ],
        edit: Some(|v, _| {
            v["attempt"] = json!("retry");
            v["error"] = json!("pregame phase failed: base/init.lua:3: attempt to index nil");
        }),
    });
    let b = boot(Case {
        config: Some(config),
        ..Case::default()
    });
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[solo] solo:ran")),
        "{:?}",
        b.lines()
    );
    for id in ["base", "addon", "over-addon"] {
        assert!(!b.has(&format!("{id}:ran")), "{id}");
    }
    assert!(
        b.has("WARNING: ShroudForge package 'base' will not run: base/init.lua:3"),
        "{:?}",
        b.lines()
    );
    assert!(
        b.has("'over-addon' will not run: depends on withdrawn package addon (via addon -> base)"),
        "{:?}",
        b.lines()
    );
    assert!(eventually(WAIT, || b
        .record()
        .package("solo")
        .is_some_and(|p| p.lua == Lua::Active)));
    let record = b.record();
    let base = record.package("base").unwrap();
    assert_eq!(
        (base.data, base.archive_present),
        (Data::Failed, Some(false))
    );
    let skip = base.skip.as_ref().unwrap();
    assert_eq!(
        (skip.code.as_str(), skip.part),
        ("startup-failed", Part::Data)
    );
    assert_eq!(
        base.warning.as_ref().unwrap().presence,
        Presence::NotApplied
    );
    let addon = record.package("addon").unwrap();
    assert_eq!(
        (addon.data, addon.archive_present),
        (Data::Excluded, Some(false))
    );
    let skip = addon.skip.as_ref().unwrap();
    assert_eq!(
        (skip.code.as_str(), skip.chain.clone()),
        ("dependency-skipped", vec!["base".to_owned()])
    );
    let solo = record.package("solo").unwrap();
    assert_eq!(
        (solo.data, solo.archive_present, solo.skip.is_none()),
        (Data::Committed, Some(true), true)
    );
    let over = record.package("over-addon").unwrap();
    assert_eq!(
        over.skip
            .as_ref()
            .map(|s| (s.code.as_str(), s.chain.clone())),
        Some((
            "dependency-skipped",
            vec!["addon".to_owned(), "base".to_owned()]
        ))
    );
    b.stop();
}

#[test]
fn the_verdict_withdraws_with_the_outcomes_code_or_holds_without_one() {
    let document = data_plan();
    let parsed = sfi_policy::launch_plan::LaunchPlan::parse(
        &serde_json::to_vec(&document).unwrap(),
        &sfi_policy::launch_plan::ValidationContext {
            exe_sha256: FAKE_EXE.into(),
            side: "client".into(),
            upstream_180_final: false,
        },
    )
    .unwrap();
    let verdicts = |startup| plan_source::Verdicts {
        native: plan_source::NativeVerdict::Concluded {
            refused: Default::default(),
        },
        startup,
    };
    let a = plan_source::admission(
        &parsed,
        &verdicts(StartupVerdict::Committed {
            packages: ["solo".to_owned()].into(),
            refused: [(
                "base".to_owned(),
                plan_source::Refusal {
                    code: "plan-stale".into(),
                    text: "the archive input changed".into(),
                    chain: vec![],
                },
            )]
            .into(),
        }),
    );
    let w: Vec<(&str, &str, Vec<String>)> = a
        .withdrawn
        .iter()
        .map(|w| (w.id.as_str(), w.code.as_str(), w.chain.clone()))
        .collect();
    assert_eq!(
        w,
        [
            ("base", "plan-stale", vec![]),
            // not named: not applied
            ("addon", "startup-not-committed", vec![]),
            ("over-addon", "dependency-skipped", vec!["addon".to_owned()]),
        ]
    );
    assert_eq!(a.withdrawn[0].reason, "the archive input changed");
    // no outcome could be read: held with that reason
    let a = plan_source::admission(
        &parsed,
        &verdicts(StartupVerdict::Missing {
            code: "startup-outcome-missing".into(),
            text: "no outcome".into(),
        }),
    );
    assert_eq!(a.withdrawn[0].code, "startup-outcome-missing");
    assert!(a.withdrawn[0].reason.starts_with("no outcome"));
    assert!(a.admitted.is_empty());
}

#[test]
fn an_eml_dialect_outcome_that_names_kiln_emls_package_lets_its_dependents_run() {
    let game = game_dir();
    let uses = put_package(
        &game,
        "uses-eml",
        "uses-eml",
        "shroudforge.log.info('uses-eml:ran')",
    );
    // its folder is registered (a dependency of a running package), never run
    let data = put_package(
        &game,
        "eml-data",
        "eml-data",
        "shroudforge.log.info('eml-data:WRONG')",
    );
    let mut eml = candidate("eml-data", "eml-data", data);
    eml["kind"] = json!("eml");
    eml["capabilities"] = json!([]);
    eml["data"] = json!({"required":true,"admission":"admit"});
    eml["lua"] = json!({"required":false,"admission":"not-needed"});
    let document = plan(
        vec![
            eml,
            depends_on(candidate("uses-eml", "uses-eml", uses), &["eml-data"]),
        ],
        &["eml-data", "uses-eml"],
        "eml",
    );
    use_plan(&document);
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        ..Case::default()
    });
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[uses-eml] uses-eml:ran")),
        "{:?}",
        b.lines()
    );
    assert!(
        !b.has("startup-unwitnessed") && !b.has("will not run"),
        "{:?}",
        b.lines()
    );
    let record = b.record();
    let row = record.package("eml-data").unwrap();
    assert_eq!(
        (row.data, row.archive_present, row.lua),
        (Data::Committed, Some(true), Lua::NotNeeded)
    );
    b.stop();
}

// ---- 6. refused at the plugin step after kiln-eml applied the data ----

#[test]
fn a_start_refused_after_its_data_was_applied_says_the_data_stays() {
    let game = game_dir();
    let content = put_package(&game, "patch", "patch", "shroudforge.log.info('patch:ran')");
    let free = put_package(&game, "free", "free", "shroudforge.log.info('free:ran')");
    let document = plan(
        vec![
            with_data(candidate("patch", "patch", content)),
            candidate("free", "free", free),
        ],
        &["patch", "free"],
        "shroudforge",
    );
    use_plan(&document);
    // build_info says the build is not verified, the record says it is: refused at Kiln's plugin step
    let b = boot(Case {
        config: Some(Config::granting(&document)),
        native: (ALWAYS, 1, false),
        ..Case::default()
    });
    assert!(b.info.on_ready.is_none());
    let v = b.file();
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("verified-disagrees"))
    );
    let rows = v["packages"].as_array().unwrap();
    let patch = rows
        .iter()
        .find(|r| r["id"] == "patch")
        .expect("a row for patch");
    assert_eq!(
        (patch["data"].clone(), patch["archivePresent"].clone()),
        (json!("committed"), json!(true))
    );
    assert_eq!(patch["skip"]["code"], "verified-disagrees");
    assert_eq!(patch["skip"]["part"], "lua");
    assert_eq!(patch["warning"]["presence"], "data-applied-runtime-stopped");
    assert!(
        patch["warning"]["text"]
            .as_str()
            .unwrap()
            .contains("Its startup data is still applied")
    );
    // a Lua-only package has no data to warn about
    assert!(rows.iter().all(|r| r["id"] != "free"), "{v}");
    // the outcome was read once, and nothing was granted to a start that does not run
    assert_eq!(b.seen().count("outcome"), 2);
}

// ---- 7. an incoherent native run ----

#[test]
fn a_run_that_loaded_a_dll_and_cannot_confirm_the_witnessed_input_is_incoherent() {
    let terminal = |coherent: Result<(), String>, outcome| NativeTerminal {
        records: vec![
            record("dll", "x.dll", outcome),
            record("other", "y.dll", Outcome::Refused("no".into())),
        ],
        withdrawn: vec![],
        coherent,
        deadline_passed: false,
        not_armed: None,
    };
    let why = plan_source::incoherent(&terminal(
        Err("archive_001.dat changed".into()),
        Outcome::Loaded { module: 1 },
    ))
    .unwrap();
    assert!(
        why.contains("archive_001.dat changed") && why.contains("DLLs of dll loaded"),
        "{why}"
    );
    assert!(!why.contains("other"), "{why}");
    // coherent, or nothing loaded: nothing to say
    assert!(plan_source::incoherent(&terminal(Ok(()), Outcome::Loaded { module: 1 })).is_none());
    assert!(
        plan_source::incoherent(&terminal(
            Err("no startup witness was issued".into()),
            Outcome::Refused("x".into())
        ))
        .is_none()
    );
    assert!(
        plan_source::incoherent(&terminal(
            Err("x".into()),
            Outcome::ForeignOwned { module: 1 }
        ))
        .is_none()
    );
}

#[test]
fn the_seam_witness_wires_an_arm_and_its_push_goes_to_kiln_eml() {
    // arm_and_push with the seam: a run concluded without the loader pushes at once
    let seam = bound(base_config());
    let witness = Arc::new(SeamWitness::new(Arc::clone(&seam)));
    let run = NativeRun::new(
        NativePlan {
            plan_id: [5; 16],
            packages: vec![],
        },
        Arc::clone(&witness) as Arc<dyn Witness>,
        NativePermission {
            policy: NativePolicy::Always,
            verified: true,
        },
    );
    let pushed: Arc<Mutex<Vec<String>>> = Arc::default();
    let sink = Arc::clone(&pushed);
    let push: TerminalPush = Box::new(move |t: &NativeTerminal| {
        sink.lock()
            .unwrap()
            .push(startup_seam::terminal_json([5; 16], t, None, false));
    });
    let armed = arming::arm_and_push(
        &run,
        &WitnessPort::Wired(witness as Arc<dyn Witness>),
        |_| -> Result<(), String> { panic!("no thread for a plan without a DLL") },
        |_| -> Result<(), String> { panic!("no deadline waiter for a plan without a DLL") },
        Duration::ZERO,
        || -> LoaderBody { Box::new(|| {}) },
        push,
    );
    assert_eq!(armed, arming::Armed::NotArmed(arming::NO_DLLS.to_owned()));
    let pushed = pushed.lock().unwrap();
    assert_eq!(pushed.len(), 1);
    assert!(pushed[0].contains("\"notArmed\":true"));
}

// ---- 8. the strict reader against the frozen contract's fixtures (schema/sfi/eml-startup/fixtures) ----

fn fixtures(name: &str) -> Value {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../schema/sfi/eml-startup/fixtures")
        .join(name);
    serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
}
fn bytes_of(text: &str) -> Vec<u8> {
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap())
        .collect()
}
/// The launch a fixture names, and the identity kiln-eml would fill for it.
fn launch_of(record: &Value) -> (u64, [u8; 16], [u8; 32], KilnEmlArchiveIdentityV1) {
    let launch_id =
        u64::from_str_radix(record["launchId"].as_str().unwrap_or("1"), 16).unwrap_or(1);
    let plan_id: [u8; 16] = bytes_of(&record["planId"].as_str().unwrap_or("").replace('-', ""))
        .try_into()
        .unwrap_or([0; 16]);
    let plan_sha256: [u8; 32] = bytes_of(record["planSha256"].as_str().unwrap_or(""))
        .try_into()
        .unwrap_or([0; 32]);
    let mut identity: kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1 =
        unsafe { std::mem::zeroed() };
    identity.size = 160;
    if record["grant"] == true {
        identity = fake_startup::granted_identity(launch_id, plan_id, plan_sha256);
        if let Some(archive) = record["archiveSha256"].as_str() {
            identity.archive_sha256 = bytes_of(archive).try_into().unwrap_or([0; 32]);
        }
    }
    (launch_id, plan_id, plan_sha256, identity)
}
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;

#[test]
fn the_strict_reader_takes_every_valid_fixture_and_no_invalid_one() {
    let valid = fixtures("valid.json");
    let mut taken = 0;
    for (name, record) in valid["startup-outcome"].as_object().unwrap() {
        let (launch_id, plan_id, plan_sha256, identity) = launch_of(record);
        let read = startup_seam::check_outcome(
            &serde_json::to_vec(record).unwrap(),
            &identity,
            launch_id,
            plan_id,
            plan_sha256,
            |_| true,
        );
        assert!(
            matches!(read.verdict, StartupVerdict::Committed { .. }),
            "{name}: {read:?}"
        );
        assert_eq!(read.grant.is_some(), record["grant"] == true, "{name}");
        assert_eq!(
            read.rows.len(),
            record["packages"].as_array().unwrap().len(),
            "{name}"
        );
        taken += 1;
    }
    assert!(taken >= 7, "{taken}");
    let mut refused = 0;
    let mut taken_invalid: Vec<String> = Vec::new();
    for case in fixtures("invalid.json").as_array().unwrap() {
        if case["schema"] != "startup-outcome" {
            continue;
        }
        let record = &case["record"];
        let (launch_id, plan_id, plan_sha256, identity) = launch_of(record);
        let read = startup_seam::check_outcome(
            &serde_json::to_vec(record).unwrap(),
            &identity,
            launch_id,
            plan_id,
            plan_sha256,
            |_| true,
        );
        if !(read.grant.is_none() && matches!(read.verdict, StartupVerdict::Missing { .. })) {
            taken_invalid.push(case["why"].to_string());
        }
        refused += 1;
    }
    assert!(refused > 0);
    assert!(taken_invalid.is_empty(), "taken: {taken_invalid:#?}");
}
