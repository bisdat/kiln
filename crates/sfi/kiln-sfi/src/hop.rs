// SPDX-License-Identifier: MIT
//! Hops: run a short, owned, non-blocking closure on a world thread for the
//! interpreter worker and give the result back.
//!
//! Only Kiln facilities that are tick-bound by design need a hop; every
//! engine operation is an any-thread Kiln member instead.
//!
//! Protocol:
//! 1. The worker captures the live `(world, session)` from `KilnApi.session`, boxes
//!    the closure in a [`HopCell`] and posts a trampoline with `post_world`, whose
//!    `user` owns one reference. A refusal answers [`HopError::Unavailable`] at once.
//! 2. The trampoline, on the world thread, CASes Pending -> Running (else it only
//!    drops its reference), checks that the captured session is still the live one
//!    and was not seen stopping (else Cancelled), runs the closure, stores the result,
//!    sets Done and unparks the worker. It never blocks or waits for the worker.
//! 3. The worker waits in parks of at most 16 ms, without the watchdog counting the
//!    wait, and with no ordinary timeout. It ends on Done; on
//!    Cancelled; on `cancelled()` or kiln-sfi's stop by CAS Pending -> Abandoned;
//!    on the lifecycle subscriber's stopped floor or a newer session by CAS
//!    Pending -> Cancelled. A failed CAS means Running: the closure is short, so it
//!    keeps waiting for Done.
//! 4. The subscriber only raises a per-world stopped floor and unparks (any thread);
//!    the worker and the trampoline both read it, so no thread other than the cell's
//!    owners ever touches a cell.
//! 5. Leak bound: a posted function that never runs (no further tick, owner revoked,
//!    exit) leaks its one boxed reference; `WorkerShared::hop_counts` counts them.
//! 6. A core without `world.lifecycle` (`KilnSessionInfo.lifecycle == 0`) refuses every
//!    hop at its start with [`HopError::Unavailable`] and a status counter; no timeout
//!    is added is added.
//!
//! A hop's closure reaches foreign or engine code only through kiln-sfi's SEH fault
//! shim. The runtime's service members ([`crate::runtime::services`])
//! are `!Send`, so a closure cannot capture one.

use crate::lifecycle::Watchdog;
use crate::worker::{Host, Interrupt, TasksTable, WorkerShared};
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::thread::Thread;
use std::time::Duration;

/// The longest single park of a hop wait (`park_timeout(<= 16 ms)`).
pub const HOP_PARK: Duration = Duration::from_millis(16);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum HopState {
    Pending = 0,
    Running = 1,
    Done = 2,
    Cancelled = 3,
    Abandoned = 4,
}
impl HopState {
    fn from_u32(value: u32) -> Self {
        match value {
            1 => Self::Running,
            2 => Self::Done,
            3 => Self::Cancelled,
            4 => Self::Abandoned,
            _ => Self::Pending,
        }
    }
}

/// How a hop ended without a result. Each maps to ShroudForge's shape for the
/// operation that hopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HopError {
    /// `post_world` refused, or the world has no live session: SF's "unavailable".
    Unavailable,
    /// The captured session ended (or a newer one began) before the closure ran.
    SessionEnded,
    /// The interpreter is stopping (exit, revoke, kiln-sfi's stop): abandoned.
    Stopping,
    /// The closure panicked on the world thread (caught there).
    Failed,
}

type Work<R> = Box<dyn FnOnce() -> R + Send>;

/// One hop's shared state. The closure is taken only by whoever wins Pending ->
/// Running; the result is written before Done (release) and read after it
/// (acquire) by the single waiter.
pub struct HopCell<R> {
    state: AtomicU32,
    world: u32,
    session: u32,
    waiter: Thread,
    work: UnsafeCell<Option<Work<R>>>,
    result: UnsafeCell<Option<Result<R, HopError>>>,
    taken: AtomicBool,
}
// SAFETY: `work` and `result` are reached only under the state protocol above;
// R and the closure are Send.
unsafe impl<R: Send> Send for HopCell<R> {}
unsafe impl<R: Send> Sync for HopCell<R> {}

impl<R: Send> HopCell<R> {
    /// A Pending cell for `(world, session)`; the calling thread is the waiter.
    pub fn new(world: u32, session: u32, work: impl FnOnce() -> R + Send + 'static) -> Arc<Self> {
        Arc::new(Self {
            state: AtomicU32::new(HopState::Pending as u32),
            world,
            session,
            waiter: std::thread::current(),
            work: UnsafeCell::new(Some(Box::new(work))),
            result: UnsafeCell::new(None),
            taken: AtomicBool::new(false),
        })
    }
    pub fn state(&self) -> HopState {
        HopState::from_u32(self.state.load(Ordering::Acquire))
    }
    pub fn world(&self) -> u32 {
        self.world
    }
    pub fn session(&self) -> u32 {
        self.session
    }

    /// The world-thread side. `live(world, session)` says whether the captured
    /// session is still the live one. Returns the state it left the cell in (or
    /// found it in, when it lost the race).
    pub fn run(&self, live: impl FnOnce(u32, u32) -> bool) -> HopState {
        if self
            .state
            .compare_exchange(
                HopState::Pending as u32,
                HopState::Running as u32,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            // The worker cancelled or abandoned it first: no effect.
            return self.state();
        }
        // Running must always end: the worker waits for it whatever stops it. So a
        // panicking session check counts as "not live" instead of leaving Running.
        let live = catch_unwind(AssertUnwindSafe(|| live(self.world, self.session)));
        if !live.unwrap_or(false) {
            self.finish(HopState::Cancelled, None);
            return HopState::Cancelled;
        }
        // SAFETY: winning Pending -> Running made this thread the only one that
        // touches `work` (nobody else ever takes it).
        let work = unsafe { (*self.work.get()).take() };
        let outcome = catch_unwind(AssertUnwindSafe(|| work.map(|work| work())));
        let result = match outcome {
            Ok(Some(value)) => Ok(value),
            _ => Err(HopError::Failed),
        };
        self.finish(HopState::Done, Some(result));
        HopState::Done
    }

    fn finish(&self, state: HopState, result: Option<Result<R, HopError>>) {
        if let Some(result) = result {
            // SAFETY: Running is exclusive; the waiter reads only after Done.
            unsafe { *self.result.get() = Some(result) };
        }
        self.state.store(state as u32, Ordering::Release);
        self.waiter.unpark();
    }

    /// The worker gives up a hop that has not started (stop): Pending -> Abandoned.
    pub fn abandon(&self) -> bool {
        self.transition(HopState::Abandoned)
    }
    /// The worker cancels a hop whose session ended: Pending -> Cancelled.
    pub fn cancel(&self) -> bool {
        self.transition(HopState::Cancelled)
    }
    fn transition(&self, to: HopState) -> bool {
        self.state
            .compare_exchange(
                HopState::Pending as u32,
                to as u32,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
    }

    /// The result after Done, once (the single waiter).
    pub fn take(&self) -> Option<Result<R, HopError>> {
        if self.state() != HopState::Done || self.taken.swap(true, Ordering::AcqRel) {
            return None;
        }
        // SAFETY: Done was stored (release) after the result; `taken` makes this
        // the only reader.
        unsafe { (*self.result.get()).take() }
    }
}

/// What the trampoline's `user` owns: one cell reference and the worker's shared
/// state, so a late trampoline touches only memory it keeps alive itself.
struct Posted<R> {
    cell: Arc<HopCell<R>>,
    shared: Arc<WorkerShared>,
}

/// Whether `(world, session)` is still live and was not seen stopping.
fn session_live(shared: &WorkerShared, world: u32, session: u32) -> bool {
    if shared.world_stopped(world) >= session {
        return false;
    }
    match shared.host().live_session(world) {
        Some(live) => live == session,
        // A core without `session` cannot say; the subscriber's floor still holds.
        None => shared.host().session.is_none(),
    }
}

/// The posted function: on the world thread, never blocks.
unsafe extern "C" fn trampoline<R: Send + 'static>(user: *mut c_void) {
    if user.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: `user` is the Box the worker leaked for this post; this is its
        // only reclaim (the worker reclaims only after a refused post).
        let posted = unsafe { Box::from_raw(user.cast::<Posted<R>>()) };
        let shared = &posted.shared;
        posted
            .cell
            .run(|world, session| session_live(shared, world, session));
        shared.hop_returned();
    }));
}

/// The worker's hop entry, built by the lifecycle host per runtime.
pub struct Hops {
    shared: Arc<WorkerShared>,
    interrupt: Box<dyn Fn() -> Option<Interrupt>>,
    watchdog: std::rc::Rc<Watchdog>,
}

impl Hops {
    pub(crate) fn new(
        shared: Arc<WorkerShared>,
        interrupt: Box<dyn Fn() -> Option<Interrupt>>,
        watchdog: std::rc::Rc<Watchdog>,
    ) -> Self {
        Self {
            shared,
            interrupt,
            watchdog,
        }
    }

    /// Runs `work` on world `world`'s thread for the live session and waits for it.
    /// Worker thread only. The wait is not Lua time (the watchdog does not count it).
    pub fn call<R: Send + 'static>(
        &self,
        world: u32,
        work: impl FnOnce() -> R + Send + 'static,
    ) -> Result<R, HopError> {
        let shared = &self.shared;
        debug_assert!(
            shared
                .worker_thread()
                .is_none_or(|id| id == std::thread::current().id()),
            "a hop waits on the worker thread only: the lifecycle subscriber unparks that thread"
        );
        let host: Host = *shared.host();
        let Some(post_world) = host.post_world else {
            return Err(HopError::Unavailable);
        };
        let Some(info) = host.session_info() else {
            return Err(HopError::Unavailable);
        };
        // A core without `world.lifecycle` never ends a stopped world's session, so a
        // wait could outlive its world: refuse the hop rather than add a timeout.
        // Nothing is posted.
        if info.lifecycle == 0 {
            shared.hop_refused_no_lifecycle();
            return Err(HopError::Unavailable);
        }
        let index = world as usize;
        let session = match info.worlds.get(index) {
            Some(&live) if live != 0 => info.sessions[index],
            _ => return Err(HopError::Unavailable),
        };
        if shared.world_stopped(world) >= session {
            return Err(HopError::SessionEnded);
        }
        let cell = HopCell::new(world, session, work);
        let user = Box::into_raw(Box::new(Posted {
            cell: cell.clone(),
            shared: shared.clone(),
        }));
        shared.hop_posted();
        // SAFETY: valid KilnApi member; `user` stays valid until the trampoline
        // reclaims it (or forever, the documented leak).
        let result =
            unsafe { post_world(shared.owner(), world, Some(trampoline::<R>), user.cast()) };
        if result != KILN_OK {
            // A refused post transfers nothing: take the box back.
            // SAFETY: the core did not accept `user`.
            drop(unsafe { Box::from_raw(user) });
            shared.hop_returned();
            return Err(HopError::Unavailable);
        }
        let _not_lua_time = self.watchdog.pause();
        loop {
            match cell.state() {
                HopState::Done => return cell.take().unwrap_or(Err(HopError::Failed)),
                HopState::Cancelled => return Err(HopError::SessionEnded),
                HopState::Abandoned => return Err(HopError::Stopping),
                HopState::Running => {}
                HopState::Pending => {
                    if (self.interrupt)().is_some() {
                        if cell.abandon() {
                            return Err(HopError::Stopping);
                        }
                        continue; // Running now: wait for Done.
                    }
                    if !session_live(shared, world, session) {
                        if cell.cancel() {
                            return Err(HopError::SessionEnded);
                        }
                        continue;
                    }
                }
            }
            std::thread::park_timeout(HOP_PARK);
        }
    }
}

// ---- submit_in: native follow-up work after a session's Post edge ----

/// What a follow-up body gets: its run's cancellation, valid only during the body.
pub struct FollowUpRun {
    run: *const KilnTaskRun,
}
impl FollowUpRun {
    pub fn cancelled(&self) -> bool {
        // SAFETY: valid during the body (kiln_tasks.h:23); this value never leaves
        // the body (not Send).
        unsafe {
            core::ptr::addr_of!((*self.run).cancelled)
                .read()
                .is_none_or(|f| f(self.run) != 0)
        }
    }
}

type FollowUpBody = Box<dyn FnOnce(&FollowUpRun) + Send>;
struct FollowUp {
    body: Option<FollowUpBody>,
    done: Option<Box<dyn FnOnce(u32) + Send>>,
    owner: KilnOwner,
    tasks: TasksTable,
}

/// `submit_in(owner, world, session, AFTER_STEP)` for
/// native work that must run on a pool worker after that session's next Post edge
/// and needs no Lua (for example a retry of a frame-quiet read). Kiln cancels it
/// when the session ends and still delivers `done`. That done frees the box and
/// releases the task's record, which would otherwise count toward the task owner's
/// 1,024 records until a drain (kiln_tasks.h "Queue full"). So nothing leaks, except
/// at exit, where done is not guaranteed (kiln_tasks.h "Exit"). It never hosts Lua.
/// Returns the task handle, valid until done (then `cancel`/`status` answer
/// NOT_FOUND), or (result, `KILN_TASK_REFUSAL_*`).
pub fn submit_after_step(
    shared: &WorkerShared,
    tasks: &TasksTable,
    world: u32,
    session: u32,
    name: &'static str,
    body: impl FnOnce(&FollowUpRun) + Send + 'static,
    done: impl FnOnce(u32) + Send + 'static,
) -> Result<KilnHandle, (KilnResult, u32)> {
    let Some(submit_in) = tasks.submit_in else {
        return Err((KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_CAPABILITY as u32));
    };
    let info = KilnTaskInfo {
        size: core::mem::size_of::<KilnTaskInfo>() as u32,
        flags: KILN_TASK_AFTER_STEP as u32,
        name: KilnStr {
            ptr: name.as_ptr().cast(),
            len: name.len(),
        },
        priority: KILN_TASK_PRIORITY_NORMAL as u32,
        reserved: 0,
    };
    let user = Box::into_raw(Box::new(FollowUp {
        body: Some(Box::new(body)),
        done: Some(Box::new(done)),
        owner: shared.owner(),
        tasks: *tasks,
    }));
    let (mut task, mut refusal) = (0, 0);
    // SAFETY: valid member; `user` is reclaimed by `follow_up_done` (or below).
    let result = unsafe {
        submit_in(
            shared.owner(),
            &info,
            world,
            session,
            Some(follow_up_body),
            Some(follow_up_done),
            user.cast(),
            &mut task,
            &mut refusal,
        )
    };
    if result != KILN_OK {
        // SAFETY: a refused submit transfers nothing.
        drop(unsafe { Box::from_raw(user) });
        return Err((result, refusal));
    }
    Ok(task)
}

unsafe extern "C" fn follow_up_body(run: *const KilnTaskRun, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: done reclaims the box after this returns; the body only borrows.
        let follow = unsafe { &mut *user.cast::<FollowUp>() };
        if let Some(body) = follow.body.take() {
            body(&FollowUpRun { run });
        }
    }));
}

unsafe extern "C" fn follow_up_done(task: KilnHandle, state: u32, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the one reclaim; the pool calls done once.
        let mut follow = unsafe { Box::from_raw(user.cast::<FollowUp>()) };
        // Terminal now; releasing inside its own done is legal (kiln_tasks.h). First,
        // so a panicking callback below cannot keep the record.
        let _ = follow.tasks.release(follow.owner, task);
        if let Some(done) = follow.done.take() {
            done(state);
        }
    }));
}
