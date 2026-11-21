// SPDX-License-Identifier: MIT
//! The native manager wired into kiln-sfi's early phase. Under backend `kiln`, right
//! after the plan read, the boundary makes one native run from the kiln.launch record (its frozen policy and
//! `build_verified`; never `build_info().verified` in the early phase), arms the loader only when the policy
//! allows, the plan lists a DLL and the startup witness is wired (kiln-eml's startup seam), and otherwise concludes every
//! planned DLL refused at once. Kiln's plugin step reads build_info again and refuses the interpreter when it
//! disagrees with the record. The run's terminal decides which packages run and gives sfi.json its DLL rows.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::lifecycle;
use kiln_sfi::native_manager::arming::{self, LoaderBody, WitnessPort};
use kiln_sfi::native_manager::executor::{NativeRun, Outcome, Witness, WitnessToken};
use kiln_sfi::native_manager::selection::{NativePermission, NativePolicy};
use kiln_sfi::plan_source::KilnPlanSource;
use kiln_sfi::state::{DllState, Intent, Lua, Phase as RecordPhase};
use kiln_sfi::worker::{Worker, WorkerPhase};
use kiln_sfi::{Boundary, InactiveReason, NativeHooks, Phase};
use serde_json::{Value, json};
use sfi_policy::digest::package_content_sha256;
use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;
use support::*;

const WAIT: Duration = Duration::from_secs(10);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
const ALWAYS: u32 = KILN_LAUNCH_NATIVE_POLICY_ALWAYS as u32;
const VERIFIED_ONLY: u32 = KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY as u32;
const NEVER: u32 = KILN_LAUNCH_NATIVE_POLICY_NEVER as u32;

// ---- packages and plans (as tests/plan_source.rs writes them) ----

fn put_package(game: &Path, folder: &str, id: &str, capabilities: &[&str], lua: &str) -> [u8; 32] {
    let dir = game.join("mods").join(folder);
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(
        dir.join("mod.json"),
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities}).to_string(),
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
fn with_dll(mut package: Value) -> Value {
    package["dlls"] = json!([{"path":"native/x.dll","sha256":"e".repeat(64),"mode":"library","source":"ini",
        "required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]);
    package
}
fn plan(packages: Vec<Value>, order: &[&str]) -> Value {
    let mut plan = plan_document("client");
    plan["packages"] = Value::Array(packages);
    plan["loadOrder"] = json!(order);
    plan["startupDialect"] = json!("none");
    plan
}
/// One package with a DLL and one Lua-only package that depends on it (no folders: the early phase reads none).
fn native_plan() {
    let c = [1u8; 32];
    set_plan(Some(
        serde_json::to_vec(&plan(
            vec![
                with_dll(candidate("dll", "dll", c)),
                depends_on(candidate("needs-dll", "needs-dll", c), &["dll"]),
            ],
            &["dll", "needs-dll"],
        ))
        .unwrap(),
    ));
}
fn lua_plan() {
    let c = [1u8; 32];
    set_plan(Some(
        serde_json::to_vec(&plan(vec![candidate("lua", "lua", c)], &["lua"])).unwrap(),
    ));
}

// ---- the native hooks: a counting spawn, a loader that loads nothing, a witness that is wired or not ----

thread_local! {
    static SPAWNS: Cell<usize> = const { Cell::new(0) };
    static BODIES: Cell<usize> = const { Cell::new(0) };
}
fn counting_spawn(body: LoaderBody) -> Result<(), String> {
    SPAWNS.set(SPAWNS.get() + 1);
    std::thread::spawn(body);
    Ok(())
}
fn abandoning_loader(
    run: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    BODIES.set(BODIES.get() + 1);
    Box::new(move || run.abandon("the test loader loads nothing"))
}
struct Ready;
impl Witness for Ready {
    fn ready(&self) -> Result<WitnessToken, String> {
        Ok(WitnessToken {
            plan_id: [0; 16],
            revision: 1,
        })
    }
    fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
        Ok(())
    }
}
fn wired() -> WitnessPort {
    WitnessPort::Wired(Arc::new(Ready))
}
/// Production's witness when no startup seam is attached, with a counting spawn and a loader that loads nothing.
const COUNTED: NativeHooks = NativeHooks {
    witness: arming::not_wired,
    spawn: counting_spawn,
    loader: abandoning_loader,
    ..NativeHooks::PRODUCTION
};
const COUNTED_WIRED: NativeHooks = NativeHooks {
    witness: wired,
    ..COUNTED
};

// ---- a start ----

struct Boot {
    boundary: &'static Boundary,
    worker: &'static Worker,
    early: KilnOwner,
    normal: KilnOwner,
    pool: Arc<FakePool>,
    info: KilnPluginInfo,
    state: PathBuf,
    /// The native run right after the early phase, before main.
    early_run: Option<Arc<NativeRun>>,
}
impl Boot {
    fn lines(&self) -> Vec<String> {
        logs(self.normal).into_iter().map(|(_, t)| t).collect()
    }
    fn has(&self, text: &str) -> bool {
        self.lines().iter().any(|l| l.contains(text))
    }
    fn running(&self) -> bool {
        eventually(WAIT, || {
            self.worker
                .shared()
                .is_some_and(|s| s.phase() == WorkerPhase::Running)
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
}

/// The early phase, Kiln's plugin step and its on_ready, with `policy` and `build_verified` in the record and
/// `info_verified` from build_info. kiln-eml has no startup seam here: the witness port is the hooks', and no archive
/// generation is granted, so the worker's runtime-create is refused.
fn boot(hooks: NativeHooks, policy: u32, build_verified: u32, info_verified: bool) -> Boot {
    boot_with(hooks, policy, build_verified, info_verified, true, false)
}
/// [`boot`] where kiln-eml's startup seam grants the plan: the early phase attaches, the seam's witness wires the
/// run, and the worker's runtime runs with the granted generation.
fn boot_granted(hooks: NativeHooks, policy: u32, build_verified: u32, info_verified: bool) -> Boot {
    boot_with(hooks, policy, build_verified, info_verified, true, true)
}
/// [`boot`], with or without the early phase and kiln-eml's startup seam.
fn boot_with(
    hooks: NativeHooks,
    policy: u32,
    build_verified: u32,
    info_verified: bool,
    early_phase: bool,
    seam: bool,
) -> Boot {
    boot_steps(
        hooks,
        policy,
        build_verified,
        info_verified,
        early_phase,
        seam,
        || None,
        &|_| {},
    )
}
/// [`boot_with`] with the look for ShroudForge's own loader `look` and `between` run after the early phase, before main.
#[allow(clippy::too_many_arguments)]
fn boot_steps(
    hooks: NativeHooks,
    policy: u32,
    build_verified: u32,
    info_verified: bool,
    early_phase: bool,
    seam: bool,
    look: fn() -> Option<String>,
    between: &dyn Fn(&Boundary),
) -> Boot {
    set_launch(Some(KILN));
    set_native(policy, build_verified, info_verified);
    let state = fresh_dir("a4d-state");
    set_state_dir(state.clone());
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(look)
            .with_native(hooks)
            .with_startup(if seam {
                fake_startup::resolve
            } else {
                fake_startup::none
            }),
    ));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let api = Box::leak(api(true));
    fake_archive::wrap(api);
    if seam {
        fake_startup::install(fake_startup::Config::granting_current());
    }
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    if early_phase {
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
            KILN_OK
        );
        assert!(early_info.on_ready.is_none());
    }
    let early_run = boundary.native_run().cloned();
    between(boundary);
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut info) },
        KILN_OK
    );
    fake_startup::grant_archive(normal);
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
        early_run,
    }
}

fn policy(value: u32) -> NativePolicy {
    NativePolicy::from_launch(value).unwrap()
}

// ---- 1. the early phase: one run from the record, never build_info's verified ----

#[test]
fn the_early_phase_makes_one_native_run_from_the_records_policy_and_build_verified() {
    for (value, build_verified) in [
        (ALWAYS, 0),
        (ALWAYS, 1),
        (VERIFIED_ONLY, 0),
        (VERIFIED_ONLY, 1),
        (NEVER, 0),
        (NEVER, 1),
    ] {
        let what = format!("policy {value}, build_verified {build_verified}");
        native_plan();
        SPAWNS.set(0);
        BODIES.set(0);
        // build_info says the same as the record (it is only read at Kiln's plugin step)
        let b = boot(COUNTED, value, build_verified, build_verified == 1);
        let run = b.early_run.clone().expect("the early phase made the run");
        let expected = NativePermission {
            policy: policy(value),
            verified: build_verified == 1,
        };
        assert_eq!(run.permission(), expected, "{what}");
        // one run per boundary: main and the worker use the early phase's
        assert!(
            Arc::ptr_eq(b.boundary.native_run().unwrap(), &run),
            "{what}"
        );
        assert_eq!(b.boundary.native_permission(), Some(expected), "{what}");
        // the witness is not wired: never armed, whatever the policy
        let terminal = run.wait(Duration::ZERO);
        let reason = expected
            .refusal()
            .map_or(arming::WITNESS_NOT_WIRED.to_owned(), str::to_owned);
        assert_eq!(
            terminal.not_armed.as_deref(),
            Some(reason.as_str()),
            "{what}"
        );
        assert_eq!(
            terminal
                .records
                .iter()
                .map(|r| r.outcome.clone())
                .collect::<Vec<_>>(),
            [Outcome::Refused(reason.clone())],
            "{what}"
        );
        assert_eq!(
            (SPAWNS.get(), BODIES.get()),
            (0, 0),
            "{what}: no thread, no loader"
        );
        assert!(
            logs(b.early)
                .iter()
                .any(|(level, l)| *level == KILN_LOG_INFO
                    && l == &format!(
                        "kiln-sfi: the native loader is not armed this start: {reason}"
                    )),
            "{what}: {:?}",
            logs(b.early)
        );
        assert!(
            b.info.on_ready.is_some(),
            "{what}: the interpreter still starts"
        );
        b.stop();
    }
}

#[test]
fn the_early_phase_follows_the_record_not_build_info() {
    // build_info disagrees with the record: the early run follows the record (verified-only on an unverified build
    // refuses), and the plugin step then refuses the interpreter
    native_plan();
    let b = boot(COUNTED_WIRED, VERIFIED_ONLY, 0, true);
    let run = b.early_run.clone().unwrap();
    assert_eq!(
        run.permission(),
        NativePermission {
            policy: NativePolicy::VerifiedOnly,
            verified: false
        }
    );
    assert!(run.wait(Duration::ZERO).not_armed.is_some());
    assert!(b.info.on_ready.is_none());
}

#[test]
fn a_lua_only_plan_arms_nothing_and_says_nothing_about_the_loader() {
    lua_plan();
    SPAWNS.set(0);
    BODIES.set(0);
    let b = boot(COUNTED_WIRED, ALWAYS, 1, true);
    let run = b.early_run.clone().unwrap();
    assert_eq!(run.dll_count(), 0);
    assert_eq!(
        run.wait(Duration::ZERO).not_armed.as_deref(),
        Some(arming::NO_DLLS)
    );
    assert_eq!((SPAWNS.get(), BODIES.get()), (0, 0));
    assert!(
        !logs(b.early)
            .iter()
            .any(|(_, l)| l.contains("native loader"))
    );
    b.stop();
}

// ---- 2. the arming rule, from the record ----

#[test]
fn only_a_policy_that_allows_a_planned_dll_and_a_wired_witness_start_the_loader_thread() {
    for (hooks, value, build_verified, dlls, arms) in [
        (COUNTED_WIRED, NEVER, 1, true, false),
        (COUNTED_WIRED, VERIFIED_ONLY, 0, true, false),
        (COUNTED_WIRED, ALWAYS, 1, false, false),
        (COUNTED, ALWAYS, 1, true, false),
        (COUNTED, VERIFIED_ONLY, 1, true, false),
        (COUNTED_WIRED, ALWAYS, 0, true, true),
        (COUNTED_WIRED, VERIFIED_ONLY, 1, true, true),
    ] {
        let what = format!(
            "policy {value}, build_verified {build_verified}, dlls {dlls}, wired {}",
            core::ptr::fn_addr_eq(hooks.witness, wired as fn() -> WitnessPort)
        );
        if dlls {
            native_plan()
        } else {
            lua_plan()
        }
        SPAWNS.set(0);
        BODIES.set(0);
        let b = boot(hooks, value, build_verified, build_verified == 1);
        let run = b.early_run.clone().unwrap();
        let expected = usize::from(arms);
        assert_eq!((SPAWNS.get(), BODIES.get()), (expected, expected), "{what}");
        let terminal = run.wait(WAIT);
        if arms {
            assert!(terminal.not_armed.is_none(), "{what}");
            assert!(
                terminal
                    .records
                    .iter()
                    .all(|r| r.outcome == Outcome::Refused("the test loader loads nothing".into())),
                "{what}"
            );
        } else {
            assert!(terminal.not_armed.is_some(), "{what}");
        }
        b.stop();
    }
}

/// A run made at Kiln's plugin step (the early phase never ran) is never armed: package DLLs load in the early window.
#[test]
fn a_run_made_at_the_plugin_step_is_never_armed() {
    native_plan();
    SPAWNS.set(0);
    BODIES.set(0);
    let b = boot_with(COUNTED_WIRED, ALWAYS, 1, true, false, false);
    assert!(b.early_run.is_none());
    let run = b.boundary.native_run().expect("main made the run").clone();
    assert_eq!(
        run.wait(Duration::ZERO).not_armed.as_deref(),
        Some(arming::EARLY_PHASE_MISSED)
    );
    assert_eq!((SPAWNS.get(), BODIES.get()), (0, 0));
    b.stop();
    // the policy's words still come first
    native_plan();
    let b = boot_with(COUNTED_WIRED, NEVER, 1, true, false, false);
    assert_eq!(
        b.boundary
            .native_run()
            .unwrap()
            .wait(Duration::ZERO)
            .not_armed
            .as_deref(),
        Some(r#"kiln.json native.policy is "never""#)
    );
    b.stop();
}

/// The gate the blocking loader waits at, and the name of the thread it ran on.
static GATE: (Mutex<bool>, Condvar) = (Mutex::new(false), Condvar::new());
static LOADER_THREAD: Mutex<Option<String>> = Mutex::new(None);
/// A loader body that waits at [`GATE`] until the test opens it, then loads nothing.
fn blocking_loader(
    run: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    Box::new(move || {
        *LOADER_THREAD.lock().unwrap() = std::thread::current().name().map(str::to_owned);
        let (open, changed) = &GATE;
        let mut open = open.lock().unwrap();
        while !*open {
            open = changed.wait(open).unwrap();
        }
        drop(open);
        run.abandon("the test loader loads nothing");
    })
}

/// kiln_plugin_early never waits for the loader: the early phase returns while the armed run's thread (the production
/// spawn, "kiln-sfi native") is still held, and the run's terminal is the loader's own, not a deadline's.
#[test]
fn the_early_phase_never_waits_for_the_loader_thread() {
    native_plan();
    set_launch(Some(KILN));
    set_native(ALWAYS, 1, true);
    set_state_dir(fresh_dir("a4d-state"));
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_native(NativeHooks {
                witness: wired,
                spawn: arming::spawn_native_thread,
                loader: blocking_loader,
                ..NativeHooks::PRODUCTION
            }),
    ));
    let api = Box::leak(api(true));
    let mut info = plugin_info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, next_owner(), &mut info) },
        KILN_OK
    );
    // the early phase is back while the loader thread waits at its gate
    let run = boundary
        .native_run()
        .expect("the early phase made the run")
        .clone();
    {
        let (open, changed) = &GATE;
        *open.lock().unwrap() = true;
        changed.notify_all();
    }
    let terminal = run.wait(Duration::from_secs(300));
    assert!(
        terminal.not_armed.is_none() && !terminal.deadline_passed,
        "{terminal:?}"
    );
    assert_eq!(
        terminal
            .records
            .iter()
            .map(|r| r.outcome.clone())
            .collect::<Vec<_>>(),
        [Outcome::Refused("the test loader loads nothing".into())],
        "the loader thread decided the run, not a wait in the early phase"
    );
    assert_eq!(
        LOADER_THREAD.lock().unwrap().as_deref(),
        Some(arming::NATIVE_THREAD)
    );
}

// ---- 3. the plugin step's cross-check ----

#[test]
fn the_plugin_step_refuses_the_interpreter_when_build_info_disagrees_with_the_record() {
    for (value, build_verified, info_verified) in [
        (VERIFIED_ONLY, 1, false),
        (VERIFIED_ONLY, 0, true),
        // the same under "always", where the answer does not change the decision: a core that disagrees with
        // itself is not trusted
        (ALWAYS, 1, false),
        (NEVER, 0, true),
    ] {
        let what = format!("policy {value}, record {build_verified}, build_info {info_verified}");
        native_plan();
        let b = boot(COUNTED, value, build_verified, info_verified);
        assert!(b.info.on_ready.is_none(), "{what}: no on_ready, no start");
        assert!(
            b.pool.submissions().is_empty() && !b.boundary.worker_started(),
            "{what}"
        );
        let record = b.record();
        assert_eq!(record.phase(), RecordPhase::Refused, "{what}");
        let v: Value =
            serde_json::from_slice(&std::fs::read(b.state.join("sfi.json")).unwrap()).unwrap();
        assert_eq!(v["reason"]["code"], "verified-disagrees", "{what}");
        let said = format!(
            "kiln.launch's record says this game build is {} and build_info says {}",
            if build_verified == 1 {
                "verified"
            } else {
                "not verified"
            },
            if info_verified {
                "verified"
            } else {
                "not verified"
            }
        );
        assert!(
            v["reason"]["text"].as_str().unwrap().contains(&said),
            "{what}: {v}"
        );
        assert!(
            logs(b.normal)
                .iter()
                .any(|(level, l)| *level == KILN_LOG_ERROR
                    && l.starts_with("kiln-sfi: WARNING:")
                    && l.contains(&said)),
            "{what}: {:?}",
            b.lines()
        );
        assert!(
            reports(b.normal).iter().any(|(_, row)| row.contains(&said)),
            "{what}"
        );
    }
    // agreeing answers start (kiln-eml's startup seam grants them)
    for (value, verified) in [
        (VERIFIED_ONLY, true),
        (VERIFIED_ONLY, false),
        (ALWAYS, true),
        (NEVER, false),
    ] {
        native_plan();
        let b = boot_granted(COUNTED, value, u32::from(verified), verified);
        assert!(
            b.info.on_ready.is_some(),
            "policy {value}, verified {verified}"
        );
        assert!(b.running());
        b.stop();
    }
}

// ---- 4. the terminal reaches admission and sfi.json ----

#[test]
fn the_native_terminal_decides_admission_and_gives_sfi_json_its_dll_rows() {
    // (policy, build_verified, why the loader is not armed): kiln-eml's startup seam wires the witness, so only
    // the policy keeps the loader from being armed; an armed run's loader here loads nothing (no package DLL loads on
    // this host, and none is there)
    for (value, build_verified, not_armed) in [
        (ALWAYS, 1, None),
        (NEVER, 1, Some(r#"kiln.json native.policy is "never""#)),
        (
            VERIFIED_ONLY,
            0,
            Some(
                r#"kiln.json native.policy is "verified-only" and this game build is not verified"#,
            ),
        ),
    ] {
        let what = format!("{not_armed:?}");
        let game = game_dir();
        let dll = put_package(
            &game,
            "dll",
            "dll",
            &["runtime"],
            "shroudforge.log.info('dll:ran')",
        );
        let needs = put_package(
            &game,
            "needs-dll",
            "needs-dll",
            &["runtime"],
            "shroudforge.log.info('needs-dll:ran')",
        );
        let registers = put_package(
            &game,
            "registers",
            "registers",
            &["runtime", "runtime-register-dll"],
            "shroudforge.log.info('registers:ran')",
        );
        let free = put_package(
            &game,
            "free",
            "free",
            &["runtime"],
            "shroudforge.log.info('free:ran')",
        );
        let mut registering = candidate("registers", "registers", registers);
        registering["capabilities"] = json!(["runtime", "runtime-register-dll"]);
        set_plan(Some(
            serde_json::to_vec(&plan(
                vec![
                    with_dll(candidate("dll", "dll", dll)),
                    depends_on(candidate("needs-dll", "needs-dll", needs), &["dll"]),
                    registering,
                    candidate("free", "free", free),
                ],
                &["dll", "needs-dll", "registers", "free"],
            ))
            .unwrap(),
        ));
        let b = boot_granted(
            NativeHooks::PRODUCTION,
            value,
            build_verified,
            build_verified == 1,
        );
        assert!(b.running(), "{what}");
        assert!(
            eventually(WAIT, || b.has("[free] free:ran")),
            "{what}: {:?}",
            b.lines()
        );
        for id in ["dll", "needs-dll"] {
            assert!(!b.has(&format!("{id}:ran")), "{what}: {id} ran");
        }
        let terminal = b.boundary.native_run().unwrap().wait(WAIT);
        assert_eq!(terminal.not_armed.as_deref(), not_armed, "{what}");
        let record = b.record();
        let row = record.package("dll").unwrap();
        assert_eq!(row.dlls.len(), 1, "{what}");
        assert_eq!(
            (row.dlls[0].name.as_str(), row.dlls[0].state),
            ("x.dll", DllState::Refused)
        );
        assert_eq!(row.dlls[0].sha256, Some([0xee; 32]));
        let skip = row.skip.as_ref().unwrap();
        assert_eq!(skip.code, "native-refused", "{what}");
        match not_armed {
            Some(reason) => assert_eq!(skip.text, reason, "{what}"),
            None => assert_eq!(skip.text, "native DLL native/x.dll did not load", "{what}"),
        }
        let skip = record.package("needs-dll").unwrap().skip.as_ref().unwrap();
        assert_eq!(
            (skip.code.as_str(), skip.chain.clone()),
            ("dependency-skipped", vec!["dll".to_owned()]),
            "{what}: through a Lua-only package"
        );
        // a package that registers DLLs from its Lua: refused when the loader is not armed (nothing could register);
        // under an armed run it runs, and its registrations meet the runtime's register_dll table
        match not_armed {
            Some(reason) => {
                assert!(!b.has("registers:ran"), "{what}");
                let skip = record.package("registers").unwrap().skip.as_ref().unwrap();
                assert_eq!(skip.code, "native-refused", "{what}");
                assert!(
                    skip.text.contains("it registers DLLs from its Lua")
                        && skip.text.contains(reason),
                    "{}",
                    skip.text
                );
            }
            None => {
                assert!(
                    eventually(WAIT, || b.has("[registers] registers:ran")),
                    "{what}: {:?}",
                    b.lines()
                );
                assert!(
                    record.package("registers").unwrap().skip.is_none(),
                    "{what}"
                );
            }
        }
        let free = record.package("free").unwrap();
        assert!(
            free.skip.is_none() && free.intent == Intent::Selected,
            "{what}"
        );
        assert!(
            eventually(WAIT, || b.record().package("free").unwrap().lua
                == Lua::Active),
            "{what}"
        );
        // the loud warning for each withdrawal
        assert!(
            b.has("WARNING: ShroudForge package 'dll' will not run"),
            "{what}: {:?}",
            b.lines()
        );
        // the terminal went back to kiln-eml's seam
        let pushed = fake_startup::seen().unwrap();
        assert!(
            eventually(WAIT, || fake_startup::seen().unwrap().terminal.is_some()),
            "{what}: {pushed:?}"
        );
        b.stop();
    }
}

// ---- 5. a native-selection package the plan skips ----

#[test]
fn a_native_selection_package_the_plan_skips_gets_the_loud_warning() {
    let game = game_dir();
    let free = put_package(
        &game,
        "free",
        "free",
        &["runtime"],
        "shroudforge.log.info('free:ran')",
    );
    let mut skipped = with_dll(candidate("dlls-only", "dlls-only", [1; 32]));
    skipped["kind"] = json!("native-selection");
    skipped["lua"] = json!({"required":false,"admission":"not-needed"});
    skipped["decision"] = json!("skip");
    skipped["reasons"] = json!([{"code":"native-policy","message":"Kiln's native policy refuses its DLLs","causedBy":[],"severity":"warning"}]);
    set_plan(Some(
        serde_json::to_vec(&plan(
            vec![candidate("free", "free", free), skipped],
            &["free"],
        ))
        .unwrap(),
    ));
    let b = boot_granted(NativeHooks::PRODUCTION, ALWAYS, 1, true);
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[free] free:ran")),
        "{:?}",
        b.lines()
    );
    assert!(
        b.has("WARNING: ShroudForge package 'dlls-only' will not run: this start's launch plan skips it: Kiln's native policy refuses its DLLs"),
        "{:?}",
        b.lines()
    );
    let record = b.record();
    let skip = record.package("dlls-only").unwrap().skip.as_ref().unwrap();
    assert_eq!(skip.code, "plan-skip");
    b.stop();
}

// ---- the look for ShroudForge's own loader moves to the early phase ----

/// What the counting looks saw: the loader threads started (`SPAWNS`, this thread's) when each look ran.
static LOOKS_SAW: Mutex<Vec<usize>> = Mutex::new(Vec::new());
fn counting_look() -> Option<String> {
    LOOKS_SAW.lock().unwrap().push(SPAWNS.get());
    None
}
static FOUND_LOOKS: Mutex<usize> = Mutex::new(0);
fn finding_look() -> Option<String> {
    *FOUND_LOOKS.lock().unwrap() += 1;
    Some("ShroudForge's own loader (C:\\g\\dbghelp.dll) runs it".into())
}
/// This file's own gate for a held loader (GATE is another test's).
static HELD: (Mutex<bool>, Condvar) = (Mutex::new(false), Condvar::new());
/// A loader body that waits at [`HELD`] until the test opens it, then loads nothing.
fn held_loader(
    run: Arc<NativeRun>,
    _: unsafe extern "C" fn(KilnOwner, i32, KilnStr),
    _: KilnOwner,
) -> LoaderBody {
    Box::new(move || {
        let (open, changed) = &HELD;
        let mut open = open.lock().unwrap();
        while !*open {
            open = changed.wait(open).unwrap();
        }
        drop(open);
        run.abandon("the test loader loads nothing");
    })
}
/// The looks counted by `counting_look` and `finding_look` are process-wide: their tests take turns.
static LOOK_TESTS: Mutex<()> = Mutex::new(());

/// The early phase looks before it arms its run (no loader thread yet); Kiln's plugin step looks again at main and right
/// before the start because the run's loader is done: every DLL decided, nothing loading.
#[test]
fn the_early_phase_looks_before_arming_and_the_plugin_step_again_once_nothing_loads() {
    let _turn = LOOK_TESTS.lock().unwrap_or_else(|p| p.into_inner());
    native_plan();
    SPAWNS.set(0);
    LOOKS_SAW.lock().unwrap().clear();
    let b = boot_steps(
        COUNTED_WIRED,
        ALWAYS,
        1,
        true,
        true,
        true,
        counting_look,
        &|boundary| {
            assert_eq!(
                *LOOKS_SAW.lock().unwrap(),
                [0],
                "one look in the early phase, before the loader thread"
            );
            let run = boundary.native_run().expect("armed");
            let _ = run.wait(Duration::from_secs(300));
            assert!(!run.may_be_loading(), "the test loader decided every DLL");
        },
    );
    assert_eq!(SPAWNS.get(), 1, "the run was armed after the look");
    assert_eq!(
        LOOKS_SAW.lock().unwrap().len(),
        3,
        "the early phase, main and on_ready"
    );
    assert!(!b.has("stands aside"), "{:?}", b.lines());
    b.stop();
}

/// A run whose loader may still be inside a package DLL's load at Kiln's plugin step (here: still open, its loader
/// held before it decided anything) keeps the early phase's answer: main and on_ready ask the loader nothing, and the
/// interpreter's start goes on.
#[test]
fn while_the_run_may_be_loading_the_plugin_step_keeps_the_early_answer() {
    let _turn = LOOK_TESTS.lock().unwrap_or_else(|p| p.into_inner());
    native_plan();
    SPAWNS.set(0);
    LOOKS_SAW.lock().unwrap().clear();
    *HELD.0.lock().unwrap() = false;
    let hooks = NativeHooks {
        witness: wired,
        spawn: counting_spawn,
        loader: held_loader,
        ..NativeHooks::PRODUCTION
    };
    let b = boot_steps(
        hooks,
        ALWAYS,
        1,
        true,
        true,
        true,
        counting_look,
        &|boundary| {
            assert!(boundary.native_run().expect("armed").may_be_loading());
        },
    );
    assert_eq!(
        *LOOKS_SAW.lock().unwrap(),
        [0],
        "only the early phase looked"
    );
    assert!(
        b.info.on_ready.is_some() && !b.has("stands aside"),
        "{:?}",
        b.lines()
    );
    {
        let (open, changed) = &HELD;
        *open.lock().unwrap() = true;
        changed.notify_all();
    }
    let run = b.early_run.clone().unwrap();
    let _ = run.wait(Duration::from_secs(300));
    b.stop();
}

/// A loader the early phase found stands kiln-sfi aside there, and the plugin step stands aside with the same words
/// without looking again.
#[test]
fn a_loader_the_early_phase_found_is_not_looked_for_again() {
    let _turn = LOOK_TESTS.lock().unwrap_or_else(|p| p.into_inner());
    native_plan();
    *FOUND_LOOKS.lock().unwrap() = 0;
    let b = boot_steps(
        COUNTED_WIRED,
        ALWAYS,
        1,
        true,
        true,
        true,
        finding_look,
        &|boundary| {
            assert!(
                boundary.native_run().is_none(),
                "nothing armed: kiln-sfi stands aside"
            );
        },
    );
    assert_eq!(
        *FOUND_LOOKS.lock().unwrap(),
        1,
        "the early phase looked; main did not"
    );
    assert!(b.info.on_ready.is_none(), "no start");
    assert!(
        logs(b.early)
            .iter()
            .any(|(_, l)| l.contains("dbghelp.dll") && l.contains("stands aside")),
        "{:?}",
        logs(b.early)
    );
    let record: Value =
        serde_json::from_slice(&std::fs::read(b.state.join("sfi.json")).unwrap()).unwrap();
    assert_eq!(
        (record["phase"].as_str(), record["reason"]["code"].as_str()),
        (Some("refused"), Some("shroudforge-in-process")),
        "{record}"
    );
    assert!(
        record["reason"]["text"]
            .as_str()
            .unwrap()
            .contains("dbghelp.dll"),
        "{record}"
    );
}

/// Without an early phase (no run was armed) the plugin step looks as before: at main and right before the start.
#[test]
fn without_an_early_phase_the_plugin_step_looks_as_before() {
    let _turn = LOOK_TESTS.lock().unwrap_or_else(|p| p.into_inner());
    native_plan();
    SPAWNS.set(0);
    LOOKS_SAW.lock().unwrap().clear();
    let b = boot_steps(
        COUNTED_WIRED,
        ALWAYS,
        1,
        true,
        false,
        true,
        counting_look,
        &|_| {},
    );
    assert_eq!(LOOKS_SAW.lock().unwrap().len(), 2, "main and on_ready");
    b.stop();
}
