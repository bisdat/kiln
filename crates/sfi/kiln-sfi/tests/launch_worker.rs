// SPDX-License-Identifier: MIT
//! The interpreter worker under the core's frozen backend (kiln.launch): a boundary with a worker
//! submits it at the plugin step's on_ready only when the record froze backend `kiln` and ShroudForge's own loader is
//! not in the process, looked for at main and once more right before the start. Every other record,
//! `KILN_SHROUDFORGE=0` and no table never submit it. A boundary without a worker refuses the start at on_ready (the old
//! stub start is gone).
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use kiln_sfi::worker::{Worker, WorkerPhase, empty_runtime};
use kiln_sfi::{Boundary, InactiveReason, Phase};
use std::cell::Cell;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
const LOADER_WHY: &str = "ShroudForge's own loader (C:\\g\\dbghelp.dll) runs it";

/// KILN_SHROUDFORGE is process-wide: every boot in this file holds this lock, and the one that sets the variable
/// removes it before letting go. Nothing else in this test binary reads the environment.
static ENV: Mutex<()> = Mutex::new(());

thread_local! {
    /// What the boundary's look for ShroudForge's own loader finds on this thread.
    static LOADER: Cell<Option<&'static str>> = const { Cell::new(None) };
}
fn own_loader() -> Option<String> {
    LOADER.get().map(str::to_owned)
}

#[derive(Clone, Copy, Default)]
struct Case {
    launch: Option<(i32, i32)>,
    env_off: bool,
    loader_at_main: bool,
    loader_at_ready: bool,
    /// The core has no kiln.tasks.
    no_tasks: bool,
    /// kiln.tasks refuses every submit with (result, refusal).
    refuse: Option<(KilnResult, u32)>,
}

struct Boot {
    boundary: &'static Boundary,
    worker: Option<&'static Worker>,
    early: KilnOwner,
    normal: KilnOwner,
    pool: Arc<FakePool>,
    early_pool: Arc<FakePool>,
    early_ready: bool,
    normal_ready: bool,
}
impl Boot {
    fn submissions(&self) -> usize {
        self.pool.submissions().len() + self.early_pool.submissions().len()
    }
    fn stub_lines(&self) -> usize {
        [self.early, self.normal]
            .into_iter()
            .flat_map(lines)
            .filter(|l| l.starts_with("kiln-sfi: interpreter start (stub)"))
            .count()
    }
    /// Lets a started worker go (an orderly stop) so the fake pool's thread ends.
    fn stop(&self) {
        if let Some(shared) = self.worker.and_then(Worker::shared) {
            shared.request_stop();
            assert!(self.pool.task(0).wait_finished(WAIT));
        }
    }
}

fn lines(owner: KilnOwner) -> Vec<String> {
    logs(owner).into_iter().map(|(_, text)| text).collect()
}
/// The interpreter's capability rows `owner` reported, in order.
fn rows(owner: KilnOwner) -> Vec<String> {
    reports(owner)
        .into_iter()
        .map(|(name, text)| {
            assert_eq!(name, "kiln-sfi.interpreter");
            text
        })
        .collect()
}

/// A real start's order for kiln-sfi: the early phase, then Kiln's plugin step and its on_ready (when installed).
fn boot(with_worker: bool, case: Case) -> Boot {
    let _env = ENV.lock().unwrap_or_else(|p| p.into_inner());
    set_launch(case.launch);
    LOADER.set(case.loader_at_main.then_some(LOADER_WHY));
    let worker: Option<&'static Worker> =
        with_worker.then(|| &*Box::leak(Box::new(Worker::new(empty_runtime))));
    let boundary = match worker {
        Some(worker) => Boundary::with_worker(InactiveReason::NegotiationMissing, worker),
        None => Boundary::new(),
    }
    .with_own_loader(own_loader);
    let boundary: &'static Boundary = Box::leak(Box::new(boundary));
    let (early, normal) = (next_owner(), next_owner());
    let early_pool = FakePool::new(early, PoolConfig::default());
    let pool = FakePool::new(
        normal,
        PoolConfig {
            refuse: case.refuse,
            ..PoolConfig::default()
        },
    );
    let api = api(!case.no_tasks);
    let (mut early_info, mut normal_info) = (plugin_info(), plugin_info());
    if case.env_off {
        // SAFETY: ENV is held; no other thread of this binary reads the environment.
        unsafe { std::env::set_var("KILN_SHROUDFORGE", "0") };
    }
    let entered = (
        unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut normal_info) },
    );
    if case.env_off {
        // SAFETY: as above.
        unsafe { std::env::remove_var("KILN_SHROUDFORGE") };
    }
    assert_eq!(entered, (KILN_OK, KILN_OK));
    assert!(
        !boundary.worker_started() && pool.submissions().is_empty(),
        "nothing starts before on_ready"
    );
    // ShroudForge's loader may arrive between the plugin's main and its on_ready.
    LOADER.set(case.loader_at_ready.then_some(LOADER_WHY));
    if let Some(ready) = normal_info.on_ready {
        unsafe { ready(normal_info.user) };
    }
    Boot {
        boundary,
        worker,
        early,
        normal,
        pool,
        early_pool,
        early_ready: early_info.on_ready.is_some(),
        normal_ready: normal_info.on_ready.is_some(),
    }
}

#[test]
fn with_a_worker_kiln_submits_it_at_on_ready_under_the_plugin_owner() {
    let b = boot(
        true,
        Case {
            launch: Some(KILN),
            ..Case::default()
        },
    );
    assert!(!b.early_ready, "the early phase installs no on_ready");
    assert!(b.normal_ready);
    assert!(b.boundary.worker_started());
    let submissions = b.pool.submissions();
    assert_eq!(submissions.len(), 1, "{submissions:?}");
    let s = &submissions[0];
    assert_eq!(s.owner, b.normal);
    assert_eq!(s.name, "interpreter");
    assert_eq!(
        s.flags,
        (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32
    );
    assert_eq!(s.result, KILN_OK);
    assert!(b.early_pool.submissions().is_empty());
    let shared = b.worker.unwrap().shared().expect("shared").clone();
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    // The stub is gone: no stub line.
    assert_eq!(b.stub_lines(), 0);
    // Both phases' rows say Kiln's interpreter has this start, and none claims there is no worker.
    for owner in [b.early, b.normal] {
        let rows = rows(owner);
        assert_eq!(rows.len(), 1, "{rows:?}");
        assert!(
            rows[0].starts_with(&format!(
                "Kiln's interpreter has this start ({LAUNCH_REASON})"
            )) && !rows[0].contains("no interpreter worker"),
            "{rows:?}"
        );
    }
    b.stop();
}

#[test]
fn with_a_worker_every_other_selection_never_submits_it() {
    let record = |backend, code| Case {
        launch: Some((backend, code)),
        ..Case::default()
    };
    let cases = [
        (
            "hosted",
            record(
                KILN_LAUNCH_BACKEND_HOSTED,
                KILN_LAUNCH_REASON_HOSTED_DEFAULT,
            ),
            "backend hosted: kiln-shroudforge has this start",
        ),
        (
            "hosted (environment)",
            record(KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_ENV_RUNTIME),
            "backend hosted: kiln-shroudforge has this start",
        ),
        (
            "installed",
            record(
                KILN_LAUNCH_BACKEND_INSTALLED,
                KILN_LAUNCH_REASON_USE_INSTALLED,
            ),
            "backend installed: kiln-shroudforge has this start",
        ),
        (
            "refused: interpreter held",
            record(
                KILN_LAUNCH_BACKEND_NONE,
                KILN_LAUNCH_REASON_INTERPRETER_HELD,
            ),
            "every ShroudForge backend stands aside this start",
        ),
        (
            "refused: setting invalid",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID),
            "every ShroudForge backend stands aside this start",
        ),
        (
            "refused: 1.8 veto",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SF18_VETO),
            "every ShroudForge backend stands aside this start",
        ),
        (
            "refused: plan missing",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_MISSING),
            "every ShroudForge backend stands aside this start",
        ),
        (
            "idle: safe idle",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE),
            "the core decided no ShroudForge backend for this start",
        ),
        (
            "idle: profile invalid",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PROFILE_INVALID),
            "the core decided no ShroudForge backend for this start",
        ),
        (
            "idle: undecided",
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_UNDECIDED),
            "the core decided no ShroudForge backend for this start",
        ),
        (
            "no table",
            Case::default(),
            "backend negotiation not integrated",
        ),
        (
            "unknown backend",
            record(7, KILN_LAUNCH_REASON_PLAN),
            "backend negotiation not integrated",
        ),
        (
            "none of neither class",
            record(KILN_LAUNCH_BACKEND_NONE, 0x300),
            "backend negotiation not integrated",
        ),
        (
            "kiln, KILN_SHROUDFORGE=0",
            Case {
                env_off: true,
                ..record(KILN.0, KILN.1)
            },
            "KILN_SHROUDFORGE=0",
        ),
        (
            "kiln, ShroudForge's own loader at main",
            Case {
                loader_at_main: true,
                ..record(KILN.0, KILN.1)
            },
            "Kiln's interpreter has this start",
        ),
    ];
    for (label, case, said) in cases {
        let b = boot(true, case);
        assert!(
            !b.early_ready && !b.normal_ready,
            "{label}: no on_ready in either phase"
        );
        assert!(!b.boundary.worker_started(), "{label}");
        assert_eq!(b.submissions(), 0, "{label}");
        assert!(b.worker.unwrap().shared().is_none(), "{label}");
        assert_eq!(b.stub_lines(), 0, "{label}");
        for owner in [b.early, b.normal] {
            let rows = rows(owner);
            assert!(
                rows.len() == 1 && rows[0].starts_with(said),
                "{label}: {rows:?}"
            );
        }
        if case.loader_at_main {
            let row = &rows(b.normal)[0];
            assert!(
                row.contains(LOADER_WHY) && row.ends_with("kiln-sfi stands aside"),
                "{row}"
            );
        }
    }
}

#[test]
fn with_a_worker_a_loader_that_arrives_after_main_stands_it_aside_at_on_ready() {
    let b = boot(
        true,
        Case {
            launch: Some(KILN),
            loader_at_ready: true,
            ..Case::default()
        },
    );
    assert!(b.normal_ready, "no loader at main: on_ready is installed");
    assert!(!b.boundary.worker_started());
    assert_eq!(b.submissions(), 0);
    assert!(b.worker.unwrap().shared().is_none());
    assert_eq!(b.stub_lines(), 0);
    // The same reason as when the loader is already there at main, in the row and in kiln.log.
    let at_main = boot(
        true,
        Case {
            launch: Some(KILN),
            loader_at_main: true,
            ..Case::default()
        },
    );
    let reason = rows(at_main.normal).pop().expect("a row");
    assert!(
        reason.contains(LOADER_WHY) && reason.ends_with("kiln-sfi stands aside"),
        "{reason}"
    );
    // Both phases' rows: a reader sees the oldest of equal reports, the early phase's.
    for owner in [b.early, b.normal] {
        let rows = rows(owner);
        assert_eq!(rows.len(), 2, "{rows:?}");
        assert_eq!(rows.last(), Some(&reason), "the row says it");
    }
    assert!(
        lines(b.normal).contains(&reason),
        "kiln.log says it: {:?}",
        lines(b.normal)
    );
}

#[test]
fn without_a_worker_kiln_is_refused_at_on_ready() {
    let b = boot(
        false,
        Case {
            launch: Some(KILN),
            ..Case::default()
        },
    );
    assert!(b.normal_ready && !b.early_ready);
    assert_eq!(b.stub_lines(), 0, "the stub start is gone");
    assert!(!b.boundary.worker_started());
    assert_eq!(b.submissions(), 0);
    assert_eq!(
        b.boundary.plan_reads(),
        0,
        "a boundary that cannot start reads no plan"
    );
    let record = b.boundary.status().record().expect("a record");
    assert_eq!(record.phase(), kiln_sfi::state::Phase::Refused);
    for owner in [b.early, b.normal] {
        let rows = rows(owner);
        assert!(
            rows.len() == 1 && rows[0].contains("no interpreter worker yet"),
            "{rows:?}"
        );
    }
    // A loader that arrives after main stands it aside with its own reason.
    let late = boot(
        false,
        Case {
            launch: Some(KILN),
            loader_at_ready: true,
            ..Case::default()
        },
    );
    assert_eq!(late.stub_lines(), 0);
    assert!(
        lines(late.normal)
            .iter()
            .any(|l| l.contains(LOADER_WHY) && l.ends_with("kiln-sfi stands aside")),
        "{:?}",
        lines(late.normal)
    );
}

#[test]
fn with_a_worker_a_start_kiln_tasks_refuses_says_so_in_both_rows() {
    let cases = [
        (
            "no kiln.tasks",
            Case {
                launch: Some(KILN),
                no_tasks: true,
                ..Case::default()
            },
            "kiln.tasks is not available",
        ),
        (
            "QUEUE_FULL",
            Case {
                launch: Some(KILN),
                refuse: Some((KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_QUEUE_FULL as u32)),
                ..Case::default()
            },
            "kiln.tasks refused its worker (QUEUE_FULL",
        ),
        (
            "SHUTDOWN",
            Case {
                launch: Some(KILN),
                refuse: Some((KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_SHUTDOWN as u32)),
                ..Case::default()
            },
            "kiln.tasks refused its worker (SHUTDOWN",
        ),
    ];
    for (label, case, why) in cases {
        let b = boot(true, case);
        assert!(b.normal_ready, "{label}");
        assert_eq!(
            b.worker.unwrap().shared().map(|s| s.phase()),
            Some(WorkerPhase::Stopped),
            "{label}"
        );
        assert_eq!(b.stub_lines(), 0, "{label}");
        // Main's row said the worker starts at the plugin step; once it did not, both rows say why (readers see
        // the oldest of equal reports, the early phase's).
        for owner in [b.early, b.normal] {
            let rows = rows(owner);
            assert_eq!(rows.len(), 2, "{label}: {rows:?}");
            let last = &rows[1];
            assert!(
                last.starts_with(&format!(
                    "Kiln's interpreter has this start ({LAUNCH_REASON}), but {why}"
                )) && last.ends_with("no ShroudForge mod runs"),
                "{label}: {rows:?}"
            );
        }
    }
}
