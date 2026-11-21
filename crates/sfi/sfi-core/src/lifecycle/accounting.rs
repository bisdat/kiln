// SPDX-License-Identifier: MIT
//! Pure watchdog accounting seam. This is NOT a Lua interrupter or a worker hook.
//! The reviewed worker must enforce the latched failure at every callback boundary
//! AND install non-evadable interruption across require/protected calls/coroutines.
//! Until that integration is proven, watchdog/runtime parity remains partial.
use super::Monotonic;
use std::{cell::Cell, rc::Rc, time::Duration};

struct State {
    clock: Rc<dyn Monotonic>,
    active: Cell<bool>,
    start: Cell<Duration>,
    paused: Cell<Duration>,
    pause_start: Cell<Duration>,
    depth: Cell<usize>,
    finished: Cell<Duration>,
    exceeded: Cell<bool>,
}
#[derive(Clone)]
pub struct WatchdogAccounting {
    state: Rc<State>,
}
pub struct CallbackAccounting {
    state: Rc<State>,
}
pub struct DeferredWait {
    state: Rc<State>,
}
impl WatchdogAccounting {
    pub fn new(clock: Rc<dyn Monotonic>) -> Self {
        Self {
            state: Rc::new(State {
                clock,
                active: Cell::new(false),
                start: Cell::new(Duration::ZERO),
                paused: Cell::new(Duration::ZERO),
                pause_start: Cell::new(Duration::ZERO),
                depth: Cell::new(0),
                finished: Cell::new(Duration::ZERO),
                exceeded: Cell::new(false),
            }),
        }
    }
    /// One guard for the ENTIRE callback, including nested require, coroutines and
    /// protected calls. Never begin a fresh account for one of those nested calls.
    pub fn begin(&self) -> Result<CallbackAccounting, &'static str> {
        let s = &self.state;
        if s.active.get() || s.depth.get() != 0 {
            return Err("callback accounting is already active");
        }
        s.active.set(true);
        s.start.set(s.clock.now());
        s.paused.set(Duration::ZERO);
        s.finished.set(Duration::ZERO);
        s.exceeded.set(false);
        Ok(CallbackAccounting { state: s.clone() })
    }
    /// Host-only port for an ACTUAL synchronous deferred wait or SF-bounded native
    /// wait. Never use for ordinary caller-thread engine operations/busy refusals.
    /// This pauses ONLY accounting, not the host scheduler/process clock.
    pub fn pause_deferred_wait(&self) -> Result<DeferredWait, &'static str> {
        let s = &self.state;
        if !s.active.get() {
            return Err("no callback accounting is active");
        }
        // Charge execution up to the first wait, before pausing.
        s.sample();
        if s.depth.get() == 0 {
            s.pause_start.set(s.clock.now());
        }
        s.depth.set(
            s.depth
                .get()
                .checked_add(1)
                .ok_or("deferred wait nesting overflow")?,
        );
        Ok(DeferredWait { state: s.clone() })
    }
    pub fn execution(&self) -> Duration {
        self.state.execution()
    }
    /// Strictly >10s, not >=10s. Once sampled, a caught Lua error cannot clear it.
    pub fn exceeded(&self) -> bool {
        self.state.sample();
        self.state.exceeded.get()
    }
}
impl State {
    fn execution(&self) -> Duration {
        if !self.active.get() {
            return self.finished.get();
        }
        let now = self.clock.now();
        let paused = self.paused.get().saturating_add(if self.depth.get() > 0 {
            now.saturating_sub(self.pause_start.get())
        } else {
            Duration::ZERO
        });
        now.saturating_sub(self.start.get()).saturating_sub(paused)
    }
    fn sample(&self) {
        if self.execution() > Duration::from_secs(10) {
            self.exceeded.set(true);
        }
    }
}
impl Drop for DeferredWait {
    fn drop(&mut self) {
        let s = &self.state;
        s.depth.set(s.depth.get() - 1);
        if s.depth.get() == 0 {
            s.paused.set(
                s.paused
                    .get()
                    .saturating_add(s.clock.now().saturating_sub(s.pause_start.get())),
            );
        }
    }
}
impl Drop for CallbackAccounting {
    fn drop(&mut self) {
        let s = &self.state;
        s.sample();
        s.finished.set(s.execution());
        s.active.set(false);
    }
}
