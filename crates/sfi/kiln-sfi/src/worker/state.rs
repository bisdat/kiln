// SPDX-License-Identifier: MIT
//! The worker thread's side: the `!Send` `RuntimeWorkerState`, the
//! runtime seam the lifecycle host plugs into, and the turn loop with
//! ShroudForge's cadence (ShroudForge 1.7.9 `windows-bootstrap/runtime_lifecycle.cpp:150-171`:
//! `WaitForSingleObject(stop_event, 16)`, then the tick and update).

use super::inbox::{Consumer, WorkerCommand};
use super::shared::WorkerShared;
use kiln_abi::KilnOwner;
use kiln_abi::services::kiln_tasks::KilnTaskRun;
use std::marker::PhantomData;
use std::thread::ThreadId;
use std::time::{Duration, Instant};

/// SF's fixed wait between turns.
pub const TURN_GAP: Duration = Duration::from_millis(16);

/// Why Lua must stop now (checked in this order).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interrupt {
    /// `run->cancelled(run)` said 1: exit, drain, revoke (fast leave).
    Cancelled,
    /// kiln-sfi's own `shared.stop` (orderly leave).
    Stop,
}

/// The private error the count hook raises when the worker must stop. The
/// lifecycle host treats it as **stop**, never as a mod failure, and runs no mod
/// error handler for it. Mods cannot construct it; they only see its message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StopSentinel(Interrupt);
impl StopSentinel {
    pub(crate) fn new(interrupt: Interrupt) -> Self {
        Self(interrupt)
    }
    pub fn interrupt(&self) -> Interrupt {
        self.0
    }
}
impl std::fmt::Display for StopSentinel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the interpreter is stopping")
    }
}
impl std::error::Error for StopSentinel {}

/// Finds a [`StopSentinel`] in an error or its `source()` chain.
///
/// Not enough for an `mlua::Error`: mlua hides what it wraps from `source()`
/// (`ExternalError` answers the wrapped error's own source, `CallbackError`
/// answers `None`; mlua 0.11.6 `src/error.rs:329-342`), so this finds nothing in
/// an error raised from a count hook or a callback. Use
/// `stop_sentinel_in(error.chain())` there ([`stop_sentinel_in`]).
pub fn stop_sentinel(error: &(dyn std::error::Error + 'static)) -> Option<StopSentinel> {
    stop_sentinel_in([error])
}

/// Finds a [`StopSentinel`] in any error `errors` yields, or in its `source()`
/// chain. For an `mlua::Error`, pass `error.chain()`, which yields the errors
/// mlua wraps.
pub fn stop_sentinel_in<'e>(
    errors: impl IntoIterator<Item = &'e (dyn std::error::Error + 'static)>,
) -> Option<StopSentinel> {
    errors.into_iter().find_map(|error| {
        let mut current = Some(error);
        while let Some(error) = current {
            if let Some(sentinel) = error.downcast_ref::<StopSentinel>() {
                return Some(*sentinel);
            }
            current = error.source();
        }
        None
    })
}

/// The pool's run record, valid only while the body runs on its own thread.
pub(crate) struct TaskRun {
    run: *const KilnTaskRun,
    cancelled: unsafe extern "C" fn(*const KilnTaskRun) -> u32,
    owner: KilnOwner,
}
impl TaskRun {
    /// # Safety
    /// `run` is the body's argument, read only during the body (K kiln_tasks.h:23).
    pub(crate) unsafe fn new(run: *const KilnTaskRun) -> Option<Self> {
        if run.is_null() || !run.is_aligned() {
            return None;
        }
        // SAFETY: the pool fills at least `size` bytes; v1 is 40 (kiln_tasks.h:39).
        let size = unsafe { core::ptr::addr_of!((*run).size).read() } as usize;
        if size < core::mem::size_of::<KilnTaskRun>() {
            return None;
        }
        let cancelled = unsafe { core::ptr::addr_of!((*run).cancelled).read() }?;
        let owner = unsafe { core::ptr::addr_of!((*run).owner).read() };
        Some(Self {
            run,
            cancelled,
            owner,
        })
    }
    fn cancelled(&self) -> bool {
        // SAFETY: called on the body's own thread during the body (TaskRun is
        // created and dropped inside the body and is !Send through its pointer).
        unsafe { (self.cancelled)(self.run) != 0 }
    }
}

/// A raw, `Copy` view of the run and the shared state for the Lua count hook, whose
/// closure must be `'static`. `!Send` through its pointers.
#[derive(Clone, Copy)]
pub(crate) struct InterruptProbe {
    run: *const KilnTaskRun,
    cancelled: unsafe extern "C" fn(*const KilnTaskRun) -> u32,
    shared: *const WorkerShared,
}
impl InterruptProbe {
    /// The order: the pool's cancellation, then (when `honour_stop`) kiln-sfi's stop.
    ///
    /// # Safety
    /// Only on the worker thread while its body runs. Lua runs nowhere else,
    /// and a fast leave forgets the Lua state, so it never runs again.
    pub(crate) unsafe fn check(&self, honour_stop: bool) -> Option<Interrupt> {
        // SAFETY: the caller's contract: the body still runs, so `run` and the
        // `WorkerShared` its `user` points to are both live.
        if unsafe { (self.cancelled)(self.run) } != 0 {
            Some(Interrupt::Cancelled)
        } else if honour_stop && unsafe { (*self.shared).stop_requested() } {
            Some(Interrupt::Stop)
        } else {
            None
        }
    }
}

/// What the runtime sees on the worker thread. `!Send`: it never leaves the body.
pub struct WorkerContext<'a> {
    run: &'a TaskRun,
    shared: &'a WorkerShared,
    _not_send: PhantomData<*mut ()>,
}
impl<'a> WorkerContext<'a> {
    pub(crate) fn new(run: &'a TaskRun, shared: &'a WorkerShared) -> Self {
        Self {
            run,
            shared,
            _not_send: PhantomData,
        }
    }
    /// `run->cancelled(run)`.
    pub fn cancelled(&self) -> bool {
        self.run.cancelled()
    }
    /// The order: the pool's cancellation first, then kiln-sfi's stop.
    pub fn interrupt(&self) -> Option<Interrupt> {
        if self.cancelled() {
            Some(Interrupt::Cancelled)
        } else if self.shared.stop_requested() {
            Some(Interrupt::Stop)
        } else {
            None
        }
    }
    pub(crate) fn interrupt_probe(&self) -> InterruptProbe {
        InterruptProbe {
            run: self.run.run,
            cancelled: self.run.cancelled,
            shared: self.shared,
        }
    }
    /// The error a count hook raises for [`Self::interrupt`].
    pub fn sentinel(&self) -> Option<StopSentinel> {
        self.interrupt().map(StopSentinel::new)
    }
    pub fn shared(&self) -> &'a WorkerShared {
        self.shared
    }
    /// kiln-sfi's plugin owner, as the pool reported it for this run.
    pub fn owner(&self) -> KilnOwner {
        self.run.owner
    }
    /// `KilnApi.log` under kiln-sfi's owner (callable from the worker).
    pub fn log(&self, level: i32, text: &str) {
        self.shared.host().log(self.shared.owner(), level, text);
    }
}

/// The runtime inside the worker: an empty one in tests, the Lua
/// lifecycle host. Not `Send`: it is built, used and dropped on the worker thread.
pub trait WorkerRuntime {
    /// One drained inbox command, before the turn that follows the drain.
    fn command(&mut self, cx: &WorkerContext<'_>, command: WorkerCommand) {
        let _ = (cx, command);
    }
    /// One scheduler turn; `dt` is the time since the previous turn began.
    fn turn(&mut self, cx: &WorkerContext<'_>, dt: Duration);
    /// The orderly leave: ShroudForge's stop for each package. The shell
    /// drops the runtime right after, on this thread.
    fn stop(&mut self, cx: &WorkerContext<'_>);
}

/// Builds the runtime on the worker thread. `Err` text goes to the status.
pub type RuntimeFactory = fn(&WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String>;

/// An empty runtime: no Lua, an empty turn.
pub struct EmptyRuntime;
impl WorkerRuntime for EmptyRuntime {
    fn turn(&mut self, _: &WorkerContext<'_>, _: Duration) {}
    fn stop(&mut self, _: &WorkerContext<'_>) {}
}
pub fn empty_runtime(_: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
    Ok(Box::new(EmptyRuntime))
}

/// Everything the interpreter holds, on the worker thread only. Built inside
/// the body, never moved, shared or referenced from another thread, dropped there
/// at an orderly stop and leaked at a fast one (and when a panic unwinds out of
/// the body).
///
/// It is neither `Send` nor `Sync`:
/// ```compile_fail
/// fn assert_send<T: Send>() {}
/// assert_send::<kiln_sfi::worker::RuntimeWorkerState<'static>>();
/// ```
/// ```compile_fail
/// fn assert_sync<T: Sync>() {}
/// assert_sync::<kiln_sfi::worker::RuntimeWorkerState<'static>>();
/// ```
/// while the shared half is both:
/// ```
/// fn assert_send_sync<T: Send + Sync>() {}
/// assert_send_sync::<kiln_sfi::worker::WorkerShared>();
/// ```
pub struct RuntimeWorkerState<'a> {
    runtime: Box<dyn WorkerRuntime>,
    consumer: Consumer<'a>,
    thread: ThreadId,
    _not_send: PhantomData<*mut ()>,
}

impl<'a> RuntimeWorkerState<'a> {
    pub(crate) fn create(cx: &WorkerContext<'a>) -> Result<Self, String> {
        let consumer = cx
            .shared()
            .inbox()
            .consumer()
            .ok_or_else(|| "the worker inbox already has a consumer".to_owned())?;
        let runtime = (cx.shared().factory())(cx)?;
        Ok(Self {
            runtime,
            consumer,
            thread: std::thread::current().id(),
            _not_send: PhantomData,
        })
    }

    /// Returns whether the leave is orderly: kiln-sfi's stop without the
    /// pool's cancellation.
    pub(crate) fn run(&mut self, cx: &WorkerContext<'_>) -> bool {
        let mut previous = Instant::now();
        let mut turn_end = previous;
        loop {
            if cx.interrupt().is_some() {
                break;
            }
            if !park_until(cx, turn_end + TURN_GAP) {
                break;
            }
            let now = Instant::now();
            let dt = now - previous;
            previous = now;
            self.drain(cx);
            self.runtime.turn(cx, dt);
            cx.shared().turn_done();
            turn_end = Instant::now();
        }
        cx.shared().stop_requested() && !cx.cancelled()
    }

    fn drain(&mut self, cx: &WorkerContext<'_>) {
        debug_assert_eq!(self.thread, std::thread::current().id());
        let runtime = &mut self.runtime;
        self.consumer.drain(|command| runtime.command(cx, command));
    }

    /// Orderly: SF's stop, then drop everything here. Fast: run no mod
    /// code at all and leak the state (no Lua close, no `__gc`). A cancellation that
    /// arrives during SF's stop (the exit's signal) makes the rest fast: closing the
    /// state would run every `__gc`, with hooks off, after the pool asked us to go.
    pub(crate) fn leave(mut self, cx: &WorkerContext<'_>, orderly: bool) {
        if orderly {
            self.runtime.stop(cx);
            if cx.cancelled() {
                std::mem::forget(self);
            } else {
                drop(self);
            }
        } else {
            std::mem::forget(self);
        }
    }
}

/// Parks until `deadline`. An unpark (a producer, a hop) does not end the wait:
/// inbox items wait for the next turn, as ShroudForge's do. Returns false as soon
/// as the worker must stop. Each park is at most the turn gap, so an idle worker
/// sees `cancelled()` within 16 ms.
fn park_until(cx: &WorkerContext<'_>, deadline: Instant) -> bool {
    loop {
        if cx.interrupt().is_some() {
            return false;
        }
        let now = Instant::now();
        if now >= deadline {
            return true;
        }
        let wait = (deadline - now).min(TURN_GAP);
        cx.shared().parked_for(wait.as_nanos() as u64);
        std::thread::park_timeout(wait);
    }
}
