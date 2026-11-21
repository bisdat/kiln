// SPDX-License-Identifier: MIT
//! The file side of `<game>/kiln/state/sfi.json`: one atomic replace per write, and the read of the file a start
//! replaces.
//!
//! **Write.** Each try writes a temp file with a name no other write uses (`sfi.json.<pid>-<n>.tmp`, created new),
//! flushes it to disk, then renames it over `sfi.json`. A failed try removes its temp file and leaves the old
//! `sfi.json` whole. On Windows (and under Wine) the replacing rename is refused while any other handle on the old
//! file is open, a launcher's read included, so a write is tried up to [`ATTEMPTS`] times, [`WAIT`] apart: about
//! half a second, as the core's `kiln/state/launch.json` writer does (native/core/src/launch.h `write_retrying`,
//! 20 × 25 ms; 1000 of 1000 native writes went through it when measured). Nothing here panics: every failure is an
//! `io::Error` for the caller to log.
//!
//! **Read.** The file this start replaces says how the last start ended: `clean` (it wrote its end), `incomplete` (it
//! never did: it died), `suspect` (unreadable, malformed, too big, or written by this very pid: a reused pid), or
//! `none`. Nothing is concluded from a suspect file.

use super::reducer::{self, Disposition, PreviousStart};
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

pub const FILE: &str = "sfi.json";
/// The core's state-file retry (launch.h `kStateWriteAttempts`, `kStateWriteWaitMs`).
pub const ATTEMPTS: u32 = 20;
pub const WAIT: Duration = Duration::from_millis(25);
/// The largest previous file read (4096 rows of the schema's bounds fit easily).
pub const MAX_PREVIOUS: u64 = 32 << 20;

/// The two operations a test replaces: the replacing rename and the wait between tries.
#[derive(Clone, Copy)]
pub struct WriteOps {
    pub rename: fn(&Path, &Path) -> io::Result<()>,
    pub sleep: fn(Duration),
}
impl WriteOps {
    pub const REAL: Self = Self {
        rename: real_rename,
        sleep: std::thread::sleep,
    };
}
fn real_rename(from: &Path, to: &Path) -> io::Result<()> {
    fs::rename(from, to)
}

static TEMPS: AtomicU64 = AtomicU64::new(0);

/// A temp file name in `dir` no other write of this process uses, and no other process's (its pid is in it).
fn temp_path(dir: &Path) -> PathBuf {
    let n = TEMPS.fetch_add(1, Ordering::Relaxed);
    dir.join(format!("{FILE}.{}-{n}.tmp", std::process::id()))
}

/// One try: unique temp, write, flush to disk, rename over `sfi.json`. The temp file never outlives a failed try.
pub fn write_once(dir: &Path, bytes: &[u8], ops: &WriteOps) -> io::Result<()> {
    let tmp = temp_path(dir);
    let result = (|| {
        let mut file = OpenOptions::new().write(true).create_new(true).open(&tmp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        (ops.rename)(&tmp, &dir.join(FILE))
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

/// [`write_once`] up to [`ATTEMPTS`] times, [`WAIT`] apart. `Ok(tries)` or the last try's error. Creates `dir` when
/// it is missing (the core creates kiln/state at boot; an older core's fallback folder may not exist yet).
pub fn write_retrying(dir: &Path, bytes: &[u8], ops: &WriteOps) -> Result<u32, io::Error> {
    let _ = fs::create_dir_all(dir);
    let mut last = None;
    for attempt in 1..=ATTEMPTS {
        match write_once(dir, bytes, ops) {
            Ok(()) => return Ok(attempt),
            Err(error) => last = Some(error),
        }
        if attempt < ATTEMPTS {
            (ops.sleep)(WAIT);
        }
    }
    Err(last.unwrap_or_else(|| io::Error::other("no write was tried")))
}

/// Removes temp files earlier processes left behind (a write that died between create and rename). This process's
/// own are never touched: they may be in flight.
pub fn sweep_temps(dir: &Path) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    let mine = format!("{FILE}.{}-", std::process::id());
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Some(name) = name.to_str() else { continue };
        if name.starts_with(&format!("{FILE}."))
            && name.ends_with(".tmp")
            && !name.starts_with(&mine)
        {
            let _ = fs::remove_file(entry.path());
        }
    }
}

/// A save-sensitive package that was active at the previous kiln start, as its sfi.json listed it: in `packages` for a
/// kiln record, in `carriedSaveSensitive` for an aside or refused one, which hands the list on unchanged. A
/// refused record's own active `packages` count too (a start can be refused at on_ready after its startup data was
/// committed); each id once, the record's own row first.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PreviouslyActive {
    pub id: String,
    pub version: Option<String>,
    /// Display only; a carried entry has none.
    pub name: Option<String>,
    pub basis: String,
    pub policy_row: Option<(String, String)>,
    /// The kiln start it was active in (its `launch.launchId`); `None` when that id could not be read.
    pub from_launch_id: Option<u64>,
}

/// What the replaced file said.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Previous {
    pub start: PreviousStart,
    /// Save-sensitive packages it listed as selected and active (runtime running or data in the game), or, for an
    /// aside or refused record, the list it carried forward from the start before it; empty for a suspect file.
    pub save_sensitive_active: Vec<PreviouslyActive>,
}

/// Reads `<dir>/sfi.json` before this start replaces it. `pid` is this process's.
pub fn read_previous(dir: &Path, pid: u32) -> Previous {
    let suspect = |launch_id, pid| Previous {
        start: PreviousStart {
            disposition: Disposition::Suspect,
            launch_id,
            pid,
        },
        save_sensitive_active: Vec::new(),
    };
    let file = match fs::File::open(dir.join(FILE)) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            return Previous {
                start: PreviousStart::NONE,
                save_sensitive_active: Vec::new(),
            };
        }
        Err(_) => return suspect(None, None),
    };
    let mut bytes = Vec::new();
    if file.take(MAX_PREVIOUS + 1).read_to_end(&mut bytes).is_err()
        || bytes.len() as u64 > MAX_PREVIOUS
    {
        return suspect(None, None);
    }
    let Ok(doc) = serde_json::from_slice::<serde_json::Value>(&bytes) else {
        return suspect(None, None);
    };
    let launch_id = doc["launch"]["launchId"].as_str().and_then(parse_launch_id);
    let their_pid = doc["launch"]["pid"]
        .as_u64()
        .and_then(|p| u32::try_from(p).ok());
    if doc["format"].as_u64() != Some(1) || !doc["packages"].is_array() || their_pid == Some(pid) {
        return suspect(launch_id, their_pid);
    }
    let disposition = match doc["incomplete"].as_bool() {
        Some(false) => Disposition::Clean,
        Some(true) => Disposition::Incomplete,
        None => return suspect(launch_id, their_pid),
    };
    let mut save_sensitive_active: Vec<PreviouslyActive> = doc["packages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|p| {
            p["intent"] == "selected"
                && p["saveSensitive"]["value"] == true
                && (p["runtimeActive"] == true || p["archivePresent"] == true)
        })
        .filter_map(|p| {
            Some(PreviouslyActive {
                id: p["id"].as_str()?.to_owned(),
                version: p["version"].as_str().map(str::to_owned),
                name: p["name"].as_str().map(str::to_owned),
                basis: p["saveSensitive"]["basis"]
                    .as_str()
                    .unwrap_or("unknown-mixed")
                    .to_owned(),
                policy_row: p["saveSensitive"]["policyRow"].as_object().and_then(|row| {
                    Some((
                        row.get("id")?.as_str()?.to_owned(),
                        row.get("version")?.as_str()?.to_owned(),
                    ))
                }),
                from_launch_id: launch_id,
            })
        })
        .collect();
    // An aside or refused start ran no package of its own from the previous list: its record carries that list
    // forward, so kiln -> hosted -> kiln still knows what was active. Bounded and clamped as it is written.
    if matches!(doc["phase"].as_str(), Some("aside" | "refused")) {
        let carried = doc["carriedSaveSensitive"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|c| {
                Some(PreviouslyActive {
                    id: c["id"].as_str()?.to_owned(),
                    version: c["version"].as_str().map(str::to_owned),
                    name: None,
                    basis: c["basis"].as_str().unwrap_or("unknown-mixed").to_owned(),
                    policy_row: c["policyRow"].as_object().and_then(|row| {
                        Some((
                            row.get("id")?.as_str()?.to_owned(),
                            row.get("version")?.as_str()?.to_owned(),
                        ))
                    }),
                    from_launch_id: c["fromLaunchId"].as_str().and_then(parse_launch_id),
                })
            });
        let all = std::mem::take(&mut save_sensitive_active)
            .into_iter()
            .chain(carried);
        save_sensitive_active = reducer::carried(all);
    }
    Previous {
        start: PreviousStart {
            disposition,
            launch_id,
            pid: their_pid,
        },
        save_sensitive_active,
    }
}

/// The 16 lower-case hex digits launch.json and sfi.json write.
pub fn parse_launch_id(text: &str) -> Option<u64> {
    (text.len() == 16
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)))
    .then(|| u64::from_str_radix(text, 16).ok())
    .flatten()
}
