// SPDX-License-Identifier: MIT
//! `WorkerShared`: the one object other threads see. `Send + Sync`, held in an
//! `Arc` whose raw pointer is the task's `user`. Everything here is atomics or
//! set-once cells, so no reader or producer can find a lock held by the worker.

use super::Host;
use super::inbox::{Accepted, Inbox, InboxConfig, InboxCounters, Refused, WorkerCommand};
use super::state::RuntimeFactory;
use kiln_abi::KilnOwner;
use kiln_abi::services::kiln_tasks::*;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, OnceLock, Weak};
use std::thread::{Thread, ThreadId};

/// `phase`, read by status and Kiln.Ui.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum WorkerPhase {
    /// Not submitted (inactive boundary, or before on_ready).
    Inactive = 0,
    /// Accepted by kiln.tasks, no LongRunning thread yet.
    Queued = 1,
    /// The body runs and is creating the runtime.
    Creating = 2,
    Running = 3,
    Stopping = 4,
    Stopped = 5,
    Faulted = 6,
}
impl WorkerPhase {
    fn from_u32(value: u32) -> Self {
        match value {
            1 => Self::Queued,
            2 => Self::Creating,
            3 => Self::Running,
            4 => Self::Stopping,
            5 => Self::Stopped,
            6 => Self::Faulted,
            _ => Self::Inactive,
        }
    }
    pub fn terminal(self) -> bool {
        matches!(self, Self::Stopped | Self::Faulted)
    }
}

/// Why the worker is not running (the first reason wins, faults override).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum StopReason {
    None = 0,
    /// kiln.tasks refused the submit; `WorkerStatus::refusal` has the code.
    Refused = 1,
    /// No usable kiln.tasks table (missing service, short table).
    NoTasksService = 2,
    /// The runtime could not be created (`WorkerStatus::detail`).
    CreateFailed = 3,
    /// kiln-sfi asked for an orderly stop.
    StopRequested = 4,
    /// `cancelled()` without kiln-sfi's stop: exit, drain, revoke (fast leave).
    Cancelled = 5,
    /// Cancelled while still queued: the body never ran.
    CancelledBeforeStart = 6,
    /// A Rust panic in the body, caught at the task boundary.
    Panicked = 7,
    /// The pool delivered FAULTED: a structured fault reached its boundary.
    NativeFault = 8,
    /// The pool handed the body an unusable `KilnTaskRun`.
    InvalidRun = 9,
}
impl StopReason {
    fn from_u32(value: u32) -> Self {
        match value {
            1 => Self::Refused,
            2 => Self::NoTasksService,
            3 => Self::CreateFailed,
            4 => Self::StopRequested,
            5 => Self::Cancelled,
            6 => Self::CancelledBeforeStart,
            7 => Self::Panicked,
            8 => Self::NativeFault,
            9 => Self::InvalidRun,
            _ => Self::None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WorkerStatus {
    pub phase: WorkerPhase,
    pub reason: StopReason,
    /// `KILN_TASK_REFUSAL_*` of the last refused submit (0 = none).
    pub refusal: u32,
    /// The pool accepted the RESIDENT bit (false: an older Kiln).
    pub resident: bool,
    pub turns: u64,
    pub detail: Option<String>,
    pub inbox: InboxCounters,
    /// Packages the lifecycle host reports active and failed.
    pub mods_active: u32,
    pub mods_failed: u32,
    /// kiln-sfi drained its owner after an interpreter stop, and saw it quiet.
    pub drained: bool,
    pub quiescent: bool,
    /// The live session per world kind as the worker last drained it (0 = none).
    pub worlds: [u32; super::inbox::WORLD_KINDS],
    /// Hops refused at their start because the core has no world lifecycle
    /// (`KilnSessionInfo.lifecycle == 0`).
    pub hops_refused_no_lifecycle: u64,
}
impl WorkerStatus {
    /// The status line kiln-sfi shows (the lifecycle host adds its phases).
    pub fn line(&self) -> String {
        let base = match self.phase {
            WorkerPhase::Inactive => "interpreter inactive".to_owned(),
            WorkerPhase::Queued => "waiting for a task thread".to_owned(),
            WorkerPhase::Creating => "interpreter starting (runtime-create)".to_owned(),
            WorkerPhase::Running => format!(
                "interpreter running: {} mods active, {} failed",
                self.mods_active, self.mods_failed
            ),
            WorkerPhase::Stopping => "interpreter stopping".to_owned(),
            WorkerPhase::Stopped | WorkerPhase::Faulted => {
                format!("interpreter stopped: {}", self.reason_text())
            }
        };
        match &self.detail {
            Some(detail) if self.phase.terminal() => format!("{base} ({detail})"),
            _ => base,
        }
    }
    pub fn reason_text(&self) -> String {
        match self.reason {
            StopReason::None => "no reason recorded".into(),
            StopReason::Refused => {
                format!(
                    "kiln.tasks refused the worker ({})",
                    refusal_text(self.refusal)
                )
            }
            StopReason::NoTasksService => "kiln.tasks is not available".into(),
            StopReason::CreateFailed => "the runtime could not be created".into(),
            StopReason::StopRequested => "stopped by kiln-sfi".into(),
            StopReason::Cancelled => "cancelled by Kiln (exit, drain or revoke)".into(),
            StopReason::CancelledBeforeStart => "cancelled before a task thread was free".into(),
            StopReason::Panicked => "internal error in kiln-sfi (caught panic)".into(),
            StopReason::NativeFault => "native fault".into(),
            StopReason::InvalidRun => "kiln.tasks passed an unusable task run".into(),
        }
    }
}

pub fn refusal_text(refusal: u32) -> &'static str {
    match refusal as i32 {
        KILN_TASK_REFUSAL_QUEUE_FULL => "QUEUE_FULL: the task records of kiln-sfi's owner are full",
        KILN_TASK_REFUSAL_OWNER_SEALED => "OWNER_SEALED: kiln-sfi's owner is drained or retired",
        KILN_TASK_REFUSAL_OWNER_QUARANTINED => "OWNER_QUARANTINED: a native fault in kiln-sfi",
        KILN_TASK_REFUSAL_CAPABILITY => "CAPABILITY: tasks.pool is unavailable",
        KILN_TASK_REFUSAL_ARGUMENT => "ARGUMENT: the request was malformed",
        KILN_TASK_REFUSAL_SHUTDOWN => "SHUTDOWN: the process is exiting",
        KILN_TASK_REFUSAL_NO_SERVER_WORLD => "NO_SERVER_WORLD",
        KILN_TASK_REFUSAL_NO_WORLD => "NO_WORLD",
        _ => "unknown refusal",
    }
}

/// Fixed for the worker's life; given by kiln-sfi when it creates the worker.
#[derive(Clone, Copy)]
pub struct WorkerConfig {
    /// kiln-sfi's own plugin owner: no sub-owner.
    pub owner: KilnOwner,
    pub host: Host,
    /// Builds the runtime on the worker thread (`RuntimeWorkerState::create`).
    pub factory: RuntimeFactory,
    pub inbox: InboxConfig,
}

/// The Steam-messages lease (kiln.net) the runtime holds, so that it is closed
/// exactly once: after the Lua stop of an orderly leave (on the worker), or by kiln-sfi's tick
/// once the phase is `Faulted`, whichever takes it first. A fast leave takes nothing:
/// no network call at all. 0 = no lease.
#[derive(Debug, Default)]
pub struct LeaseSlot(AtomicU64);
impl LeaseSlot {
    /// The runtime opened `lease`.
    pub fn hold(&self, lease: u64) {
        self.0.store(lease, Ordering::Release);
    }
    /// The lease, for the one caller that closes it (0 = none, or taken already).
    pub fn take(&self) -> u64 {
        self.0.swap(0, Ordering::AcqRel)
    }
    pub fn get(&self) -> u64 {
        self.0.load(Ordering::Acquire)
    }
}

pub struct WorkerShared {
    config: WorkerConfig,
    /// This object's own `Arc` (it only ever lives in one, [`Self::new`]), for producers the
    /// worker starts that must outlive a borrow (kiln-sfi's IO thread jobs).
    this: Weak<WorkerShared>,
    network_lease: Arc<LeaseSlot>,
    stop: AtomicBool,
    phase: AtomicU32,
    reason: AtomicU32,
    refusal: AtomicU32,
    resident: AtomicBool,
    detail: OnceLock<String>,
    worker: OnceLock<Thread>,
    task: AtomicU64,
    pub(crate) tasks: OnceLock<super::task::TasksTable>,
    inbox: Inbox,
    turns: AtomicU64,
    longest_park_ns: AtomicU64,
    mods_active: AtomicU32,
    mods_failed: AtomicU32,
    hook_checks: AtomicU64,
    hook_raises: AtomicU64,
    drained: AtomicBool,
    quiescent: AtomicBool,
    worlds: [AtomicU32; super::inbox::WORLD_KINDS],
    /// Per world kind, the newest session the lifecycle subscriber saw stopping
    /// (a hop of that session or an older one is cancelled).
    world_stopped: [AtomicU32; super::inbox::WORLD_KINDS],
    hops_posted: AtomicU64,
    hops_returned: AtomicU64,
    hops_refused_no_lifecycle: AtomicU64,
    /// kiln-sfi's sfi.json record, told when the runtime runs. Set before the
    /// submit; absent for a worker started without a boundary (tests).
    status: OnceLock<std::sync::Arc<crate::state::StateHub>>,
    /// The launch plan the core froze, as the boundary read it once. Set before the
    /// submit; the worker never reads the plan file itself.
    plan: OnceLock<std::sync::Arc<crate::plan_source::FrozenPlan>>,
    /// The native manager's run of this start, made by the boundary's early phase. Set
    /// before the submit; absent for a worker started without one (no native verdict: every
    /// package that ships native code is withdrawn).
    native: OnceLock<std::sync::Arc<crate::native_manager::executor::NativeRun>>,
}

impl WorkerShared {
    pub fn new(config: WorkerConfig) -> std::sync::Arc<Self> {
        std::sync::Arc::new_cyclic(|this| Self {
            this: this.clone(),
            network_lease: Arc::default(),
            stop: AtomicBool::new(false),
            phase: AtomicU32::new(WorkerPhase::Inactive as u32),
            reason: AtomicU32::new(StopReason::None as u32),
            refusal: AtomicU32::new(0),
            resident: AtomicBool::new(false),
            detail: OnceLock::new(),
            worker: OnceLock::new(),
            task: AtomicU64::new(0),
            tasks: OnceLock::new(),
            inbox: Inbox::new(config.inbox),
            turns: AtomicU64::new(0),
            longest_park_ns: AtomicU64::new(0),
            mods_active: AtomicU32::new(0),
            mods_failed: AtomicU32::new(0),
            hook_checks: AtomicU64::new(0),
            hook_raises: AtomicU64::new(0),
            drained: AtomicBool::new(false),
            quiescent: AtomicBool::new(false),
            worlds: Default::default(),
            world_stopped: Default::default(),
            hops_posted: AtomicU64::new(0),
            hops_returned: AtomicU64::new(0),
            hops_refused_no_lifecycle: AtomicU64::new(0),
            status: OnceLock::new(),
            plan: OnceLock::new(),
            native: OnceLock::new(),
            config,
        })
    }

    /// This worker's `Arc` (`None` only while it is being dropped).
    pub fn arc(&self) -> Option<Arc<Self>> {
        self.this.upgrade()
    }
    /// The Steam-messages lease the runtime holds ([`LeaseSlot`]).
    pub fn network_lease(&self) -> &Arc<LeaseSlot> {
        &self.network_lease
    }
    /// The sfi.json record this worker reports its runtime to (set once).
    pub(crate) fn report_status_to(&self, hub: std::sync::Arc<crate::state::StateHub>) {
        let _ = self.status.set(hub);
    }
    /// The sfi.json record, for the runtime's in-memory updates (never a file wait).
    pub(crate) fn status_hub(&self) -> Option<&std::sync::Arc<crate::state::StateHub>> {
        self.status.get()
    }
    /// The frozen launch plan the runtime follows (set once, before the submit).
    pub(crate) fn set_frozen_plan(&self, plan: std::sync::Arc<crate::plan_source::FrozenPlan>) {
        let _ = self.plan.set(plan);
    }
    /// The frozen launch plan, as the boundary read it (`None` for a worker started without one).
    pub fn frozen_plan(&self) -> Option<&std::sync::Arc<crate::plan_source::FrozenPlan>> {
        self.plan.get()
    }
    /// The native manager's run (set once, before the submit).
    pub(crate) fn set_native_run(
        &self,
        run: std::sync::Arc<crate::native_manager::executor::NativeRun>,
    ) {
        let _ = self.native.set(run);
    }
    /// The native manager's run of this start, as the boundary made it (`None` for a worker
    /// started without one).
    pub fn native_run(
        &self,
    ) -> Option<&std::sync::Arc<crate::native_manager::executor::NativeRun>> {
        self.native.get()
    }
    /// The worker created its runtime: the record says `running`. Never waits for a
    /// file (the hub's writer thread writes it).
    pub(crate) fn status_running(&self) {
        if let Some(hub) = self.status.get() {
            hub.worker_running();
        }
    }

    pub fn owner(&self) -> KilnOwner {
        self.config.owner
    }
    pub fn host(&self) -> &Host {
        &self.config.host
    }
    pub(crate) fn factory(&self) -> RuntimeFactory {
        self.config.factory
    }
    pub fn inbox(&self) -> &Inbox {
        &self.inbox
    }

    /// Producer entry, any thread, never blocks: enqueue, then unpark the worker.
    /// `Stop` sets the stop flag instead and is never refused.
    pub fn submit(&self, command: WorkerCommand) -> Result<Accepted, Refused> {
        if matches!(command, WorkerCommand::Stop) {
            self.request_stop();
            self.inbox.count(command.class(), Ok(Accepted::Queued));
            return Ok(Accepted::Queued);
        }
        // Nothing drains the inbox once the worker leaves its loop: refuse from
        // kiln-sfi's stop request and the Stopping phase on, not only at the end.
        if self.stop_requested()
            || matches!(
                self.phase(),
                WorkerPhase::Stopping | WorkerPhase::Stopped | WorkerPhase::Faulted
            )
        {
            self.inbox.count(command.class(), Err(Refused::Stopped));
            return Err(Refused::Stopped);
        }
        let outcome = self.inbox.push(command);
        if outcome.is_ok() {
            self.unpark();
        }
        outcome
    }

    /// kiln-sfi's own orderly stop: the worker leaves in order at its next
    /// check. There is no restart in the same process.
    pub fn request_stop(&self) {
        self.stop.store(true, Ordering::Release);
        self.unpark();
    }
    pub fn stop_requested(&self) -> bool {
        self.stop.load(Ordering::Acquire)
    }

    /// Ends a hop wait or the 16 ms gap's park early; never starts a turn.
    pub fn unpark(&self) {
        if let Some(thread) = self.worker.get() {
            thread.unpark();
        }
    }
    pub fn worker_thread(&self) -> Option<ThreadId> {
        self.worker.get().map(Thread::id)
    }
    pub(crate) fn attach_thread(&self) {
        let _ = self.worker.set(std::thread::current());
    }

    pub fn phase(&self) -> WorkerPhase {
        WorkerPhase::from_u32(self.phase.load(Ordering::Acquire))
    }
    pub(crate) fn set_phase(&self, phase: WorkerPhase) {
        // A fault is final: nothing moves the phase off Faulted.
        let mut current = self.phase.load(Ordering::Acquire);
        while current != WorkerPhase::Faulted as u32 {
            match self.phase.compare_exchange_weak(
                current,
                phase as u32,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => break,
                Err(actual) => current = actual,
            }
        }
    }
    pub(crate) fn enter_phase(&self, from: WorkerPhase, to: WorkerPhase) -> bool {
        self.phase
            .compare_exchange(from as u32, to as u32, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }

    /// Records `reason` if none is recorded yet; returns whether it was recorded.
    pub(crate) fn note_reason(&self, reason: StopReason) -> bool {
        self.reason
            .compare_exchange(
                StopReason::None as u32,
                reason as u32,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
    }
    pub(crate) fn note_detail(&self, detail: impl Into<String>) {
        let _ = self.detail.set(detail.into());
    }
    /// A fault overrides any earlier reason and makes the phase final.
    pub(crate) fn fault(&self, reason: StopReason) {
        self.reason.store(reason as u32, Ordering::Release);
        self.phase
            .store(WorkerPhase::Faulted as u32, Ordering::Release);
    }
    pub(crate) fn refused(&self, reason: StopReason, refusal: u32) {
        self.refusal.store(refusal, Ordering::Release);
        self.note_reason(reason);
        self.set_phase(WorkerPhase::Stopped);
    }
    pub(crate) fn set_resident(&self, resident: bool) {
        self.resident.store(resident, Ordering::Release);
    }
    pub(crate) fn set_task(&self, task: u64) {
        self.task.store(task, Ordering::Release);
    }
    /// The kiln.tasks handle of the worker task (0 before acceptance).
    pub fn task(&self) -> u64 {
        self.task.load(Ordering::Acquire)
    }
    pub(crate) fn turn_done(&self) {
        self.turns.fetch_add(1, Ordering::Relaxed);
    }
    pub(crate) fn parked_for(&self, nanos: u64) {
        self.longest_park_ns.fetch_max(nanos, Ordering::Relaxed);
    }
    /// The longest single park the worker asked for (the idle cancel bound).
    pub fn longest_park_ns(&self) -> u64 {
        self.longest_park_ns.load(Ordering::Relaxed)
    }

    /// The lifecycle subscriber saw `session` of world `kind` stopping: flip the
    /// floor, then wake a hop wait. Any thread; never blocks.
    pub fn note_world_stopping(&self, kind: u32, session: u32) {
        if let Some(slot) = self.world_stopped.get(kind as usize) {
            slot.fetch_max(session, Ordering::AcqRel);
            self.unpark();
        }
    }
    /// The newest stopped session of world `kind` (0 = none seen).
    pub fn world_stopped(&self, kind: u32) -> u32 {
        self.world_stopped
            .get(kind as usize)
            .map_or(0, |slot| slot.load(Ordering::Acquire))
    }
    pub(crate) fn hop_posted(&self) {
        self.hops_posted.fetch_add(1, Ordering::Relaxed);
    }
    pub(crate) fn hop_returned(&self) {
        self.hops_returned.fetch_add(1, Ordering::Relaxed);
    }
    pub(crate) fn hop_refused_no_lifecycle(&self) {
        self.hops_refused_no_lifecycle
            .fetch_add(1, Ordering::Relaxed);
    }
    /// (hops posted, trampolines returned). The difference is the leak bound of
    /// one cell per posted hop whose function has not run (yet, or ever).
    pub fn hop_counts(&self) -> (u64, u64) {
        (
            self.hops_posted.load(Ordering::Relaxed),
            self.hops_returned.load(Ordering::Relaxed),
        )
    }
    pub(crate) fn set_world(&self, kind: usize, live: Option<u32>) {
        if let Some(slot) = self.worlds.get(kind) {
            slot.store(live.unwrap_or(0), Ordering::Release);
        }
    }
    pub(crate) fn set_mods(&self, active: u32, failed: u32) {
        self.mods_active.store(active, Ordering::Relaxed);
        self.mods_failed.store(failed, Ordering::Relaxed);
    }
    pub(crate) fn hook_checked(&self, raised: bool) {
        self.hook_checks.fetch_add(1, Ordering::Relaxed);
        if raised {
            self.hook_raises.fetch_add(1, Ordering::Relaxed);
        }
    }
    /// Count-hook checks so far, and how many of them raised (stop or watchdog).
    pub fn hook_counts(&self) -> (u64, u64) {
        (
            self.hook_checks.load(Ordering::Relaxed),
            self.hook_raises.load(Ordering::Relaxed),
        )
    }
    /// The drain after a stop seen once: returns true for the first caller only.
    pub(crate) fn mark_drained(&self) -> bool {
        !self.drained.swap(true, Ordering::AcqRel)
    }
    pub(crate) fn mark_quiescent(&self) {
        self.quiescent.store(true, Ordering::Release);
    }
    pub fn quiescent(&self) -> bool {
        self.quiescent.load(Ordering::Acquire)
    }

    pub fn status(&self) -> WorkerStatus {
        WorkerStatus {
            phase: self.phase(),
            reason: StopReason::from_u32(self.reason.load(Ordering::Acquire)),
            refusal: self.refusal.load(Ordering::Acquire),
            resident: self.resident.load(Ordering::Acquire),
            turns: self.turns.load(Ordering::Relaxed),
            detail: self.detail.get().cloned(),
            inbox: self.inbox.counters(),
            mods_active: self.mods_active.load(Ordering::Relaxed),
            mods_failed: self.mods_failed.load(Ordering::Relaxed),
            drained: self.drained.load(Ordering::Acquire),
            quiescent: self.quiescent.load(Ordering::Acquire),
            worlds: self.worlds.each_ref().map(|w| w.load(Ordering::Acquire)),
            hops_refused_no_lifecycle: self.hops_refused_no_lifecycle.load(Ordering::Relaxed),
        }
    }
}
