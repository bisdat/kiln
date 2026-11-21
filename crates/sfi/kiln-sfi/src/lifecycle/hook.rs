// SPDX-License-Identifier: MIT
//! Lua interruption and the watchdog at the VM.
//!
//! One count hook per VM, installed with `set_global_hook` before any mod code
//! runs. A coroutine created from Lua inherits the VM's hook procedure; with the
//! global form that procedure finds the callback, while mlua's per-thread
//! `set_hook` form removes itself in a coroutine it did not register, so a mod
//! could run `coroutine.wrap(function() while true do end end)()` unchecked.
//!
//! Every check, in this order:
//! 1. `run->cancelled(run)`, then kiln-sfi's stop (not during the orderly leave,
//!    which is the stop kiln-sfi asked for): raise [`StopSentinel`];
//! 2. the open call's watchdog account over 10 s: raise [`WatchdogExpired`];
//! 3. during runtime-create, SF's 15 s "has not returned" log line, once.
//!
//! Both errors are re-raised at every later check while their condition holds, so
//! a mod's `pcall` cannot run more than one hook interval past them. A call that went
//! over the limit fails when it ends even if the mod caught the error and returned
//! (`CallbackPort::exit`).
//!
//! What the hook cannot bound (only the exit park ends it, then the thread is
//! named and detached):
//! - a mod that keeps catching the sentinel in a loop: each re-entry is interrupted
//!   at its next check, but an orderly stop then waits for that mod indefinitely;
//! - a `__gc` finalizer: Lua 5.4 runs finalizers with hooks off (lgc.c `GCTM` sets
//!   `allowhook = 0`), so no check runs inside one, whether it ran from a GC step,
//!   `collectgarbage()` or the state's close in the orderly leave;
//! - a DLL callback that blocks in native code.

use super::watchdog::Watchdog;
use crate::worker::{
    Host, Interrupt, InterruptProbe, StopSentinel, WorkerContext, WorkerShared, stop_sentinel_in,
};
use kiln_abi::*;
use mlua::{HookTriggers, Lua, VmState};
use sfi_core::lifecycle::{CallbackPort, Monotonic};
use std::cell::Cell;
use std::rc::Rc;
use std::time::Duration;

/// VM instructions between two checks (1,000 to 10,000). Any count hook keeps
/// Lua 5.4 in trap mode (each instruction passes `luaG_traceexec`), so the floor
/// cost does not depend on this number; the hook call itself does. Measured in
/// release (measured when it landed): a tables/strings workload +17% at 1,000 and +7.9% at
/// 10,000; a tight arithmetic loop +170% / +157%. 10,000 instructions are still
/// only tens of microseconds, so the stop and the watchdog stay prompt.
pub const HOOK_INSTRUCTIONS: u32 = 10_000;

/// SF's runtime-create watchdog line (ShroudForge 1.7.9 windows-bootstrap/runtime_lifecycle.cpp:102-104).
pub(crate) const CREATE_LOG_AFTER: Duration = Duration::from_secs(15);
pub(crate) const CREATE_LOG_LINE: &str = "Runtime initialization has not returned after 15 seconds at \
     runtime-create. Inspect the last [shroudforge::runtime] startup stage above";

/// The error a call that went over the watchdog limit fails with. A mod failure
/// (that mod stops), never the worker's stop.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WatchdogExpired {
    pub owner: String,
}
impl std::fmt::Display for WatchdogExpired {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "lifecycle failure: a call into '{}' ran for more than 10 s of Lua time and was \
             stopped by Kiln's watchdog",
            self.owner
        )
    }
}
impl std::error::Error for WatchdogExpired {}

/// The worker side of sfi-core's [`CallbackPort`], shared with the count hook.
pub(crate) struct HostPort {
    probe: InterruptProbe,
    shared: *const WorkerShared,
    host: Host,
    owner: KilnOwner,
    pub(crate) watchdog: Rc<Watchdog>,
    clock: Rc<dyn Monotonic>,
    /// Set for the orderly leave: kiln-sfi's stop no longer interrupts on_unload.
    pub(crate) leaving: Cell<bool>,
    creating: Cell<Option<Duration>>,
    create_logged: Cell<bool>,
}

impl HostPort {
    pub(crate) fn new(
        cx: &WorkerContext<'_>,
        watchdog: Rc<Watchdog>,
        clock: Rc<dyn Monotonic>,
    ) -> Self {
        Self {
            probe: cx.interrupt_probe(),
            shared: cx.shared(),
            host: *cx.shared().host(),
            owner: cx.shared().owner(),
            watchdog,
            clock,
            leaving: Cell::new(false),
            creating: Cell::new(None),
            create_logged: Cell::new(false),
        }
    }

    pub(crate) fn log(&self, level: i32, text: &str) {
        self.host.log(self.owner, level, text);
    }

    /// The interrupt in force now (kiln-sfi's stop is ignored while leaving).
    pub(crate) fn pending(&self) -> Option<Interrupt> {
        // SAFETY: the port lives in the runtime, which exists only inside the body
        // on the worker thread; a fast leave forgets it without running it again.
        unsafe { self.probe.check(!self.leaving.get()) }
    }

    /// The check in a hop wait: the pool's cancellation or kiln-sfi's stop,
    /// the stop honoured even during the orderly leave.
    pub(crate) fn interrupt_any(&self) -> Option<Interrupt> {
        // SAFETY: as in `pending`.
        unsafe { self.probe.check(true) }
    }

    /// An owned reference to the worker's shared state (a late trampoline keeps it).
    pub(crate) fn shared_arc(&self) -> std::sync::Arc<WorkerShared> {
        // SAFETY: `shared` is the task's `user`, from `Arc::into_raw`, and its
        // reference is live while the body runs (done reclaims it afterwards).
        unsafe {
            std::sync::Arc::increment_strong_count(self.shared);
            std::sync::Arc::from_raw(self.shared)
        }
    }

    fn shared(&self) -> &WorkerShared {
        // SAFETY: as in `pending`: the body still runs.
        unsafe { &*self.shared }
    }

    pub(crate) fn begin_create(&self) {
        self.creating.set(Some(self.clock.now()));
    }
    pub(crate) fn end_create(&self) {
        self.create_watch();
        self.creating.set(None);
    }
    fn create_watch(&self) {
        if let Some(start) = self.creating.get()
            && !self.create_logged.get()
            && self.clock.now().saturating_sub(start) > CREATE_LOG_AFTER
        {
            self.create_logged.set(true);
            self.log(KILN_LOG_ERROR, &format!("kiln-sfi: {CREATE_LOG_LINE}"));
        }
    }

    /// One count-hook check.
    pub(crate) fn check(&self) -> mlua::Result<VmState> {
        let raised = if let Some(interrupt) = self.pending() {
            Err(mlua::Error::external(StopSentinel::new(interrupt)))
        } else if self.watchdog.exceeded() {
            Err(mlua::Error::external(WatchdogExpired {
                owner: self.watchdog.owner().unwrap_or_default(),
            }))
        } else {
            Ok(VmState::Continue)
        };
        self.shared().hook_checked(raised.is_err());
        self.create_watch();
        raised
    }
}

impl CallbackPort for HostPort {
    fn enter(&self, owner: &str) {
        self.watchdog.begin(owner);
    }
    fn exit(&self) -> Option<String> {
        let overrun = self.watchdog.end()?;
        self.log(
            KILN_LOG_WARN,
            &format!(
                "kiln-sfi: mod '{}' stopped: one call ran {:.1} s of Lua time (Kiln's watchdog \
                 stops a call after 10 s; ShroudForge would hang every mod)",
                overrun.owner,
                overrun.execution.as_secs_f64()
            ),
        );
        // The call fails with this even if the mod caught WatchdogExpired and returned,
        // or the overrun was one engine call with no check after it.
        Some(
            WatchdogExpired {
                owner: overrun.owner,
            }
            .to_string(),
        )
    }
    fn interrupted(&self) -> bool {
        self.pending().is_some()
    }
}

/// Installs the count hook on every thread of `lua`, before any mod code runs.
pub(crate) fn install(lua: &Lua, port: Rc<HostPort>, instructions: u32) -> mlua::Result<()> {
    lua.set_global_hook(
        HookTriggers::new().every_nth_instruction(instructions.clamp(1, 10_000)),
        move |_, _| port.check(),
    )
}

/// True when `error` carries the worker's stop, through any wrapping mlua adds
/// (`chain()` yields the wrapped errors that `source()` hides).
///
/// The lifecycle runtime does not need this to tell a stop from a mod failure: its
/// port asks whether the stop is in force ([`CallbackPort::interrupted`]), which
/// also covers a mod that caught the sentinel and failed with its own error.
pub fn is_stop(error: &mlua::Error) -> bool {
    stop_sentinel_in(error.chain()).is_some()
}
/// The watchdog's error, if `error` carries it.
pub fn watchdog_expired(error: &mlua::Error) -> Option<&WatchdogExpired> {
    error
        .chain()
        .find_map(|e| e.downcast_ref::<WatchdogExpired>())
}
