// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
//! The dedicated server log follower (kiln-sfi's IO thread: the worker never reads a file).
//!
//! ShroudForge reads the whole `<game>/logs/enshrouded_server.log` on every maintenance pass and every
//! `connected_peers` call, on its mods thread. The interpreter's worker never waits for a file, so kiln-sfi follows
//! that one file incrementally on its IO thread instead and hands the worker the result as a coalescing state
//! (`NetworkAuth`). Parity row P-N1: the state is at most about one follower interval plus one maintenance
//! pass older than ShroudForge's per-call scan, and a line over 64 KiB is ignored.
//!
//! Each pass:
//! - opens the file anew with full sharing (read, write and delete on Windows), so the game's writer can append to,
//!   rename or delete it at any time; the handle is closed at the end of the pass;
//! - starts over from byte 0 (an empty state) when the file's identity changed (rotated or re-created), it got
//!   shorter, or its first bytes are not the ones seen before (rewritten in place);
//! - reads at most [`PASS_BYTES`] new bytes and feeds the complete lines to ShroudForge's line state machine
//!   ([`LogAuth`]); the unfinished last line is counted as ShroudForge's `std::getline` would count it, without
//!   being kept;
//! - publishes `{generation, readable, peers}` once it has read everything the file held at the pass's start, and only
//!   when that differs from the last publication. An unreadable file publishes `readable = false` (no peers) and
//!   forgets the state, as ShroudForge's scan finds nothing then.
//!
//! Lines longer than [`LINE_BYTES`] are skipped up to their line break and counted ([`Follower::ignored_lines`]).

use super::authorization::LogAuth;
use crate::state::{IoJob, IoNext};
use crate::worker::{NetworkAuth, WorkerCommand, WorkerShared};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Weak};
use std::time::Duration;

/// New bytes read per pass at most.
pub const PASS_BYTES: usize = 4 << 20;
/// Longer lines are ignored.
pub const LINE_BYTES: usize = 64 << 10;
/// How often the IO thread runs a pass.
pub const INTERVAL: Duration = Duration::from_millis(500);
/// The bytes compared at the file's start to notice a rewrite in place.
const HEAD_BYTES: usize = 64;

/// `<game>/logs/enshrouded_server.log`: the only file the follower reads.
pub fn server_log(game: &Path) -> PathBuf {
    game.join("logs").join("enshrouded_server.log")
}

/// What identifies an open file across passes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileId(u64, u64);

#[cfg(unix)]
fn identity(file: &File) -> Option<FileId> {
    use std::os::unix::fs::MetadataExt;
    let meta = file.metadata().ok()?;
    Some(FileId(meta.dev(), meta.ino()))
}

#[cfg(windows)]
fn identity(file: &File) -> Option<FileId> {
    use std::os::windows::io::AsRawHandle;
    #[repr(C)]
    #[derive(Default)]
    struct Info {
        attributes: u32,
        times: [u32; 6],
        volume: u32,
        size_high: u32,
        size_low: u32,
        links: u32,
        index_high: u32,
        index_low: u32,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetFileInformationByHandle(file: *mut core::ffi::c_void, info: *mut Info) -> i32;
    }
    let mut info = Info::default();
    // SAFETY: a live file handle and a BY_HANDLE_FILE_INFORMATION-sized out record.
    let ok = unsafe { GetFileInformationByHandle(file.as_raw_handle().cast(), &mut info) };
    (ok != 0).then(|| {
        FileId(
            u64::from(info.volume),
            (u64::from(info.index_high) << 32) | u64::from(info.index_low),
        )
    })
}

#[cfg(not(any(unix, windows)))]
fn identity(_: &File) -> Option<FileId> {
    None
}

fn open_shared(path: &Path) -> std::io::Result<File> {
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        // FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE: never in the writer's way.
        options.share_mode(0x1 | 0x2 | 0x4);
    }
    options.open(path)
}

/// The follower of one log file.
#[derive(Debug)]
pub struct Follower {
    path: PathBuf,
    id: Option<FileId>,
    head: Vec<u8>,
    offset: u64,
    state: LogAuth,
    /// The bytes after the last line break (at most [`LINE_BYTES`]).
    partial: Vec<u8>,
    /// Inside a line longer than [`LINE_BYTES`]: skipped up to its line break.
    skipping: bool,
    ignored_lines: u64,
    generation: u64,
    published: Option<(bool, Vec<u64>)>,
    rescans: u64,
}

impl Follower {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            id: None,
            head: Vec::new(),
            offset: 0,
            state: LogAuth::default(),
            partial: Vec::new(),
            skipping: false,
            ignored_lines: 0,
            generation: 0,
            published: None,
            rescans: 0,
        }
    }
    /// Lines skipped for their length.
    pub fn ignored_lines(&self) -> u64 {
        self.ignored_lines
    }
    /// How often it started over from byte 0 (the first read counts).
    pub fn rescans(&self) -> u64 {
        self.rescans
    }

    fn reset(&mut self) {
        self.id = None;
        self.head.clear();
        self.offset = 0;
        self.state = LogAuth::default();
        self.partial.clear();
        self.skipping = false;
    }

    /// The state ShroudForge's scan would find in what was read so far (the unfinished line included).
    pub fn peers(&self) -> Vec<u64> {
        if self.partial.is_empty() || self.skipping {
            return self.state.peers();
        }
        let mut state = self.state.clone();
        state.apply(&self.partial);
        state.peers()
    }

    /// One pass: the new state to publish, if it changed.
    pub fn pass(&mut self) -> Option<NetworkAuth> {
        let peers = match self.read() {
            Some(true) => Some(self.peers()),
            Some(false) => return None,
            None => {
                self.reset();
                None
            }
        };
        let now = (peers.is_some(), peers.unwrap_or_default());
        if self.published.as_ref() == Some(&now) {
            return None;
        }
        self.generation += 1;
        self.published = Some(now.clone());
        Some(NetworkAuth {
            generation: self.generation,
            readable: now.0,
            peers: now.1,
        })
    }

    /// Reads what is new. `None`: unreadable; `Some(caught_up)`.
    fn read(&mut self) -> Option<bool> {
        let mut file = open_shared(&self.path).ok()?;
        let size = file.metadata().ok()?.len();
        let id = identity(&file);
        let mut head = vec![0u8; HEAD_BYTES.min(size as usize)];
        file.read_exact(&mut head).ok()?;
        let same_head = head.len() >= self.head.len() && head[..self.head.len()] == self.head[..];
        if self.id.is_none() || id != self.id || size < self.offset || !same_head {
            self.reset();
            self.rescans += 1;
            self.id = id;
        }
        if self.head.len() < head.len() {
            self.head = head;
        }
        let want = (size - self.offset).min(PASS_BYTES as u64) as usize;
        let mut bytes = Vec::with_capacity(want);
        file.seek(SeekFrom::Start(self.offset)).ok()?;
        file.take(want as u64).read_to_end(&mut bytes).ok()?;
        self.offset += bytes.len() as u64;
        self.feed(&bytes);
        Some(self.offset >= size)
    }

    fn feed(&mut self, bytes: &[u8]) {
        let mut rest = bytes;
        while let Some(at) = rest.iter().position(|b| *b == b'\n') {
            let (piece, tail) = (&rest[..at], &rest[at + 1..]);
            rest = tail;
            if self.skipping {
                self.skipping = false;
                self.ignored_lines += 1;
                continue;
            }
            if self.partial.len() + piece.len() > LINE_BYTES {
                self.partial.clear();
                self.ignored_lines += 1;
                continue;
            }
            let line = if self.partial.is_empty() {
                piece.to_vec()
            } else {
                let mut line = std::mem::take(&mut self.partial);
                line.extend_from_slice(piece);
                line
            };
            let line = line.strip_suffix(b"\r").unwrap_or(&line);
            self.state.apply(line);
        }
        if self.skipping {
            return;
        }
        if self.partial.len() + rest.len() > LINE_BYTES {
            self.partial.clear();
            self.skipping = true;
        } else {
            self.partial.extend_from_slice(rest);
        }
    }
}

/// The follower as a job of kiln-sfi's IO thread: a pass every [`INTERVAL`] while `active` (the network is enabled
/// on a dedicated server), each new state handed to the worker's inbox. It ends with the worker.
pub struct FollowJob {
    follower: Follower,
    active: Arc<AtomicBool>,
    worker: Weak<WorkerShared>,
}
impl FollowJob {
    pub fn new(follower: Follower, active: Arc<AtomicBool>, worker: Weak<WorkerShared>) -> Self {
        Self {
            follower,
            active,
            worker,
        }
    }
}
impl IoJob for FollowJob {
    fn run(&mut self) -> IoNext {
        let Some(worker) = self.worker.upgrade() else {
            return IoNext::Done;
        };
        if worker.phase().terminal() || worker.stop_requested() {
            return IoNext::Done;
        }
        if self.active.load(Ordering::Acquire)
            && let Some(auth) = self.follower.pass()
        {
            // A state class: never refused while the worker runs, and never waited for.
            let _ = worker.submit(WorkerCommand::NetworkAuth(auth));
        }
        IoNext::After(INTERVAL)
    }
}
