// SPDX-License-Identifier: MIT
// the interpreter's live reload/unload transport.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Both pins (the files are identical in 1.7.9 and 1.8.0):
// modules/mod-runtime/ingame/process_runtime_reload_request.rs:5-46 (the request read; a file that cannot
// be read or parsed is ignored without a result), modules/loader-config/write_json_atomically.rs:4-25 (the
// result's atomic replace) and modules/loader-config/resolve_installation_paths.rs:114-120 (runtime_dir).
//! The host side of ShroudForge's live reload/unload transport in kiln mode.
//!
//! **Files.** ShroudForge's own documents, in Kiln's folder: `<game>/kiln/sfi/runtime/mod-reload-request.json`
//! (`{"requestId", "operation", "modIds"}`, written by the requester) and `mod-reload-result.json`
//! (`{"requestId", "status", "message"}`, written here). Nothing below `<game>/shroudforge` is touched.
//!
//! **Threads.**
//! - kiln-sfi's IO thread polls the request every [`POLL`] ([`PollJob`]): at most [`REQUEST_MAX`] bytes, a
//!   file that cannot be read, is larger, or does not parse as JSON is ignored silently (as ShroudForge ignores it
//!   at its update). A request whose bytes differ from the last one handed over goes to the worker's inbox as a
//!   [`ReloadRequest`] (a state class: only the latest counts). The worker never waits for the file.
//! - The worker takes it at the start of its next turn (never inside a hop wait), deduplicates on `requestId`
//!   with sfi-core's `ReloadProtocol` (the token survives the runtime's replacement) and runs
//!   `LifecycleRuntime::{unload, reload}` (lifecycle/mod.rs). No world or tick thread is involved.
//! - The result goes back to the IO thread ([`Transport::publish`]): one atomic replace with ShroudForge's
//!   retries, best effort, never tried again.
//!
//! **What a reload may do**: re-read and re-run the Lua of the
//! packages admitted when the game started that have no startup data and no DLLs, after their folder's content
//! digest is checked again against the launch plan (a changed package is refused). It never
//! admits a package the start did not admit, re-reads the plan, the rules or the profile, or loads, unloads or
//! retries a DLL; such a request is answered with ShroudForge's `updated runtime mod '<id>' failed to activate:
//! <reason>`, decided before anything is unloaded, so the running packages are left as they are.
//! ShroudForge's own refusals (a failed rediscovery or initialization) answer after the unload and restore, as
//! ShroudForge orders them. It never runs while kiln-sfi is stopping or at exit.

use crate::state::{IoJob, IoNext, StateHub};
use crate::worker::{ReloadRequest, WorkerCommand, WorkerShared};
use serde_json::Value;
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Weak};
use std::time::Duration;

/// The request ShroudForge's requester writes (process_runtime_reload_request.rs:6-7).
pub const REQUEST_FILE: &str = "mod-reload-request.json";
/// The result written back (process_runtime_reload_request.rs:31-32).
pub const RESULT_FILE: &str = "mod-reload-result.json";
/// How often the IO thread reads the request. ShroudForge reads it at every update of its mods thread; its
/// requester polls the result every 100 ms for up to 30 s.
pub const POLL: Duration = Duration::from_millis(100);
/// The largest request read. ShroudForge reads the whole file; a larger file is ignored here.
pub const REQUEST_MAX: u64 = 64 * 1024;

/// The reason a requested package that did not run at the start cannot be activated by a reload.
pub const NOT_ADMITTED: &str = "it was not admitted when the game started, and a live reload never adds a package; restart the game to run it";
/// The reason a package with startup data cannot be reloaded.
pub const RESTART_DATA: &str = "it ships startup data, which is applied only when the game starts; restart the game to reload it";
/// The reason a package with DLLs cannot be reloaded.
pub const RESTART_NATIVE: &str = "it ships native code (DLLs), which is loaded only when the game starts and never again; restart the game to reload it";
/// sfi-core's answer while the runtime stops (lifecycle/reload.rs).
pub const STOPPING: &str = "interpreter is stopping";

/// `<game>/kiln/sfi/runtime`: Kiln's own runtime folder for the interpreter.
pub fn runtime_dir(game: &Path) -> PathBuf {
    game.join("kiln").join("sfi").join("runtime")
}

/// ShroudForge's refusal of a reload whose package did not come back (reload_runtime_packages.rs:73-75).
pub fn failed_to_activate(id: &str, reason: &str) -> String {
    format!("updated runtime mod '{id}' failed to activate: {reason}")
}

/// Where a runtime's reload files live and the IO thread that reads and writes them.
#[derive(Clone)]
pub struct Transport {
    /// The folder of [`REQUEST_FILE`] and [`RESULT_FILE`].
    pub dir: PathBuf,
    pub io: Arc<StateHub>,
}
impl Transport {
    /// Starts the request poll on the IO thread for `worker`; false when there is no IO thread.
    pub fn start_poll(&self, worker: &Arc<WorkerShared>) -> bool {
        self.io.add_io_job(Box::new(PollJob::new(
            self.dir.join(REQUEST_FILE),
            Arc::downgrade(worker),
        )))
    }
    /// Hands the result document to the IO thread (never waits for the file); false when there is no IO thread.
    pub fn publish(&self, value: &Value) -> bool {
        self.io.add_io_job(Box::new(PublishJob {
            dir: self.dir.clone(),
            bytes: result_bytes(value),
        }))
    }
}

/// The request at `path` as ShroudForge would see it: `None` when it cannot be read, is larger than
/// [`REQUEST_MAX`] or is not JSON. The bytes come with it. Opened with Rust's default sharing, so a requester can
/// replace or delete the file while it is read.
pub fn read_request(path: &Path) -> Option<(Vec<u8>, Value)> {
    let file = fs::File::open(path).ok()?;
    let mut bytes = Vec::new();
    file.take(REQUEST_MAX + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > REQUEST_MAX {
        return None;
    }
    let value = serde_json::from_slice(&bytes).ok()?;
    Some((bytes, value))
}

/// The result file's bytes: ShroudForge's `serde_json::to_vec_pretty` with its key order
/// (write_json_atomically.rs:9, serde_json `preserve_order`).
pub fn result_bytes(value: &Value) -> Vec<u8> {
    serde_json::to_vec_pretty(value).unwrap_or_default()
}

static TEMPS: AtomicU64 = AtomicU64::new(0);

/// ShroudForge's atomic JSON write (write_json_atomically.rs:4-25): the folder is created, a temp file in it
/// written and flushed to disk, then renamed over [`RESULT_FILE`], tried again 4 more times 10, 20, 40 and 80 ms
/// apart while the rename is refused (a reader that holds the old file without delete sharing). The temp file
/// never outlives a failed write.
pub fn write_result(dir: &Path, bytes: &[u8]) -> io::Result<()> {
    fs::create_dir_all(dir)?;
    let temp = dir.join(format!(
        ".{RESULT_FILE}.{}-{}.tmp",
        std::process::id(),
        TEMPS.fetch_add(1, Ordering::Relaxed)
    ));
    let target = dir.join(RESULT_FILE);
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        for attempt in 0..5u32 {
            match fs::rename(&temp, &target) {
                Ok(()) => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::PermissionDenied && attempt < 4 => {
                    std::thread::sleep(Duration::from_millis(10 << attempt));
                }
                Err(error) => return Err(error),
            }
        }
        Err(io::Error::other("state file replacement retries exhausted"))
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

/// The request poll on kiln-sfi's IO thread. It ends with the worker.
pub struct PollJob {
    path: PathBuf,
    worker: Weak<WorkerShared>,
    /// The bytes of the last request handed to the worker.
    last: Option<Vec<u8>>,
}
impl PollJob {
    pub fn new(path: PathBuf, worker: Weak<WorkerShared>) -> Self {
        Self {
            path,
            worker,
            last: None,
        }
    }
}
impl IoJob for PollJob {
    fn run(&mut self) -> IoNext {
        let Some(worker) = self.worker.upgrade() else {
            return IoNext::Done;
        };
        if worker.phase().terminal() || worker.stop_requested() {
            return IoNext::Done;
        }
        if let Some((bytes, request)) = read_request(&self.path)
            && self.last.as_deref() != Some(bytes.as_slice())
            // a state class: never refused while the worker runs, and never waited for
            && worker
                .submit(WorkerCommand::ReloadRequest(ReloadRequest { request }))
                .is_ok()
        {
            self.last = Some(bytes);
        }
        IoNext::After(POLL)
    }
}

/// One result write on the IO thread (ShroudForge's `let _ = write_json(..)`: best effort, no retry of the job).
struct PublishJob {
    dir: PathBuf,
    bytes: Vec<u8>,
}
impl IoJob for PublishJob {
    fn run(&mut self) -> IoNext {
        let _ = write_result(&self.dir, &self.bytes);
        IoNext::Done
    }
}
