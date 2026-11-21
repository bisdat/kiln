// SPDX-License-Identifier: MIT
//! kiln-sfi's own state file, `<game>/kiln/state/sfi.json` (schema/sfi/sfi-state.schema.json). Every
//! loader plugin writes its own `kiln/state/<loader>.json`, as kiln-shroudforge and kiln-eml do; the core
//! writes no sfi.json. The launcher, Kiln.Ui and support reports read it.
//!
//! **One writer per phase change.** The record ([`Reducer`]) is written when its phase changes (starting, running,
//! ended, aside, refused), when an owner epoch changes ([`Change::Urgent`], at once like a phase) and when a warning
//! is added or changes; any other change waits for the next of those.
//! While running, warning-only writes are at most one per [`RUNNING_GAP`] (ShroudForge writes
//! its own state at most every 3 s). Every write replaces the whole file atomically ([`writer`]).
//!
//! **Who writes, and on which thread.**
//! - `aside` and `refused` at kiln_plugin_main, and `starting` in kiln-sfi's early phase under `kiln` (at on_ready when
//!   the early phase never ran), are written right there (before the game's entry point; at most the retry's half
//!   second). `starting` is written before any native run is armed, kiln-eml's startup seam is attached or the worker
//!   is submitted, so before any package effect; if it cannot be written, the interpreter does not start.
//! - `ended` is written by on_unload (kiln-sfi may write its own status file there, never
//!   waiting for the worker).
//! - Everything else, from the worker or any other thread, only updates the record in memory and hands a snapshot
//!   to kiln-sfi's own writer thread: **the tick and the worker never wait for a file.** The only lock they take is the
//!   record's own, held for in-memory work and never across IO.
//! - The writer thread is a plain thread, not a kiln.tasks task: the status must still be written after an
//!   interpreter stop seals kiln-sfi's owner, after a quarantine (OWNER_QUARANTINED), and while the pool
//!   refuses submissions at exit.
//!
//! Snapshots are numbered; the file never goes back to an older one, whichever thread writes last.
//!
//! **kiln-sfi's IO thread** (the worker never reads or writes a file). The writer thread is kiln-sfi's one general
//! IO thread: besides sfi.json it runs the [`IoJob`]s added with [`StateHub::add_io_job`] (the dedicated
//! server log follower; later the loader settings producer), each when it is due, between writes. A job never runs Lua
//! or calls Steam; it hands what it found to the worker through the inbox and never waits for the worker. Jobs end
//! with the thread, after [`StateHub::end`].

pub mod identity;
pub mod reducer;
pub mod safety;
pub mod writer;

pub use reducer::{
    Backend, Basis, Change, Data, Disposition, Dll, DllState, Identity, Intent, Lua, MenuBy,
    OutcomePackage, Package, PackageInit, Part, Phase, Presence, PreviousStart, Reason, Reducer,
    SaveSensitive, Side, Skip, StartupOutcome, Trigger, TurnedOff, Warning,
};
pub use writer::{Previous, PreviouslyActive, WriteOps};

use kiln_abi::*;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::{Duration, Instant};

/// While running, warning-only writes are at most this often (phase changes are written at once).
pub const RUNNING_GAP: Duration = Duration::from_secs(3);
/// After a failed background write (every try of the retry refused), the next one waits this long, a phase change
/// that arrives meanwhile too: the file refuses, not the snapshot, so a newer one is written by that next write. After
/// [`BACKGROUND_TRIES`] failed writes in a row the writer gives up (logged); the next change or the end starts over.
pub const BACKGROUND_RETRY: Duration = Duration::from_secs(3);
pub const BACKGROUND_TRIES: u32 = 5;

type LogFn = unsafe extern "C" fn(KilnOwner, i32, KilnStr);

/// Milliseconds since 1970 (0 if the clock is before it).
pub fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|p| p.into_inner())
}

/// One serialized record, numbered.
struct Snapshot {
    seq: u64,
    bytes: Vec<u8>,
}

/// The record changed since the last write: the writer thread serializes it, not the producer.
#[derive(Clone, Copy)]
struct Due {
    /// A phase change: written at once.
    urgent: bool,
    /// The record is running (warning-only writes wait for the gap).
    running: bool,
}

struct Io {
    dir: Option<PathBuf>,
    /// The newest snapshot on disk.
    written: u64,
    last_write: Option<Instant>,
}

/// Why a write did not reach the disk.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WriteError {
    /// The core gave no state folder.
    NoFolder,
    /// Every try of the retry failed: the last error.
    Io(String),
}
impl std::fmt::Display for WriteError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoFolder => f.write_str("the core gives no state folder"),
            Self::Io(e) => f.write_str(e),
        }
    }
}

/// The result of [`StateHub::begin`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Begun {
    /// The first record of this start is on disk.
    Written,
    /// The first record exists only in memory.
    NotWritten(WriteError),
    /// This start's record was begun before; its phase now.
    Already(Phase),
}

/// The one state record of this kiln-sfi image and its writes.
pub struct StateHub {
    model: Mutex<Option<Reducer>>,
    /// The save-sensitive packages the file [`begin`](Self::begin) replaced listed as active.
    previously_active: OnceLock<Vec<PreviouslyActive>>,
    io: Mutex<Io>,
    due: Mutex<Option<Due>>,
    wake: Condvar,
    /// The writer thread: started once, at the first record that is still going on.
    thread: OnceLock<bool>,
    stop: AtomicBool,
    log: OnceLock<(LogFn, KilnOwner)>,
    ops: WriteOps,
    // The IO thread's jobs, each with the time it is due next.
    jobs: Mutex<Vec<(Instant, Box<dyn IoJob>)>>,
}

/// When an [`IoJob`] wants to run again.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IoNext {
    /// Again after this long.
    After(Duration),
    /// Never again: the job is dropped.
    Done,
}

/// Work on kiln-sfi's IO thread (file reads that the worker and the tick must never wait for).
pub trait IoJob: Send {
    /// One run. A panic drops the job (logged once).
    fn run(&mut self) -> IoNext;
}

impl Default for StateHub {
    fn default() -> Self {
        Self::new()
    }
}

impl StateHub {
    pub const fn new() -> Self {
        Self::with_ops(WriteOps::REAL)
    }
    /// Tests replace the rename and the wait between tries.
    pub const fn with_ops(ops: WriteOps) -> Self {
        Self {
            model: Mutex::new(None),
            previously_active: OnceLock::new(),
            io: Mutex::new(Io {
                dir: None,
                written: 0,
                last_write: None,
            }),
            due: Mutex::new(None),
            wake: Condvar::new(),
            thread: OnceLock::new(),
            stop: AtomicBool::new(false),
            log: OnceLock::new(),
            ops,
            jobs: Mutex::new(Vec::new()),
        }
    }

    /// Where sfi.json goes (`<game>/kiln/state`); `None`: nothing is written. Set before [`begin`](Self::begin).
    pub fn set_dir(&self, dir: Option<PathBuf>) {
        let mut io = lock(&self.io);
        if io.dir.is_none() {
            io.dir = dir;
        }
    }
    pub fn dir(&self) -> Option<PathBuf> {
        lock(&self.io).dir.clone()
    }
    /// kiln.log for write failures, as `owner` (the first one set stays).
    pub fn set_log(&self, log: LogFn, owner: KilnOwner) {
        let _ = self.log.set((log, owner));
    }
    pub(crate) fn log(&self, level: i32, text: &str) {
        if let Some((log, owner)) = self.log.get() {
            // SAFETY: the core's log copies the text during the call.
            unsafe {
                log(
                    *owner,
                    level,
                    KilnStr {
                        ptr: text.as_ptr().cast(),
                        len: text.len(),
                    },
                )
            };
        }
    }

    /// The record's phase, `None` before [`begin`](Self::begin).
    pub fn phase(&self) -> Option<Phase> {
        lock(&self.model).as_ref().map(Reducer::phase)
    }
    /// A copy of the record (tests, support).
    pub fn record(&self) -> Option<Reducer> {
        lock(&self.model).clone()
    }
    /// The save-sensitive packages the file this start replaced listed as selected and active, or carried forward when
    /// it was an aside or refused record (empty before [`begin`](Self::begin), without a state folder, and for a
    /// suspect file): what the turned-off comparison
    /// ([`safety::turned_off`], [`Reducer::turned_off`]) needs. begin replaces that file, so this is the only copy.
    pub fn previously_active(&self) -> &[PreviouslyActive] {
        self.previously_active.get().map_or(&[], Vec::as_slice)
    }

    /// The first record of this start (`starting`, `aside` or `refused`), written now on the calling thread. It reads
    /// the file it replaces first (`previousStart`, and [`previously_active`](Self::previously_active)). Once per
    /// process: a second call changes nothing.
    pub fn begin(&self, identity: Identity, phase: Phase, reason: Option<Reason>) -> Begun {
        guard(
            Begun::NotWritten(WriteError::Io("internal error".into())),
            || {
                let snapshot = {
                    let mut model = lock(&self.model);
                    if let Some(record) = model.as_ref() {
                        return Begun::Already(record.phase());
                    }
                    let previous = match self.dir() {
                        Some(dir) => {
                            writer::sweep_temps(&dir);
                            writer::read_previous(&dir, identity.pid)
                        }
                        None => Previous {
                            start: PreviousStart::NONE,
                            save_sensitive_active: Vec::new(),
                        },
                    };
                    // an aside or refused record hands the list on; a kiln record lists its own packages
                    let record = Reducer::new(identity, previous.start, now_ms(), phase, reason)
                        .with_carried(&previous.save_sensitive_active);
                    let _ = self.previously_active.set(previous.save_sensitive_active);
                    let snapshot = snapshot_of(&record);
                    *model = Some(record);
                    snapshot
                };
                match self.write_now(&snapshot) {
                    Ok(()) => Begun::Written,
                    Err(e) => Begun::NotWritten(e),
                }
            },
        )
    }

    /// Changes the record from any thread without waiting for a file: `change` runs on the record under its lock (in
    /// memory only, no serializing), and a change that is due wakes the writer thread, which serializes and writes
    /// the record as it is then (so a burst of changes is one write). Before [`begin`](Self::begin) and after the end
    /// it does nothing.
    pub fn update(&self, change: impl FnOnce(&mut Reducer) -> Change) -> Change {
        guard(Change::None, || {
            let mut model = lock(&self.model);
            let Some(record) = model.as_mut() else {
                return Change::None;
            };
            if record.sealed() {
                return Change::None;
            }
            let done = change(record);
            if done >= Change::Warning {
                self.mark_due(Due {
                    urgent: done >= Change::Urgent,
                    running: record.phase() == Phase::Running,
                });
            }
            done
        })
    }

    /// The worker created the runtime (phase Running): `running`, written soon by the writer thread.
    pub fn worker_running(&self) {
        self.update(|r| r.set_phase(Phase::Running, None));
    }

    /// kiln-sfi will not run the interpreter after all (on_ready): `refused`, written now on the calling thread.
    pub fn refuse(&self, reason: Reason) -> Result<(), WriteError> {
        self.finish(Phase::Refused, Some(reason))
    }

    /// The final flush (on_unload): `ended`, written now on the calling thread; the writer thread is told to stop. A
    /// record that is already final changes nothing.
    pub fn end(&self) -> Result<(), WriteError> {
        let result = self.finish(Phase::Ended, None);
        self.stop.store(true, Ordering::Release);
        self.wake.notify_all();
        result
    }

    fn finish(&self, phase: Phase, reason: Option<Reason>) -> Result<(), WriteError> {
        guard(Err(WriteError::Io("internal error".into())), || {
            let snapshot = {
                let mut model = lock(&self.model);
                let Some(record) = model.as_mut() else {
                    return Ok(());
                };
                if record.set_phase(phase, reason) == Change::None {
                    return Ok(());
                }
                snapshot_of(record)
            };
            self.write_now(&snapshot)
        })
    }

    /// Writes `snapshot` on this thread unless a newer one is on disk already.
    fn write_now(&self, snapshot: &Snapshot) -> Result<(), WriteError> {
        let mut io = lock(&self.io);
        if snapshot.seq <= io.written {
            return Ok(());
        }
        let Some(dir) = io.dir.clone() else {
            return Err(WriteError::NoFolder);
        };
        match writer::write_retrying(&dir, &snapshot.bytes, &self.ops) {
            Ok(_) => {
                io.written = snapshot.seq;
                io.last_write = Some(Instant::now());
                Ok(())
            }
            Err(e) => Err(WriteError::Io(e.to_string())),
        }
    }

    /// Tells the writer thread the record is due (urgency kept until it is written) and wakes it.
    fn mark_due(&self, due: Due) {
        let mut slot = lock(&self.due);
        *slot = Some(match *slot {
            Some(old) => Due {
                urgent: old.urgent || due.urgent,
                running: due.running,
            },
            None => due,
        });
        drop(slot);
        self.wake.notify_one();
    }

    /// Starts the writer thread (once): background writes need it. Without it (no thread could be made) only the
    /// writes on the calling thread happen: the start and the end. The thread holds its own reference to the hub, so
    /// it never outlives what it writes from.
    pub fn start_writer(self: &Arc<Self>) {
        self.thread.get_or_init(|| {
            let hub = Arc::clone(self);
            let spawned = std::thread::Builder::new()
                .name("kiln-sfi state".into())
                .spawn(move || hub.run())
                .is_ok();
            if !spawned {
                self.log(
                    KILN_LOG_WARN,
                    "kiln-sfi: no thread for kiln/state/sfi.json; it is written at the start and the end only",
                );
            }
            spawned
        });
    }

    /// Adds `job` to kiln-sfi's IO thread (starting the thread if it is not running), due at once. False when no
    /// thread could be made, or after [`end`](Self::end): the job is dropped.
    pub fn add_io_job(self: &Arc<Self>, job: Box<dyn IoJob>) -> bool {
        if self.stop.load(Ordering::Acquire) {
            return false;
        }
        self.start_writer();
        if self.thread.get() != Some(&true) {
            return false;
        }
        lock(&self.jobs).push((Instant::now(), job));
        // Wake the thread under the lock it waits with, so the new job's time is seen.
        drop(lock(&self.due));
        self.wake.notify_all();
        true
    }
    /// How many IO jobs are queued (tests, status).
    pub fn io_jobs(&self) -> usize {
        lock(&self.jobs).len()
    }

    /// Runs every job that is due (outside every other lock) and returns when the next one is.
    fn run_jobs(&self) -> Option<Instant> {
        let mut jobs = std::mem::take(&mut *lock(&self.jobs));
        let now = Instant::now();
        jobs.retain_mut(|(at, job)| {
            if *at > now {
                return true;
            }
            match catch_unwind(AssertUnwindSafe(|| job.run())) {
                Ok(IoNext::After(wait)) => {
                    *at = Instant::now() + wait;
                    true
                }
                Ok(IoNext::Done) => false,
                Err(_) => {
                    self.log(
                        KILN_LOG_WARN,
                        "kiln-sfi: an IO thread job failed (caught panic) and was dropped",
                    );
                    false
                }
            }
        });
        let mut slot = lock(&self.jobs);
        // jobs added while these ran come after them
        jobs.append(&mut slot);
        *slot = jobs;
        slot.iter().map(|(at, _)| *at).min()
    }

    /// The writer thread: waits until the record is due, waits out the running gap for a warning-only change (or the
    /// retry wait after a failure), serializes the record as it is then, writes it, and tries a failed write again a
    /// few times. Never panics out; ends after [`end`](Self::end) once nothing is due.
    fn run(&self) {
        let mut state = Background {
            failures: 0,
            retry_at: None,
        };
        loop {
            match catch_unwind(AssertUnwindSafe(|| self.step(&mut state))) {
                Ok(false) => return,
                Ok(true) => {}
                // a panic in one turn: forget that turn's retry state and go on
                Err(_) => {
                    state = Background {
                        failures: 0,
                        retry_at: None,
                    }
                }
            }
        }
    }

    /// One turn of the writer thread; false when it should end. Every wait also ends when the next IO job is due
    /// (or the thread is woken): the turn then returns and the next one runs the jobs first.
    fn step(&self, state: &mut Background) -> bool {
        let job_at = self.run_jobs();
        let job_wait = || job_at.map(|at| at.saturating_duration_since(Instant::now()));
        let mut slot = lock(&self.due);
        loop {
            let stop = self.stop.load(Ordering::Acquire);
            let Some(next) = slot.as_ref() else {
                if stop {
                    return false;
                }
                match job_wait() {
                    None => drop(self.wake.wait(slot).unwrap_or_else(|p| p.into_inner())),
                    Some(wait) if wait.is_zero() => {}
                    Some(wait) => drop(
                        self.wake
                            .wait_timeout(slot, wait)
                            .unwrap_or_else(|p| p.into_inner()),
                    ),
                }
                return true;
            };
            // at the end, everything at once; after a failure, the retry wait (a phase change too, also one that
            // arrives during the wait: a refused write is not tried again at once, and the only background phase
            // change, running, is no safety fact: incomplete stays true); a phase change goes at once; a
            // warning-only change while running, RUNNING_GAP after the last write
            let deadline = if stop {
                None
            } else if let Some(at) = state.retry_at {
                Some(at)
            } else if next.urgent {
                None
            } else if next.running {
                lock(&self.io).last_write.map(|last| last + RUNNING_GAP)
            } else {
                None
            };
            match deadline.and_then(|at| at.checked_duration_since(Instant::now())) {
                Some(wait) if !wait.is_zero() => {
                    // a job due before the write waits for it to run first
                    let wait = match job_wait() {
                        Some(job) if job < wait => {
                            if !job.is_zero() {
                                drop(
                                    self.wake
                                        .wait_timeout(slot, job)
                                        .unwrap_or_else(|p| p.into_inner()),
                                );
                            }
                            return true;
                        }
                        _ => wait,
                    };
                    slot = self
                        .wake
                        .wait_timeout(slot, wait)
                        .unwrap_or_else(|p| p.into_inner())
                        .0;
                }
                _ => break,
            }
        }
        let Some(due) = slot.take() else { return true };
        drop(slot);
        // a copy under the record's lock, serialized outside it: producers never wait for the serializing
        let Some(record) = lock(&self.model).clone() else {
            return true;
        };
        self.write_background(snapshot_of(&record), due, state);
        true
    }

    fn write_background(&self, snapshot: Snapshot, due: Due, state: &mut Background) {
        match self.write_now(&snapshot) {
            Ok(()) => {
                if state.failures > 0 {
                    self.log(
                        KILN_LOG_INFO,
                        &format!(
                            "kiln-sfi: kiln/state/sfi.json written after {} more tries",
                            state.failures
                        ),
                    );
                }
                *state = Background {
                    failures: 0,
                    retry_at: None,
                };
            }
            Err(e) => {
                if state.failures == 0 {
                    self.log(
                        KILN_LOG_WARN,
                        &format!("kiln-sfi: cannot write kiln/state/sfi.json ({e}); trying again"),
                    );
                }
                state.failures += 1;
                if state.failures < BACKGROUND_TRIES {
                    // due again (a change meanwhile is due already: that record is tried instead)
                    let mut slot = lock(&self.due);
                    if slot.is_none() {
                        *slot = Some(due);
                    }
                    state.retry_at = Some(Instant::now() + BACKGROUND_RETRY);
                } else {
                    self.log(
                        KILN_LOG_WARN,
                        "kiln-sfi: kiln/state/sfi.json keeps an older record; the next change or the end writes it again",
                    );
                    *state = Background {
                        failures: 0,
                        retry_at: None,
                    };
                }
            }
        }
    }
}

/// The writer thread's retry state.
struct Background {
    failures: u32,
    retry_at: Option<Instant>,
}

fn snapshot_of(record: &Reducer) -> Snapshot {
    Snapshot {
        seq: record.seq(),
        bytes: record.to_bytes(now_ms()),
    }
}

/// Runs `f`; a panic becomes `fallback` (nothing the writer does may unwind into the core).
fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_older_or_equal_snapshot_never_replaces_the_file() {
        // the writer thread's copy can be older than what end() put on disk meanwhile
        let dir = std::env::temp_dir().join(format!("kiln-sfi-state-seq-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let hub = StateHub::new();
        hub.set_dir(Some(dir.clone()));
        let snapshot = |seq, bytes: &[u8]| Snapshot {
            seq,
            bytes: bytes.to_vec(),
        };
        hub.write_now(&snapshot(2, b"new")).unwrap();
        hub.write_now(&snapshot(1, b"old")).unwrap();
        hub.write_now(&snapshot(2, b"same seq")).unwrap();
        assert_eq!(std::fs::read(dir.join(writer::FILE)).unwrap(), b"new");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
