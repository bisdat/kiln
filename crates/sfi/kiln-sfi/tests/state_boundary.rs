// SPDX-License-Identifier: MIT
//! sfi.json from the boundary: `aside` for every backend that is not kiln and for an older core, `refused`
//! when kiln-sfi stands aside under `kiln`, `starting` on disk before the worker is submitted (and no start without
//! it), `running` from the worker, `ended` from on_unload, which never waits for the worker.
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::state::{Begun, Phase};
use kiln_sfi::worker::{Worker, WorkerContext, WorkerPhase, WorkerRuntime, empty_runtime};
use kiln_sfi::{Boundary, InactiveReason};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use support::*;

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
/// KILN_SHROUDFORGE is process-wide: every boot here holds this lock.
static ENV: Mutex<()> = Mutex::new(());

fn read(dir: &Path) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(dir.join("sfi.json")).unwrap()).unwrap()
}
fn eventually(limit: Duration, mut ok: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + limit;
    while Instant::now() < end {
        if ok() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    ok()
}

struct Boot {
    boundary: &'static Boundary,
    worker: Option<&'static Worker>,
    normal: KilnOwner,
    info: KilnPluginInfo,
    pool: std::sync::Arc<FakePool>,
    dir: PathBuf,
}
impl Boot {
    fn ready(&self) {
        if let Some(ready) = self.info.on_ready {
            unsafe { ready(self.info.user) };
        }
    }
    fn unload(&self) {
        if let Some(unload) = self.info.on_unload {
            unsafe { unload(self.info.user) };
        }
    }
}

#[derive(Default)]
struct Case {
    launch: Option<(i32, i32)>,
    worker: bool,
    env_off: bool,
    no_tasks: bool,
    /// The state folder; unset: a fresh one.
    dir: Option<PathBuf>,
    factory: Option<kiln_sfi::worker::RuntimeFactory>,
}

/// The early phase, then Kiln's plugin step (its on_ready is the test's to call).
fn boot(case: Case) -> Boot {
    let _env = ENV.lock().unwrap_or_else(|p| p.into_inner());
    set_launch(case.launch);
    let dir = case.dir.clone().unwrap_or_else(|| fresh_dir("boundary"));
    set_state_dir(dir.clone());
    let worker: Option<&'static Worker> = case
        .worker
        .then(|| &*Box::leak(Box::new(Worker::new(case.factory.unwrap_or(empty_runtime)))));
    let boundary = match worker {
        Some(worker) => Boundary::with_worker(InactiveReason::NegotiationMissing, worker),
        None => Boundary::new(),
    }
    .with_own_loader(|| None);
    let boundary: &'static Boundary = Box::leak(Box::new(boundary));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let _early_pool = FakePool::new(early, PoolConfig::default());
    let api = Box::leak(api(!case.no_tasks));
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    if case.env_off {
        unsafe { std::env::set_var("KILN_SHROUDFORGE", "0") };
    }
    let entered = (
        unsafe { boundary.enter(kiln_sfi::Phase::Early, &*api, early, &mut early_info) },
        unsafe { boundary.enter(kiln_sfi::Phase::Normal, &*api, normal, &mut info) },
    );
    if case.env_off {
        unsafe { std::env::remove_var("KILN_SHROUDFORGE") };
    }
    assert_eq!(entered, (KILN_OK, KILN_OK));
    assert!(
        early_info.on_unload.is_none(),
        "the early phase installs no on_unload"
    );
    Boot {
        boundary,
        worker,
        normal,
        info,
        pool,
        dir,
    }
}

#[test]
fn every_backend_that_is_not_kiln_writes_aside_at_main() {
    for (launch, code) in [
        (
            Some((
                KILN_LAUNCH_BACKEND_HOSTED,
                KILN_LAUNCH_REASON_HOSTED_DEFAULT,
            )),
            "hosted-default",
        ),
        (
            Some((
                KILN_LAUNCH_BACKEND_INSTALLED,
                KILN_LAUNCH_REASON_USE_INSTALLED,
            )),
            "use-installed",
        ),
        (
            Some((
                KILN_LAUNCH_BACKEND_NONE,
                KILN_LAUNCH_REASON_INTERPRETER_HELD,
            )),
            "interpreter-held",
        ),
        (
            Some((KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE)),
            "safe-idle",
        ),
        (None, "no-kiln-launch"),
    ] {
        let b = boot(Case {
            launch,
            worker: true,
            ..Case::default()
        });
        assert!(
            b.info.on_ready.is_none() && b.info.on_unload.is_none(),
            "{code}"
        );
        let v = read(&b.dir);
        assert_eq!(
            (v["phase"].as_str(), v["incomplete"].as_bool()),
            (Some("aside"), Some(false)),
            "{code}"
        );
        assert_eq!(v["reason"]["code"], code);
        assert_eq!(v["packages"], serde_json::json!([]));
        assert_eq!(v["launch"]["pid"], std::process::id());
        if launch.is_some() {
            assert_eq!(
                v["launch"]["launchId"], "0123456789abcdef",
                "{code}: kiln.launch's id, 16 hex digits"
            );
            assert_eq!(v["launch"]["backend"]["source"], "kiln.launch");
        } else {
            assert_eq!(
                v["launch"]["launchId"],
                serde_json::Value::Null,
                "an older core has no launch id"
            );
            assert_eq!(v["launch"]["backend"]["source"], "absent");
        }
        assert!(b.pool.submissions().is_empty());
    }
}

#[test]
fn starting_is_on_disk_before_the_worker_is_submitted_then_running_then_ended() {
    static SEEN: Mutex<Option<serde_json::Value>> = Mutex::new(None);
    static DIR: Mutex<Option<PathBuf>> = Mutex::new(None);
    // the runtime's creation is the first thing a package could do: sfi.json must already say starting
    fn probe(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
        let dir = DIR.lock().unwrap().clone().unwrap();
        *SEEN.lock().unwrap() =
            Some(serde_json::from_slice(&std::fs::read(dir.join("sfi.json")).unwrap()).unwrap());
        empty_runtime(cx)
    }
    let dir = fresh_dir("start");
    *DIR.lock().unwrap() = Some(dir.clone());
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        dir: Some(dir.clone()),
        factory: Some(probe),
        ..Case::default()
    });
    // Package DLLs load in kiln-sfi's early window and kiln-eml commits startup data in its
    // early on_ready, so `starting` is on disk once the early phase returned, before anything is armed or attached
    assert_eq!(
        read(&dir)["phase"],
        "starting",
        "the early phase wrote starting for kiln"
    );
    assert!(
        b.info.on_unload.is_some(),
        "kiln installs on_unload for the end"
    );
    b.ready();
    assert_eq!(b.pool.submissions().len(), 1);
    let shared = b.worker.unwrap().shared().unwrap().clone();
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    let seen = SEEN
        .lock()
        .unwrap()
        .clone()
        .expect("the runtime was created");
    assert_eq!(
        (seen["phase"].as_str(), seen["incomplete"].as_bool()),
        (Some("starting"), Some(true))
    );
    assert!(
        eventually(WAIT, || read(&dir)["phase"] == "running"),
        "the worker's running reaches the file"
    );
    // on_unload: the end, at once, while the worker still runs (it never waits for it). Proven by the worker's
    // phase below, not by a wall-clock bound: the worker leaves Running only when this test asks it to stop, so an
    // on_unload that waited for the worker would never return.
    b.unload();
    let v = read(&dir);
    assert_eq!(
        (v["phase"].as_str(), v["incomplete"].as_bool()),
        (Some("ended"), Some(false))
    );
    assert_eq!(
        shared.phase(),
        WorkerPhase::Running,
        "on_unload did not touch the worker"
    );
    b.unload();
    assert_eq!(
        read(&dir)["phase"],
        "ended",
        "a second on_unload changes nothing"
    );
    shared.request_stop();
    assert!(b.pool.task(0).wait_finished(WAIT));
}

#[test]
fn no_start_when_sfi_json_cannot_be_written() {
    let dir = fresh_dir("unwritable");
    let file = dir.join("state-is-a-file");
    std::fs::write(&file, b"x").unwrap();
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        dir: Some(file),
        ..Case::default()
    });
    b.ready();
    assert!(
        b.pool.submissions().is_empty(),
        "the worker is never submitted"
    );
    assert!(!b.boundary.worker_started());
    let rows: Vec<String> = reports(b.normal).into_iter().map(|(_, t)| t).collect();
    assert!(
        rows.last()
            .unwrap()
            .contains("kiln/state/sfi.json cannot be written"),
        "{rows:?}"
    );
    let errors: Vec<String> = logs(b.normal)
        .into_iter()
        .filter(|(l, _)| *l == KILN_LOG_ERROR)
        .map(|(_, t)| t)
        .collect();
    assert!(
        errors
            .iter()
            .any(|t| t.contains("cannot be written") && t.contains("no ShroudForge mod runs")),
        "{errors:?}"
    );
    assert_eq!(b.boundary.status().phase(), Some(Phase::Refused));
    // a second on_ready does not start it either
    b.ready();
    assert!(b.pool.submissions().is_empty());
}

#[test]
fn kiln_sfi_standing_aside_under_kiln_writes_refused() {
    // KILN_SHROUDFORGE=0 at main
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        env_off: true,
        ..Case::default()
    });
    assert!(b.info.on_ready.is_none());
    let v = read(&b.dir);
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("env-off"))
    );
    assert_eq!(v["launch"]["backend"]["selected"], "kiln");
    // kiln.tasks missing at on_ready
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        no_tasks: true,
        ..Case::default()
    });
    b.ready();
    let v = read(&b.dir);
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("no-tasks"))
    );
    assert_eq!(v["incomplete"], false);
    b.unload();
    assert_eq!(
        read(&b.dir)["phase"],
        "refused",
        "on_unload leaves a final record alone"
    );
    // a boundary without a worker: refused, it says why
    let b = boot(Case {
        launch: Some(KILN),
        ..Case::default()
    });
    b.ready();
    let v = read(&b.dir);
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("no-worker"))
    );
}

/// On a Kiln with resident tasks, kiln.tasks' ARGUMENT for the worker's resident submit
/// can mean kiln-sfi's owner already holds one (main's one-per-owner cap): the start is
/// refused with that reason, and no plain LongRunning retry is submitted.
#[test]
fn a_resident_cap_refusal_is_recorded_refused_with_its_reason() {
    // must-happen waits: a liveness bound, not a latency claim (worker.rs WAIT)
    const LIVE: Duration = Duration::from_secs(300);
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        ..Case::default()
    });
    let hold = hold_resident(&b.pool, LIVE);
    b.ready();
    let v = read(&b.dir);
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("tasks-refused")),
        "{v}"
    );
    let text = v["reason"]["text"].as_str().unwrap_or_default();
    assert!(
        text.contains("ARGUMENT") && text.contains("already holds") && text.contains("update Kiln"),
        "{v}"
    );
    let flags: Vec<u32> = b.pool.submissions().iter().map(|s| s.flags).collect();
    assert_eq!(
        flags,
        [(kiln_abi::services::kiln_tasks::KILN_TASK_LONG_RUNNING
            | kiln_abi::services::kiln_tasks::KILN_TASK_RESIDENT) as u32; 2],
        "the hold, then the worker's one resident submit"
    );
    assert_eq!(
        b.worker.and_then(Worker::shared).map(|s| s.phase()),
        Some(WorkerPhase::Stopped)
    );
    assert!(hold.let_go(LIVE));
    b.unload();
    assert_eq!(read(&b.dir)["phase"], "refused");
}

#[test]
fn a_loader_that_arrives_before_on_ready_is_recorded_as_refused() {
    let _env = ENV.lock().unwrap_or_else(|p| p.into_inner());
    set_launch(Some(KILN));
    let dir = fresh_dir("late-loader");
    set_state_dir(dir.clone());
    static ARRIVED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
    fn probe() -> Option<String> {
        ARRIVED
            .load(std::sync::atomic::Ordering::SeqCst)
            .then(|| "ShroudForge's own loader (C:\\g\\dbghelp.dll) runs it".into())
    }
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(empty_runtime)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker).with_own_loader(probe),
    ));
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = Box::leak(api(true));
    let mut info = plugin_info();
    assert_eq!(
        unsafe { boundary.enter(kiln_sfi::Phase::Normal, &*api, owner, &mut info) },
        KILN_OK
    );
    ARRIVED.store(true, std::sync::atomic::Ordering::SeqCst);
    unsafe { info.on_ready.unwrap()(info.user) };
    assert!(pool.submissions().is_empty());
    let v = read(&dir);
    assert_eq!(
        (v["phase"].as_str(), v["reason"]["code"].as_str()),
        (Some("refused"), Some("shroudforge-in-process"))
    );
    assert!(
        v["reason"]["text"]
            .as_str()
            .unwrap()
            .contains("dbghelp.dll")
    );
}

#[test]
fn the_previous_start_is_read_before_it_is_replaced() {
    let dir = fresh_dir("previous");
    std::fs::write(
        dir.join("sfi.json"),
        br#"{"format":1,"launch":{"launchId":"9a1c7e5b2d3f4a6b","pid":1},"incomplete":true,"packages":[]}"#,
    )
    .unwrap();
    std::fs::write(dir.join("sfi.json.1-3.tmp"), b"left by a start that died").unwrap();
    let b = boot(Case {
        launch: Some(KILN),
        worker: true,
        dir: Some(dir.clone()),
        ..Case::default()
    });
    b.ready();
    let v = read(&dir);
    assert_eq!(
        v["previousStart"],
        serde_json::json!({ "disposition": "incomplete", "launchId": "9a1c7e5b2d3f4a6b", "pid": 1 })
    );
    assert!(
        !dir.join("sfi.json.1-3.tmp").exists(),
        "an earlier process's temp file is swept"
    );
    assert!(matches!(
        b.boundary.status().begin(
            b.boundary.status().record().unwrap().identity().clone(),
            Phase::Starting,
            None
        ),
        Begun::Already(_)
    ));
    if let Some(shared) = b.worker.and_then(Worker::shared) {
        shared.request_stop();
        assert!(b.pool.task(0).wait_finished(WAIT));
    }
}
