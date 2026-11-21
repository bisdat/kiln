// SPDX-License-Identifier: MIT
//! The one budget the interpreter adds. A single mod call (module top level,
//! a lifecycle lookup, on_load, on_update, on_unload) that has run for more than
//! 10 s of Lua time is stopped, and that mod stops with a lifecycle failure.
//!
//! Lua time is the call's elapsed monotonic time minus:
//! - time inside a [`WatchdogPause`]: a synchronous wait for a world thread (a hop)
//!   or another deferred wait;
//! - time a native call reports as spent in ShroudForge's own bounded waits
//!   (`source_wait_ns` in a native reply), through a [`NativeCall`] guard
//!   ([`Watchdog::native_call`]) that timed that call: the exclusion is clamped to the
//!   call's measured duration, so a wrong report never forgives Lua time spent before
//!   the call.
//!
//! Ordinary engine-call time still counts. The scheduler, deadlines and
//! `shroudforge.clock` keep the actual monotonic time; only this account pauses.
//! Worker-thread only (`Rc`/`Cell`).

use sfi_core::lifecycle::Monotonic;
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::Duration;

/// Strictly more than this is a lifecycle failure (the default).
pub const WATCHDOG_LIMIT: Duration = Duration::from_secs(10);

struct Account {
    owner: String,
    start: Duration,
    paused: Duration,
    pause_start: Duration,
    pauses: u32,
    excluded: Duration,
    exceeded: bool,
}

pub struct Watchdog {
    clock: Rc<dyn Monotonic>,
    limit: Duration,
    account: RefCell<Option<Account>>,
    /// Everything [`NativeCall`] guards ever excluded. A guard whose call contains
    /// another timed call (a member that calls back into Rust, which makes a timed call
    /// of its own) counts only the part the inner guards did not already exclude, so the
    /// exclusions inside any measured call never add up to more than that call.
    native_excluded: Cell<Duration>,
}

/// Ends the pause when dropped.
pub struct WatchdogPause<'a> {
    watchdog: &'a Watchdog,
}

/// Times one native member call that can report `source_wait_ns`. Taken just before the call;
/// [`NativeCall::call`] runs and times the call,
/// [`NativeCall::exclude`] then subtracts the reported wait, clamped to the measured
/// duration of the call(s) it timed less what guards nested inside them excluded.
/// Dropping it without `exclude` excludes nothing, and so does a call that unwinds.
/// The runtime's families never take one themselves: the service adapter's timed path
/// does (`runtime::services`), and a structure test keeps `native_call` out of the
/// families' files.
#[must_use = "a native call guard excludes nothing unless its call is timed and excluded"]
pub struct NativeCall<'a> {
    watchdog: &'a Watchdog,
    measured: Duration,
}

impl NativeCall<'_> {
    /// Runs `call` (the member call itself, nothing else) and adds its duration to
    /// this guard's measurement.
    pub fn call<R>(&mut self, call: impl FnOnce() -> R) -> R {
        let nested_before = self.watchdog.native_excluded.get();
        let start = self.watchdog.clock.now();
        let value = call();
        let elapsed = self.watchdog.clock.now().saturating_sub(start);
        // What timed calls inside this one already excluded is not this call's to forgive.
        let nested = self
            .watchdog
            .native_excluded
            .get()
            .saturating_sub(nested_before);
        self.measured = self.measured.saturating_add(elapsed.saturating_sub(nested));
        value
    }
    /// The duration of the call(s) timed so far, less what nested guards excluded.
    pub fn measured(&self) -> Duration {
        self.measured
    }
    /// Subtracts the call's reported `source_wait_ns` from the open account: never
    /// more than the call's measured duration, and never more than the account's Lua
    /// time so far. Returns what was excluded.
    pub fn exclude(self, wait: Duration) -> Duration {
        self.watchdog.exclude(wait.min(self.measured))
    }
    /// [`Self::exclude`] for a raw `source_wait_ns` value.
    pub fn exclude_ns(self, wait_ns: u64) -> Duration {
        self.exclude(Duration::from_nanos(wait_ns))
    }
}

/// What [`Watchdog::end`] reports for a call that went over the limit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Overrun {
    pub owner: String,
    pub execution: Duration,
}

impl Watchdog {
    pub fn new(clock: Rc<dyn Monotonic>) -> Self {
        Self::with_limit(clock, WATCHDOG_LIMIT)
    }
    pub fn with_limit(clock: Rc<dyn Monotonic>, limit: Duration) -> Self {
        Self {
            clock,
            limit,
            account: RefCell::new(None),
            native_excluded: Cell::new(Duration::ZERO),
        }
    }

    /// One account for the whole call. A nested `begin` (never done by the
    /// lifecycle runtime) keeps the outer account.
    pub(crate) fn begin(&self, owner: &str) {
        let mut account = self.account.borrow_mut();
        if account.is_none() {
            *account = Some(Account {
                owner: owner.to_owned(),
                start: self.clock.now(),
                paused: Duration::ZERO,
                pause_start: Duration::ZERO,
                pauses: 0,
                excluded: Duration::ZERO,
                exceeded: false,
            });
        }
    }

    /// Closes the account; `Some` when the call went over the limit.
    pub(crate) fn end(&self) -> Option<Overrun> {
        self.sample();
        let account = self.account.borrow_mut().take()?;
        account.exceeded.then(|| Overrun {
            execution: self.execution_of(&account),
            owner: account.owner,
        })
    }

    /// Time while the returned guard lives does not count (a hop or other deferred
    /// wait). Nested pauses count once.
    pub fn pause(&self) -> WatchdogPause<'_> {
        let now = self.clock.now();
        if let Some(account) = self.account.borrow_mut().as_mut() {
            if account.pauses == 0 {
                account.pause_start = now;
            }
            account.pauses += 1;
        }
        WatchdogPause { watchdog: self }
    }

    /// The guard every native call that can report `source_wait_ns` is made through
    /// (the runtime's service adapter makes it the only path). Take it just before the
    /// member call.
    pub fn native_call(&self) -> NativeCall<'_> {
        NativeCall {
            watchdog: self,
            measured: Duration::ZERO,
        }
    }

    /// Subtracts a native call's reported `source_wait_ns` (SF-bounded waits only).
    /// Clamped to the call's Lua time so far: a wait cannot be longer than the time
    /// that passed, so a wrong or huge report never hides Lua time still to come.
    /// Only [`NativeCall::exclude`] calls this, with the wait already clamped to the
    /// measured call. Returns what was excluded (zero when no call is open).
    fn exclude(&self, wait: Duration) -> Duration {
        let room = self.execution();
        match self.account.borrow_mut().as_mut() {
            Some(account) => {
                let excluded = wait.min(room);
                account.excluded = account.excluded.saturating_add(excluded);
                self.native_excluded
                    .set(self.native_excluded.get().saturating_add(excluded));
                excluded
            }
            None => Duration::ZERO,
        }
    }

    /// The open call's Lua time so far (zero when no call is open).
    pub fn execution(&self) -> Duration {
        self.account
            .borrow()
            .as_ref()
            .map_or(Duration::ZERO, |account| self.execution_of(account))
    }

    /// Strictly over the limit. Latched for the rest of the call: a caught error
    /// cannot clear it.
    pub fn exceeded(&self) -> bool {
        self.sample();
        self.account.borrow().as_ref().is_some_and(|a| a.exceeded)
    }

    /// The package that owns the open call.
    pub fn owner(&self) -> Option<String> {
        self.account.borrow().as_ref().map(|a| a.owner.clone())
    }

    fn sample(&self) {
        let over = self
            .account
            .borrow()
            .as_ref()
            .is_some_and(|account| self.execution_of(account) > self.limit);
        if over && let Some(account) = self.account.borrow_mut().as_mut() {
            account.exceeded = true;
        }
    }

    fn execution_of(&self, account: &Account) -> Duration {
        let now = self.clock.now();
        let paused = account.paused.saturating_add(if account.pauses > 0 {
            now.saturating_sub(account.pause_start)
        } else {
            Duration::ZERO
        });
        now.saturating_sub(account.start)
            .saturating_sub(paused)
            .saturating_sub(account.excluded)
    }
}

impl Drop for WatchdogPause<'_> {
    fn drop(&mut self) {
        let now = self.watchdog.clock.now();
        if let Some(account) = self.watchdog.account.borrow_mut().as_mut()
            && account.pauses > 0
        {
            account.pauses -= 1;
            if account.pauses == 0 {
                account.paused = account
                    .paused
                    .saturating_add(now.saturating_sub(account.pause_start));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    struct Fake(Cell<Duration>);
    impl Monotonic for Fake {
        fn now(&self) -> Duration {
            self.0.get()
        }
    }
    fn setup() -> (Rc<Fake>, Watchdog) {
        let clock = Rc::new(Fake(Cell::new(Duration::from_secs(100))));
        let watchdog = Watchdog::new(clock.clone());
        (clock, watchdog)
    }
    fn advance(clock: &Fake, ms: u64) {
        clock.0.set(clock.0.get() + Duration::from_millis(ms));
    }

    #[test]
    fn exactly_ten_seconds_is_allowed_and_one_millisecond_more_is_not() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 9_999);
        assert!(!watchdog.exceeded());
        advance(&clock, 1);
        assert!(!watchdog.exceeded(), "exactly 10 s is not over");
        advance(&clock, 1);
        assert!(watchdog.exceeded());
        let overrun = watchdog.end().expect("over");
        assert_eq!(overrun.owner, "a");
        assert_eq!(overrun.execution, Duration::from_millis(10_001));
        assert!(!watchdog.exceeded(), "no open call");
    }

    #[test]
    fn hop_waits_and_source_waits_do_not_count_but_ordinary_time_does() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 4_000); // Lua
        advance(&clock, 30_000); // a native call's SF-bounded wait...
        watchdog.exclude(Duration::from_millis(30_000)); // ...reported after it returned
        {
            let _hop = watchdog.pause();
            let _nested = watchdog.pause();
            advance(&clock, 60_000); // waiting for a world thread
        }
        advance(&clock, 5_000); // Lua
        assert_eq!(watchdog.execution(), Duration::from_secs(9));
        assert!(!watchdog.exceeded());
        advance(&clock, 1_001); // ordinary engine-call time counts
        assert!(watchdog.exceeded());
        assert!(watchdog.end().is_some());
    }

    #[test]
    fn an_exclusion_never_covers_more_than_the_time_that_passed() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 1_000);
        // A reply claims a minute of source wait inside one second.
        watchdog.exclude(Duration::from_secs(60));
        assert_eq!(watchdog.execution(), Duration::ZERO);
        advance(&clock, 10_000);
        assert!(!watchdog.exceeded(), "exactly 10 s of later Lua time");
        advance(&clock, 1);
        assert!(
            watchdog.exceeded(),
            "the excess did not hide later Lua time"
        );
    }

    #[test]
    fn a_native_call_exclusion_never_exceeds_the_measured_call() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 9_900); // Lua before the call
        let mut call = watchdog.native_call();
        call.call(|| advance(&clock, 1)); // the member call itself: 1 ms
        assert_eq!(call.measured(), Duration::from_millis(1));
        // The reply claims 10 s of source wait: only the 1 ms the call took is forgiven.
        assert_eq!(
            call.exclude(Duration::from_secs(10)),
            Duration::from_millis(1)
        );
        assert_eq!(watchdog.execution(), Duration::from_millis(9_900));
        advance(&clock, 100);
        assert!(!watchdog.exceeded(), "exactly 10 s of Lua time");
        advance(&clock, 1);
        assert!(
            watchdog.exceeded(),
            "the report did not forgive the Lua time before the call"
        );
    }

    #[test]
    fn a_native_call_excludes_at_most_its_report_and_nothing_untimed() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        let mut call = watchdog.native_call();
        call.call(|| advance(&clock, 5_000));
        assert_eq!(call.exclude_ns(2_000_000_000), Duration::from_secs(2));
        assert_eq!(watchdog.execution(), Duration::from_secs(3));
        // Time between the guard and its call, or after the call, is not the call.
        let mut call = watchdog.native_call();
        advance(&clock, 4_000);
        call.call(|| ());
        advance(&clock, 4_000);
        assert_eq!(call.exclude(Duration::from_secs(60)), Duration::ZERO);
        assert_eq!(watchdog.execution(), Duration::from_secs(11));
        assert!(watchdog.exceeded());
        // A guard whose call never ran forgives nothing; no open call, nothing either.
        assert_eq!(
            watchdog.native_call().exclude(Duration::from_secs(60)),
            Duration::ZERO
        );
        let _ = watchdog.end();
        let mut call = watchdog.native_call();
        call.call(|| advance(&clock, 1_000));
        assert_eq!(call.exclude(Duration::from_secs(1)), Duration::ZERO);
    }

    #[test]
    fn a_nested_timed_call_is_not_forgiven_twice() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 9_000); // Lua before the outer call
        let mut outer = watchdog.native_call();
        outer.call(|| {
            // The outer member calls back into Rust, which makes a timed call of its own:
            // 5 s, all of it reported as source wait.
            let mut inner = watchdog.native_call();
            inner.call(|| advance(&clock, 5_000));
            assert_eq!(
                inner.exclude(Duration::from_secs(5)),
                Duration::from_secs(5)
            );
        });
        // The outer call took the same 5 s and reports them too: they were forgiven once.
        assert_eq!(outer.measured(), Duration::ZERO);
        assert_eq!(outer.exclude(Duration::from_secs(5)), Duration::ZERO);
        assert_eq!(watchdog.execution(), Duration::from_millis(9_000));
        // A wait the inner guard did not cover is still the outer call's.
        let mut outer = watchdog.native_call();
        outer.call(|| {
            advance(&clock, 2_000);
            let mut inner = watchdog.native_call();
            inner.call(|| advance(&clock, 1_000));
            assert_eq!(
                inner.exclude(Duration::from_secs(1)),
                Duration::from_secs(1)
            );
        });
        assert_eq!(outer.measured(), Duration::from_secs(2));
        assert_eq!(
            outer.exclude(Duration::from_secs(3)),
            Duration::from_secs(2)
        );
        assert_eq!(watchdog.execution(), Duration::from_millis(9_000));
    }

    #[test]
    fn a_native_call_that_unwinds_excludes_nothing() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 1_000);
        let caught = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let mut call = watchdog.native_call();
            call.call(|| {
                advance(&clock, 3_000);
                panic!("the call unwinds");
            });
            call.exclude(Duration::from_secs(60))
        }));
        assert!(caught.is_err());
        assert_eq!(watchdog.execution(), Duration::from_secs(4));
    }

    #[test]
    fn the_overrun_latches_for_the_call_and_each_call_has_its_own_account() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 10_001);
        assert!(watchdog.exceeded());
        // A pause or exclusion afterwards cannot clear what was sampled.
        watchdog.exclude(Duration::from_secs(60));
        assert!(watchdog.exceeded(), "latched");
        assert!(watchdog.end().is_some());
        for _ in 0..3 {
            watchdog.begin("b");
            advance(&clock, 6_000);
            assert!(!watchdog.exceeded());
            assert!(watchdog.end().is_none());
            advance(&clock, 60_000); // between calls: not Lua time of any call
        }
    }

    #[test]
    fn a_nested_begin_keeps_the_outer_account() {
        let (clock, watchdog) = setup();
        watchdog.begin("a");
        advance(&clock, 6_000);
        watchdog.begin("b");
        advance(&clock, 6_000);
        assert!(watchdog.exceeded());
        assert_eq!(watchdog.owner().as_deref(), Some("a"));
    }
}
