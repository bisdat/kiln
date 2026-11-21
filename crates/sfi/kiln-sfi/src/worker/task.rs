// SPDX-License-Identifier: MIT
//! The worker at the ABI: one kiln.tasks task, flags
//! `LONG_RUNNING | RESIDENT`, priority Normal, name `interpreter`, under kiln-sfi's
//! own plugin owner. `user` is `Arc::into_raw` of the `WorkerShared`; the body only
//! borrows it and `done` takes it back exactly once. No panic crosses either
//! `extern "C"` function.

use super::shared::{StopReason, WorkerConfig, WorkerPhase, WorkerShared, refusal_text};
use super::state::{RuntimeWorkerState, TaskRun, WorkerContext};
use super::{Host, inbox::InboxConfig, state::RuntimeFactory};
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, OnceLock};

pub const TASK_NAME: &str = "interpreter";

/// The Kiln release that added `KILN_TASK_RESIDENT` (kiln_tasks.h and
/// docs/kiln/services/tasks.md: "since 0.2.0-dev.29.19").
pub const RESIDENT_SINCE: &str = "0.2.0-dev.29.19";

/// Whether a Kiln whose `KilnApi.version()` is `version` predates RESIDENT, so that its
/// INVALID_ARGUMENT + ARGUMENT for the bit can only mean it does not know it
/// (kiln_tasks.h: "KilnApi.version() tells an older Kiln from another wrong field").
///
/// Fails closed, since a wrong yes starts a second interpreter body for one owner:
/// a version that is no semantic version, and a source build of RESIDENT's own line
/// (`0.2.0-dev`: the VERSION file as checked in, at any commit), count as having it.
/// Build metadata (`+...`) is ignored, as semver's precedence does.
pub fn predates_resident(version: &str) -> bool {
    let Ok(mut version) = semver::Version::parse(version.trim()) else {
        return false;
    };
    version.build = semver::BuildMetadata::EMPTY;
    let since = semver::Version::parse(RESIDENT_SINCE).expect("RESIDENT_SINCE is a version");
    let same_line =
        (version.major, version.minor, version.patch) == (since.major, since.minor, since.patch);
    if same_line && version.pre.as_str() == "dev" {
        return false;
    }
    version < since
}

type SubmitFn = unsafe extern "C" fn(
    KilnOwner,
    *const KilnTaskInfo,
    KilnTaskBodyFn,
    KilnTaskDoneFn,
    *mut c_void,
    *mut KilnHandle,
    *mut u32,
) -> KilnResult;
type ReleaseFn = unsafe extern "C" fn(KilnOwner, KilnHandle) -> KilnResult;
type DrainFn = unsafe extern "C" fn(KilnOwner, u32, *mut u32) -> KilnResult;
type CancelFn = unsafe extern "C" fn(KilnOwner, KilnHandle, *mut u32) -> KilnResult;
type SubmitInFn = unsafe extern "C" fn(
    KilnOwner,
    *const KilnTaskInfo,
    u32,
    u32,
    KilnTaskBodyFn,
    KilnTaskDoneFn,
    *mut c_void,
    *mut KilnHandle,
    *mut u32,
) -> KilnResult;

/// The v1 members kiln-sfi uses, copied once from a size-checked table.
#[derive(Clone, Copy)]
pub struct TasksTable {
    submit: SubmitFn,
    cancel: CancelFn,
    release: ReleaseFn,
    drain_owner: DrainFn,
    /// Appended member (header.size >= 56); `None` on an older Kiln.
    pub(crate) submit_in: Option<SubmitInFn>,
}
impl TasksTable {
    /// # Safety
    /// `table` is NULL or a `kiln.tasks` table readable for its declared size.
    pub unsafe fn read(table: *const KilnTasksApi) -> Option<Self> {
        if table.is_null() || !table.is_aligned() {
            return None;
        }
        let header = table.cast::<KilnServiceHeader>();
        // SAFETY: every table starts with its header (kiln_api.h service docs).
        let size = unsafe { core::ptr::addr_of!((*header).size).read() } as usize;
        let version = unsafe { core::ptr::addr_of!((*header).version).read() };
        // The first five members are v1's 48 bytes (kiln_tasks.h, "Appended members").
        if version < KILN_TASKS_VERSION
            || size
                < core::mem::offset_of!(KilnTasksApi, drain_owner) + core::mem::size_of::<usize>()
        {
            return None;
        }
        // SAFETY: the size check above covers these five members; no reference
        // to the whole (possibly longer) struct is formed.
        let (submit, cancel, release, drain_owner) = unsafe {
            (
                core::ptr::addr_of!((*table).submit).read()?,
                core::ptr::addr_of!((*table).cancel).read()?,
                core::ptr::addr_of!((*table).release).read()?,
                core::ptr::addr_of!((*table).drain_owner).read()?,
            )
        };
        let submit_in = if size
            >= core::mem::offset_of!(KilnTasksApi, submit_in) + core::mem::size_of::<usize>()
        {
            // SAFETY: the size covers the appended member.
            unsafe { core::ptr::addr_of!((*table).submit_in).read() }
        } else {
            None
        };
        Some(Self {
            submit,
            cancel,
            release,
            drain_owner,
            submit_in,
        })
    }

    /// `drain_owner(owner, 0, &running)`: seal, cancel queued, poll. Never a timed
    /// drain. Returns the result and `*running`.
    pub fn drain_poll(&self, owner: KilnOwner) -> (KilnResult, u32) {
        let mut running = 0;
        // SAFETY: valid table member; `running` is a live out pointer.
        let result = unsafe { (self.drain_owner)(owner, 0, &mut running) };
        (result, running)
    }
    pub fn cancel(&self, owner: KilnOwner, task: KilnHandle) -> (KilnResult, u32) {
        let mut state = 0;
        // SAFETY: as above.
        let result = unsafe { (self.cancel)(owner, task, &mut state) };
        (result, state)
    }
    /// `release(owner, task)`: frees a terminal task's record and its slot of the
    /// owner's 1,024 (legal inside that task's own done).
    pub(crate) fn release(&self, owner: KilnOwner, task: KilnHandle) -> KilnResult {
        // SAFETY: valid table member.
        unsafe { (self.release)(owner, task) }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StartError {
    /// The boundary never bound a host and owner (no Normal-phase entry).
    NotBound,
    /// The worker was already started once: no restart in one process.
    AlreadyStarted,
    NoTasksService,
    /// kiln.tasks refused: (result, `KILN_TASK_REFUSAL_*`).
    Refused(KilnResult, u32),
}

/// Submits the worker. On a refusal the interpreter does not start: one loud
/// warning, status `stopped` with the refusal, no fallback thread.
pub fn submit(shared: &Arc<WorkerShared>, tasks: &TasksTable) -> Result<KilnHandle, StartError> {
    let _ = shared.tasks.set(*tasks);
    // Queued before the call: the body may start before submit returns.
    if !shared.enter_phase(WorkerPhase::Inactive, WorkerPhase::Queued) {
        return Err(StartError::AlreadyStarted);
    }
    let user = Arc::into_raw(shared.clone()).cast_mut().cast::<c_void>();
    let attempt = |flags: i32| {
        let info = KilnTaskInfo {
            size: core::mem::size_of::<KilnTaskInfo>() as u32,
            flags: flags as u32,
            name: KilnStr {
                ptr: TASK_NAME.as_ptr().cast(),
                len: TASK_NAME.len(),
            },
            priority: KILN_TASK_PRIORITY_NORMAL as u32,
            reserved: 0,
        };
        let (mut task, mut refusal) = (0, KILN_TASK_REFUSAL_NONE as u32);
        // SAFETY: valid member; `info`, `task`, `refusal` are live for the call;
        // `user` stays valid until done returns (done reclaims it).
        let result = unsafe {
            (tasks.submit)(
                shared.owner(),
                &info,
                Some(body),
                Some(done),
                user,
                &mut task,
                &mut refusal,
            )
        };
        (result, task, refusal)
    };
    let mut resident = true;
    let mut outcome = attempt(KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT);
    // Why an ARGUMENT refusal was not retried without the bit (the status detail).
    let mut not_retried = None;
    if outcome.0 == KILN_ERR_INVALID_ARGUMENT && outcome.2 == KILN_TASK_REFUSAL_ARGUMENT as u32 {
        let version = shared.host().kiln_version();
        if version.as_deref().is_some_and(predates_resident) {
            // A Kiln older than RESIDENT refuses the bit like any unknown one.
            // Retry without it and accept the watchdog's one running-too-long line.
            resident = false;
            shared.host().log(
                shared.owner(),
                KILN_LOG_WARN,
                "kiln-sfi: this Kiln has no resident tasks; the interpreter runs as a plain \
                 LongRunning task and kiln.tasks' watchdog may report it once",
            );
            outcome = attempt(KILN_TASK_LONG_RUNNING);
        } else {
            // A Kiln with RESIDENT refuses a second resident task of an owner with the
            // same code (kiln_tasks.h "Resident tasks"). A plain retry would run a
            // second interpreter body for kiln-sfi's owner: the start fails instead.
            // So does a Kiln that cannot be told apart from one with RESIDENT.
            // A test or source build from before RESIDENT reports a version that is not
            // older (a beta keeps its release's version): it is refused too, and told so.
            not_retried = Some(match version {
                Some(version) if semver::Version::parse(&version).is_ok() => format!(
                    "on Kiln {version}, not known to predate resident tasks \
                     ({RESIDENT_SINCE}), it can also mean kiln-sfi's owner already holds one; \
                     not retried as a plain task. A test or source build of Kiln from before \
                     resident tasks refuses the same way: update Kiln"
                ),
                _ => "this Kiln's version cannot be read, so it cannot be told from one \
                      where kiln-sfi's owner already holds a resident task; not retried as \
                      a plain task: update Kiln"
                    .to_owned(),
            });
        }
    }
    let (result, task, refusal) = outcome;
    if result == KILN_OK {
        shared.set_resident(resident);
        shared.set_task(task);
        return Ok(task);
    }
    // A refused submit transfers nothing (kiln_tasks.h "Lifetime"): take `user` back.
    // SAFETY: the pool did not accept `user`; this is the only reclaim.
    drop(unsafe { Arc::from_raw(user.cast_const().cast::<WorkerShared>()) });
    let why = match &not_retried {
        Some(detail) => {
            shared.note_detail(detail.as_str());
            format!("{}; {detail}", refusal_text(refusal))
        }
        None => refusal_text(refusal).to_owned(),
    };
    shared.refused(StopReason::Refused, refusal);
    shared.host().log(
        shared.owner(),
        KILN_LOG_ERROR,
        &format!(
            "kiln-sfi: interpreter not started: kiln.tasks refused the worker ({why}); \
             ShroudForge mods do not run this start"
        ),
    );
    Err(StartError::Refused(result, refusal))
}

/// The task body. Borrows `user`; never takes ownership.
unsafe extern "C" fn body(run: *const KilnTaskRun, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    // SAFETY: `user` is the Arc reference that done reclaims after this returns.
    let shared = unsafe { &*user.cast_const().cast::<WorkerShared>() };
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: `run` is this body's argument, used only during the body.
        unsafe { run_body(run, shared) }
    }));
    if outcome.is_err() {
        shared.fault(StopReason::Panicked);
        shared.host().log(
            shared.owner(),
            KILN_LOG_ERROR,
            "kiln-sfi: interpreter stopped: internal error (a caught panic in the worker)",
        );
    }
}

/// The worker thread's interpreter mark (the native-load enforcement's), set for the
/// body's whole run and cleared on every way out, unwinding included.
struct ThreadMark;
impl ThreadMark {
    fn enter() -> Self {
        #[cfg(windows)]
        crate::native_enforcement::mark_interpreter(true);
        Self
    }
}
impl Drop for ThreadMark {
    fn drop(&mut self) {
        #[cfg(windows)]
        crate::native_enforcement::mark_interpreter(false);
    }
}

unsafe fn run_body(run: *const KilnTaskRun, shared: &WorkerShared) {
    let _mark = ThreadMark::enter();
    // SAFETY: forwarded body argument.
    let Some(run) = (unsafe { TaskRun::new(run) }) else {
        shared.fault(StopReason::InvalidRun);
        return;
    };
    shared.attach_thread();
    if !shared.enter_phase(WorkerPhase::Queued, WorkerPhase::Creating) {
        shared.set_phase(WorkerPhase::Creating);
    }
    let cx = WorkerContext::new(&run, shared);
    // Dropped only by an orderly leave. A panic unwinding out of the loop
    // leaks the state like a fast leave: no Lua close, no mod `__gc`
    // after an internal error.
    let mut state = std::mem::ManuallyDrop::new(match RuntimeWorkerState::create(&cx) {
        Ok(state) => state,
        Err(error) => {
            shared.note_reason(StopReason::CreateFailed);
            shared.note_detail(error.clone());
            shared.set_phase(WorkerPhase::Stopping);
            cx.log(
                KILN_LOG_ERROR,
                &format!("kiln-sfi: interpreter not started: runtime-create failed: {error}"),
            );
            return;
        }
    });
    shared.set_phase(WorkerPhase::Running);
    shared.status_running();
    let orderly = state.run(&cx);
    shared.note_reason(if orderly {
        StopReason::StopRequested
    } else {
        StopReason::Cancelled
    });
    shared.set_phase(WorkerPhase::Stopping);
    std::mem::ManuallyDrop::into_inner(state).leave(&cx, orderly);
}

/// The done callback: takes the `user` reference back (exactly once), records
/// the terminal phase and releases the task record. At exit it may never run;
/// the reference then leaks with the process (kiln_tasks.h "Exit").
unsafe extern "C" fn done(task: KilnHandle, state: u32, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the reference submit handed over; the pool calls done once.
        let shared = unsafe { Arc::from_raw(user.cast_const().cast::<WorkerShared>()) };
        match state as i32 {
            KILN_TASK_FAULTED => {
                shared.fault(StopReason::NativeFault);
                shared.host().log(
                    shared.owner(),
                    KILN_LOG_ERROR,
                    "kiln-sfi: interpreter stopped: native fault in the worker (owner quarantined)",
                );
            }
            KILN_TASK_CANCELLED => {
                shared.note_reason(StopReason::CancelledBeforeStart);
                shared.set_phase(WorkerPhase::Stopped);
            }
            _ => shared.set_phase(WorkerPhase::Stopped),
        }
        if let Some(tasks) = shared.tasks.get() {
            // Legal inside this task's own done (kiln_tasks.h "release").
            // SAFETY: valid member of the table submit used.
            let _ = unsafe { (tasks.release)(shared.owner(), task) };
        }
    }));
}

/// The plugin-side handle: one per kiln-sfi image, `'static`. Binds at the
/// Normal-phase `kiln_plugin_main` when kiln.launch froze backend `kiln`, and
/// starts the worker once, at `on_ready`.
pub struct Worker {
    factory: RuntimeFactory,
    inbox: InboxConfig,
    bound: OnceLock<(Host, KilnOwner)>,
    shared: OnceLock<Arc<WorkerShared>>,
    status: OnceLock<Arc<crate::state::StateHub>>,
    plan: OnceLock<Arc<crate::plan_source::FrozenPlan>>,
    native: OnceLock<Arc<crate::native_manager::executor::NativeRun>>,
}
impl Worker {
    pub const fn new(factory: RuntimeFactory) -> Self {
        Self::with_inbox(factory, InboxConfig::DEFAULT)
    }
    pub const fn with_inbox(factory: RuntimeFactory, inbox: InboxConfig) -> Self {
        Self {
            factory,
            inbox,
            bound: OnceLock::new(),
            shared: OnceLock::new(),
            status: OnceLock::new(),
            plan: OnceLock::new(),
            native: OnceLock::new(),
        }
    }
    /// The sfi.json record the worker tells when its runtime runs. Set before
    /// [`start`](Self::start); the first one set stays.
    pub fn report_status_to(&self, hub: Arc<crate::state::StateHub>) {
        let _ = self.status.set(hub);
    }
    /// The launch plan the core froze, as the boundary read it once: the runtime follows
    /// it and never reads the plan file again. Set before [`start`](Self::start); the first one
    /// set stays.
    pub fn follow_plan(&self, plan: Arc<crate::plan_source::FrozenPlan>) {
        let _ = self.plan.set(plan);
    }
    /// The native manager's run of this start, made by the boundary's early phase: the
    /// runtime admits packages by its terminal. Set before [`start`](Self::start); the first one
    /// set stays.
    pub fn follow_native(&self, run: Arc<crate::native_manager::executor::NativeRun>) {
        let _ = self.native.set(run);
    }
    /// Records the host and kiln-sfi's plugin owner. A second bind with another
    /// owner is refused.
    pub fn bind(&self, host: Host, owner: KilnOwner) -> bool {
        let _ = self.bound.set((host, owner));
        self.bound.get().is_some_and(|(_, bound)| *bound == owner)
    }
    pub fn shared(&self) -> Option<&Arc<WorkerShared>> {
        self.shared.get()
    }
    pub fn started(&self) -> bool {
        self.shared
            .get()
            .is_some_and(|s| s.phase() != WorkerPhase::Inactive)
    }

    /// on_ready: look up kiln.tasks and submit the worker, then subscribe kiln-sfi's
    /// tick and world lifecycle (kept for the process). Never twice.
    pub fn start(&'static self) -> Result<KilnHandle, StartError> {
        let &(host, owner) = self.bound.get().ok_or(StartError::NotBound)?;
        let mut created = false;
        let shared = self.shared.get_or_init(|| {
            created = true;
            WorkerShared::new(WorkerConfig {
                owner,
                host,
                factory: self.factory,
                inbox: self.inbox,
            })
        });
        if !created {
            return Err(StartError::AlreadyStarted);
        }
        if let Some(hub) = self.status.get() {
            shared.report_status_to(Arc::clone(hub));
        }
        if let Some(plan) = self.plan.get() {
            shared.set_frozen_plan(Arc::clone(plan));
        }
        if let Some(run) = self.native.get() {
            shared.set_native_run(Arc::clone(run));
        }
        // SAFETY: Host::service only forwards to the core's lookup.
        let table = host.service(KILN_TASKS_SERVICE, KILN_TASKS_VERSION);
        let Some(tasks) = (unsafe { TasksTable::read(table.cast()) }) else {
            shared.refused(StopReason::NoTasksService, 0);
            host.log(
                owner,
                KILN_LOG_ERROR,
                "kiln-sfi: interpreter not started: kiln.tasks is not available; \
                 ShroudForge mods do not run this start",
            );
            return Err(StartError::NoTasksService);
        };
        let task = submit(shared, &tasks)?;
        crate::lifecycle::exit::subscribe(self, shared);
        Ok(task)
    }
}
