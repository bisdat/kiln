// SPDX-License-Identifier: MIT
//! The interpreter worker as one LongRunning RESIDENT kiln.tasks task,
//! against the fake pool in `support` (real body threads, kiln_tasks.h's cancel,
//! drain and park rules).
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use kiln_sfi::worker::*;
use kiln_sfi::{Boundary, InactiveReason, Phase};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::thread::ThreadId;
use std::time::{Duration, Instant};
use support::*;

// ---- a probe runtime: records where and what the worker runs ----

#[derive(Default)]
struct Probe {
    threads: Mutex<Vec<ThreadId>>,
    commands: Mutex<Vec<WorkerCommand>>,
    turns: AtomicU64,
    stopped: AtomicBool,
    dropped: AtomicBool,
    panic_in_turn: AtomicBool,
    fail_create: AtomicBool,
}
static PROBES: LazyLock<Mutex<HashMap<KilnOwner, Arc<Probe>>>> = LazyLock::new(Default::default);
fn probe(owner: KilnOwner) -> Arc<Probe> {
    PROBES.lock().unwrap().entry(owner).or_default().clone()
}
impl Probe {
    fn here(&self) {
        self.threads
            .lock()
            .unwrap()
            .push(std::thread::current().id());
    }
}

struct ProbeRuntime(Arc<Probe>);
impl WorkerRuntime for ProbeRuntime {
    fn command(&mut self, _: &WorkerContext<'_>, command: WorkerCommand) {
        self.0.here();
        self.0.commands.lock().unwrap().push(command);
    }
    fn turn(&mut self, _: &WorkerContext<'_>, _: Duration) {
        self.0.here();
        self.0.turns.fetch_add(1, Ordering::SeqCst);
        if self.0.panic_in_turn.load(Ordering::SeqCst) {
            // An unwind that skips the panic hook: with RUST_BACKTRACE set (a developer's
            // shell sets it) the default hook captures and symbolizes a backtrace on this
            // body thread before catch_unwind returns, reading the test binary's debug
            // info (about 0.5 s cold here, and many seconds under idle-priority IO). The
            // worker's catch_unwind sees the same unwind either way.
            std::panic::resume_unwind(Box::new("probe: panic inside a turn"));
        }
    }
    fn stop(&mut self, _: &WorkerContext<'_>) {
        self.0.here();
        self.0.stopped.store(true, Ordering::SeqCst);
    }
}
impl Drop for ProbeRuntime {
    fn drop(&mut self) {
        self.0.here();
        self.0.dropped.store(true, Ordering::SeqCst);
    }
}
fn probe_factory(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
    let probe = probe(cx.owner());
    probe.here();
    if probe.fail_create.load(Ordering::SeqCst) {
        return Err("probe: create refused".into());
    }
    Ok(Box::new(ProbeRuntime(probe)))
}

struct Started {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    shared: Arc<WorkerShared>,
    result: Result<KilnHandle, StartError>,
}
fn start_with(config: PoolConfig, before: impl FnOnce(&Probe)) -> Started {
    let owner = next_owner();
    let pool = FakePool::new(owner, config);
    before(&probe(owner));
    submit_on(pool, Some(version_with_resident))
}
type VersionFn = unsafe extern "C" fn() -> KilnStr;
/// Submits the worker of `pool`'s owner on a Kiln whose `KilnApi.version` is `version`.
fn submit_on(pool: Arc<FakePool>, version: Option<VersionFn>) -> Started {
    let owner = pool.owner;
    let mut api = api(true);
    api.version = version;
    let shared = WorkerShared::new(WorkerConfig {
        owner,
        host: unsafe { Host::read(&*api) },
        factory: probe_factory,
        inbox: InboxConfig::DEFAULT,
    });
    let tasks = unsafe { TasksTable::read(tasks_table()) }.expect("fake table");
    let result = submit(&shared, &tasks);
    Started {
        owner,
        pool,
        shared,
        result,
    }
}
fn start(config: PoolConfig) -> Started {
    start_with(config, |_| {})
}
/// A liveness bound, not a latency claim: how long a wait for something that must happen
/// (the worker running, a body returning, done) holds before the test reports a hang.
/// Latencies are measured and asserted on their own (leave_latency, exit park). A passing
/// wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause at nice 19 on a loaded machine.
const WAIT: Duration = Duration::from_secs(300);
fn running(s: &Started) {
    assert!(
        eventually(WAIT, || s.shared.phase() == WorkerPhase::Running),
        "worker never ran: {:?}",
        s.shared.status()
    );
}
fn errors(owner: KilnOwner) -> Vec<String> {
    at_level(owner, KILN_LOG_ERROR)
}
fn at_level(owner: KilnOwner, wanted: i32) -> Vec<String> {
    logs(owner)
        .into_iter()
        .filter(|(level, _)| *level == wanted)
        .map(|(_, text)| text)
        .collect()
}
/// The worker's own submissions (a [`ResidentHold`] is another task of the same owner).
fn worker_submissions(pool: &FakePool) -> Vec<Submission> {
    let all = pool.submissions().into_iter();
    all.filter(|s| s.name == TASK_NAME).collect()
}

// ---- submission ----

#[test]
fn on_ready_submits_one_resident_long_running_task_named_interpreter_under_the_plugin_owner() {
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(probe_factory)));
    let boundary = Boundary::with_worker(InactiveReason::NegotiationMissing, worker);
    // The worker starts only when kiln.launch froze backend kiln.
    set_launch(Some((KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let early_pool = FakePool::new(early, PoolConfig::default());
    let api = api(true);
    let (mut early_info, mut normal_info) = (plugin_info(), plugin_info());
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
        KILN_OK
    );
    assert!(
        early_info.on_ready.is_none(),
        "the early phase has no worker"
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut normal_info) },
        KILN_OK
    );
    assert!(!boundary.worker_started(), "nothing starts before on_ready");
    assert!(pool.submissions().is_empty());
    let on_ready = normal_info.on_ready.expect("on_ready installed");
    unsafe { on_ready(normal_info.user) };
    assert!(boundary.worker_started());
    let submissions = pool.submissions();
    assert_eq!(submissions.len(), 1, "{submissions:?}");
    let s = &submissions[0];
    assert_eq!(s.owner, normal);
    assert_eq!(
        s.flags,
        (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32
    );
    assert_eq!(s.priority, KILN_TASK_PRIORITY_NORMAL as u32);
    assert_eq!(s.name, "interpreter");
    assert_eq!(s.result, KILN_OK);
    assert!(early_pool.submissions().is_empty());
    assert!(
        logs(normal)
            .iter()
            .all(|(level, _)| *level != KILN_LOG_WARN),
        "{:?}",
        logs(normal)
    );
    let shared = worker.shared().expect("shared").clone();
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    assert!(shared.status().resident);
    // A second on_ready (or any later start) never submits again.
    unsafe { on_ready(normal_info.user) };
    assert_eq!(worker.start(), Err(StartError::AlreadyStarted));
    assert_eq!(pool.submissions().len(), 1);
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
}

/// The fake pool's cap: main's kiln_tasks.h (PR #100) allows one resident task per
/// owner. A second one while the first is queued or running (a cancelled body that
/// has not returned yet included) is refused INVALID_ARGUMENT + ARGUMENT, after the
/// exit and owner-state refusals. The worker submits exactly one, so with this cap
/// in the fake every test above would see a second resident submission as a
/// refusal and a fallback.
#[test]
fn the_fake_pool_refuses_a_second_resident_task_of_an_owner_as_kiln_tasks_does() {
    use std::ffi::c_void;
    static HOLD: AtomicBool = AtomicBool::new(true);
    unsafe extern "C" fn held_body(run: *const KilnTaskRun, _: *mut c_void) {
        let cancelled = unsafe { (*run).cancelled }.expect("cancelled");
        while unsafe { cancelled(run) } == 0 || HOLD.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    unsafe extern "C" fn no_done(_: KilnHandle, _: u32, _: *mut c_void) {}
    let name = "cap";
    let info = |flags: i32| KilnTaskInfo {
        size: std::mem::size_of::<KilnTaskInfo>() as u32,
        flags: flags as u32,
        name: KilnStr {
            ptr: name.as_ptr().cast(),
            len: name.len(),
        },
        priority: KILN_TASK_PRIORITY_NORMAL as u32,
        reserved: 0,
    };
    let submit = |owner: KilnOwner, flags: i32| {
        let (mut task, mut refusal) = (0, KILN_TASK_REFUSAL_NONE as u32);
        let info = info(flags);
        let result = unsafe {
            ((*tasks_table()).submit.expect("submit"))(
                owner,
                &info,
                Some(held_body),
                Some(no_done),
                std::ptr::null_mut(),
                &mut task,
                &mut refusal,
            )
        };
        (result, task, refusal)
    };
    let resident = KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT;
    let argument = (
        KILN_ERR_INVALID_ARGUMENT,
        0,
        KILN_TASK_REFUSAL_ARGUMENT as u32,
    );

    // One owner: the first resident task runs; a second is refused; a plain
    // LongRunning one is not capped.
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let (result, first, _) = submit(owner, resident);
    assert_eq!(result, KILN_OK);
    assert!(eventually(WAIT, || pool.task(0).state() == KILN_TASK_RUNNING as u32));
    assert_eq!(submit(owner, resident), argument, "a second resident task");
    assert_eq!(
        submit(owner, KILN_TASK_LONG_RUNNING).0,
        KILN_OK,
        "plain LongRunning"
    );
    // Cancelled, its body still running: still capped.
    assert_eq!(pool.cancel(first), KILN_TASK_RUNNING as u32);
    assert_eq!(submit(owner, resident), argument, "cancelled, not returned");
    // The task owner's state refusal comes first: a sealed owner hears OWNER_SEALED.
    pool.drain_poll();
    assert_eq!(
        submit(owner, resident),
        (
            KILN_ERR_UNAVAILABLE,
            0,
            KILN_TASK_REFUSAL_OWNER_SEALED as u32
        )
    );

    // Another owner: once its resident task ended, the next one is accepted (and
    // queued: it holds the cap again).
    let other = next_owner();
    let other_pool = FakePool::new(other, PoolConfig::default());
    let (result, task, _) = submit(other, resident);
    assert_eq!(result, KILN_OK);
    assert!(eventually(WAIT, || other_pool.task(0).state()
        == KILN_TASK_RUNNING as u32));
    assert_eq!(other_pool.cancel(task), KILN_TASK_RUNNING as u32);
    assert_eq!(submit(other, resident), argument);
    HOLD.store(false, Ordering::SeqCst);
    assert!(pool.task(0).wait_finished(WAIT));
    assert!(other_pool.task(0).wait_finished(WAIT));
    assert_eq!(
        submit(other, resident).0,
        KILN_OK,
        "after the first one ended"
    );
    let submissions = other_pool.submissions();
    assert_eq!(
        submissions
            .iter()
            .map(|s| (s.result, s.refusal))
            .collect::<Vec<_>>(),
        vec![
            (KILN_OK, KILN_TASK_REFUSAL_NONE as u32),
            (KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT as u32),
            (KILN_OK, KILN_TASK_REFUSAL_NONE as u32),
        ]
    );
    other_pool.drain_poll();
}

#[test]
fn the_inactive_boundary_installs_no_on_ready_and_starts_nothing() {
    let boundary = Boundary::new();
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = api(true);
    let mut info = plugin_info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, owner, &mut info) },
        KILN_OK
    );
    assert!(info.on_ready.is_none() && info.user.is_null());
    assert!(!boundary.worker_started());
    assert!(pool.submissions().is_empty());
}

#[test]
fn the_body_runs_on_one_thread_and_the_state_never_leaves_it() {
    let s = start(PoolConfig::default());
    assert!(s.result.is_ok());
    running(&s);
    for i in 0..5 {
        s.shared
            .submit(WorkerCommand::UiAction(UiAction {
                package: "p".into(),
                action: format!("a{i}"),
                payload: vec![],
            }))
            .unwrap();
    }
    let probe = probe(s.owner);
    assert!(eventually(WAIT, || probe.commands.lock().unwrap().len()
        == 5
        && probe.turns.load(Ordering::SeqCst) >= 3));
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
    let body = s
        .pool
        .task(0)
        .body_thread
        .lock()
        .unwrap()
        .expect("body ran");
    assert_ne!(body, std::thread::current().id());
    assert_eq!(s.shared.worker_thread(), Some(body));
    let threads = probe.threads.lock().unwrap().clone();
    assert!(threads.len() > 8, "create, commands, turns, stop, drop");
    assert!(
        threads.iter().all(|t| *t == body),
        "every runtime entry (create, command, turn, stop, drop) is on the body's thread"
    );
    assert!(probe.stopped.load(Ordering::SeqCst) && probe.dropped.load(Ordering::SeqCst));
}

// ---- refusals ----

#[test]
fn refusals_mean_no_start_with_the_status_reason() {
    for refusal in [
        KILN_TASK_REFUSAL_CAPABILITY,
        KILN_TASK_REFUSAL_QUEUE_FULL,
        KILN_TASK_REFUSAL_SHUTDOWN,
    ] {
        let s = start(PoolConfig {
            refuse: Some((KILN_ERR_UNAVAILABLE, refusal as u32)),
            ..PoolConfig::default()
        });
        assert_eq!(
            s.result,
            Err(StartError::Refused(KILN_ERR_UNAVAILABLE, refusal as u32))
        );
        assert_eq!(s.pool.task_count(), 0, "no task was accepted");
        assert!(
            probe(s.owner).threads.lock().unwrap().is_empty(),
            "no body ran"
        );
        let status = s.shared.status();
        assert_eq!(status.phase, WorkerPhase::Stopped);
        assert_eq!(status.reason, StopReason::Refused);
        assert_eq!(status.refusal, refusal as u32);
        assert!(
            status.line().contains(refusal_text(refusal as u32)),
            "{}",
            status.line()
        );
        let errors = errors(s.owner);
        assert_eq!(errors.len(), 1, "one loud warning: {errors:?}");
        assert!(errors[0].contains("interpreter not started"));
        assert_eq!(
            Arc::strong_count(&s.shared),
            1,
            "a refused submit transfers nothing"
        );
        assert_eq!(
            s.shared.submit(WorkerCommand::InputSnapshot(InputSnapshot {
                payload: vec![]
            })),
            Err(Refused::Stopped)
        );
    }
}

#[test]
fn a_missing_tasks_service_means_no_start() {
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(probe_factory)));
    let owner = next_owner();
    let api = api(false);
    assert!(worker.bind(unsafe { Host::read(&*api) }, owner));
    assert_eq!(worker.start(), Err(StartError::NoTasksService));
    let status = worker.shared().unwrap().status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::NoTasksService);
    assert_eq!(errors(owner).len(), 1);
}

#[test]
fn a_queued_worker_shows_waiting_for_a_task_thread() {
    let s = start(PoolConfig {
        threads: 0,
        ..PoolConfig::default()
    });
    assert!(s.result.is_ok());
    std::thread::sleep(Duration::from_millis(50));
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Queued);
    assert_eq!(status.line(), "waiting for a task thread");
    assert!(probe(s.owner).threads.lock().unwrap().is_empty());
    s.pool.free_thread();
    running(&s);
    assert_eq!(
        s.shared.status().line(),
        "interpreter running: 0 mods active, 0 failed"
    );
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
}

#[test]
fn a_queued_worker_cancelled_before_start_never_runs_and_done_reclaims_once() {
    let s = start(PoolConfig {
        threads: 0,
        ..PoolConfig::default()
    });
    assert_eq!(Arc::strong_count(&s.shared), 2);
    assert_eq!(s.pool.cancel(s.shared.task()), KILN_TASK_CANCELLED as u32);
    assert!(s.pool.task(0).wait_finished(WAIT));
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::CancelledBeforeStart);
    assert!(probe(s.owner).threads.lock().unwrap().is_empty());
    assert_eq!(Arc::strong_count(&s.shared), 1);
}

// ---- leaving ----

#[test]
fn cancelled_leaves_within_16_ms_when_idle() {
    let s = start(PoolConfig::default());
    running(&s);
    // Idle: a few empty turns.
    assert!(eventually(WAIT, || probe(s.owner)
        .turns
        .load(Ordering::SeqCst)
        >= 3));
    let task = s.pool.task(0);
    s.pool.cancel(task.handle);
    assert!(task.wait_finished(WAIT));
    let latency = task.leave_latency().expect("cancel and return seen");
    assert!(
        s.shared.longest_park_ns() <= TURN_GAP.as_nanos() as u64,
        "no single park is longer than the 16 ms gap: {} ns",
        s.shared.longest_park_ns()
    );
    // 16 ms of park plus OS wake-up slack on a loaded, niced test machine.
    assert!(
        latency <= Duration::from_millis(16 + 24),
        "left after {latency:?}"
    );
    let probe = probe(s.owner);
    let status = s.shared.status();
    assert_eq!(status.reason, StopReason::Cancelled);
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert!(
        !probe.stopped.load(Ordering::SeqCst) && !probe.dropped.load(Ordering::SeqCst),
        "a cancellation kiln-sfi did not ask for is a fast leave: no stop, no drop"
    );
    eprintln!("cancelled -> body returned in {latency:?}");
}

#[test]
fn an_orderly_stop_runs_stop_then_drops_the_state_on_the_worker() {
    let s = start(PoolConfig::default());
    running(&s);
    assert_eq!(
        s.shared.submit(WorkerCommand::Stop),
        Ok(Accepted::Queued),
        "Stop is never refused"
    );
    assert!(s.pool.task(0).wait_finished(WAIT));
    let probe = probe(s.owner);
    assert!(probe.stopped.load(Ordering::SeqCst));
    assert!(probe.dropped.load(Ordering::SeqCst));
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::StopRequested);
    assert_eq!(
        Arc::strong_count(&s.shared),
        1,
        "done took its reference back"
    );
}

#[test]
fn stop_and_cancel_together_leave_fast() {
    let s = start(PoolConfig::default());
    running(&s);
    // Cancel first, then stop: the pool's cancellation wins.
    let task = s.pool.task(0);
    s.pool.cancel(task.handle);
    s.shared.request_stop();
    assert!(task.wait_finished(WAIT));
    let probe = probe(s.owner);
    assert!(!probe.stopped.load(Ordering::SeqCst) && !probe.dropped.load(Ordering::SeqCst));
}

#[test]
fn a_failed_runtime_create_stops_with_the_reason() {
    let s = start_with(PoolConfig::default(), |p| {
        p.fail_create.store(true, Ordering::SeqCst)
    });
    assert!(s.pool.task(0).wait_finished(WAIT));
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::CreateFailed);
    assert_eq!(status.detail.as_deref(), Some("probe: create refused"));
    assert!(status.line().contains("probe: create refused"));
    assert_eq!(errors(s.owner).len(), 1);
    assert_eq!(Arc::strong_count(&s.shared), 1);
}

#[test]
fn a_panic_in_the_body_is_caught_done_still_runs_and_status_is_faulted() {
    let s = start_with(PoolConfig::default(), |p| {
        p.panic_in_turn.store(true, Ordering::SeqCst)
    });
    let task = s.pool.task(0);
    assert!(task.wait_finished(WAIT));
    assert_eq!(task.done_calls.load(Ordering::SeqCst), 1, "done still ran");
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Faulted);
    assert_eq!(status.reason, StopReason::Panicked);
    assert!(status.line().contains("caught panic"), "{}", status.line());
    assert_eq!(Arc::strong_count(&s.shared), 1);
    assert!(
        task.released.load(Ordering::SeqCst),
        "done released the record"
    );
    let probe = probe(s.owner);
    assert!(
        !probe.stopped.load(Ordering::SeqCst) && !probe.dropped.load(Ordering::SeqCst),
        "a panic is not an orderly leave: the state is leaked, never dropped while unwinding \
         (no Lua close, no mod __gc after an internal error)"
    );
}

#[test]
fn a_refused_fallback_submit_reclaims_the_arc_exactly_once() {
    // A Kiln older than RESIDENT refuses the bit with ARGUMENT, then refuses the plain retry.
    let config = PoolConfig {
        l1: false,
        refuse: Some((KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_CAPABILITY as u32)),
        ..PoolConfig::default()
    };
    let s = submit_on(
        FakePool::new(next_owner(), config),
        Some(version_before_resident),
    );
    assert_eq!(
        s.result,
        Err(StartError::Refused(
            KILN_ERR_UNAVAILABLE,
            KILN_TASK_REFUSAL_CAPABILITY as u32
        ))
    );
    let submissions = s.pool.submissions();
    assert_eq!(submissions.len(), 2, "{submissions:?}");
    assert_eq!(submissions[0].refusal, KILN_TASK_REFUSAL_ARGUMENT as u32);
    assert_eq!(submissions[1].flags, KILN_TASK_LONG_RUNNING as u32);
    assert_eq!(s.pool.task_count(), 0);
    assert_eq!(
        Arc::strong_count(&s.shared),
        1,
        "neither refused attempt kept the user reference, and it was reclaimed once"
    );
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::Refused);
    assert_eq!(status.refusal, KILN_TASK_REFUSAL_CAPABILITY as u32);
    assert_eq!(errors(s.owner).len(), 1, "one loud warning");
}

#[test]
fn an_exit_park_sees_the_body_leave_fast_and_done_owns_the_only_reclaim() {
    let s = start(PoolConfig::default());
    running(&s);
    assert!(eventually(WAIT, || probe(s.owner)
        .turns
        .load(Ordering::SeqCst)
        >= 2));
    // tasks.md "Exit": SHUTDOWN, seal + signal, then the park (500 ms).
    let outcome = s.pool.exit(Duration::from_millis(500));
    assert!(
        outcome.returned,
        "the body returned inside the park: {outcome:?}"
    );
    assert!(
        outcome.waited <= Duration::from_millis(100),
        "back within 100 ms of the signal: {outcome:?}"
    );
    let task = s.pool.task(0);
    // A body that returns after the park began gets no done (not guaranteed at
    // exit), so the pool's reference leaks with the process; it is never freed
    // early. Only in the tiny window between signal and park can done still run,
    // and then it is the one reclaim.
    let done = task.done_calls.load(Ordering::SeqCst) as usize;
    assert_eq!(
        done + (Arc::strong_count(&s.shared) - 1),
        1,
        "exactly one of: done reclaimed it, or it leaked (done {done})"
    );
    let probe = probe(s.owner);
    assert!(
        !probe.stopped.load(Ordering::SeqCst) && !probe.dropped.load(Ordering::SeqCst),
        "the exit is an external cancellation: fast leave"
    );
    assert_eq!(s.shared.status().reason, StopReason::Cancelled);
    eprintln!("exit park: {outcome:?}, done calls {done}");
}

#[test]
fn a_faulted_done_sets_faulted() {
    let s = start(PoolConfig {
        fault_done: true,
        ..PoolConfig::default()
    });
    running(&s);
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Faulted);
    assert_eq!(status.reason, StopReason::NativeFault);
    assert!(status.line().contains("native fault"));
}

#[test]
fn done_reclaims_the_arc_exactly_once() {
    for _ in 0..20 {
        let s = start(PoolConfig::default());
        assert_eq!(
            Arc::strong_count(&s.shared),
            2,
            "one reference went to the pool"
        );
        running(&s);
        s.shared.request_stop();
        let task = s.pool.task(0);
        assert!(task.wait_finished(WAIT));
        assert_eq!(task.done_calls.load(Ordering::SeqCst), 1);
        assert_eq!(Arc::strong_count(&s.shared), 1);
    }
}

#[test]
fn an_older_kiln_refusing_the_resident_bit_falls_back_once_to_plain_long_running() {
    // KilnApi.version() below 0.2.0-dev.29.19: ARGUMENT says the bit is unknown.
    let config = PoolConfig {
        l1: false,
        ..PoolConfig::default()
    };
    let s = submit_on(
        FakePool::new(next_owner(), config),
        Some(version_before_resident),
    );
    assert!(s.result.is_ok());
    let submissions = s.pool.submissions();
    assert_eq!(submissions.len(), 2, "{submissions:?}");
    assert_eq!(
        submissions[0].flags,
        (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32
    );
    assert_eq!(submissions[0].result, KILN_ERR_INVALID_ARGUMENT);
    assert_eq!(submissions[0].refusal, KILN_TASK_REFUSAL_ARGUMENT as u32);
    assert_eq!(submissions[1].flags, KILN_TASK_LONG_RUNNING as u32);
    assert_eq!(submissions[1].result, KILN_OK);
    running(&s);
    assert!(!s.shared.status().resident);
    let warnings: Vec<_> = logs(s.owner)
        .into_iter()
        .filter(|(level, _)| *level == KILN_LOG_WARN)
        .collect();
    assert_eq!(warnings.len(), 1);
    assert!(warnings[0].1.contains("watchdog"));
    assert!(
        warnings[0].1.contains("this Kiln has no resident tasks"),
        "{warnings:?}"
    );
    assert!(errors(s.owner).is_empty());
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
}

/// main's cap (kiln_tasks.h "Resident tasks"): on a Kiln that has RESIDENT, ARGUMENT for
/// the worker's resident submit also means kiln-sfi's owner already holds a resident
/// task. A plain retry would start a second interpreter body for the same owner, so
/// the refusal is the start's failure: one loud line, the reason in the status, no
/// other submission.
#[test]
fn a_kiln_with_resident_tasks_reports_the_cap_refusal_and_submits_nothing_else() {
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let hold = hold_resident(&pool, WAIT);
    let s = submit_on(pool.clone(), Some(version_with_resident));
    assert_eq!(
        s.result,
        Err(StartError::Refused(
            KILN_ERR_INVALID_ARGUMENT,
            KILN_TASK_REFUSAL_ARGUMENT as u32
        ))
    );
    let mine = worker_submissions(&pool);
    assert_eq!(mine.len(), 1, "no second submission: {mine:?}");
    assert_eq!(
        mine[0].flags,
        (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32
    );
    assert_eq!(
        (mine[0].result, mine[0].refusal),
        (KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT as u32)
    );
    assert_eq!(pool.task_count(), 1, "only the hold was accepted");
    assert!(
        probe(owner).threads.lock().unwrap().is_empty(),
        "no body ran"
    );
    assert!(
        at_level(owner, KILN_LOG_WARN).is_empty(),
        "not the older-Kiln line: {:?}",
        logs(owner)
    );
    let errors = errors(owner);
    assert_eq!(errors.len(), 1, "one loud line: {errors:?}");
    assert!(errors[0].contains("interpreter not started"), "{errors:?}");
    assert!(
        errors[0].contains(KILN_WITH_RESIDENT) && errors[0].contains("already holds"),
        "{errors:?}"
    );
    assert!(
        errors[0].contains("update Kiln"),
        "says what to do: {errors:?}"
    );
    let status = s.shared.status();
    assert_eq!(status.phase, WorkerPhase::Stopped);
    assert_eq!(status.reason, StopReason::Refused);
    assert_eq!(status.refusal, KILN_TASK_REFUSAL_ARGUMENT as u32);
    assert!(!status.resident);
    assert!(
        status
            .detail
            .as_deref()
            .is_some_and(|d| d.contains("already holds")),
        "{status:?}"
    );
    assert!(status.line().contains("already holds"), "{}", status.line());
    assert_eq!(
        Arc::strong_count(&s.shared),
        1,
        "the refused submit transferred nothing"
    );
    assert!(hold.let_go(WAIT));
    assert_eq!(worker_submissions(&pool).len(), 1, "nothing later either");
}

/// KilnApi.version() against RESIDENT's release (kiln_tasks.h: since 0.2.0-dev.29.19),
/// in tools/release/version.sh's forms: X.Y.Z, X.Y.Z-dev.N, a quick fix X.Y.Z-dev.N.M, and a
/// source build's X.Y.Z-dev. Only a clear "older" says yes.
#[test]
fn predates_resident_reads_the_version_against_the_release_that_added_it() {
    assert_eq!(RESIDENT_SINCE, KILN_WITH_RESIDENT);
    for older in [
        KILN_BEFORE_RESIDENT,
        "0.2.0-dev.29.9",
        "0.2.0-dev.29.1",
        "0.2.0-dev.29",
        "0.2.0-dev.28",
        "0.2.0-dev.9",
        "0.1.4",
        "0.1.0-dev",
        "0.2.0-dev.29.18+20261115.ddb9bd4",
        " 0.2.0-dev.29.18 ",
    ] {
        assert!(predates_resident(older), "{older}");
    }
    for has_it in [
        KILN_WITH_RESIDENT,
        "0.2.0-dev.29.19+20261120.0123abc",
        "0.2.0-dev.29.20",
        "0.2.0-dev.29.100",
        "0.2.0-dev.30",
        "0.2.0-dev.30.1",
        "0.2.0",
        "0.2.1-dev",
        "1.0.0",
        // a source build of the same line: any commit, cannot tell, so no
        "0.2.0-dev",
        "0.2.0-dev+20261115.ddb9bd4",
        // no semantic version: cannot tell, so no
        "",
        "kiln (local build)",
        "0.2",
        "v0.2.0-dev.29.18",
    ] {
        assert!(!predates_resident(has_it), "{has_it}");
    }
}

/// Only a Kiln that says it is older than RESIDENT gets the plain retry: one that has it
/// (whatever its ARGUMENT is about), and one whose version cannot be read, do not.
#[test]
fn an_argument_refusal_is_retried_plain_only_when_the_version_predates_resident() {
    let versions: [(&str, Option<VersionFn>); 4] = [
        ("with resident", Some(version_with_resident)),
        ("with resident, padded", Some(version_padded)),
        ("unreadable", Some(version_unreadable)),
        ("no version member", None),
    ];
    for (label, version) in versions {
        let config = PoolConfig {
            l1: false,
            ..PoolConfig::default()
        };
        let s = submit_on(FakePool::new(next_owner(), config), version);
        assert_eq!(
            s.result,
            Err(StartError::Refused(
                KILN_ERR_INVALID_ARGUMENT,
                KILN_TASK_REFUSAL_ARGUMENT as u32
            )),
            "{label}"
        );
        assert_eq!(s.pool.submissions().len(), 1, "{label}");
        assert_eq!(s.pool.task_count(), 0, "{label}");
        assert!(at_level(s.owner, KILN_LOG_WARN).is_empty(), "{label}");
        let errors = errors(s.owner);
        assert_eq!(errors.len(), 1, "{label}");
        assert!(!errors[0].contains('\n'), "{label}: one line: {errors:?}");
        assert_eq!(Arc::strong_count(&s.shared), 1, "{label}");
        assert_eq!(s.shared.status().reason, StopReason::Refused, "{label}");
        let detail = s.shared.status().detail.unwrap_or_default();
        assert!(
            detail.contains("update Kiln"),
            "{label}: says what to do: {detail}"
        );
    }
}

/// `KilnApi.version` with the whitespace of a VERSION file read without trimming.
unsafe extern "C" fn version_padded() -> KilnStr {
    const TEXT: &str = " 0.2.0-dev.29.19\n";
    KilnStr {
        ptr: TEXT.as_ptr().cast(),
        len: TEXT.len(),
    }
}

thread_local! {
    /// What [`version_answer`] returns on this thread: (pointer, length).
    static ANSWER: std::cell::Cell<(usize, usize)> = const { std::cell::Cell::new((0, 0)) };
}
unsafe extern "C" fn version_answer() -> KilnStr {
    let (ptr, len) = ANSWER.get();
    KilnStr {
        ptr: (ptr as *const u8).cast(),
        len,
    }
}

/// `Host::kiln_version` copies only a bounded UTF-8 answer of a table long enough for the
/// member, without its surrounding whitespace: anything else is `None` (no plain retry).
#[test]
fn kiln_version_copies_only_a_bounded_utf8_answer() {
    static LONG: [u8; 257] = [b'1'; 257];
    static NOT_UTF8: [u8; 7] = *b"0.2.0-\xff";
    let read = |bytes: Option<&'static [u8]>, len: usize| {
        ANSWER.set((bytes.map_or(0, |b| b.as_ptr() as usize), len));
        let mut api = api(true);
        api.version = Some(version_answer);
        unsafe { Host::read(&*api) }.kiln_version()
    };
    let padded: &'static [u8] = b" 0.2.0-dev.29.18\n";
    assert_eq!(
        read(Some(padded), padded.len()).as_deref(),
        Some(KILN_BEFORE_RESIDENT)
    );
    assert_eq!(read(Some(&LONG), 256).map(|v| v.len()), Some(256));
    assert_eq!(read(Some(&LONG), 257), None, "over 256 bytes");
    assert_eq!(read(None, 5), None, "NULL");
    assert_eq!(read(Some(padded), 0), None, "empty");
    assert_eq!(read(Some(b" \n\t"), 3), None, "blank");
    assert_eq!(read(Some(&NOT_UTF8), NOT_UTF8.len()), None, "not UTF-8");
    let mut api = api(true);
    assert_eq!(
        unsafe { Host::read(&*api) }.kiln_version().as_deref(),
        Some(KILN_WITH_RESIDENT)
    );
    api.size = std::mem::offset_of!(KilnApi, version) as u32;
    assert_eq!(
        unsafe { Host::read(&*api) }.kiln_version(),
        None,
        "a table too short for the member"
    );
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.version = None;
    assert_eq!(
        unsafe { Host::read(&*api) }.kiln_version(),
        None,
        "no member"
    );
}

// ---- cadence and the inbox through the worker ----

#[test]
fn early_wakes_never_start_an_extra_turn() {
    let s = start(PoolConfig::default());
    running(&s);
    let probe = probe(s.owner);
    let before = probe.turns.load(Ordering::SeqCst);
    let started = Instant::now();
    // Flood the worker with unparks for 200 ms.
    while started.elapsed() < Duration::from_millis(200) {
        let _ = s.shared.submit(WorkerCommand::InputSnapshot(InputSnapshot {
            payload: vec![1],
        }));
        s.shared.unpark();
    }
    let turns = probe.turns.load(Ordering::SeqCst) - before;
    let elapsed = started.elapsed();
    let ceiling = elapsed.as_millis() as u64 / 16 + 2;
    assert!(
        turns <= ceiling,
        "{turns} turns in {elapsed:?}: an unpark started a turn"
    );
    assert!(turns >= 1, "the worker kept turning");
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
}

#[test]
fn drained_commands_reach_the_runtime_before_the_next_turn() {
    let s = start(PoolConfig::default());
    running(&s);
    let lifecycle = |session, event| {
        WorkerCommand::WorldLifecycle(WorldLifecycle {
            world: KILN_WORLD_SERVER as u32,
            session,
            event,
        })
    };
    s.shared
        .submit(lifecycle(1, LifecycleEvent::Started))
        .unwrap();
    let probe = probe(s.owner);
    assert!(eventually(WAIT, || !probe
        .commands
        .lock()
        .unwrap()
        .is_empty()));
    s.shared
        .submit(lifecycle(1, LifecycleEvent::Stopping))
        .unwrap();
    s.shared
        .submit(lifecycle(2, LifecycleEvent::Started))
        .unwrap();
    assert!(eventually(WAIT, || probe.commands.lock().unwrap().len() >= 3));
    s.shared.request_stop();
    assert!(s.pool.task(0).wait_finished(WAIT));
    let commands = probe.commands.lock().unwrap().clone();
    assert_eq!(
        commands,
        vec![
            lifecycle(1, LifecycleEvent::Started),
            lifecycle(1, LifecycleEvent::Stopping),
            lifecycle(2, LifecycleEvent::Started),
        ],
        "a superseded STOPPING is still delivered before the newer STARTED"
    );
}

#[test]
fn the_stop_sentinel_is_found_through_wrapping_errors() {
    #[derive(Debug)]
    struct Wrap(Box<dyn std::error::Error + Send + Sync>);
    impl std::fmt::Display for Wrap {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            write!(f, "callback error: {}", self.0)
        }
    }
    impl std::error::Error for Wrap {
        fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
            Some(&*self.0)
        }
    }
    struct Capture(Arc<Mutex<Option<StopSentinel>>>);
    type Seen = Arc<Mutex<Option<StopSentinel>>>;
    static SEEN: LazyLock<Mutex<HashMap<KilnOwner, Seen>>> = LazyLock::new(Default::default);
    impl WorkerRuntime for Capture {
        fn turn(&mut self, cx: &WorkerContext<'_>, _: Duration) {
            assert!(cx.sentinel().is_none(), "no interrupt while running");
        }
        fn stop(&mut self, cx: &WorkerContext<'_>) {
            *self.0.lock().unwrap() = cx.sentinel();
        }
    }
    fn factory(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
        Ok(Box::new(Capture(
            SEEN.lock().unwrap().entry(cx.owner()).or_default().clone(),
        )))
    }
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = api(true);
    let shared = WorkerShared::new(WorkerConfig {
        owner,
        host: unsafe { Host::read(&*api) },
        factory,
        inbox: InboxConfig::DEFAULT,
    });
    let tasks = unsafe { TasksTable::read(tasks_table()) }.unwrap();
    submit(&shared, &tasks).unwrap();
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
    let sentinel = SEEN.lock().unwrap()[&owner]
        .lock()
        .unwrap()
        .expect("stop() runs with the stop interrupt pending");
    assert_eq!(sentinel.interrupt(), Interrupt::Stop);
    let wrapped = Wrap(Box::new(Wrap(Box::new(sentinel))));
    assert_eq!(stop_sentinel(&wrapped), Some(sentinel));
    assert_eq!(stop_sentinel(&std::fmt::Error), None);

    // mlua's shape (0.11.6 src/error.rs:329-342): ExternalError's source() is the
    // wrapped error's own source, so a source walk never sees the sentinel; its
    // Error::chain() yields the wrapped error, which stop_sentinel_in searches.
    #[derive(Debug)]
    struct LuaShaped(Box<dyn std::error::Error + Send + Sync>);
    impl std::fmt::Display for LuaShaped {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            self.0.fmt(f)
        }
    }
    impl std::error::Error for LuaShaped {
        fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
            self.0.source()
        }
    }
    let shaped = LuaShaped(Box::new(sentinel));
    assert_eq!(
        stop_sentinel(&shaped),
        None,
        "source() hides it, as in mlua"
    );
    let chain: [&(dyn std::error::Error + 'static); 2] = [&shaped, &*shaped.0];
    assert_eq!(stop_sentinel_in(chain), Some(sentinel));
    assert_eq!(
        stop_sentinel_in([&shaped as &(dyn std::error::Error + 'static)]),
        None
    );
}
