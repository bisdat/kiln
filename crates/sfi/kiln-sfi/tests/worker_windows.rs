// SPDX-License-Identifier: MIT
//! The worker on Windows: its body marks its thread as the interpreter's for
//! the native-load enforcement (`shim/seh_guard.c` `interpreter_thread`, read by the
//! stack-overflow park decision) on entry, and clears it on every way out: an
//! orderly stop, a cancellation and a caught panic. Runs under headless Wine;
//! empty on other targets.
#![cfg(windows)]
mod support;

use kiln_abi::*;
use kiln_sfi::worker::*;
use std::collections::HashMap;
use std::sync::{Arc, LazyLock, Mutex};
use std::time::Duration;
use support::*;

unsafe extern "C" {
    fn sfi_guard_interpreter_thread() -> u32;
}
unsafe extern "system" {
    fn GetCurrentThreadId() -> u32;
}
fn marked_thread() -> u32 {
    unsafe { sfi_guard_interpreter_thread() }
}

#[derive(Clone, Copy, PartialEq)]
enum Mode {
    Run,
    Panic,
}
#[derive(Default)]
struct Seen {
    turns: Vec<bool>,
}
static MODES: LazyLock<Mutex<HashMap<KilnOwner, Mode>>> = LazyLock::new(Default::default);
static SEEN: LazyLock<Mutex<HashMap<KilnOwner, Seen>>> = LazyLock::new(Default::default);

struct MarkProbe(KilnOwner, Mode);
impl WorkerRuntime for MarkProbe {
    fn turn(&mut self, _: &WorkerContext<'_>, _: Duration) {
        let mine = marked_thread() == unsafe { GetCurrentThreadId() };
        let mut seen = SEEN.lock().unwrap();
        let turns = &mut seen.entry(self.0).or_default().turns;
        turns.push(mine);
        if self.1 == Mode::Panic && turns.len() >= 2 {
            drop(seen);
            // An unwind that skips the panic hook (no backtrace capture before the body
            // can return); ThreadMark's drop runs during this unwind all the same.
            std::panic::resume_unwind(Box::new("mark probe: panic inside a turn"));
        }
    }
    fn stop(&mut self, _: &WorkerContext<'_>) {}
}
fn factory(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
    let mode = MODES.lock().unwrap()[&cx.owner()];
    Ok(Box::new(MarkProbe(cx.owner(), mode)))
}

/// A liveness bound, not a latency claim: a passing wait returns as soon as its condition
/// holds, so this sits far above any delay scheduling alone can cause (Wine, Idle priority).
const LIVENESS: Duration = Duration::from_secs(300);

fn run_case(mode: Mode, leave: impl FnOnce(&FakePool, &WorkerShared)) -> Vec<bool> {
    let owner = next_owner();
    MODES.lock().unwrap().insert(owner, mode);
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = api(true);
    let shared: Arc<WorkerShared> = WorkerShared::new(WorkerConfig {
        owner,
        host: unsafe { Host::read(&*api) },
        factory,
        inbox: InboxConfig::DEFAULT,
    });
    let tasks = unsafe { TasksTable::read(tasks_table()) }.unwrap();
    assert_eq!(
        marked_thread(),
        0,
        "no thread is marked before the worker runs"
    );
    submit(&shared, &tasks).unwrap();
    assert!(eventually(LIVENESS, || {
        SEEN.lock()
            .unwrap()
            .get(&owner)
            .is_some_and(|s| s.turns.len() >= 2)
    }));
    leave(&pool, &shared);
    assert!(pool.task(0).wait_finished(LIVENESS));
    assert_eq!(
        marked_thread(),
        0,
        "the mark is cleared when the body returns"
    );
    SEEN.lock().unwrap().remove(&owner).unwrap().turns
}

#[test]
fn the_worker_body_marks_its_thread_and_clears_the_mark_on_every_way_out() {
    // One process-wide mark: the cases run one after another.
    let orderly = run_case(Mode::Run, |_, shared| shared.request_stop());
    assert!(
        orderly.iter().all(|m| *m),
        "every turn ran on the marked thread"
    );
    let cancelled = run_case(Mode::Run, |pool, shared| {
        pool.cancel(shared.task());
    });
    assert!(cancelled.iter().all(|m| *m));
    let panicked = run_case(Mode::Panic, |_, _| {});
    assert!(panicked.iter().all(|m| *m));
}
