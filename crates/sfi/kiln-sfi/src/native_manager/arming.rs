// SPDX-License-Identifier: MIT
//! Whether this start's native loader is armed, and the one thread that runs it.
//!
//! kiln-sfi makes one [`NativeRun`] per process right after its plan read (the early phase, or Kiln's
//! plugin step when the early phase never ran) from the frozen launch plan and Kiln's native.policy as
//! the kiln.launch record carries it (the frozen policy and `build_verified`). The loader (the native-load enforcement's
//! [`Enforcer::early`]: the loader notification, its supervisor thread, then the ordered loads) is
//! armed only for a run made in the early phase (the boundary concludes a run made at Kiln's plugin
//! step with [`EARLY_PHASE_MISSED`] before it gets here: package DLLs load only in the early window),
//! and only when all three hold ([`not_armed`]):
//! - the policy allows package DLLs (`always`, or `verified-only` on a verified build);
//! - the plan lists at least one DLL;
//! - the startup witness port is wired (kiln-eml's startup seam attached, [`crate::startup_seam::SeamWitness`];
//!   [`WitnessPort::NotWired`] without it).
//!
//! Otherwise [`NativeRun::conclude_without_loader`] gives every planned DLL its `Refused` terminal
//! at once: no notifier is registered, no thread starts, nothing is captured, hashed or loaded. A
//! start with only Lua packages therefore arms nothing; its "no ShroudForge runtime of its own" claim
//! rests on the early module audit and the look right before the start.
//!
//! An armed run gets one plain thread, [`NATIVE_THREAD`] (not kiln.tasks: this is the early window,
//! before Kiln's plugin step), and the caller never waits for it.
//!
//! With kiln-eml's startup seam attached, the witness port is wired to it and the run's terminal goes back to
//! kiln-eml once it is known ([`arm_and_push`]): at once for a run concluded without the loader, on the loader thread
//! right after its body for an armed one.
//!
//! An armed run that pushes its terminal also gets ShroudForge's startup deadline in the early window, so a package
//! DLL whose initializer hangs or is slow never leaves kiln-eml without a terminal inside its own wait (attach + 20 s).
//! ShroudForge starts its 15 s once the whole plan is handed to its ordered worker (1.7.9
//! `modules/mod-runtime/native_dlls.rs:229-244`: every entry queued, then `Instant::now() + STARTUP_LOAD_TIMEOUT`);
//! kiln-sfi hands its whole plan to the loader thread when it arms the run, so the clock starts there, before the
//! witness wait and the first load. A second thread, the deadline waiter ([`DEADLINE_THREAD`]), is started before the
//! loader thread, and the loader thread waits until it runs before anything else: a thread started while a package's
//! DllMain holds the loader lock could not run until that DllMain returned. At the deadline the waiter concludes a run
//! whose DLLs are not all terminal ([`NativeRun::expire`]: unfinished DLLs `TimedOut`, their packages and dependents
//! withdrawn, no load starts afterwards) and pushes that terminal. The loader thread is never stopped: like
//! ShroudForge's worker it is left inside the DLL's initializer, and what it loads afterwards is only logged. Exactly
//! one terminal is pushed, once: the run concludes once ([`NativeRun::wait`] and `expire` share one terminal) and the
//! push sits in one slot both threads take from. A run every DLL of which is terminal in time is pushed by the loader
//! thread as before; the waiter then ends without pushing. Neither thread waits for the other's witness recheck longer
//! than [`super::executor::RECHECK_ALLOWANCE`]: then it pushes the run's terminal not coherent itself. A run that is not
//! armed starts no waiter.
use super::executor::{NativeRun, NativeTerminal, Witness, WitnessToken};
use crate::{
    native_contracts::NativeContracts,
    native_enforcement::{Backend, Enforcer, Sink},
};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Condvar, Mutex, mpsc};
use std::time::{Duration, Instant};

/// The name of the one thread an armed run gets.
pub const NATIVE_THREAD: &str = "kiln-sfi native";
/// The name of an armed run's deadline waiter.
pub const DEADLINE_THREAD: &str = "kiln-sfi native deadline";

/// Why the loader is not armed: the plan lists no DLL.
pub const NO_DLLS: &str = "this start's launch plan lists no ShroudForge package DLL";
/// Why the loader is not armed: the run was made at Kiln's plugin step because kiln-sfi's early phase never ran, and
/// package DLLs load only in the early window (before the data seal).
pub const EARLY_PHASE_MISSED: &str = "kiln-sfi's early phase did not run this start, and ShroudForge package DLLs load only in the early window: no ShroudForge package DLL loads this start";
/// Why the loader is not armed: no startup witness is wired (no kiln-eml startup seam was attached this start).
pub const WITNESS_NOT_WIRED: &str = "kiln-eml's guarded startup baseline is not available this start (no startup seam was attached): no ShroudForge package DLL loads this start, so a package that ships one does not run";

/// Where the startup witness comes from.
#[derive(Clone)]
pub enum WitnessPort {
    /// No startup witness this start (no kiln-eml startup seam attached): the loader is never armed.
    NotWired,
    /// The startup pass's witness: kiln-eml's startup seam ([`crate::startup_seam::SeamWitness`]); tests use a
    /// fake.
    Wired(Arc<dyn Witness>),
}
impl WitnessPort {
    pub fn is_wired(&self) -> bool {
        matches!(self, Self::Wired(_))
    }
    /// The witness a [`NativeRun`] is made with: the wired one, or one that is never ready.
    pub fn witness(&self) -> Arc<dyn Witness> {
        match self {
            Self::Wired(witness) => Arc::clone(witness),
            Self::NotWired => Arc::new(NotWired),
        }
    }
}
/// The port without a startup seam (the boundary wires kiln-eml's seam when it attached one).
pub fn not_wired() -> WitnessPort {
    WitnessPort::NotWired
}

/// A witness that is never ready (the port is not wired).
struct NotWired;
impl Witness for NotWired {
    fn ready(&self) -> Result<WitnessToken, String> {
        Err(WITNESS_NOT_WIRED.into())
    }
    fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
        Err(WITNESS_NOT_WIRED.into())
    }
}

/// `None` when the loader may be armed: the permission allows, the plan lists at least one DLL and
/// the witness port is wired. Otherwise why not, the policy's words first (the player's own choice),
/// then the empty plan, then the missing witness.
pub fn not_armed(run: &NativeRun, witness: &WitnessPort) -> Option<String> {
    if let Some(refusal) = run.permission().refusal() {
        return Some(refusal.to_owned());
    }
    if run.dll_count() == 0 {
        return Some(NO_DLLS.to_owned());
    }
    if !witness.is_wired() {
        return Some(WITNESS_NOT_WIRED.to_owned());
    }
    None
}

/// What [`arm`] did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Armed {
    /// The loader thread was started; its run reports through [`NativeRun::wait`].
    Started,
    /// The run was concluded without the loader, and why.
    NotArmed(String),
}

/// The body of the loader thread: `Enforcer`'s early arming, then the ordered loads.
pub type LoaderBody = Box<dyn FnOnce() + Send + 'static>;

/// Arms the loader for `run`, or concludes it without one ([`not_armed`]). `body` is asked for the
/// thread's body only when the loader is armed, and `spawn` starts it (production:
/// [`spawn_native_thread`]); a thread that cannot start concludes the run with that reason. Never
/// waits for the thread. One call per run: a run concluded or started before changes nothing.
pub fn arm(
    run: &Arc<NativeRun>,
    witness: &WitnessPort,
    spawn: impl FnOnce(LoaderBody) -> Result<(), String>,
    body: impl FnOnce() -> LoaderBody,
) -> Armed {
    if let Some(reason) = not_armed(run, witness) {
        run.conclude_without_loader(&reason);
        return Armed::NotArmed(reason);
    }
    match spawn(body()) {
        Ok(()) => Armed::Started,
        Err(error) => {
            let reason = format!(
                "Kiln's native loader thread could not start ({error}): no ShroudForge package DLL loads this start"
            );
            run.conclude_without_loader(&reason);
            Armed::NotArmed(reason)
        }
    }
}

/// Where a run's native terminal goes once it is known (kiln-eml's startup seam). Called at most once.
pub type TerminalPush = Box<dyn FnOnce(&NativeTerminal) + Send + 'static>;

/// Why an armed run's loader stopped when its body panicked.
pub const LOADER_STOPPED: &str = "Kiln's native loader stopped with an internal error: no further ShroudForge package DLL loads this start";
/// Why an armed run loads nothing when its deadline waiter did not run before the deadline (no DLL loads without
/// the waiter that guarantees the terminal).
pub const WAITER_NEVER_RAN: &str = "Kiln's native startup deadline passed before its deadline waiter ran: no ShroudForge package DLL loads this start";

/// When an armed run's deadline passes ([`arm_and_push`]).
pub enum Deadline {
    /// This long after the arming (production: [`super::executor::STARTUP_NATIVE_DEADLINE`], ShroudForge's 15 s; a
    /// `Duration` converts to it).
    After(Duration),
    /// When this receiver gets a message or its sender is dropped, never by the clock: for tests whose deadline must pass
    /// after something they wait for (a load in progress, a waiter that ran), however slowly a loaded machine schedules
    /// the run's threads. The loader thread then waits for its waiter without a bound, and the waiter waits for the
    /// signal even when the run is done before it.
    Signalled(mpsc::Receiver<()>),
}
impl From<Duration> for Deadline {
    fn from(after: Duration) -> Self {
        Self::After(after)
    }
}

/// [`arm`], and `push` given the run's terminal once it is known: at once when the run is concluded without the loader
/// (a thread that could not start included), and for an armed run when the run is concluded: on the loader thread right
/// after its body returned, or on the deadline waiter when `deadline` passes first. The deadline counts from this
/// call (the plan is handed to the loader here, ShroudForge's start point; [`Deadline::After`], or a test's
/// [`Deadline::Signalled`] that passes when signalled). `waiter` starts the deadline waiter, before
/// `spawn` starts the loader thread (production: [`spawn_deadline_thread`], [`spawn_native_thread`],
/// [`super::executor::STARTUP_NATIVE_DEADLINE`]); a waiter that cannot start concludes the run without the loader. The
/// loader thread does nothing before the waiter runs, and loads nothing when the deadline passes first
/// ([`WAITER_NEVER_RAN`]). A body that panics abandons the run first (every DLL not yet terminal is refused), so the
/// terminal still goes out; a body that never returns leaves the push to the waiter. Exactly one push. Never waits.
pub fn arm_and_push(
    run: &Arc<NativeRun>,
    witness: &WitnessPort,
    spawn: impl FnOnce(LoaderBody) -> Result<(), String>,
    waiter: impl FnOnce(LoaderBody) -> Result<(), String>,
    deadline: impl Into<Deadline>,
    body: impl FnOnce() -> LoaderBody,
    push: TerminalPush,
) -> Armed {
    // ShroudForge's start point: the whole plan is handed to the ordered loader, before its first load.
    let (after, until, signal) = match deadline.into() {
        Deadline::After(after) => (after, Some(Instant::now() + after), None),
        // the loader thread's body ends with every DLL terminal, or the run concluded by the waiter: nothing to wait for
        Deadline::Signalled(signal) => (Duration::ZERO, None, Some(signal)),
    };
    let slot = Arc::new(Mutex::new(Some(push)));
    let gate = Arc::new(Gate::default());
    let (thread_slot, thread_run, thread_gate) =
        (Arc::clone(&slot), Arc::clone(run), Arc::clone(&gate));
    let (waiter_slot, waiter_run) = (Arc::clone(&slot), Arc::clone(run));
    let armed = arm(
        run,
        witness,
        move |loader| {
            // the waiter first: it exists, and runs, before the loader thread does anything
            waiter(Box::new(move || {
                gate.open();
                let until = until.unwrap_or_else(|| {
                    // a signalled deadline: it passes now (a dropped sender is a signal too)
                    let _ = signal.map(|signal| signal.recv());
                    Instant::now()
                });
                if let Some(terminal) = waiter_run.expire(until) {
                    push_once(&waiter_slot, &terminal);
                }
            }))
            .map_err(|error| format!("its deadline waiter did not start: {error}"))?;
            spawn(loader)
        },
        move || {
            let inner = body();
            Box::new(move || {
                if !thread_gate.wait_open(until) {
                    thread_run.abandon(WAITER_NEVER_RAN);
                } else if catch_unwind(AssertUnwindSafe(inner)).is_err() {
                    thread_run.abandon(LOADER_STOPPED);
                }
                // the body ran the plan to its end (or abandoned it): the terminal is there at once, or is the one
                // the waiter concluded at the deadline (then the slot is empty)
                push_once(&thread_slot, &thread_run.wait(after));
            })
        },
    );
    if let Armed::NotArmed(_) = armed {
        push_once(&slot, &run.wait(Duration::ZERO));
    }
    armed
}

fn push_once(slot: &Mutex<Option<TerminalPush>>, terminal: &NativeTerminal) {
    let push = slot.lock().unwrap_or_else(|e| e.into_inner()).take();
    if let Some(push) = push {
        push(terminal);
    }
}

/// Opened by the deadline waiter once it runs; the loader thread waits for it before anything else.
#[derive(Default)]
struct Gate {
    open: Mutex<bool>,
    changed: Condvar,
}
impl Gate {
    fn open(&self) {
        *self.open.lock().unwrap_or_else(|e| e.into_inner()) = true;
        self.changed.notify_all();
    }
    /// True once it is open; false when `until` passed first (no `until`: a signalled deadline, waits until open).
    fn wait_open(&self, until: Option<Instant>) -> bool {
        let mut open = self.open.lock().unwrap_or_else(|e| e.into_inner());
        while !*open {
            let Some(until) = until else {
                open = self.changed.wait(open).unwrap_or_else(|e| e.into_inner());
                continue;
            };
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return false;
            }
            open = self
                .changed
                .wait_timeout(open, left)
                .unwrap_or_else(|e| e.into_inner())
                .0;
        }
        true
    }
}

/// The loader thread's body for `run`: `make` builds the backend, the sink and the contract registry
/// on that thread (the Windows backend stays on its thread), the native-load enforcement arms early (the
/// notification is live before the first package DLL, then the scan again), and the run executes
/// once. A backend or an early enforcement that refuses loads nothing. The enforcement is kept for
/// the life of the process, as the enforcement's notification and pinned callback are; nothing is unloaded.
pub fn loader_body<B, S, M>(run: Arc<NativeRun>, make: M) -> LoaderBody
where
    B: Backend + 'static,
    S: Sink + Send + 'static,
    M: FnOnce() -> Result<(B, S, NativeContracts), String> + Send + 'static,
{
    Box::new(move || {
        let (backend, sink, registry) = match make() {
            Ok(parts) => parts,
            Err(error) => {
                return run.abandon(&format!(
                    "Kiln's native loader is not available ({error}): no ShroudForge package DLL loads this start"
                ));
            }
        };
        match Enforcer::early(backend, sink) {
            Ok(mut enforcer) => {
                run.execute(&mut enforcer, &registry);
                // The notification, its supervisor and the quarantine state live until process exit.
                std::mem::forget(enforcer);
            }
            Err(error) => run.abandon(&format!(
                "Kiln's native enforcement could not start ({error}): no ShroudForge package DLL loads this start"
            )),
        }
    })
}

/// The production spawn: one plain thread named [`NATIVE_THREAD`], never joined.
pub fn spawn_native_thread(body: LoaderBody) -> Result<(), String> {
    std::thread::Builder::new()
        .name(NATIVE_THREAD.into())
        .spawn(body)
        .map(drop)
        .map_err(|e| e.to_string())
}

/// The production spawn of an armed run's deadline waiter: one plain thread named [`DEADLINE_THREAD`], never
/// joined (at the deadline it may still be waiting to exit while a package's DllMain holds the loader lock).
pub fn spawn_deadline_thread(body: LoaderBody) -> Result<(), String> {
    std::thread::Builder::new()
        .name(DEADLINE_THREAD.into())
        .spawn(body)
        .map(drop)
        .map_err(|e| e.to_string())
}
