// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, the runtime's side of its files.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9; the same in 1.8.0): modules/mod-runtime/lua/shroudforge/v1/dispatch_mod_ui_actions.rs:65-107 (the
// actions folder read: every entry, 32 per update, unreadable or malformed removed, a request whose callback is not
// registered kept, removal best effort), create_mod_api.rs:232-263 (the notice payload) and
// modules/notifications/write_notification_event.rs:4-21 (one event per key, a later write of the key replaces it).
//! The runtime's end of the UI bridge between Kiln.Ui and kiln-sfi's Lua runtime
//! (schema/sfi/sfi-ui-envelope.schema.json; Kiln.Ui's end is managed/services/Kiln.Ui/SfiBridgeFiles.cs). Both ends
//! share schema/fixtures/sfi/ui/: wire/ (the exact bytes), bounds.json and drain/cases.json (how actions are read).
//!
//! ```text
//! <game>/kiln/sfi/ui/<launchId>/launch.json            Kiln.Ui's receipt: never read or written here
//! <game>/kiln/sfi/ui/<launchId>/staging/               Kiln.Ui's records being written: never read here
//! <game>/kiln/sfi/ui/<launchId>/actions/<requestId>.json   read, and removed, by [`drain`] on the worker
//! <configDir>/sfi/notifications/<sha256(key)>.json      written by [`publish_notice`], kept across launches
//! ```
//!
//! **Actions.** A turn examines the first [`ENTRIES_PER_TURN`] entries the folder lists, whatever their names (SF
//! `take(32)`). The runtime never creates `actions/` or anything else in the launch folder; its one change there
//! is removing entries of `actions/`. Kiln's own rules on top of ShroudForge's (drain/cases.json rules):
//! - nothing is read or removed while a folder of the bridge's path (`<game>/kiln` down to `actions/`) is a link
//!   ([`link_on_path`], Kiln.Ui's own rule for its writes);
//! - an entry that is not a regular file is never opened: a folder stays (SF cannot read or remove it either), a link
//!   or any other kind is removed unread (no link is ever followed);
//! - an entry larger than [`RECORD_BYTES`] is removed unread, decided from its metadata before it is opened (a file
//!   another program holds can still be measured);
//! - an entry whose open (or read) fails with a sharing or lock violation (Windows error 32 or 33: an antivirus scan,
//!   a backup) stays, unread and not removed, for a later turn (SF removes it and the request is lost);
//! - the record must be this launch's (format 1, kind action, launchId, planHash, an integer ownerEpoch), and its package
//!   must be active at that epoch; otherwise it is removed and never called.
//!
//! The callback itself, the active owners and their epochs are the caller's ([`ActionTarget`]): the Lua side is the UI family's.
//!
//! **Notices.** [`publish_notice`] writes synchronously on the calling thread (the worker, inside the Lua call), so a
//! failure is the same call's error, as ShroudForge's own write is. One file per key: the key is `modId-id` as
//! ShroudForge writes it, so its collisions (`a-b`/`c` and `a`/`b-c`) share one file and the later write wins. A
//! profile keeps at most [`NOTICE_KEYS_PER_PROFILE`] keys; a new key past that is refused (an existing key is still
//! replaced). Each write is a temporary file in the same folder, flushed, then renamed over the key's file.
//!
//! **Owner epochs.** [`owner_epoch`] is the number Kiln.Ui puts in an action's `ownerEpoch` (it reads it from
//! sfi.json's `packages[].ownerEpoch`): one per runtime generation of a VM, new for every VM and renewed
//! ([`renew_owner_epoch`]) after each live reload or unload, so a request made for an earlier generation is never
//! called.

use mlua::Lua;
use serde_json::Value;
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

// schema/fixtures/sfi/ui/bounds.json; tests hold them equal.
/// The largest action or notice record.
pub const RECORD_BYTES: u64 = 2 * 1024 * 1024;
/// Kiln.Ui's bound on one owner's waiting requests (its side refuses the 1025th); listed for the bounds check.
pub const PENDING_ACTIONS_PER_OWNER: usize = 1024;
/// The notice keys one profile keeps.
pub const NOTICE_KEYS_PER_PROFILE: usize = 4096;
/// The directory entries one turn examines (SF `take(32)`).
pub const ENTRIES_PER_TURN: usize = 32;
/// Kiln.Ui's bound on its receipt (never read here); listed for the bounds check.
pub const RECEIPT_BYTES: u64 = 64 * 1024;
/// How long Kiln.Ui keeps an ended launch's folder (its cleanup, never this side); listed for the bounds check.
pub const KEEP_ENDED_LAUNCH_DAYS: u32 = 7;

/// The record format of every kind.
pub const FORMAT: u64 = 1;

/// Lower-case hex of `bytes`.
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// SHA-256 of a notice key's UTF-8 bytes, lower-case hex: its file name and `keySha256`.
pub fn key_sha256(key: &str) -> String {
    use sha2::Digest;
    hex(&sha2::Sha256::digest(key.as_bytes()))
}

/// ShroudForge's identifier (create_mod_api.rs:383-389): 1 to 80 of A-Z a-z 0-9 . _ -.
pub fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
}

/// The launch whose folder this runtime reads: sfi.json `launch.launchId` and `launch.planHash`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Launch {
    pub launch_id: u64,
    pub plan_hash: [u8; 32],
}
impl Launch {
    /// kiln.launch's u64 as 16 lower-case hex digits: the folder's name.
    pub fn launch_id_hex(&self) -> String {
        format!("{:016x}", self.launch_id)
    }
    pub fn plan_hash_hex(&self) -> String {
        hex(&self.plan_hash)
    }
}

/// `<game>/kiln/sfi/ui`.
pub fn ui_root(game: &Path) -> PathBuf {
    game.join("kiln").join("sfi").join("ui")
}
/// `<game>/kiln/sfi/ui/<launchId>`.
pub fn launch_dir(game: &Path, launch: &Launch) -> PathBuf {
    ui_root(game).join(launch.launch_id_hex())
}
/// `<game>/kiln/sfi/ui/<launchId>/actions`.
pub fn actions_dir(game: &Path, launch: &Launch) -> PathBuf {
    launch_dir(game, launch).join("actions")
}
/// The first folder of the bridge's path, from `<game>/kiln` down to the launch's `actions/`, that is a link (Kiln.Ui's
/// LinkOnPath rule, which never writes through one): the runtime never reads or removes anything through it.
pub fn link_on_path(game: &Path, launch: &Launch) -> Option<PathBuf> {
    let kiln = game.join("kiln");
    let sfi = kiln.join("sfi");
    [
        kiln,
        sfi,
        ui_root(game),
        launch_dir(game, launch),
        actions_dir(game, launch),
    ]
    .into_iter()
    .find(|path| is_link(path))
}
/// `<configDir>/sfi/notifications`.
pub fn notices_dir(config: &Path) -> PathBuf {
    config.join("sfi").join("notifications")
}

// ---- the bytes (wire/: compact, members in the schema's order, strings escaped as serde_json does)

fn put_str(out: &mut String, text: &str) {
    out.push_str(&serde_json::to_string(text).expect("a string serializes"));
}
fn put_opt(out: &mut String, text: Option<&str>) {
    match text {
        Some(text) => put_str(out, text),
        None => out.push_str("null"),
    }
}

/// One action record (schema `$defs/action`). Kiln.Ui writes these; the runtime only reads them. The serializer is
/// here for the shared byte check and the tests' producers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActionRecord {
    pub request_id: String,
    pub sequence: u64,
    pub launch_id: String,
    pub plan_hash: String,
    pub owner_epoch: u64,
    pub mod_id: String,
    pub action: String,
    pub value: Option<String>,
    pub created_ms: u64,
}
impl ActionRecord {
    /// The record's exact bytes (wire/action-*.json).
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = String::from("{\"format\":1,\"kind\":\"action\",\"requestId\":");
        put_str(&mut out, &self.request_id);
        out.push_str(&format!(",\"sequence\":{},\"launchId\":", self.sequence));
        put_str(&mut out, &self.launch_id);
        out.push_str(",\"planHash\":");
        put_str(&mut out, &self.plan_hash);
        out.push_str(&format!(",\"ownerEpoch\":{},\"modId\":", self.owner_epoch));
        put_str(&mut out, &self.mod_id);
        out.push_str(",\"action\":");
        put_str(&mut out, &self.action);
        if let Some(value) = &self.value {
            out.push_str(",\"value\":");
            put_str(&mut out, value);
        }
        out.push_str(&format!(",\"createdMs\":{}}}", self.created_ms));
        out.into_bytes()
    }
}

/// The payload ShroudForge 1.7.9's `notifications.publish` stores, field for field (create_mod_api.rs:254-262).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Notice {
    /// `modId.id`.
    pub id: String,
    pub mod_id: String,
    pub title: String,
    pub message: String,
    pub level: String,
    pub action_url: Option<String>,
    /// Seconds since 1970.
    pub updated_at: u64,
}

/// One notice record (schema `$defs/notice`): `keySha256` is derived from the key.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NoticeRecord {
    /// `modId-id`, ShroudForge's event key.
    pub key: String,
    /// The launch that published it (16 lower-case hex digits): provenance only.
    pub launch_id: String,
    pub owner_epoch: u64,
    pub notice: Notice,
}
impl NoticeRecord {
    /// The record's exact bytes (wire/notice*.json).
    pub fn to_bytes(&self) -> Vec<u8> {
        let n = &self.notice;
        let mut out = String::from("{\"format\":1,\"kind\":\"notice\",\"key\":");
        put_str(&mut out, &self.key);
        out.push_str(",\"keySha256\":");
        put_str(&mut out, &key_sha256(&self.key));
        out.push_str(",\"launchId\":");
        put_str(&mut out, &self.launch_id);
        out.push_str(&format!(
            ",\"ownerEpoch\":{},\"notice\":{{\"id\":",
            self.owner_epoch
        ));
        put_str(&mut out, &n.id);
        out.push_str(",\"modId\":");
        put_str(&mut out, &n.mod_id);
        out.push_str(",\"title\":");
        put_str(&mut out, &n.title);
        out.push_str(",\"message\":");
        put_str(&mut out, &n.message);
        out.push_str(",\"level\":");
        put_str(&mut out, &n.level);
        out.push_str(",\"actionUrl\":");
        put_opt(&mut out, n.action_url.as_deref());
        out.push_str(&format!(",\"updatedAt\":{}}}}}", n.updated_at));
        out.into_bytes()
    }
}

// ---- notices

static TEMPS: AtomicU64 = AtomicU64::new(0);

/// Whether `name` is a notice file's name: 64 lower-case hex digits and `.json` (what Kiln.Ui reads and counts).
pub fn notice_file_name(name: &str) -> bool {
    name.strip_suffix(".json").is_some_and(|stem| {
        stem.len() == 64 && stem.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
    })
}

fn is_link(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|m| m.file_type().is_symlink() || reparse_point(&m))
}
#[cfg(windows)]
fn reparse_point(m: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    m.file_attributes() & 0x400 != 0
}
#[cfg(not(windows))]
fn reparse_point(_: &fs::Metadata) -> bool {
    false
}

/// Writes `record` as `<configDir>/sfi/notifications/<sha256(key)>.json`, now, on this thread. `Err` is the text
/// the Lua call raises: the folder is a link or cannot be made, the profile already keeps
/// [`NOTICE_KEYS_PER_PROFILE`] other keys, the record is over [`RECORD_BYTES`], or the write failed. An existing key's file
/// is replaced whatever the count. Nothing temporary outlives a failed write.
pub fn publish_notice(config: &Path, record: &NoticeRecord) -> Result<(), String> {
    let failed = |what: String| format!("notification not written: {what}");
    let bytes = record.to_bytes();
    if bytes.len() as u64 > RECORD_BYTES {
        return Err(failed(format!(
            "the notice is {} bytes, more than {RECORD_BYTES}",
            bytes.len()
        )));
    }
    let sfi = config.join("sfi");
    let dir = notices_dir(config);
    for path in [&sfi, &dir] {
        if is_link(path) {
            return Err(failed(format!("{} is a link", path.display())));
        }
    }
    fs::create_dir_all(&dir).map_err(|e| failed(format!("{}: {e}", dir.display())))?;
    if is_link(&sfi) || is_link(&dir) {
        return Err(failed(format!("{} is a link", dir.display())));
    }
    let sha = key_sha256(&record.key);
    let target = dir.join(format!("{sha}.json"));
    if fs::symlink_metadata(&target).is_err() {
        let entries = fs::read_dir(&dir).map_err(|e| failed(format!("{}: {e}", dir.display())))?;
        let stored = entries
            .flatten()
            .filter(|e| e.file_name().to_str().is_some_and(notice_file_name))
            .count();
        if stored >= NOTICE_KEYS_PER_PROFILE {
            return Err(failed(format!(
                "this profile already keeps {stored} notifications, the most it keeps ({NOTICE_KEYS_PER_PROFILE}); publish an existing id to update it"
            )));
        }
    }
    let temp = dir.join(format!(
        ".{sha}.{}-{}.tmp",
        std::process::id(),
        TEMPS.fetch_add(1, Ordering::Relaxed)
    ));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        drop(file);
        // ShroudForge's atomic replace retries (write_json_atomically.rs:4-25): a reader that holds the old file
        // without delete sharing refuses the rename for a moment.
        for attempt in 0..5u32 {
            match fs::rename(&temp, &target) {
                Ok(()) => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::PermissionDenied && attempt < 4 => {
                    std::thread::sleep(Duration::from_millis(10 << attempt));
                }
                Err(error) => return Err(error),
            }
        }
        Err(io::Error::other("replacement retries exhausted"))
    })();
    if let Err(error) = result {
        let _ = fs::remove_file(&temp);
        return Err(failed(format!("{}: {error}", target.display())));
    }
    Ok(())
}

// ---- actions

/// What a directory entry is, from its listing (never following a link).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EntryKind {
    File,
    Folder,
    /// A symbolic link or another reparse point.
    Link,
    /// Anything else (a FIFO, a socket, a device).
    Other,
}

/// One entry of `actions/` as listed.
#[derive(Clone, Debug)]
pub struct Entry {
    pub path: PathBuf,
    /// `None` when the listing could not say.
    pub kind: Option<EntryKind>,
    /// Its size from its metadata, `None` when that could not be read.
    pub len: Option<u64>,
}

/// What reading one entry gave.
#[derive(Debug)]
pub enum Opened {
    Bytes(Vec<u8>),
    /// More than the limit (it grew after it was listed).
    TooLarge,
    /// Another program holds it without sharing, or has it locked (Windows error 32 or 33).
    Locked,
    /// It is gone.
    Gone,
    /// Any other failure.
    Failed(io::Error),
}

/// The file operations of a drain (tests replace them to make a removal fail or an entry locked).
pub trait DrainIo {
    /// The first `max` entries of `dir` in the order the folder lists them; `None` when it cannot be listed (it does
    /// not exist).
    fn list(&mut self, dir: &Path, max: usize) -> Option<Vec<Entry>>;
    /// Reads at most `limit` bytes of the file.
    fn read(&mut self, path: &Path, limit: u64) -> Opened;
    fn remove(&mut self, path: &Path) -> io::Result<()>;
}

/// Whether `error` is Windows' ERROR_SHARING_VIOLATION (32) or ERROR_LOCK_VIOLATION (33): another program holds the
/// file without sharing, or has a range of it locked. Never on another system (there 32 and 33 mean other things).
pub fn is_sharing_violation(error: &io::Error) -> bool {
    cfg!(windows) && matches!(error.raw_os_error(), Some(32 | 33))
}

/// The real files. An entry is opened with Rust's default sharing (read, write and delete on Windows), so a writer's
/// rename in flight never makes it unreadable.
pub struct StdIo;
impl DrainIo for StdIo {
    fn list(&mut self, dir: &Path, max: usize) -> Option<Vec<Entry>> {
        let entries = fs::read_dir(dir).ok()?;
        Some(
            entries
                .flatten()
                .take(max)
                .map(|entry| {
                    let metadata = entry.metadata().ok();
                    let kind = entry.file_type().ok().map(|t| {
                        if t.is_symlink() || metadata.as_ref().is_some_and(reparse_point) {
                            EntryKind::Link
                        } else if t.is_dir() {
                            EntryKind::Folder
                        } else if t.is_file() {
                            EntryKind::File
                        } else {
                            EntryKind::Other
                        }
                    });
                    Entry {
                        path: entry.path(),
                        kind,
                        len: metadata.map(|m| m.len()),
                    }
                })
                .collect(),
        )
    }
    fn read(&mut self, path: &Path, limit: u64) -> Opened {
        let classify = |error: io::Error| {
            if is_sharing_violation(&error) {
                Opened::Locked
            } else if error.kind() == io::ErrorKind::NotFound {
                Opened::Gone
            } else {
                Opened::Failed(error)
            }
        };
        let file = match fs::File::open(path) {
            Ok(file) => file,
            Err(error) => return classify(error),
        };
        let mut bytes = Vec::new();
        if let Err(error) = file.take(limit + 1).read_to_end(&mut bytes) {
            return classify(error);
        }
        if bytes.len() as u64 > limit {
            return Opened::TooLarge;
        }
        Opened::Bytes(bytes)
    }
    fn remove(&mut self, path: &Path) -> io::Result<()> {
        fs::remove_file(path)
    }
}

/// What a callback lookup gave.
#[derive(Debug, PartialEq, Eq)]
pub enum Invoke {
    /// No callback is registered for the mod and action: the request waits.
    Unregistered,
    /// The callback ran; its error, if it raised one.
    Called(Result<(), String>),
}

/// The owning packages and their callbacks (the UI family's Lua side; tests' fakes).
pub trait ActionTarget {
    /// The package's current epoch when it is active now, else `None`.
    fn epoch(&self, mod_id: &str) -> Option<u64>;
    /// Looks the callback up (ShroudForge's dotted registry key) and calls it with `value`.
    fn invoke(&mut self, mod_id: &str, action: &str, value: Option<&str>) -> Invoke;
}

/// Why an entry was removed without its callback being called.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dropped {
    /// A link or anything that is not a file or a folder: never opened.
    NotAFile,
    /// Larger than [`RECORD_BYTES`].
    TooLarge,
    /// It could not be read (vanished, refused).
    Unreadable,
    /// Not a JSON object.
    Malformed,
    /// Not this launch's action record (format, kind, launchId, planHash, ownerEpoch).
    NotThisLaunch,
    /// modId or action is not a string.
    NoTarget,
    /// The owning package is not active.
    Inactive,
    /// Its epoch moved on (or is not the record's).
    StaleEpoch,
    /// modId or action is not an SF identifier.
    InvalidIdentifier,
}

/// What became of one entry this turn.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// A folder: never read or removed (it still counts toward the turn's entries).
    Folder,
    /// Held by another program: left for a later turn.
    Locked,
    /// Its callback is not registered yet: left for a later turn.
    Waiting,
    /// Dropped; `removed` is false when the removal failed (it is examined again next turn).
    Dropped { why: Dropped, removed: bool },
    /// The callback ran (`error` when it raised); `removed` false: it runs again next turn.
    Called {
        mod_id: String,
        action: String,
        error: Option<String>,
        removed: bool,
    },
}

/// One turn's entries, in the order examined.
#[derive(Clone, Debug, Default)]
pub struct DrainReport {
    pub entries: Vec<(PathBuf, Outcome)>,
}
impl DrainReport {
    /// The callbacks that raised: (modId, action, error), for ShroudForge's `ui action failed` log line.
    pub fn failures(&self) -> impl Iterator<Item = (&str, &str, &str)> {
        self.entries.iter().filter_map(|(_, o)| match o {
            Outcome::Called {
                mod_id,
                action,
                error: Some(error),
                ..
            } => Some((mod_id.as_str(), action.as_str(), error.as_str())),
            _ => None,
        })
    }
}

/// One turn over `dir` (a launch's `actions/`). Never creates the folder or anything in it.
pub fn drain(
    dir: &Path,
    launch: &Launch,
    io: &mut dyn DrainIo,
    target: &mut dyn ActionTarget,
) -> DrainReport {
    let mut report = DrainReport::default();
    let Some(entries) = io.list(dir, ENTRIES_PER_TURN) else {
        return report;
    };
    let (launch_id, plan_hash) = (launch.launch_id_hex(), launch.plan_hash_hex());
    for entry in entries {
        let outcome = one(&entry, &launch_id, &plan_hash, io, target);
        report.entries.push((entry.path, outcome));
    }
    report
}

fn one(
    entry: &Entry,
    launch_id: &str,
    plan_hash: &str,
    io: &mut dyn DrainIo,
    target: &mut dyn ActionTarget,
) -> Outcome {
    let path = &entry.path;
    let drop_it = |io: &mut dyn DrainIo, why: Dropped| Outcome::Dropped {
        why,
        removed: io.remove(path).is_ok(),
    };
    match entry.kind {
        Some(EntryKind::Folder) => return Outcome::Folder,
        Some(EntryKind::Link | EntryKind::Other) => return drop_it(io, Dropped::NotAFile),
        Some(EntryKind::File) | None => {}
    }
    if entry.len.is_some_and(|len| len > RECORD_BYTES) {
        return drop_it(io, Dropped::TooLarge);
    }
    let bytes = match io.read(path, RECORD_BYTES) {
        Opened::Bytes(bytes) => bytes,
        Opened::Locked => return Outcome::Locked,
        Opened::TooLarge => return drop_it(io, Dropped::TooLarge),
        Opened::Gone | Opened::Failed(_) => return drop_it(io, Dropped::Unreadable),
    };
    let Ok(Value::Object(record)) = serde_json::from_slice::<Value>(&bytes) else {
        return drop_it(io, Dropped::Malformed);
    };
    let text = |key: &str| record.get(key).and_then(Value::as_str);
    let epoch = record.get("ownerEpoch");
    let ours = record.get("format").and_then(Value::as_u64) == Some(FORMAT)
        && text("kind") == Some("action")
        && text("launchId") == Some(launch_id)
        && text("planHash") == Some(plan_hash)
        && epoch.is_some_and(|e| e.is_u64() || e.is_i64());
    if !ours {
        return drop_it(io, Dropped::NotThisLaunch);
    }
    let (Some(mod_id), Some(action)) = (text("modId"), text("action")) else {
        return drop_it(io, Dropped::NoTarget);
    };
    let value = text("value");
    match target.epoch(mod_id) {
        None => return drop_it(io, Dropped::Inactive),
        Some(current) if epoch.and_then(Value::as_u64) != Some(current) => {
            return drop_it(io, Dropped::StaleEpoch);
        }
        Some(_) => {}
    }
    if !valid_identifier(mod_id) || !valid_identifier(action) {
        return drop_it(io, Dropped::InvalidIdentifier);
    }
    match target.invoke(mod_id, action, value) {
        Invoke::Unregistered => Outcome::Waiting,
        Invoke::Called(result) => Outcome::Called {
            mod_id: mod_id.to_owned(),
            action: action.to_owned(),
            error: result.err(),
            removed: io.remove(path).is_ok(),
        },
    }
}

// ---- owner epochs

/// The process's epoch counter: every VM generation gets the next number (never 0, never reused).
static EPOCHS: AtomicU64 = AtomicU64::new(0);

/// This VM's current owner epoch (kept in the VM's app data).
struct VmEpoch(u64);

/// The `ownerEpoch` of every package active in this VM now: given at the first ask (the VM's first package, or its
/// first sfi.json publish), then kept until [`renew_owner_epoch`].
pub fn owner_epoch(lua: &Lua) -> u64 {
    if let Some(epoch) = lua.app_data_ref::<VmEpoch>() {
        return epoch.0;
    }
    renew_owner_epoch(lua)
}

/// A new epoch for this VM's packages (after a live reload or unload: the packages that come back, or stay, are a new
/// generation, and a request for the old one is dropped). Returns it.
pub fn renew_owner_epoch(lua: &Lua) -> u64 {
    let epoch = EPOCHS.fetch_add(1, Ordering::Relaxed) + 1;
    lua.set_app_data(VmEpoch(epoch));
    epoch
}

// ---- one launch's transport

/// Where this start's UI files are: from the frozen plan (the game folder, the launch, the profile's config folder).
#[derive(Clone, Debug)]
pub struct Transport {
    pub game: PathBuf,
    /// The core's KILN_PATH_CONFIG folder: the profile's configDir, else `<game>/kiln/config`. `None`: notices cannot be
    /// published.
    pub config: Option<PathBuf>,
    pub launch: Launch,
}
impl Transport {
    pub fn actions_dir(&self) -> PathBuf {
        actions_dir(&self.game, &self.launch)
    }
    /// One turn over this launch's `actions/` with the real files. Nothing at all while a folder of the bridge's path
    /// is a link ([`link_on_path`]).
    pub fn drain(&self, target: &mut dyn ActionTarget) -> DrainReport {
        if link_on_path(&self.game, &self.launch).is_some() {
            return DrainReport::default();
        }
        drain(&self.actions_dir(), &self.launch, &mut StdIo, target)
    }
    /// Publishes a notice of this launch now (see [`publish_notice`]).
    pub fn publish(&self, key: &str, owner_epoch: u64, notice: Notice) -> Result<(), String> {
        let config = self.config.as_deref().ok_or_else(|| {
            "notification not written: the core gives no config folder".to_owned()
        })?;
        publish_notice(
            config,
            &NoticeRecord {
                key: key.to_owned(),
                launch_id: self.launch.launch_id_hex(),
                owner_epoch,
                notice,
            },
        )
    }
}
