//! Kept versions (0.2.0-dev.29): when an update replaces a mod's Library entry in a profile (a dropped archive, a Nexus
//! Mods install, "Use <version>", the Undo of a roll back), the entry it replaced is kept for a roll back, together with
//! a copy of the settings files the mod's game-folder copies held at that moment. How many per mod: `keepVersions` in
//! the launcher's `settings.json` (default [`KEEP_DEFAULT`], at most [`KEEP_MAX`]; 0 keeps none, as before dev.29). A
//! version is not kept when the Library's disk would then have less free than the data folder check asks for
//! ([`crate::data_folder::needed_bytes`]); the update's result says so.
//!
//! A roll back ([`rollback`]) puts a kept entry back into the profile in place of the mod's current one. It is an
//! ordinary profile change: its Undo is the launcher's (`quick::revert`, `profile_revert`), and the version rolled
//! away from becomes the kept one (so a second roll back returns to it). The roll back itself writes nothing in the
//! game folder: the next placement of the profile (`profile::activate` / `refresh_active`, what a profile's re-apply
//! and Play run) places the version as always, carrying the newer version's settings over, and then puts the kept
//! settings back into the copy ([`restore_after_place`]), so the player gets the settings they had, not the new ones.
//! A restore belongs to the profile that rolled back and to the game folder its settings came from: another profile,
//! or the same profile in another game, never takes or applies it.
//!
//! On disk (docs/kiln/launcher/contracts.md section 3): `<library>/kiln-kept.json` ([`KeptState`]) and
//! `<library>/kiln-kept/<kind>/<id>/<version>+<hash8>.<12 hex>/` (one settings copy, each file at its path relative to
//! the game folder; a new copy always gets a new folder). A kept entry is an ordinary Library entry otherwise: a
//! launcher before dev.29 lists it as another version of the mod and its Clean up may remove it; an entry that is gone
//! is forgotten here. Kept entries are listed after the others ([`crate::library::list`]), so nothing picks one by
//! default; Clean up ([`crate::library::unused`]) leaves them.
//!
//! **A change is one transaction, the profile save its commit point** (the pattern of `engine_mods::copy_in`: stage,
//! rename into place, then the record, and what a stop leaves is found and finished or undone). [`prepare`] runs
//! before the profile is saved, under the state's lock, held until [`Prepared::commit`] or [`Prepared::abort`]: a new
//! settings copy is staged in `.stage-<hex>` beside its place and renamed to its own new folder (a staged folder never
//! has a final name), then the new state is written in one durable write together with a record of the change
//! (`pending`: the profile, each mod's versions before and after, and each touched mod's state before). The caller
//! saves the profile; commit drops the record, abort puts the touched mods back. A stop at any point leaves either the
//! old state or the record, and the next look at the state under its lock ([`reconcile`], before any restore) finishes
//! it: when the profile still lists every change's version before, the change is undone, else it stands. It also drops
//! a kept entry whose settings copy is gone (with a note), a kept entry the profile that left it uses again (changed
//! without Kiln following it), and deletes every staged leftover and every settings folder the state does not name.
//! Nothing outside `<library>/kiln-kept/` and the state file is touched by it.

use crate::library::LibraryRef;
use crate::manifest::ModKind;
use crate::paths::DataDirs;
use crate::profile::{Profile, ProfileMod};
use crate::quick::ProfileChange;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Kept versions per mod when `settings.json` says nothing.
pub const KEEP_DEFAULT: u32 = 1;
/// The most `keepVersions` may ask for.
pub const KEEP_MAX: u32 = 5;
/// `<library>/kiln-kept.json`.
pub const STATE_FILE: &str = "kiln-kept.json";
/// `<library>/kiln-kept/`: the settings copies.
pub const SETTINGS_DIR: &str = "kiln-kept";
const LOCK_FILE: &str = "kiln-kept.lock";
/// A settings copy being made: `<library>/kiln-kept/<kind>/<id>/.stage-<12 hex>/`.
const STAGE: &str = ".stage-";
/// The largest settings file kept, and the most kept per version: what is larger stays out (the result says so).
const MAX_FILE: u64 = 16 * 1024 * 1024;
const MAX_TOTAL: u64 = 64 * 1024 * 1024;

/// A version of one mod: its Library version and hash.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VersionRef {
    pub version: String,
    pub hash: String,
}

impl VersionRef {
    fn of(m: &ProfileMod) -> VersionRef {
        VersionRef { version: m.version.clone(), hash: m.hash.clone() }
    }
}

/// The settings kept with a version: the files of its game-folder copies that were not as Kiln placed them (settings
/// files and logs the mod or the player wrote there; never code or a manifest: [`not_settings`]), and ShroudForge
/// setting values.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptSettings {
    /// The folder of the copy under `<library>/kiln-kept/<kind>/<id>/` (`<version>+<hash8>.<12 hex>`); none when no
    /// file was copied.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dir: Option<String>,
    /// Each file's path relative to the game folder ('/' separated), as in the settings copy.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub files: Vec<String>,
    /// Their bytes.
    #[serde(default)]
    pub size: u64,
    /// ShroudForge setting values (`extended.mod.json` `settings.<key>.value`) per copy (its path relative to the game
    /// folder): put back with ShroudForge's own setter, so its switch and the rest of the file stay as placed.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub values: BTreeMap<String, serde_json::Map<String, serde_json::Value>>,
    /// The game folder they were read in: a restore of them applies there only.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game: Option<String>,
    /// What was left out (too large, unreadable), one sentence each.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub left_out: Vec<String>,
}

impl KeptSettings {
    fn is_empty(&self) -> bool {
        self.files.is_empty() && self.values.is_empty()
    }
}

/// One kept version of a mod.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptVersion {
    pub version: String,
    pub hash: String,
    /// When it was kept (RFC 3339 UTC).
    pub kept: String,
    /// The version that took its place then.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replaced_by: Option<VersionRef>,
    /// The profile whose change left it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile: Option<String>,
    #[serde(default)]
    pub settings: KeptSettings,
}

impl KeptVersion {
    /// Kept by the change of `profile` (an entry that does not say is anyone's).
    fn of_profile(&self, profile: &str) -> bool {
        self.profile.as_deref().is_none_or(|p| p == profile)
    }
}

/// Hashes captured before restoration: `None` is an absent file or setting value.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RestoreGuard {
    pub before: Option<String>,
    pub snapshot: String,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RestorePreimages {
    pub files: BTreeMap<String, RestoreGuard>,
    pub values: BTreeMap<String, BTreeMap<String, RestoreGuard>>,
}

/// Settings waiting for the next placement of a version a roll back put into a profile ([`restore_after_place`]), in
/// the game folder they came from (`settings.game`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PendingRestore {
    pub version: String,
    pub hash: String,
    /// The profile that rolled back: only its placement takes or applies them.
    pub profile: String,
    pub settings: KeptSettings,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub preimages: Option<RestorePreimages>,
    /// Its files/values are settled and restored content flushed: only the record is left to drop (a stop never puts them
    /// back again over what the player changed since).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub applied: bool,
}

/// What is kept of one mod.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptMod {
    pub kind: ModKind,
    pub id: String,
    /// Newest kept first.
    #[serde(default)]
    pub kept: Vec<KeptVersion>,
    /// At most one per profile.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub restores: Vec<PendingRestore>,
}

impl KeptMod {
    fn empty(kind: ModKind, id: &str) -> KeptMod {
        KeptMod { kind, id: id.to_string(), kept: Vec::new(), restores: Vec::new() }
    }
}

/// One profile entry the change in [`KeptTxn`] replaces.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TxnChange {
    pub kind: ModKind,
    pub id: String,
    pub from: VersionRef,
    pub to: VersionRef,
}

/// A change written with the state before its profile was saved ([`prepare`]): what [`reconcile`] finishes or undoes.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptTxn {
    pub id: String,
    pub profile: String,
    pub started: String,
    pub changes: Vec<TxnChange>,
    /// The state each touched mod had before (an empty one: it had none).
    pub before: Vec<KeptMod>,
    /// Profile entries before the save, including removals which have no version-to-version change.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile_before: Option<crate::profile::ProfileMods>,
}

/// `<library>/kiln-kept.json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptState {
    pub format: u32,
    #[serde(default)]
    pub mods: Vec<KeptMod>,
    /// A change whose profile save was not confirmed yet.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub pending: Vec<KeptTxn>,
}

impl Default for KeptState {
    fn default() -> Self {
        KeptState { format: 1, mods: Vec::new(), pending: Vec::new() }
    }
}

/// What the Library list says of a kept version (`LibraryVersion.kept`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptInfo {
    /// When it was kept (RFC 3339 UTC).
    pub kept: String,
    /// The version that took its place then.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replaced_by: Option<String>,
    /// The settings files kept with it (relative to the game folder).
    #[serde(default)]
    pub settings: Vec<String>,
    /// Their bytes (the entry's own size is the version's `size`).
    #[serde(default)]
    pub settings_size: u64,
    /// The profiles whose change kept it (only they roll back to it); empty: any.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub profiles: Vec<String>,
}

/// What [`rollback`] did.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RollbackReport {
    pub profile_id: String,
    pub profile_name: String,
    pub kind: ModKind,
    pub id: String,
    /// The mod's display name.
    pub name: String,
    /// The version rolled away from, and the one now in the profile.
    pub from: VersionRef,
    pub to: VersionRef,
    /// What the launcher's Undo (`profile_revert`) puts back.
    pub changes: Vec<ProfileChange>,
    /// The version rolled away from is kept now (a second roll back returns to it).
    pub kept: bool,
    /// Settings files that go back into the mod's game-folder copy at its next placement.
    #[serde(default)]
    pub settings: Vec<String>,
    /// Something the player should know (not kept: the disk; `keepVersions` is 0), one sentence each.
    #[serde(default)]
    pub notices: Vec<String>,
}

// ------------------------------------------------------------------------------------------------ test hooks

/// Where a test stops a change as a crash would (a panic: nothing is taken back, the locks are let go).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub(crate) enum Step {
    /// A settings copy has its first file in its staging folder.
    SnapshotCopied,
    /// A settings copy is in its final folder, the state does not name it yet.
    SnapshotRenamed,
    /// The state with the change's record is written; the profile is not saved yet.
    IndexWritten,
    /// The profile is saved; the change's record is not dropped yet.
    BeforeCommit,
    /// A restore wrote its first file into the game folder.
    RestoreFile,
    /// Placement (`engine_mods`): the old copy is parked, the new one not published yet.
    Parked,
    /// Placement: the new copy is published, its receipt not written yet.
    Published,
    /// Placement: the receipt is written, the parked copy not deleted yet.
    ReceiptSaved,
    /// Activation (`profile`): the game's `kiln/profile.json` is written, the activation record not yet.
    GameProfileWritten,
    /// Placement: its staging folder is named in the staging record, not made yet.
    StagingRecorded,
    /// Placement: the staging folder is made, nothing copied into it yet.
    StagingMade,
    /// Placement: the Library copy is in the staging folder, not checked or marked yet.
    StagingCopied,
}

#[cfg(test)]
pub(crate) mod hooks {
    std::thread_local! {
        /// The change on this thread stops at this step.
        pub static STOP_AT: std::cell::Cell<Option<super::Step>> = const { std::cell::Cell::new(None) };
        /// The tags `hex12` hands out next on this thread, last first (`None`: the random source fails).
        pub static TAGS: std::cell::RefCell<Vec<Option<String>>> = const { std::cell::RefCell::new(Vec::new()) };
        /// The n-th write of the state file from now on this thread fails (0: none).
        pub static FAIL_SAVE: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    }
}

#[cfg(test)]
pub(crate) fn crash_point(step: Step) {
    if stop_here(step) {
        panic!("injected crash at {step:?}");
    }
}

#[cfg(not(test))]
#[inline]
pub(crate) fn crash_point(_: Step) {}

/// Does a test stop the work on this thread at `step` (once)? The caller then panics as a crash would, after it let go
/// of what a crash would not clean up (a temporary folder's drop).
#[cfg(test)]
pub(crate) fn stop_here(step: Step) -> bool {
    let stop = hooks::STOP_AT.with(|s| s.get()) == Some(step);
    if stop { hooks::STOP_AT.with(|s| s.set(None)); }
    stop
}

#[cfg(not(test))]
#[inline]
pub(crate) fn stop_here(_: Step) -> bool {
    false
}

// ------------------------------------------------------------------------------------------------ settings and disk

/// `keepVersions` in the launcher's `settings.json` (the launcher writes it, Settings > Launcher), at most
/// [`KEEP_MAX`]; [`KEEP_DEFAULT`] when it is missing or not a number.
pub fn keep_count(dirs: &DataDirs) -> u32 {
    crate::util::read_json::<serde_json::Value>(&dirs.settings()).ok().flatten()
        .and_then(|s| s.get("keepVersions").and_then(serde_json::Value::as_u64))
        .map(|n| n.min(KEEP_MAX as u64) as u32)
        .unwrap_or(KEEP_DEFAULT)
}

thread_local! {
    static FREE: std::cell::Cell<Option<Option<u64>>> = const { std::cell::Cell::new(None) };
}

/// Runs `f` with the Library's free space answered as `free` (None: not known), for tests of the disk check.
#[doc(hidden)]
pub fn with_free_space_for_tests<R>(free: Option<u64>, f: impl FnOnce() -> R) -> R {
    let before = FREE.with(|c| c.replace(Some(free)));
    let out = f();
    FREE.with(|c| c.set(before));
    out
}

fn library_space(dirs: &DataDirs) -> Option<crate::data_folder::Space> {
    if let Some(free) = FREE.with(|c| c.get()) {
        return free.map(|free| crate::data_folder::Space { free, unit: 4096 });
    }
    use crate::data_folder::SystemFacts;
    let lib = dirs.library();
    let at = lib.ancestors().find(|p| p.is_dir())?.to_path_buf();
    crate::data_folder::RealFacts { dirs }.space(&at)
}

/// Why keeping `bytes` in `files` files would break the disk check (the data folder's: the bytes, a unit per file,
/// 256 MiB and 5 % on top), in words; None when it does not, or the free space is not known.
fn disk_refusal(dirs: &DataDirs, bytes: u64, files: u64) -> Option<String> {
    let s = library_space(dirs)?;
    let need = crate::data_folder::needed_bytes(bytes, files, s.unit);
    (s.free < need).then(|| format!("the Library's disk has {} free, and Kiln keeps at least {} free beside a kept version of this size",
        mib(s.free), mib(need)))
}

fn mib(bytes: u64) -> String {
    format!("{:.0} MiB", bytes as f64 / (1024.0 * 1024.0))
}

/// 12 random hex characters (a settings copy's tag, a change's id). No fallback: without the OS's random source no
/// unpredictable name can be made, and the copy is not kept.
fn hex12() -> Result<String> {
    #[cfg(test)]
    if let Some(t) = hooks::TAGS.with(|t| t.borrow_mut().pop()) {
        return t.ok_or_else(|| Error::new(ErrorCode::Io, "injected random source failure"));
    }
    crate::util::random_bytes::<6>().map(|b| crate::hash::hex(&b))
}

/// A new staging folder in `parent` (`.stage-<tag>`), made exclusively, whose tag no settings copy there has yet
/// (`<version>+<hash8>.<tag>`): tried again with a new tag when either is taken. -> (the folder, its tag).
fn new_stage(parent: &Path, version: &str, h8: &str) -> Result<(PathBuf, String)> {
    std::fs::create_dir_all(parent).map_err(crate::util::io_at(parent, "creating"))?;
    for _ in 0..16 {
        let tag = hex12()?;
        if std::fs::symlink_metadata(parent.join(format!("{version}+{h8}.{tag}"))).is_ok() { continue; }
        let stage = parent.join(format!("{STAGE}{tag}"));
        match std::fs::create_dir(&stage) {
            Ok(()) => return Ok((stage, tag)),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(crate::util::io_at(&stage, "creating")(e)),
        }
    }
    Err(Error::new(ErrorCode::Io, format!("{}: no free name for a settings copy", parent.display())))
}

// ------------------------------------------------------------------------------------------------ the state file

fn state_path(dirs: &DataDirs) -> PathBuf {
    dirs.library().join(STATE_FILE)
}

/// Serializes changes of the state file between threads and launcher processes (kept: unlinking a held lock would
/// let another process lock another inode).
fn lock(dirs: &DataDirs) -> Result<crate::util::LockedFile> {
    let lib = dirs.library();
    std::fs::create_dir_all(&lib).map_err(crate::util::io_at(&lib, "creating"))?;
    let f = std::fs::OpenOptions::new().create(true).truncate(false).read(true).write(true).open(lib.join(LOCK_FILE))?;
    f.lock()?;
    Ok(crate::util::LockedFile(f))
}

/// The state as on disk; None when it is of a newer format (left as it is, never written).
fn load(dirs: &DataDirs) -> Option<KeptState> {
    match crate::util::read_json::<serde_json::Value>(&state_path(dirs)) {
        Ok(None) => Some(KeptState::default()),
        Ok(Some(v)) if v.get("format").and_then(serde_json::Value::as_u64) == Some(1) => if v.get("mods").is_some_and(serde_json::Value::is_array) { serde_json::from_value(v).ok() } else { None },
        Ok(Some(_)) => None,
        // An unreadable index is not evidence that its settings copies are unreferenced.
        Err(_) => None,
    }
}

/// One durable write: a stop leaves the old file or the new one, never a part ([`crate::util::write_atomic_durable`]).
fn save(dirs: &DataDirs, st: &KeptState) -> Result<()> {
    #[cfg(test)]
    if hooks::FAIL_SAVE.with(|f| { let n = f.get(); if n > 0 { f.set(n - 1); } n == 1 }) {
        return Err(Error::new(ErrorCode::Io, "injected state write failure"));
    }
    let mut text = serde_json::to_string_pretty(st)?;
    text.push('\n');
    // what it names (crate::durable: flushed before this record)
    let mut names = Vec::new();
    for m in st.mods.iter().chain(st.pending.iter().flat_map(|t| t.before.iter())) {
        for s in m.kept.iter().map(|v| &v.settings).chain(m.restores.iter().map(|r| &r.settings)) {
            if let Some(d) = &s.dir && let Ok(p) = copy_dir(dirs, m.kind, &m.id, d) && !names.contains(&p) { names.push(p); }
        }
    }
    let restores = st.mods.iter().map(|m| m.restores.iter().filter(|r| !r.applied).count()).sum();
    crate::durable::journal(&state_path(dirs), text.as_bytes(), &names, st.pending.len(), restores)
}

fn entry_exists(dirs: &DataDirs, kind: ModKind, id: &str, version: &str, hash: &str) -> bool {
    crate::library::record(dirs, &LibraryRef { kind, id: id.into(), version: version.into(), hash: hash.into() }).is_ok()
}

fn lists(p: &Profile, kind: ModKind, id: &str, version: &str, hash: &str) -> bool {
    p.mods.list(kind).iter().any(|m| m.id == id && m.version == version && m.hash == hash)
}

fn tidy(st: &mut KeptState) {
    st.mods.retain(|m| !m.kept.is_empty() || !m.restores.is_empty());
}

fn put_mod(st: &mut KeptState, m: KeptMod) {
    st.mods.retain(|x| !(x.kind == m.kind && x.id == m.id));
    st.mods.push(m);
}

/// Brings the state to what holds (see the module's doc): a change's record finished or undone by its profile, entries
/// gone from the Library or without their settings copy dropped, a kept entry the profile that left it uses again
/// dropped. Reads the Library and the profiles, writes nothing. -> whether it changed anything.
pub(crate) fn reconcile(dirs: &DataDirs, st: &mut KeptState, notes: &mut Vec<String>) -> bool {
    let mut changed = false;
    let mut profiles: BTreeMap<String, Option<Profile>> = BTreeMap::new();
    let mut profile = |id: &str| profiles.entry(id.to_string()).or_insert_with(|| crate::profile::get(dirs, id).ok()).clone();
    // 1. a change whose commit was not confirmed: undone while its profile still lists every version it replaced
    for t in std::mem::take(&mut st.pending) {
        changed = true;
        let p = profile(&t.profile);
        let undone = p.as_ref().is_some_and(|p| match &t.profile_before {
            Some(before) => t.before.iter().all(|m| {
                p.mods.list(m.kind).iter().find(|e| e.id == m.id)
                    == before.list(m.kind).iter().find(|e| e.id == m.id)
            }),
            None => !t.changes.is_empty() && t.changes.iter().all(|c| lists(p, c.kind, &c.id, &c.from.version, &c.from.hash)),
        });
        if undone {
            for b in t.before { put_mod(st, b); }
        }
    }
    for m in &mut st.mods {
        let (kind, id) = (m.kind, m.id.clone());
        let label = |version: &str| format!("{} {id} {version}", kind.as_str());
        let before = (m.kept.len(), m.restores.len());
        // A deleted profile cannot ask for these again. Preserve unreadable profiles: absence must be proven.
        let deleted = |pid: &str| matches!(std::fs::symlink_metadata(dirs.profile(pid).join("meta.json")),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound);
        m.kept.retain(|v| !v.profile.as_deref().is_some_and(&deleted));
        m.restores.retain(|r| !deleted(&r.profile));
        // 2. gone from the Library (removed by hand, by an older launcher, taken over)
        m.kept.retain(|v| entry_exists(dirs, kind, &id, &v.version, &v.hash));
        m.restores.retain(|r| entry_exists(dirs, kind, &id, &r.version, &r.hash));
        // a restore done, whose record a stop or a failed write left: dropped now
        m.restores.retain(|r| !r.applied);
        // 3. without its settings copy
        m.kept.retain(|v| {
            let there = copy_there(dirs, kind, &id, &v.settings);
            if !there { notes.push(format!("{}: its kept settings ({}) are gone, so it is no longer kept for a roll back", label(&v.version), dir_label(&v.settings))); }
            there
        });
        m.restores.retain(|r| {
            let there = copy_there(dirs, kind, &id, &r.settings);
            if !there { notes.push(format!("{}: the settings a roll back was to put back ({}) are gone; it keeps the settings it has", label(&r.version), dir_label(&r.settings))); }
            there
        });
        // 4. used again by the profile that left it, without Kiln following the change (an older launcher's "Use")
        m.kept.retain(|v| {
            let used = v.profile.as_deref().and_then(&mut profile).is_some_and(|p| lists(&p, kind, &id, &v.version, &v.hash));
            if used { notes.push(format!("{}: its profile uses it again, so it is no longer kept for a roll back", label(&v.version))); }
            !used
        });
        changed |= (m.kept.len(), m.restores.len()) != before;
    }
    let n = st.mods.len();
    tidy(st);
    changed | (st.mods.len() != n)
}

fn dir_label(s: &KeptSettings) -> String {
    s.dir.clone().unwrap_or_else(|| "its folder".into())
}

/// Does the settings copy `s` names exist (as a folder, with every file it lists)? True when it names none.
fn copy_there(dirs: &DataDirs, kind: ModKind, id: &str, s: &KeptSettings) -> bool {
    if s.files.is_empty() { return true; }
    let Some(dir) = s.dir.as_deref() else { return false };
    let Ok(path) = copy_dir(dirs, kind, id, dir) else { return false };
    path.is_dir() && s.files.iter().all(|f| crate::bundle::safe_rel_path(f).is_ok() && path.join(f).is_file())
}

/// Deletes what is in `<library>/kiln-kept/` and no state names: staged copies a stop left, copies the state no longer
/// names. Never through a link: a linked folder there is left as it is.
fn sweep(dirs: &DataDirs, st: &KeptState) {
    let root = dirs.library().join(SETTINGS_DIR);
    if crate::engine_mods::refuse_symlink_path(&dirs.root, &root).is_err() || !root.is_dir() { return; }
    let mut named: std::collections::BTreeSet<(String, String, String)> = Default::default();
    let mods = st.mods.iter().chain(st.pending.iter().flat_map(|t| t.before.iter()));
    for m in mods {
        for s in m.kept.iter().map(|v| &v.settings).chain(m.restores.iter().map(|r| &r.settings)) {
            if let Some(d) = &s.dir { named.insert((m.kind.as_str().to_string(), m.id.clone(), d.clone())); }
        }
    }
    let real_dir = |p: &Path| std::fs::symlink_metadata(p).is_ok_and(|m| m.is_dir() && !crate::quick::import_is_link(&m));
    for k in std::fs::read_dir(&root).into_iter().flatten().flatten() {
        let kp = k.path();
        if !real_dir(&kp) { continue; }
        let kind = k.file_name().to_string_lossy().into_owned();
        for i in std::fs::read_dir(&kp).into_iter().flatten().flatten() {
            let ip = i.path();
            if !real_dir(&ip) { continue; }
            let id = i.file_name().to_string_lossy().into_owned();
            for c in std::fs::read_dir(&ip).into_iter().flatten().flatten() {
                let name = c.file_name().to_string_lossy().into_owned();
                if named.contains(&(kind.clone(), id.clone(), name)) { continue; }
                let cp = c.path();
                if real_dir(&cp) { let _ = std::fs::remove_dir_all(&cp); }
            }
            let _ = std::fs::remove_dir(&ip);
        }
        let _ = std::fs::remove_dir(&kp);
    }
}

/// Changes the state under its lock, reconciled first ([`reconcile`]); saved when it changed, then what it no longer
/// names is swept. Nothing happens with a state of a newer format. -> `f`'s answer and the notes of the reconcile.
fn update<T>(dirs: &DataDirs, f: impl FnOnce(&mut KeptState, &mut Vec<String>) -> T) -> Result<Option<(T, Vec<String>)>> {
    let _lock = lock(dirs)?;
    let Some(mut st) = load(dirs) else { return Ok(None) };
    let mut notes = Vec::new();
    let fixed = reconcile(dirs, &mut st, &mut notes);
    let before = st.clone();
    let out = f(&mut st, &mut notes);
    tidy(&mut st);
    if fixed || st != before {
        save(dirs, &st)?;
    }
    sweep(dirs, &st);
    Ok(Some((out, notes)))
}

/// The state as it holds, for reading (reconciled in memory; nothing written).
fn view(dirs: &DataDirs) -> Option<KeptState> {
    let mut st = load(dirs)?;
    reconcile(dirs, &mut st, &mut Vec::new());
    Some(st)
}

/// Nothing is kept anywhere yet: the calls that only read or tidy the state do nothing (no lock file, no Library
/// folder made for them).
fn nothing_kept(dirs: &DataDirs) -> bool {
    !state_path(dirs).is_file()
}

fn mod_of<'a>(st: &'a mut KeptState, kind: ModKind, id: &str) -> &'a mut KeptMod {
    if let Some(i) = st.mods.iter().position(|m| m.kind == kind && m.id == id) {
        return &mut st.mods[i];
    }
    st.mods.push(KeptMod::empty(kind, id));
    st.mods.last_mut().expect("pushed")
}

/// The kept versions of every mod, read only (`library::list`): by `<kind>/<id>/<hash>`.
pub(crate) fn info(dirs: &DataDirs) -> std::collections::HashMap<String, KeptInfo> {
    let mut out = std::collections::HashMap::new();
    if nothing_kept(dirs) { return out; }
    let Some(st) = view(dirs) else { return out };
    for m in st.mods {
        for v in m.kept {
            let profile = v.profile.clone();
            let info = out.entry(info_key(m.kind, &m.id, &v.hash)).or_insert_with(|| KeptInfo {
                kept: v.kept, replaced_by: v.replaced_by.map(|r| r.version), settings: v.settings.files, settings_size: v.settings.size,
                profiles: Vec::new(),
            });
            if let Some(p) = profile.filter(|p| !info.profiles.contains(p)) { info.profiles.push(p); }
        }
    }
    out
}

pub(crate) fn info_key(kind: ModKind, id: &str, hash: &str) -> String {
    format!("{}/{id}/{hash}", kind.as_str())
}

/// The kept versions of one mod, newest kept first.
pub fn kept_versions(dirs: &DataDirs, kind: ModKind, id: &str) -> Vec<KeptVersion> {
    if nothing_kept(dirs) { return Vec::new(); }
    view(dirs).and_then(|st| st.mods.into_iter().find(|m| m.kind == kind && m.id == id)).map(|m| m.kept).unwrap_or_default()
}

/// The settings restores waiting for one mod (one per profile).
pub fn pending_restores(dirs: &DataDirs, kind: ModKind, id: &str) -> Vec<PendingRestore> {
    if nothing_kept(dirs) { return Vec::new(); }
    view(dirs).and_then(|st| st.mods.into_iter().find(|m| m.kind == kind && m.id == id)).map(|m| m.restores).unwrap_or_default()
}

/// Is this Library entry a kept version?
pub fn is_kept(dirs: &DataDirs, r: &LibraryRef) -> bool {
    kept_versions(dirs, r.kind, &r.id).iter().any(|v| v.version == r.version && v.hash == r.hash)
}

/// Finishes or undoes what a stop left (the launcher's start and every change run it too). -> sentences for the player.
pub fn recover(dirs: &DataDirs) -> Vec<String> {
    if nothing_kept(dirs) { return Vec::new(); }
    match update(dirs, |_, _| ()) {
        Ok(Some((_, notes))) => notes,
        Ok(None) => Vec::new(),
        Err(e) => vec![format!("Kiln could not tidy its kept versions: {}", e.message)],
    }
}

/// A Library entry was removed: it is no longer kept, and its settings copy goes.
pub(crate) fn forget(dirs: &DataDirs, r: &LibraryRef) {
    if nothing_kept(dirs) { return; }
    let _ = update(dirs, |st, _| {
        if let Some(m) = st.mods.iter_mut().find(|m| m.kind == r.kind && m.id == r.id) {
            m.kept.retain(|v| !(v.version == r.version && v.hash == r.hash));
            m.restores.retain(|x| !(x.version == r.version && x.hash == r.hash));
        }
    });
}

/// Settings > "Earlier versions to keep" changed: every mod keeps at most that many now (the newest kept); the others
/// stay in the Library as ordinary versions (Clean up removes them), their settings copies deleted.
pub fn apply_count(dirs: &DataDirs) -> Result<()> {
    if nothing_kept(dirs) { return Ok(()); }
    let keep = keep_count(dirs) as usize;
    update(dirs, |st, _| for m in &mut st.mods { truncate_per_profile(&mut m.kept, keep); })?;
    Ok(())
}

/// At most `keep` kept versions per profile that left them (newest kept first).
fn truncate_per_profile(kept: &mut Vec<KeptVersion>, keep: usize) {
    let mut seen: BTreeMap<Option<String>, usize> = BTreeMap::new();
    kept.retain(|v| {
        let n = seen.entry(v.profile.clone()).or_insert(0);
        *n += 1;
        *n <= keep
    });
}

// ------------------------------------------------------------------------------------------------ settings copies

/// `<library>/kiln-kept/<kind>/<id>`, refused through a link anywhere below the data folder.
fn mod_dir(dirs: &DataDirs, kind: ModKind, id: &str) -> Result<PathBuf> {
    if !crate::manifest::is_mod_id(id) {
        return Err(Error::invalid(format!("invalid kept mod id {id:?}")));
    }
    let path = dirs.library().join(SETTINGS_DIR).join(kind.as_str()).join(id);
    crate::engine_mods::refuse_symlink_path(&dirs.root, &path)?;
    Ok(path)
}

/// One settings copy's folder (a name the state gives: one component, no dot first).
fn copy_dir(dirs: &DataDirs, kind: ModKind, id: &str, dir: &str) -> Result<PathBuf> {
    if dir.is_empty() || dir.starts_with('.') || dir.contains(['/', '\\', ':']) {
        return Err(Error::invalid(format!("invalid kept settings folder {dir:?}")));
    }
    let path = mod_dir(dirs, kind, id)?.join(dir);
    crate::engine_mods::refuse_symlink_path(&dirs.root, &path)?;
    Ok(path)
}

/// One of Kiln's game-folder copies (`<game>/kiln/state/library-placements.json`, written by `engine_mods::place`):
/// where it is, which Library entry it is, and each file's sha256 as placed.
#[derive(Debug, Clone, Deserialize)]
struct PlacedCopy {
    path: PathBuf,
    marker: crate::engine_mods::Marker,
    #[serde(default)]
    files: BTreeMap<String, String>,
}

fn placed_copies(game: &Path) -> Vec<PlacedCopy> {
    let file = game.join("kiln/state/library-placements.json");
    if crate::engine_mods::refuse_symlink_path(game, &file).is_err() { return Vec::new(); }
    crate::util::read_json::<Vec<PlacedCopy>>(&file).ok().flatten().unwrap_or_default()
}

fn copies_of(game: &Path, kind: ModKind, id: &str, version: &str, hash: &str) -> Vec<(PlacedCopy, String)> {
    placed_copies(game).into_iter()
        .filter(|c| c.marker.kind == kind && c.marker.id == id && c.marker.version == version && c.marker.hash == hash && c.path.is_dir())
        .filter_map(|c| {
            let rel = game_rel(game, &c.path)?;
            Some((c, rel))
        })
        .collect()
}

/// `copy` relative to `game`, '/' separated (None: not below it, or not a safe relative path).
fn game_rel(game: &Path, copy: &Path) -> Option<String> {
    let rel = copy.strip_prefix(game).ok()?.to_str()?.replace('\\', "/");
    (!rel.is_empty() && crate::bundle::safe_rel_path(&rel).is_ok()).then_some(rel)
}

/// What a settings copy never holds and a restore never writes: anything but a settings file or a log (by its
/// extension, [`crate::settings_files`]: the list placement calls settings, and logs, in which mods keep data too), Lua and
/// everything under `src/`, the manifests at the top of a copy (`mod.json`, `kiln.mod.json`, `native.json`,
/// `flame.json`, `pack.toml`, `init.lua`, `extended.mod.json`, `native-plugin.ini`, `shroudtopia.json`: the Library's
/// files, which placement treats as code), Kiln's marker and temporary files. Unknown extensions (scripts, DLLs, data
/// of unknown kind) are never restored: they stay as the newer version left them.
fn not_settings(rel: &str) -> bool {
    let low = rel.to_ascii_lowercase();
    let top = !low.contains('/');
    low.starts_with("src/") || !(crate::settings_files::is_settings_extension(&low) || crate::settings_files::is_log(&low))
        || (top && (matches!(low.as_str(), "mod.json" | "kiln.mod.json" | "native.json" | "flame.json" | "pack.toml" | "init.lua"
            | "extended.mod.json" | "native-plugin.ini" | "shroudtopia.json") || low.starts_with(crate::engine_mods::MARKER)))
        || low.ends_with(".kiln-tmp")
}

fn walk(dir: &Path, prefix: &str, out: &mut Vec<String>) -> Result<()> {
    for e in std::fs::read_dir(dir).map_err(crate::util::io_at(dir, "reading"))? {
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        let rel = if prefix.is_empty() { name } else { format!("{prefix}/{name}") };
        let meta = std::fs::symlink_metadata(e.path())?;
        if crate::quick::import_is_link(&meta) {
            return Err(Error::new(ErrorCode::Refused, format!("{}: links are not kept", e.path().display())));
        } else if meta.is_dir() {
            walk(&e.path(), &rel, out)?;
        } else if meta.is_file() {
            out.push(rel);
        }
    }
    Ok(())
}

/// Copies the settings of version `v` from its game-folder copies in the first of `games` that has one: each file not
/// as Kiln placed it (changed or added since) that is a settings file ([`not_settings`]), at its path relative to the
/// game folder, and a ShroudForge package's setting values. The files go into a staging folder beside their place,
/// which is renamed to its own new folder (`<version>+<hash8>.<12 hex>`) once whole. Err(why) when the disk check
/// refuses (nothing stays on disk then).
fn take_settings(dirs: &DataDirs, games: &[PathBuf], kind: ModKind, id: &str, v: &VersionRef) -> std::result::Result<KeptSettings, String> {
    let version_size = crate::library::record(dirs, &LibraryRef { kind, id: id.into(), version: v.version.clone(), hash: v.hash.clone() })
        .map(|r| r.size).unwrap_or(0);
    if let Some(why) = disk_refusal(dirs, version_size, 1) { return Err(why); }
    let parent = mod_dir(dirs, kind, id).map_err(|e| e.message)?;
    let h8 = crate::hash::hash8(&v.hash).unwrap_or("00000000");
    let (stage, tag) = new_stage(&parent, &v.version, h8).map_err(|e| e.message)?;
    let name = format!("{}+{h8}.{tag}", v.version);
    let mut out = KeptSettings::default();
    let copied = (|| -> std::result::Result<(), String> {
        for game in games {
            let copies = copies_of(game, kind, id, &v.version, &v.hash);
            if copies.is_empty() { continue; }
            out.game = Some(game.display().to_string());
            for (copy, rel_copy) in copies {
                let mut files = Vec::new();
                if let Err(e) = crate::engine_mods::refuse_symlink_path(game, &copy.path).and_then(|()| walk(&copy.path, "", &mut files)) {
                    out.left_out.push(format!("{rel_copy}: {}", e.message));
                    continue;
                }
                files.sort();
                for rel in files.iter().filter(|r| !not_settings(r)) {
                    let src = copy.path.join(rel);
                    if crate::bundle::safe_rel_path(rel).is_err() || crate::engine_mods::refuse_symlink_path(game, &src).is_err() { continue; }
                    let Ok(meta) = std::fs::metadata(&src) else { continue };
                    if meta.len() > MAX_FILE || out.size.saturating_add(meta.len()) > MAX_TOTAL {
                        out.left_out.push(format!("{rel_copy}/{rel}: too large to keep ({})", mib(meta.len())));
                        continue;
                    }
                    let Ok(bytes) = read_bounded(&src) else {
                        out.left_out.push(format!("{rel_copy}/{rel}: not readable"));
                        continue;
                    };
                    if copy.files.get(rel) == Some(&crate::hash::sha256_hex(&bytes)) { continue; } // as Kiln placed it
                    let size = bytes.len() as u64;
                    if out.size + size > MAX_TOTAL {
                        out.left_out.push(format!("{rel_copy}/{rel}: too large to keep ({})", mib(size)));
                        continue;
                    }
                    if let Some(why) = disk_refusal(dirs, version_size + out.size + size, out.files.len() as u64 + 2) { return Err(why); }
                    let game_rel = format!("{rel_copy}/{rel}");
                    let dst = stage.join(&game_rel);
                    let written = crate::engine_mods::refuse_symlink_path(&parent, &dst).and_then(|()| crate::util::write_atomic_exclusive(&dst, &bytes))
                        .and_then(|()| crate::durable::flush_file(&dst));
                    match written {
                        Ok(()) => { out.files.push(game_rel); out.size += size; crash_point(Step::SnapshotCopied); }
                        Err(e) => out.left_out.push(format!("{game_rel}: {}", e.message)),
                    }
                }
                // ShroudForge keeps its setting values in extended.mod.json (an EML package ShroudForge runs too)
                if matches!(kind, ModKind::ShroudForge | ModKind::Eml) && copy.path.join("extended.mod.json").is_file()
                    && let Ok(m) = crate::shroudforge::read_mod(&copy.path)
                    && !m.settings.is_empty()
                {
                    out.values.insert(rel_copy.clone(), m.settings.iter().map(|s| (s.key.clone(), s.value.clone())).collect());
                }
            }
            break;
        }
        Ok(())
    })();
    if let Err(why) = copied {
        let _ = std::fs::remove_dir_all(&stage);
        return Err(why);
    }
    if out.files.is_empty() {
        let _ = std::fs::remove_dir_all(&stage);
        return Ok(out);
    }
    // whole: its folders flushed, then into its own folder at once, that name flushed too, and the folders above it
    // (made now for a first copy): only then may the state name it (a stop before leaves the staged folder, or a
    // folder nothing names, which the next look deletes)
    let to = parent.join(&name);
    let published = crate::durable::flush_tree(&stage)
        .and_then(|()| crate::durable::rename_dir(&stage, &to))
        .and_then(|()| crate::durable::flush_dir(&to))
        .and_then(|()| parent.ancestors().skip(1).take(3).try_for_each(crate::durable::flush_dir));
    if let Err(e) = published {
        let _ = std::fs::remove_dir_all(&stage);
        let _ = std::fs::remove_dir_all(&to);
        return Err(format!("its settings could not be kept ({})", e.message));
    }
    crash_point(Step::SnapshotRenamed);
    out.dir = Some(name);
    Ok(out)
}

/// The client games (known to the launcher, and `extra`) whose `kiln/profile.json` names `profile_id`.
fn active_games(dirs: &DataDirs, profile_id: &str, extra: Option<&Path>) -> Vec<PathBuf> {
    let mut candidates: Vec<PathBuf> = extra.map(Path::to_path_buf).into_iter().collect();
    candidates.extend(crate::game::known(dirs).unwrap_or_default().into_iter().map(|g| g.path));
    let mut out: Vec<PathBuf> = Vec::new();
    for g in candidates {
        if out.iter().any(|o| crate::steam::same_path(o, &g)) { continue; }
        if crate::profile::read_game_profile(&g).ok().flatten().is_some_and(|f| f.id == profile_id) {
            out.push(g);
        }
    }
    out
}

// ------------------------------------------------------------------------------------------------ the transaction

/// The name a mod is shown with.
fn shown(dirs: &DataDirs, kind: ModKind, m: &ProfileMod) -> String {
    crate::library::record(dirs, &LibraryRef { kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() })
        .ok().and_then(|r| r.name).filter(|n| !n.trim().is_empty()).unwrap_or_else(|| m.id.clone())
}

/// A change of profile entries between versions, prepared before its profile is saved ([`prepare`]). It holds the
/// state's lock until [`Prepared::commit`] (the profile was saved) or [`Prepared::abort`] (it was not). Dropped without
/// either (a stop), the change's record stays, and the next look at the state finishes or undoes it.
pub struct Prepared {
    lock: Option<crate::util::LockedFile>,
    dirs: DataDirs,
    txn: Option<String>,
    notices: Vec<String>,
    /// Why the kept versions could not follow the change (its lock, the state file): an update goes on without
    /// keeping; a roll back does not go on (its version would get the newer version's settings).
    failed: Option<String>,
}

impl Prepared {
    fn none(dirs: &DataDirs, notices: Vec<String>) -> Prepared {
        Prepared { lock: None, dirs: dirs.clone(), txn: None, notices, failed: None }
    }

    fn failed(dirs: &DataDirs, why: String) -> Prepared {
        Prepared { failed: Some(why.clone()), ..Prepared::none(dirs, vec![format!("Kiln could not keep the earlier versions for a roll back: {why}")]) }
    }

    /// Why the kept versions could not follow the change, when they could not.
    pub fn failure(&self) -> Option<&str> {
        self.failed.as_deref()
    }

    /// The profile was saved: the change stands. -> sentences for the player (a version not kept, and why).
    pub fn commit(mut self) -> Vec<String> {
        if self.txn.is_some() { crash_point(Step::BeforeCommit); }
        if let Some(id) = self.txn.take()
            && let Err(e) = self.finish(&id, false)
        {
            // the record stays; the next look at the state finishes the change (the profile lists it)
            self.notices.push(format!("Kiln finishes keeping the earlier versions at its next start: {}", e.message));
        }
        self.lock = None;
        std::mem::take(&mut self.notices)
    }

    /// The profile was not saved: the kept versions go back to what they were.
    pub fn abort(mut self) {
        if let Some(id) = self.txn.take() {
            let _ = self.finish(&id, true);
        }
        self.lock = None;
    }

    fn finish(&self, id: &str, undo: bool) -> Result<()> {
        let Some(mut st) = load(&self.dirs) else { return Ok(()) };
        if let Some(i) = st.pending.iter().position(|t| t.id == id) {
            let t = st.pending.remove(i);
            if undo {
                for b in t.before { put_mod(&mut st, b); }
            }
        }
        tidy(&mut st);
        save(&self.dirs, &st)?;
        sweep(&self.dirs, &st);
        Ok(())
    }
}

/// Prepares the change of `profile` (as it is about to be saved) that replaced entries by other versions of the same
/// mods (`changes`: kind, the entry before, the entry after; others are left out): per change the version left becomes
/// a kept one, with its settings as its game-folder copies hold them now (in the games where the profile is active;
/// `game`: one more), unless `keepVersions` is 0 or the disk check refuses; a kept version that comes back leaves the
/// kept ones and its settings wait for its next placement in this profile; each mod keeps at most `keepVersions`; this
/// profile's restores its new content no longer asks for are cancelled. Never fails the change it prepares: what
/// could not be done is in the sentences of [`Prepared::commit`].
pub(crate) fn prepare(dirs: &DataDirs, profile: &Profile, game: Option<&Path>, changes: &[(ModKind, ProfileMod, ProfileMod)]) -> Prepared {
    let changes: Vec<_> = changes.iter().filter(|(_, a, b)| a.id == b.id && (a.version != b.version || a.hash != b.hash)).cloned().collect();
    if changes.is_empty() && nothing_kept(dirs) { return Prepared::none(dirs, Vec::new()); }
    let lock = match lock(dirs) {
        Ok(l) => l,
        Err(e) => return Prepared::failed(dirs, e.message),
    };
    let Some(mut st) = load(dirs) else {
        return Prepared::failed(dirs, "its list of them (library/kiln-kept.json) is unreadable, invalid, or of a newer Kiln".into());
    };
    let mut notices = Vec::new();
    let fixed = reconcile(dirs, &mut st, &mut notices);
    let original = st.clone();
    let games = active_games(dirs, &profile.id, game);
    let keep = keep_count(dirs) as usize;
    // the mods this change touches, as they are now (what an undo puts back)
    let stale = |m: &KeptMod| m.restores.iter().any(|r| r.profile == profile.id && !lists(profile, m.kind, &m.id, &r.version, &r.hash));
    let mut touched: Vec<(ModKind, String)> = changes.iter().map(|(k, f, _)| (*k, f.id.clone())).collect();
    touched.extend(st.mods.iter().filter(|m| stale(m)).map(|m| (m.kind, m.id.clone())));
    touched.sort();
    touched.dedup();
    let before: Vec<KeptMod> = touched.iter()
        .map(|(k, id)| st.mods.iter().find(|m| m.kind == *k && m.id == *id).cloned().unwrap_or_else(|| KeptMod::empty(*k, id))).collect();
    for (kind, from, to) in &changes {
        if let Err(e) = follow_one(dirs, &mut st, &profile.id, &games, keep, *kind, from, to, &mut notices) {
            sweep(dirs, &original);
            return Prepared::failed(dirs, e.message);
        }
    }
    // a removal of the mod (or another version picked by hand) cancels this profile's restore it no longer asks for
    for m in &mut st.mods {
        let (kind, id) = (m.kind, m.id.clone());
        m.restores.retain(|r| r.profile != profile.id || lists(profile, kind, &id, &r.version, &r.hash));
    }
    tidy(&mut st);
    if st == original {
        if fixed && save(dirs, &st).is_ok() { sweep(dirs, &st); }
        return Prepared::none(dirs, notices);
    }
    let id = match hex12() {
        Ok(id) => id,
        Err(e) => {
            // nothing names the copies this change made: they go, and the state stays as it was
            sweep(dirs, &original);
            return Prepared::failed(dirs, e.message);
        }
    };
    st.pending.push(KeptTxn {
        id: id.clone(), profile: profile.id.clone(), started: crate::util::now_rfc3339(),
        changes: changes.iter().map(|(kind, a, b)| TxnChange { kind: *kind, id: a.id.clone(), from: VersionRef::of(a), to: VersionRef::of(b) }).collect(),
        before,
        profile_before: crate::profile::get(dirs, &profile.id).ok().map(|p| p.mods),
    });
    if let Err(e) = save(dirs, &st) {
        // nothing names the copies this change made: they go, and the state stays as it was
        sweep(dirs, &original);
        return Prepared::failed(dirs, e.message);
    }
    crash_point(Step::IndexWritten);
    Prepared { lock: Some(lock), dirs: dirs.clone(), txn: Some(id), notices, failed: None }
}

#[allow(clippy::too_many_arguments)]
fn follow_one(dirs: &DataDirs, st: &mut KeptState, profile_id: &str, games: &[PathBuf], keep: usize, kind: ModKind,
              from: &ProfileMod, to: &ProfileMod, notices: &mut Vec<String>) -> Result<()> {
    let name = shown(dirs, kind, from);
    let (from_v, to_v) = (VersionRef::of(from), VersionRef::of(to));
    let m = mod_of(st, kind, &from.id);
    // this profile's roll back to `from` that was never placed: the settings waiting for it are still the ones it has
    let mut from_settings = None;
    if let Some(i) = m.restores.iter().position(|r| r.profile == profile_id && r.version == from.version && r.hash == from.hash && !r.applied) {
        from_settings = Some(m.restores.remove(i).settings);
    }
    // a version this profile kept comes back: its settings wait for its next placement in this profile (one restore per
    // profile). One another profile kept stays that profile's.
    if let Some(i) = m.kept.iter().position(|v| v.version == to.version && v.hash == to.hash && v.of_profile(profile_id)) {
        let v = m.kept.remove(i);
        m.restores.retain(|r| r.profile != profile_id);
        if !v.settings.is_empty() {
            m.restores.push(PendingRestore { version: v.version, hash: v.hash, profile: profile_id.to_string(), preimages: Some(capture_preimages(dirs, kind, &from.id, &v.settings)?), settings: v.settings, applied: false });
        }
    }
    // the version left is kept
    if keep > 0 && entry_exists(dirs, kind, &from.id, &from.version, &from.hash) {
        let settings = from_settings.map(Ok).unwrap_or_else(|| take_settings(dirs, games, kind, &from.id, &from_v));
        let size = crate::library::record(dirs, &LibraryRef { kind, id: from.id.clone(), version: from.version.clone(), hash: from.hash.clone() })
            .map(|r| r.size).unwrap_or(0);
        let settings = settings.and_then(|s| match disk_refusal(dirs, size + s.size, s.files.len() as u64 + 1) {
            Some(why) => Err(why),
            None => Ok(s),
        });
        match settings {
            Err(why) => {
                m.kept.retain(|v| !(v.version == from.version && v.hash == from.hash && v.of_profile(profile_id)));
                notices.push(format!("{name} {} was not kept for a roll back: {why}. It stays in your Library as an earlier version.", from.version));
            }
            Ok(s) => {
                for left in &s.left_out {
                    notices.push(format!("{name} {}: kept without {left}", from.version));
                }
                m.kept.retain(|v| !(v.version == from.version && v.hash == from.hash && v.of_profile(profile_id)));
                m.kept.insert(0, KeptVersion {
                    version: from.version.clone(), hash: from.hash.clone(), kept: crate::util::now_rfc3339(), replaced_by: Some(to_v),
                    profile: Some(profile_id.to_string()), settings: s,
                });
            }
        }
    }
    truncate_per_profile(&mut m.kept, keep);
    Ok(())
}

/// [`prepare`] for the changes of an add or its Undo ([`ProfileChange`]: `before` -> `after`, or back with `undo`).
pub(crate) fn prepare_changes(dirs: &DataDirs, profile: &Profile, game: Option<&Path>, changes: &[ProfileChange], undo: bool) -> Prepared {
    let list: Vec<(ModKind, ProfileMod, ProfileMod)> = changes.iter()
        .filter_map(|c| c.before.clone().map(|b| if undo { (c.kind, c.after.clone(), b) } else { (c.kind, b, c.after.clone()) }))
        .collect();
    prepare(dirs, profile, game, &list)
}

/// [`prepare`] for a profile saved as a whole (the launcher's `profile_save`: "Use <version>" in a mod's menu, the Deck;
/// `kilnctl profile add`): every entry that is another version of the same mod than in `before`. Call it before the
/// save, then commit or abort.
pub fn prepare_saved(dirs: &DataDirs, before: &Profile, after: &Profile) -> Prepared {
    if before.id != after.id { return Prepared::none(dirs, Vec::new()); }
    let mut changes = Vec::new();
    for kind in ModKind::ALL {
        for m in after.mods.list(kind) {
            if let Some(b) = before.mods.list(kind).iter().find(|b| b.id == m.id) {
                changes.push((kind, b.clone(), m.clone()));
            }
        }
    }
    prepare(dirs, after, None, &changes)
}

/// [`prepare_saved`] and its commit for a profile already saved (the settings are read as they are now).
pub fn follow_saved(dirs: &DataDirs, before: &Profile, after: &Profile) -> Vec<String> {
    prepare_saved(dirs, before, after).commit()
}

// ------------------------------------------------------------------------------------------------ roll back

/// "Roll back to <version>": in profile `profile_id`, the mod `kind`/`id` uses the kept version `to` (None: the newest
/// one this profile's change kept; another profile's kept versions are that profile's) instead of its current entry, switched on or off as it was. The profile is saved, nothing else: the
/// caller writes the game's profile.json again as after any profile save, and the kept settings go back into the
/// mod's game-folder copy at that placement ([`restore_after_place`]). The version rolled away from becomes the kept
/// one, with its settings as its copies hold them now (in the games where the profile is active; `game`: one more).
/// Refused (NotFound) when the profile does not list the mod or the version is not kept (any more).
pub fn rollback(dirs: &DataDirs, game: Option<&crate::game::GameInstall>, profile_id: &str, kind: ModKind, id: &str, to: Option<&VersionRef>)
                -> Result<RollbackReport> {
    let _quick = crate::quick::lock(dirs)?;
    let mut p = crate::profile::get(dirs, profile_id)?;
    let Some(i) = p.mods.list(kind).iter().position(|m| m.id == id) else {
        return Err(Error::not_found(format!("{id} is not in the profile {}: add it there first", p.name)));
    };
    let before = p.mods.list(kind)[i].clone();
    let name = shown(dirs, kind, &before);
    let kept: Vec<KeptVersion> = kept_versions(dirs, kind, id).into_iter()
        .filter(|v| v.of_profile(profile_id) && !(v.version == before.version && v.hash == before.hash)).collect();
    let pick = match to {
        Some(t) => kept.iter().find(|v| v.version == t.version && v.hash == t.hash).ok_or_else(|| Error::not_found(format!(
            "{name} {} is not kept for a roll back (any more): it was removed, or a newer roll back took its place.", t.version)))?,
        None => kept.first().ok_or_else(|| Error::not_found(format!("No earlier version of {name} is kept for a roll back.")))?,
    };
    let after = ProfileMod { id: id.to_string(), version: pick.version.clone(), hash: pick.hash.clone(), enabled: before.enabled };
    let settings = pick.settings.files.clone();
    // settings kept in a game folder this profile does not run in now (it moved): said, they wait for that folder
    let moved = pick.settings.game.clone().filter(|g| !pick.settings.is_empty()
        && !active_games(dirs, profile_id, game.map(|g| g.path.as_path())).iter().any(|a| same_game_path(a, Path::new(g))));
    p.mods.list_mut(kind)[i] = after.clone();
    let prepared = prepare(dirs, &p, game.map(|g| g.path.as_path()), &[(kind, before.clone(), after.clone())]);
    // without its record the rolled back version would get the newer version's settings: nothing changes then
    if let Some(why) = prepared.failure() {
        return Err(Error::new(ErrorCode::Io, format!("{name} was not rolled back: Kiln could not write its kept versions ({why}). Nothing was changed.")));
    }
    let saved = match crate::profile::save(dirs, &p) {
        Ok(s) => s,
        Err(e) => {
            prepared.abort();
            return Err(e);
        }
    };
    let mut notices = prepared.commit();
    let is_kept = keep_count(dirs) > 0 && kept_versions(dirs, kind, id).iter()
        .any(|v| v.version == before.version && v.hash == before.hash && v.of_profile(profile_id));
    if keep_count(dirs) == 0 {
        notices.push(format!("{name} {} is not kept (Settings keeps no earlier versions): it stays in your Library as an earlier version.", before.version));
    }
    if let Some(g) = moved {
        notices.push(format!("{name} {}: its settings were kept in {g}, a game folder this profile does not run in now (did it move?). They go back only there; here it starts with the settings it has.",
            pick.version));
    }
    Ok(RollbackReport {
        profile_id: saved.id.clone(), profile_name: saved.name.clone(), kind, id: id.to_string(), name,
        from: VersionRef::of(&before), to: VersionRef::of(&after),
        changes: vec![ProfileChange { kind, before: Some(before), after }],
        kept: is_kept, settings, notices,
    })
}

// ------------------------------------------------------------------------------------------------ restore

/// Is `game` the game folder a restore's settings came from (any, when it does not say)?
fn same_game(r: &PendingRestore, game: &Path) -> bool {
    r.settings.game.as_deref().is_none_or(|g| same_game_path(Path::new(g), game))
}

/// After `engine_mods::place` placed `profile` in `game`: the settings a roll back of this profile kept, in this game,
/// go back into the copies of the version it put into the profile (each file written over the one the placement carried
/// over from the newer version; ShroudForge values through ShroudForge's setter). The restore stays recorded until every
/// file is back (a file that cannot be written now is tried again at the next placement; one Kiln must never write, a
/// link, is left out with a note), so a stop half-way is finished by the next placement. A restore of this profile for
/// a version it no longer lists is dropped; one whose copy is not there (the mod is switched off) waits; other profiles'
/// and other games' restores are left alone. -> one sentence per mod restored, and one per file not put back.
pub fn restore_after_place(dirs: &DataDirs, profile: &Profile, game: &Path) -> Vec<String> {
    if nothing_kept(dirs) { return Vec::new(); }
    let known: Vec<PathBuf> = crate::game::known(dirs).unwrap_or_default().into_iter().map(|g| g.path).collect();
    // 1. the files back and flushed, the restore marked done in one durable write
    let done = update(dirs, |st, notes| {
        let mut any = false;
        for m in &mut st.mods {
            let (kind, id) = (m.kind, m.id.clone());
            let mut waiting = Vec::new();
            for mut r in std::mem::take(&mut m.restores) {
                if r.profile != profile.id || r.applied { waiting.push(r); continue; }
                if !same_game(&r, game) {
                    // its game folder moved (a Steam library moved, the game installed again elsewhere): it waits there
                    if let Some(g) = r.settings.game.as_deref().filter(|g| !known.iter().any(|k| same_game_path(k, Path::new(g)))) {
                        notes.push(moved_note(kind, &id, &r.version, g));
                    }
                    waiting.push(r);
                    continue;
                }
                if !lists(profile, kind, &id, &r.version, &r.hash) { continue; } // the profile moved on: dropped
                let copies = copies_of(game, kind, &id, &r.version, &r.hash);
                if copies.is_empty() { waiting.push(r); continue; }
                if apply_restore(dirs, game, kind, &id, &r, &copies, notes) { r.applied = true; any = true; }
                waiting.push(r);
            }
            m.restores = waiting;
        }
        any
    });
    let (any, mut notes) = match done {
        Ok(Some((any, notes))) => (any, notes),
        Ok(None) => return Vec::new(),
        Err(e) => return vec![format!("Kiln could not put back the settings of a rolled back mod: {} (it tries again at the next start)", e.message)],
    };
    // 2. the records of the restores done dropped (a stop or failure here: the next look drops them, nothing is put
    // back again)
    if any && let Err(e) = update(dirs, |_, _| ()) {
        notes.push(format!("Kiln tidies the settings it put back at its next start: {}", e.message));
    }
    notes
}

fn same_game_path(a: &Path, b: &Path) -> bool {
    crate::steam::same_path(&crate::steam::canon(a), &crate::steam::canon(b))
}

/// The note of a restore that waits for a game folder Kiln no longer knows.
fn moved_note(kind: ModKind, id: &str, version: &str, game: &str) -> String {
    format!("{}:{id}: the settings version {version} had before the update were kept in {game}, which is not a game folder Kiln knows now (it moved?); they go back only there, so this copy keeps the settings it has",
        kind.as_str())
}

fn file_image(game: &Path, path: &Path) -> Result<Option<String>> {
    crate::engine_mods::refuse_symlink_path(game, path)?;
    match std::fs::metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
        Ok(_) => Ok(Some(crate::hash::sha256_hex(&read_bounded(path)?))),
    }
}

fn value_image(value: &serde_json::Value) -> String {
    crate::hash::sha256_hex(&serde_json::to_vec(value).expect("JSON value serializes"))
}

fn capture_preimages(dirs: &DataDirs, kind: ModKind, id: &str, settings: &KeptSettings) -> Result<RestorePreimages> {
    let mut images = RestorePreimages::default();
    let game = settings.game.as_deref().map(Path::new).ok_or_else(|| Error::invalid("restore settings have no source game"))?;
    if !settings.files.is_empty() {
        let root = copy_dir(dirs, kind, id, settings.dir.as_deref().ok_or_else(|| Error::invalid("restore has no snapshot directory"))?)?;
        for rel in &settings.files {
            crate::bundle::safe_rel_path(rel)?;
            let source = root.join(rel);
            crate::engine_mods::refuse_symlink_path(&dirs.root, &source)?;
            images.files.insert(rel.clone(), RestoreGuard { before: file_image(game, &game.join(rel))?,
                snapshot: crate::hash::sha256_hex(&read_bounded(&source)?) });
        }
    }
    for (rel, values) in &settings.values {
        crate::bundle::safe_rel_path(rel)?;
        let copy = game.join(rel);
        crate::engine_mods::refuse_symlink_path(game, &copy.join("extended.mod.json"))?;
        let now = if copy.join("extended.mod.json").try_exists()? { Some(crate::shroudforge::read_mod(&copy)?) } else { None };
        let mut guards = BTreeMap::new();
        for (key, target) in values {
            guards.insert(key.clone(), RestoreGuard { before: now.as_ref().and_then(|m| m.settings.iter().find(|s| s.key == *key)).map(|s| value_image(&s.value)), snapshot: value_image(target) });
        }
        images.values.insert(rel.clone(), guards);
    }
    Ok(images)
}

// A missing guard is an unshipped legacy record: matching bytes are settled, everything else is preserved.
fn may_restore(guard: Option<&RestoreGuard>, current: &Option<String>, snapshot: &str) -> bool {
    guard.is_some_and(|g| g.snapshot == snapshot && g.before == *current)
}

/// Puts one restore's files and values back. -> true when it is done (false: try again at the next placement).
fn apply_restore(dirs: &DataDirs, game: &Path, kind: ModKind, id: &str, r: &PendingRestore, copies: &[(PlacedCopy, String)], notes: &mut Vec<String>) -> bool {
    let tag = format!("{}:{id}", kind.as_str());
    let dir = match r.settings.dir.as_deref().map(|d| copy_dir(dirs, kind, id, d)) {
        Some(Ok(d)) => Some(d),
        Some(Err(e)) => { notes.push(format!("{tag}: its settings were not put back: {}", e.message)); return true; }
        None => None,
    };
    let mut again = false;
    let mut back = Vec::new();
    let mut preserved = Vec::new();
    for rel in &r.settings.files {
        let Some(dir) = &dir else { break };
        if crate::bundle::safe_rel_path(rel).is_err() { continue; }
        let Some((copy, c)) = copies.iter().find(|(_, c)| rel.starts_with(&format!("{c}/"))) else {
            notes.push(format!("{tag}: {rel} was not put back: the mod's copy is somewhere else now"));
            continue;
        };
        // the file and the name it has in its folder are flushed before the state may say it is back
        let to = copy.path.join(&rel[c.len() + 1..]);
        let written = (|| -> Result<bool> {
            crate::engine_mods::refuse_symlink_path(dir, &dir.join(rel))?;
            let snapshot = crate::hash::sha256_hex(&read_bounded(&dir.join(rel))?);
            let current = file_image(game, &to)?;
            let guard = r.preimages.as_ref().and_then(|p| p.files.get(rel));
            if current.as_deref() == Some(&snapshot) {
                flush_restored_path(game, &to)?;
                return Ok(false);
            }
            if !may_restore(guard, &current, &snapshot) {
                preserved.push(rel.clone());
                return Ok(false);
            }
            let changed = put_back(game, &copy.path, &rel[c.len() + 1..], dir, &dir.join(rel))?;
            flush_restored_path(game, &to)?;
            Ok(changed)
        })();
        match written {
            Ok(true) => { back.push(rel.clone()); crash_point(Step::RestoreFile); }
            Ok(false) => {}
            Err(e) if matches!(e.code, ErrorCode::Refused | ErrorCode::InvalidArgument) => notes.push(format!("{tag}: {rel} was not put back: {}", e.message)),
            Err(e) => { again = true; notes.push(format!("{tag}: {rel} was not put back yet ({}): Kiln tries again at the next start", e.message)); }
        }
    }
    for (rel_copy, values) in &r.settings.values {
        let Some((copy, _)) = copies.iter().find(|(_, c)| c == rel_copy) else { continue };
        let file = copy.path.join("extended.mod.json");
        if let Err(e) = crate::engine_mods::refuse_symlink_path(game, &file)
            .and_then(|()| crate::engine_mods::refuse_symlink_path(game, &crate::util::with_suffix(&file, ".tmp"))) {
            notes.push(format!("{tag}: its settings were not put back: {}", e.message));
            continue;
        }
        let Ok(now) = crate::shroudforge::read_mod(&copy.path) else { continue };
        for (key, value) in values {
            let current = now.settings.iter().find(|s| s.key == *key).map(|s| value_image(&s.value));
            let target = value_image(value);
            if current.as_deref() != Some(&target) {
                let guard = r.preimages.as_ref().and_then(|p| p.values.get(rel_copy)).and_then(|v| v.get(key));
                if !may_restore(guard, &current, &target) {
                    preserved.push(format!("{rel_copy} setting {key}"));
                    continue;
                }
                match crate::shroudforge::set_setting(&copy.path, key, value) {
                    Ok(_) => back.push(format!("{rel_copy} setting {key}")),
                    Err(e) if e.code == ErrorCode::InvalidArgument => notes.push(format!("{tag}: its setting {key} was not put back to {value}: {}", e.message)),
                    Err(e) => { again = true; notes.push(format!("{tag}: its setting {key} was not put back yet ({})", e.message)); }
                }
            }
        }
        // Re-flush the file and ancestor names, including values already equal on a replay, before acknowledging it.
        if let Err(e) = flush_restored_path(game, &file) {
            again = true;
            notes.push(format!("{tag}: its setting values are not durable yet ({}); Kiln tries again", e.message));
        }
    }
    if !preserved.is_empty() {
        let legacy = if r.preimages.is_none() { " (legacy restore has no preimages)" } else { "" };
        notes.push(format!("{tag}: kept the player's later change in {}{legacy}", preserved.join(", ")));
    }
    if !back.is_empty() {
        notes.push(format!("{tag}: version {} has the settings it had before the update again ({})", r.version, back.join(", ")));
    }
    !again
}

fn flush_restored_path(game: &Path, file: &Path) -> Result<()> {
    crate::engine_mods::refuse_symlink_path(game, file)?;
    crate::durable::flush_file(file)?;
    for parent in file.ancestors().skip(1).take_while(|p| p.starts_with(game)) {
        crate::durable::flush_dir(parent)?;
    }
    Ok(())
}

/// Writes the kept file `from` as `rel` in the copy `copy` (in `game`) unless it holds those bytes already. Never a
/// code file, never through a link. -> whether it was written.
fn put_back(game: &Path, copy: &Path, rel: &str, source_root: &Path, from: &Path) -> Result<bool> {
    crate::bundle::safe_rel_path(rel)?;
    if not_settings(rel) {
        return Err(Error::new(ErrorCode::Refused, "not a settings file"));
    }
    let to = copy.join(rel);
    crate::engine_mods::refuse_symlink_path(game, &to)?;
    if !from.starts_with(source_root) { return Err(Error::invalid("settings source outside its copy")); }
    crate::engine_mods::refuse_symlink_path(source_root, from)?;
    let bytes = read_bounded(from)?;
    if read_bounded(&to).is_ok_and(|now| now == bytes) { return Ok(false); }
    if let Some(parent) = to.parent() {
        crate::engine_mods::refuse_symlink_path(game, parent)?;
        std::fs::create_dir_all(parent).map_err(crate::util::io_at(parent, "creating"))?;
    }
    crate::durable::replace(&to, &bytes)?;
    Ok(true)
}

// Bound allocations even if a file grows after metadata was inspected.
fn read_bounded(path: &Path) -> Result<Vec<u8>> {
    use std::io::Read;
    let file = std::fs::File::open(path)?;
    if !file.metadata()?.is_file() || file.metadata()?.len() > MAX_FILE {
        return Err(Error::new(ErrorCode::Refused, "not a bounded settings file"));
    }
    let mut bytes = Vec::new();
    file.take(MAX_FILE + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_FILE {
        return Err(Error::new(ErrorCode::Refused, "settings file too large"));
    }
    Ok(bytes)
}

#[cfg(test)]
#[path = "kept_tests.rs"]
mod crash_tests;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn code_and_manifests_are_never_settings() {
        for rel in ["mod.json", "MOD.JSON", "src/mod.lua", "init.lua", "map.dll", "x/y.asi", "native-plugin.ini", "extended.mod.json",
                    ".kiln-library.json", ".kiln-library.json.tmp", "flame.json", "a.luac"] {
            assert!(not_settings(rel), "{rel}");
        }
        for rel in ["config.txt", "config.ini", "settings.json", "minimap_config.ini", "logs/run.log", "data/mod.json"] {
            assert!(!not_settings(rel), "{rel}");
        }
    }

    /// What the mods Kiln's tests and the engines' pages know write into their folders is kept; what placement calls
    /// code is not. Where this rule and `engine_mods::is_game_settings_file` differ, on purpose: logs (`.log`) are kept
    /// (mods keep data in them), and the other engines' manifests at the top of a copy are not (the Library's files).
    #[test]
    fn what_real_mods_write_is_kept_and_their_code_is_not() {
        // Auto Loot Plus (EML part and Lite), XHL mods, The Worst Map Mod (Greeneye), SET, Creative Mode (Flame)
        for rel in ["config.txt", "quest_items.txt", "config.ini", "minimap_config.ini", "looted_chests.ini", "unlock_map_markers.log",
                    "settings.json", "logs/creative.log", "logs/flame.log", "notice.txt", "sigs/1730000000-1234.json"] {
            assert!(!not_settings(rel), "{rel}");
        }
        for rel in ["native-plugin.ini", "XHL-Auto-Loot.dll", "src/autosmash_program.lua", "mod.json", "flame.json", "creative.dll",
                    "kiln.mod.json", "native.json", "pack.toml", "shroudtopia.json", "cache.dat", "save.bin"] {
            assert!(not_settings(rel), "{rel}");
        }
    }

    /// A kept version holds exactly the settings files of the one list (and logs), in any folder of the copy.
    #[test]
    fn kept_settings_follow_the_one_list() {
        for ext in crate::settings_files::SETTINGS_EXTENSIONS.iter().chain(&crate::settings_files::LOG_EXTENSIONS) {
            assert!(!not_settings(&format!("data/x.{ext}")), "{ext}");
            assert!(!not_settings(&format!("X.{}", ext.to_ascii_uppercase())), "{ext}");
        }
        for rel in ["x.bat", "x.cmd", "x.ps1", "x.sh", "x.dll", "x.exe", "x.asi", "x.lua", "x.dat", "x"] {
            assert!(not_settings(rel), "{rel}");
        }
    }

    #[test]
    fn review_disk_refusal_happens_before_settings_are_copied() {
        let root = std::env::temp_dir().join(format!("km-review-space-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.join("data"));
        let game = root.join("game");
        let copy = game.join("greeneye/map");
        std::fs::create_dir_all(&copy).unwrap();
        std::fs::write(copy.join("config.ini"), b"zoom=1").unwrap();
        let hash = format!("kdh1:{}", "0".repeat(64));
        let marker = crate::engine_mods::Marker { format: 1, kind: ModKind::Greeneye, id: "map".into(), version: "1.0.0".into(), hash: hash.clone() };
        std::fs::create_dir_all(game.join("kiln/state")).unwrap();
        crate::util::write_json_atomic(&game.join("kiln/state/library-placements.json"),
            &serde_json::json!([{"path": copy, "marker": marker, "files": {}}])).unwrap();
        let saved = with_free_space_for_tests(Some(0), || take_settings(&dirs, &[game], ModKind::Greeneye, "map",
            &VersionRef { version: "1.0.0".into(), hash }));
        assert!(saved.is_err(), "a refused snapshot must not first write files to a full disk");
        assert!(!dirs.library().join(SETTINGS_DIR).join("greeneye/map").read_dir().is_ok_and(|mut d| d.next().is_some()));
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn review_scripts_are_not_settings() {
        for rel in ["run.bat", "run.CMD", "setup.ps1", "code.js", "code.py", "code.vbs", "code.sh"] {
            assert!(not_settings(rel), "{rel}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn review_restore_does_not_follow_temporary_link() {
        let root = std::env::temp_dir().join(format!("km-review-tmp-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let copy = root.join("game/mod");
        std::fs::create_dir_all(&copy).unwrap();
        let outside = root.join("outside");
        std::fs::write(&outside, b"precious").unwrap();
        let from = root.join("saved");
        std::fs::write(&from, b"settings").unwrap();
        std::os::unix::fs::symlink(&outside, copy.join("config.ini.tmp")).unwrap();
        assert!(put_back(&root.join("game"), &copy, "config.ini", &root, &from).unwrap());
        assert_eq!(std::fs::read(copy.join("config.ini")).unwrap(), b"settings");
        assert_eq!(std::fs::read(&outside).unwrap(), b"precious");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn review_restore_refuses_linked_source_and_oversized_file() {
        let root = std::env::temp_dir().join(format!("km-review-source-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let copy = root.join("game/mod");
        std::fs::create_dir_all(&copy).unwrap();
        let outside = root.join("outside");
        std::fs::write(&outside, b"private").unwrap();
        let from = root.join("saved");
        std::os::unix::fs::symlink(&outside, &from).unwrap();
        assert!(put_back(&root.join("game"), &copy, "config.ini", &root, &from).is_err());
        std::fs::remove_file(&from).unwrap();
        std::fs::File::create(&from).unwrap().set_len(MAX_FILE + 1).unwrap();
        assert!(put_back(&root.join("game"), &copy, "config.ini", &root, &from).is_err());
        assert!(!copy.join("config.ini").exists());
        std::fs::remove_dir_all(root).unwrap();
    }

    /// The sweep never deletes through a linked folder below `kiln-kept/` (a mod's folder that is a link elsewhere).
    #[cfg(unix)]
    #[test]
    fn review_forget_does_not_delete_through_kept_ancestor_link() {
        let root = std::env::temp_dir().join(format!("km-review-delete-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.join("data"));
        let outside = root.join("outside");
        let victim = outside.join("1.0.0+00000000.0123456789ab");
        std::fs::create_dir_all(&victim).unwrap();
        std::fs::write(victim.join("precious"), b"save").unwrap();
        let parent = dirs.library().join(SETTINGS_DIR).join("greeneye");
        std::fs::create_dir_all(&parent).unwrap();
        std::os::unix::fs::symlink(&outside, parent.join("map")).unwrap();
        std::fs::write(state_path(&dirs), br#"{"format": 1, "mods": []}"#).unwrap();
        forget(&dirs, &LibraryRef { kind: ModKind::Greeneye, id: "map".into(), version: "1.0.0".into(), hash: format!("kdh1:{}", "0".repeat(64)) });
        assert_eq!(recover(&dirs), Vec::<String>::new());
        assert!(victim.join("precious").is_file());
        assert!(copy_dir(&dirs, ModKind::Greeneye, "map", "1.0.0+00000000.0123456789ab").is_err(), "refused through the link");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn keep_count_reads_settings_with_a_default_and_a_maximum() {
        let root = std::env::temp_dir().join(format!("km-kept-count-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let dirs = DataDirs::at(root.clone());
        assert_eq!(keep_count(&dirs), KEEP_DEFAULT);
        for (text, want) in [(r#"{"keepVersions": 0}"#, 0), (r#"{"keepVersions": 2}"#, 2), (r#"{"keepVersions": 99}"#, KEEP_MAX),
                             (r#"{"keepVersions": "x"}"#, KEEP_DEFAULT), (r#"{"savesKeep": 3}"#, KEEP_DEFAULT)] {
            std::fs::write(dirs.settings(), text).unwrap();
            assert_eq!(keep_count(&dirs), want, "{text}");
        }
        let _ = std::fs::remove_dir_all(root);
    }

    /// The schema's example is a file this code reads, and the schema lists every field this code writes.
    #[test]
    fn the_schema_example_reads_and_the_schema_lists_the_fields() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../schema");
        let example: KeptState = serde_json::from_slice(&std::fs::read(root.join("examples/kiln-kept.json")).unwrap()).unwrap();
        assert_eq!((example.mods.len(), example.mods[0].kept[0].settings.files.len()), (2, 2));
        assert_eq!(example.mods[1].restores[0].settings.values["mods/sfpack"]["speed"], 7);
        assert_eq!(example.pending.len(), 1);
        let schema: serde_json::Value = serde_json::from_slice(&std::fs::read(root.join("kiln-kept.schema.json")).unwrap()).unwrap();
        let keys = |v: &serde_json::Value| -> Vec<String> {
            let mut k: Vec<String> = v["properties"].as_object().unwrap().keys().cloned().collect();
            k.sort();
            k
        };
        let written = |v: &serde_json::Value| -> Vec<String> {
            let mut k: Vec<String> = v.as_object().unwrap().keys().cloned().collect();
            k.sort();
            k
        };
        let settings = KeptSettings { dir: Some("1.0.0+00000000.0123456789ab".into()), files: vec!["a/b.ini".into()], size: 1,
            values: [("a".to_string(), serde_json::Map::new())].into_iter().collect(), game: Some("g".into()), left_out: vec!["x".into()] };
        let v = VersionRef { version: "1.0.0".into(), hash: "kdh1:00".into() };
        let m = KeptMod { kind: ModKind::Greeneye, id: "m".into(),
            kept: vec![KeptVersion { version: "1.0.0".into(), hash: "kdh1:00".into(), kept: "t".into(), replaced_by: Some(v.clone()),
                profile: Some("p".into()), settings: settings.clone() }],
            restores: vec![PendingRestore { version: "1.0.0".into(), hash: "kdh1:00".into(), profile: "p".into(), settings, preimages: Some(RestorePreimages::default()), applied: true }] };
        let st = KeptState { format: 1, mods: vec![m.clone()], pending: vec![KeptTxn { id: "0123456789ab".into(), profile: "p".into(), started: "t".into(),
            changes: vec![TxnChange { kind: ModKind::Greeneye, id: "m".into(), from: v.clone(), to: v.clone() }], before: vec![m], profile_before: Some(crate::profile::ProfileMods::default()) }] };
        let json = serde_json::to_value(&st).unwrap();
        assert_eq!(keys(&schema), written(&json));
        assert_eq!(keys(&schema["$defs"]["mod"]), written(&json["mods"][0]));
        assert_eq!(keys(&schema["$defs"]["version"]), written(&json["mods"][0]["kept"][0]));
        assert_eq!(keys(&schema["$defs"]["versionRef"]), written(&json["mods"][0]["kept"][0]["replacedBy"]));
        assert_eq!(keys(&schema["$defs"]["restore"]), written(&json["mods"][0]["restores"][0]));
        assert_eq!(keys(&schema["$defs"]["settings"]), written(&json["mods"][0]["restores"][0]["settings"]));
        assert_eq!(keys(&schema["$defs"]["txn"]), written(&json["pending"][0]));
        assert_eq!(keys(&schema["$defs"]["txnChange"]), written(&json["pending"][0]["changes"][0]));
    }

    #[test]
    fn a_state_of_a_newer_format_is_read_as_nothing_and_never_written() {
        let root = std::env::temp_dir().join(format!("km-kept-format-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.clone());
        std::fs::create_dir_all(dirs.library()).unwrap();
        std::fs::write(state_path(&dirs), r#"{"format": 2, "mods": "later"}"#).unwrap();
        assert!(load(&dirs).is_none());
        assert!(update(&dirs, |_, _| ()).unwrap().is_none());
        assert_eq!(std::fs::read_to_string(state_path(&dirs)).unwrap(), r#"{"format": 2, "mods": "later"}"#);
        let _ = std::fs::remove_dir_all(root);
    }
}
