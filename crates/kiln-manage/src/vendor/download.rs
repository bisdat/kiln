//! Downloading a component into its version folder, checked file by file.
//!
//! - Only the manifest's URLs are fetched, through [`crate::net::Client`] with [`policy`]: https on port 443, public
//!   addresses only, no proxy, no cookie, no credential, a User-Agent naming Kiln and its version, `Accept-Encoding:
//!   identity` (the file's own bytes, never a compressed answer), `Range` when it goes on, and nothing else of Kiln's
//!   (the transport adds `Accept: */*`).
//!   A redirect is followed once, only where [`super::manifest::redirect_hosts`] allows it (a release asset to GitHub's
//!   asset storage); a raw file is never redirected.
//! - Each file goes to `<name>.part` in the version folder, never more than the manifest's size (a longer answer
//!   stops the download), and is hashed as it arrives. It becomes `<name>` only when its size and SHA-256 are the
//!   manifest's; a file that differs is deleted and the download fails, saying so.
//! - Cancel (the task's) stops between two writes and keeps the `.part` file; the next download goes on from it with
//!   a range request. When the answer to that is not the rest of the same file (a 200, a 416, another range, a body
//!   that does not verify), the part is deleted and the file is fetched again from its start, once.
//! - A file already in the folder with the manifest's size and SHA-256 is kept (an interrupted component goes on
//!   with the next file).
//! - Last, the receipt (atomically). Then, for an update, the earlier versions of the component go: only after the
//!   new one verified, so a failed update keeps the old one.
//!
//! Time bounds are the transport's: the launcher's real transport ([`crate::net::UreqTransport::new`]) gives up after
//! 10 s without a connection and 60 s without a byte.

use super::manifest::{redirect_hosts, Component, VendorFile, HOSTS};
use super::store::{Receipt, ReceiptFile, ReceiptLicense, Store, PART_SUFFIX, RECEIPT, RECEIPT_FORMAT};
use crate::net::{Client, Policy, Request, Response};
use crate::task::Progress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use sha2::Digest;
use std::io::{Read, Seek, Write};
use std::path::Path;
use std::time::{Duration, Instant};

/// Every host a vendor download may reach (the manifest's hosts and the redirect targets), none with a credential.
pub fn policy() -> Policy {
    let mut p = Policy::new("vendor downloads");
    for (host, redirects) in HOSTS {
        p = p.plain_host(host);
        for r in redirects {
            if p.rule(r).is_none() {
                p = p.plain_host(r);
            }
        }
    }
    p
}

/// `kiln/<version>`: the tool and its version, nothing about the player or the PC (the same as the update check's).
pub fn user_agent() -> String {
    format!("kiln/{}", crate::VERSION)
}

/// What [`download`] did.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DownloadReport {
    pub receipt: Receipt,
    /// The folder, relative to the game folder.
    pub folder: String,
    /// Bytes fetched in this run (0 when every file was there already).
    pub fetched: u64,
    /// Files kept because they were there already, verified.
    pub kept: Vec<String>,
    /// Earlier versions removed after this one verified.
    pub replaced: Vec<String>,
    /// What could not be cleaned up (an earlier version's folder that stays), one sentence each.
    pub warnings: Vec<String>,
}

/// The vendor's name for messages ("AMD's server"): the component's vendorName.
fn from(c: &Component, host: &str) -> String {
    format!("{} ({host})", c.vendor_name)
}

/// Downloads every file of `c` into its version folder (see the module), writes the receipt, and removes earlier
/// versions of `c`. `installer` goes into the receipt (`kiln-launcher <version>`). The caller checked that the game
/// does not run and that the player agreed to the licence.
pub fn download(store: &Store, c: &Component, client: &Client, installer: &str, progress: &mut dyn Progress) -> Result<DownloadReport> {
    if !c.pinned() {
        return Err(Error::new(ErrorCode::Refused, format!("{} is not available in this build of Kiln: its files' checksums are not pinned yet. Nothing was downloaded.", c.name)));
    }
    let _lock = store.lock()?;
    let folder = store.folder(&c.vendor, &c.version);
    store.guard(&folder)?;
    check_space(&folder, c)?;
    std::fs::create_dir_all(&folder).map_err(crate::util::io_at(&folder, "creating"))?;
    store.guard(&folder)?;
    // a receipt goes first: the folder is not complete while its files change
    let receipt_path = folder.join(RECEIPT);
    if std::fs::symlink_metadata(&receipt_path).is_ok() {
        std::fs::remove_file(&receipt_path).map_err(crate::util::io_at(&receipt_path, "removing"))?;
    }
    // what is there already and right stays; anything else under a file's name goes
    let mut kept = Vec::new();
    let mut todo: Vec<&VendorFile> = Vec::new();
    for f in &c.files {
        let p = folder.join(&f.name);
        match std::fs::symlink_metadata(&p) {
            Ok(m) if m.is_file() && m.len() == f.size => {
                let (sha, _) = crate::hash::sha256_file(&p).map_err(|e| Error::new(e.code, format!("checking {}: {}", p.display(), e.message)))?;
                if sha == f.sha256 {
                    kept.push(f.name.clone());
                    continue;
                }
                remove_plain(&p)?;
            }
            Ok(m) if m.is_file() => remove_plain(&p)?,
            Ok(_) => return Err(Error::new(ErrorCode::Refused, format!("{} is not a plain file. Remove it, then try again.", p.display()))),
            Err(_) => {}
        }
        todo.push(f);
    }
    let total: u64 = todo.iter().map(|f| f.size).sum();
    let mut done = 0u64;
    let mut fetched = 0u64;
    for f in &todo {
        if progress.canceled() {
            return Err(canceled());
        }
        fetched += fetch_file(&folder, c, f, client, Meter { done, total }, progress)?;
        done += f.size;
    }
    progress.report(Some(1.0), &format!("{} is checked", c.name));
    let receipt = Receipt {
        format: RECEIPT_FORMAT,
        component: c.id.clone(),
        vendor: c.vendor.clone(),
        version: c.version.clone(),
        installed: crate::util::now_rfc3339(),
        installer: Some(installer.to_string()),
        license: ReceiptLicense { name: c.license.name.clone(), sha256: c.license.sha256.clone() },
        files: c.files.iter().map(|f| ReceiptFile { name: f.name.clone(), size: f.size, sha256: f.sha256.clone(), url: f.url.clone() }).collect(),
    };
    crate::util::write_json_atomic(&receipt_path, &receipt)?;
    // the earlier versions go only now that this one is complete
    let (replaced, warnings) = match store.remove_locked(c, Some(&c.version)) {
        Ok(r) => (r.removed, r.kept),
        Err(e) => (Vec::new(), vec![format!("The earlier version of {} stays in the game folder: {}", c.name, e.message)]),
    };
    Ok(DownloadReport { receipt, folder: Store::rel(&c.vendor, &c.version), fetched, kept, replaced, warnings })
}

thread_local! {
    static FREE: std::cell::Cell<Option<Option<u64>>> = const { std::cell::Cell::new(None) };
}

/// Runs `f` with the game disk's free space answered as `free` (None: not known), for tests of the disk check.
#[doc(hidden)]
pub fn with_free_space_for_tests<R>(free: Option<u64>, f: impl FnOnce() -> R) -> R {
    let before = FREE.with(|c| c.replace(Some(free)));
    let out = f();
    FREE.with(|c| c.set(before));
    out
}

/// Refuses a download the game's disk has no room for, before anything is written or fetched: the bytes still to
/// come (a file there with the manifest's size counts as there, a part file as arrived) need the data folder's
/// margin on top ([`crate::data_folder::needed_bytes`]: 256 MiB and 5 %), so a download never fills the disk the
/// game saves to. A disk whose free space is not known is not refused.
fn check_space(folder: &Path, c: &Component) -> Result<()> {
    let len = |p: &Path| std::fs::symlink_metadata(p).ok().filter(|m| m.is_file()).map(|m| m.len());
    let (mut bytes, mut files) = (0u64, 0u64);
    for f in &c.files {
        if len(&folder.join(&f.name)) == Some(f.size) {
            continue;
        }
        let part = len(&folder.join(format!("{}{PART_SUFFIX}", f.name))).filter(|n| *n < f.size).unwrap_or(0);
        bytes += f.size - part;
        files += 1;
    }
    if bytes == 0 {
        return Ok(());
    }
    let space = match FREE.with(|c| c.get()) {
        Some(free) => free.map(|free| crate::data_folder::Space { free, unit: 4096 }),
        None => folder.ancestors().find(|p| p.is_dir()).and_then(crate::data_folder::space_at),
    };
    let Some(s) = space else { return Ok(()) };
    let need = crate::data_folder::needed_bytes(bytes, files, s.unit);
    if s.free < need {
        return Err(Error::new(ErrorCode::Refused, format!(
            "The game's disk has {} free. {} needs {} there, with room to spare so the disk does not fill up. Free some space, then try again. Nothing was downloaded.",
            crate::util::human_size(s.free), c.name, crate::util::human_size(need))));
    }
    Ok(())
}

fn canceled() -> Error {
    Error::new(ErrorCode::Canceled, "The download was canceled. What arrived is kept, so the next download goes on from there.")
}

fn remove_plain(p: &Path) -> Result<()> {
    std::fs::remove_file(p).map_err(crate::util::io_at(p, "removing"))
}

#[derive(Clone, Copy)]
struct Meter {
    /// Bytes of the files before this one.
    done: u64,
    /// Bytes of every file this run fetches.
    total: u64,
}

/// The sink of one file: writes to the part file, hashes, reports, and stops when the task is canceled.
struct Sink<'p> {
    file: std::fs::File,
    hasher: sha2::Sha256,
    /// Bytes in the part file (what was there plus what arrived).
    have: u64,
    size: u64,
    meter: Meter,
    name: String,
    progress: &'p mut dyn Progress,
    canceled: bool,
    last: Instant,
}

impl Write for Sink<'_> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        if self.progress.canceled() {
            self.canceled = true;
            return Err(std::io::Error::other("canceled"));
        }
        self.file.write_all(buf)?;
        self.hasher.update(buf);
        self.have += buf.len() as u64;
        if self.last.elapsed() >= Duration::from_millis(100) {
            self.last = Instant::now();
            let at = self.meter.done + self.have.min(self.size);
            let fraction = if self.meter.total == 0 { None } else { Some((at as f64 / self.meter.total as f64) as f32) };
            let msg = format!("Downloading {}: {} of {}", self.name, crate::util::human_size(self.have), crate::util::human_size(self.size));
            self.progress.report(fraction, &msg);
        }
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.file.flush()
    }
}

/// What one request for a file gave.
enum Outcome {
    /// The file is whole in the part file and verified.
    Verified,
    /// Start again from byte 0 (a resume that did not continue the same file).
    Restart,
}

/// Fetches one file into `<folder>/<name>.part`, verifies it and renames it. Returns the bytes fetched.
fn fetch_file(folder: &Path, c: &Component, f: &VendorFile, client: &Client, meter: Meter, progress: &mut dyn Progress) -> Result<u64> {
    let part = folder.join(format!("{}{PART_SUFFIX}", f.name));
    let dest = folder.join(&f.name);
    let mut fetched = 0u64;
    for attempt in 0..2 {
        match std::fs::symlink_metadata(&part) {
            Ok(m) if !m.is_file() => return Err(Error::new(ErrorCode::Refused, format!("{} is not a plain file. Remove it, then try again.", part.display()))),
            Ok(m) if attempt > 0 || m.len() >= f.size => remove_plain(&part)?,
            _ => {}
        }
        let (outcome, n) = fetch_once(&part, c, f, client, meter, progress)?;
        fetched += n;
        match outcome {
            Outcome::Verified => {
                crate::util::rename_replace(&part, &dest)?;
                return Ok(fetched);
            }
            Outcome::Restart => continue,
        }
    }
    let _ = std::fs::remove_file(&part);
    Err(mismatch(c, f))
}

fn mismatch(c: &Component, f: &VendorFile) -> Error {
    let host = crate::net::policy::parse_url(&f.url).map(|u| u.host).unwrap_or_default();
    Error::new(ErrorCode::HashMismatch, format!(
        "{} from {} is not the file Kiln expects: its size or SHA-256 differs from the one this build of Kiln pinned. Kiln deleted it. \
         Try again later; if it happens again, the file changed at {} and Kiln needs an update.", f.name, from(c, &host), c.vendor_name))
        .with_detail(serde_json::json!({ "file": f.name, "url": f.url, "size": f.size, "sha256": f.sha256 }))
}

/// One request: from byte 0, or the rest of the part file with a range request. Hashes what is there first.
fn fetch_once(part: &Path, c: &Component, f: &VendorFile, client: &Client, meter: Meter, progress: &mut dyn Progress) -> Result<(Outcome, u64)> {
    let mut file = std::fs::OpenOptions::new().read(true).append(true).create(true).open(part).map_err(crate::util::io_at(part, "writing"))?;
    let mut hasher = sha2::Sha256::new();
    let mut have = 0u64;
    {
        file.seek(std::io::SeekFrom::Start(0)).map_err(crate::util::io_at(part, "reading"))?;
        let mut buf = vec![0u8; 256 * 1024];
        loop {
            let n = file.read(&mut buf).map_err(crate::util::io_at(part, "reading"))?;
            if n == 0 {
                break;
            }
            hasher.update(&buf[..n]);
            have += n as u64;
        }
    }
    let start = have;
    let url = crate::net::policy::parse_url(&f.url)?;
    // identity: the bytes on the wire are the file's, so the size bound, the hash and a range all count the same
    // bytes (the real transport would otherwise ask for gzip, and a range of a compressed answer is not the file's)
    let mut req = Request::get(&f.url).header("Accept-Encoding", "identity").max_response(f.size - start).allow_redirect_to(redirect_hosts(&url.host));
    if start > 0 {
        req = req.header("Range", &format!("bytes={start}-"));
    }
    let mut sink = Sink { file, hasher, have, size: f.size, meter, name: f.name.clone(), progress, canceled: false, last: Instant::now() - Duration::from_secs(1) };
    let answer = client.download(req, &mut sink);
    let fetched = sink.have - start;
    // a part file this request made and left empty (a refusal, an error answer) is not kept
    let drop_empty = |p: &Path| {
        if std::fs::symlink_metadata(p).is_ok_and(|m| m.is_file() && m.len() == 0) {
            let _ = std::fs::remove_file(p);
        }
    };
    if sink.canceled {
        let _ = sink.file.sync_all();
        return Err(canceled());
    }
    let r: Response = match answer {
        Ok(r) => r,
        // more than the file's size: not the file (a resume answered with the whole file lands here too)
        Err(e) if crate::net::is_too_large(&e) => {
            drop(sink);
            remove_plain(part)?;
            if start > 0 {
                return Ok((Outcome::Restart, fetched));
            }
            return Err(Error::new(ErrorCode::HashMismatch, format!(
                "{} from {} is larger than the {} this build of Kiln expects. Kiln deleted what arrived.", f.name, from(c, &url.host), crate::util::human_size(f.size))));
        }
        Err(e) if e.code == ErrorCode::Refused => {
            drop(sink);
            drop_empty(part);
            return Err(Error::new(ErrorCode::Refused, format!("{} Nothing more was downloaded.", e.message)));
        }
        // the part file could not be written (a full disk, a folder made read-only): the disk, not the network
        Err(e) if e.code == ErrorCode::Io => {
            drop(sink);
            drop_empty(part);
            return Err(Error::new(ErrorCode::Io, format!(
                "Kiln could not write {} into {}. Check the game's disk has room and can be written to, then try again. What arrived is kept.",
                f.name, folder_of(part))));
        }
        Err(e) => {
            let _ = sink.file.sync_all();
            drop(sink);
            drop_empty(part);
            return Err(Error::new(ErrorCode::Network, format!(
                "The download of {} from {} stopped ({}). What arrived is kept: Try again goes on from there.", f.name, from(c, &url.host), e.message.trim_end_matches('.'))));
        }
    };
    match (r.status, start) {
        (200, 0) => {}
        (206, s) if s > 0 && content_range_is(&r, s, f.size) => {}
        // a resume the server did not continue (whole file, unsatisfiable, another range): from the start
        (200 | 206 | 416, s) if s > 0 => {
            drop(sink);
            remove_plain(part)?;
            return Ok((Outcome::Restart, fetched));
        }
        (status, _) => {
            drop(sink);
            drop_empty(part);
            let what = match status {
                404 | 410 => "the file is not where this build of Kiln expects it".to_string(),
                403 | 429 => "it refused or limited the request; try again later".to_string(),
                s if (500..600).contains(&s) => "it had a problem; try again later".to_string(),
                s => format!("it answered {s}"),
            };
            return Err(Error::new(ErrorCode::Network, format!("{} answered {} for {}: {what}. Nothing was kept from this answer.", from(c, &url.host), r.status, f.name)));
        }
    }
    let Sink { file, hasher, have, .. } = sink;
    file.sync_all().map_err(crate::util::io_at(part, "flushing"))?;
    drop(file);
    let sha = crate::hash::hex(hasher.finalize().as_slice());
    if have == f.size && sha == f.sha256 {
        return Ok((Outcome::Verified, fetched));
    }
    remove_plain(part)?;
    if start > 0 {
        // the piece that was there and the rest that came are not one file: once more, whole
        return Ok((Outcome::Restart, fetched));
    }
    Err(if have < f.size {
        Error::new(ErrorCode::Network, format!("The download of {} from {} ended early ({} of {}). Kiln deleted what arrived. Try again.",
            f.name, from(c, &url.host), crate::util::human_size(have), crate::util::human_size(f.size)))
    } else {
        mismatch(c, f)
    })
}

fn folder_of(part: &Path) -> String {
    part.parent().map(|p| p.display().to_string()).unwrap_or_default()
}

/// `Content-Range: bytes <start>-<size-1>/<size>`
fn content_range_is(r: &Response, start: u64, size: u64) -> bool {
    r.header("content-range").is_some_and(|v| v.trim() == format!("bytes {start}-{}/{size}", size - 1))
}
