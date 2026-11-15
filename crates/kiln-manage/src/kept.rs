//! Kept versions (0.2.0-dev.29): when an update replaces a mod's Library entry in a profile (a dropped archive, a Nexus
//! Mods install, the Undo of a roll back), the entry it replaced is kept for a roll back, together with a copy of the
//! settings files the mod's game-folder copies held at that moment. How many per mod: `keepVersions` in the launcher's
//! `settings.json` (default [`KEEP_DEFAULT`], at most [`KEEP_MAX`]; 0 keeps none, as before dev.29). A version is not
//! kept when the Library's disk would then have less free than the data folder check asks for
//! ([`crate::data_folder::needed_bytes`]); the update's result says so.
//!
//! A roll back ([`rollback`]) puts a kept entry back into the profile in place of the mod's current one. It is an
//! ordinary profile change: its Undo is the launcher's (`quick::revert`, `profile_revert`), and the version rolled
//! away from becomes the kept one (so a second roll back returns to it). The roll back itself writes nothing in the
//! game folder: the next placement of the profile (`profile::activate` / `refresh_active`, what a profile's re-apply
//! and Play run) places the version as always, carrying the newer version's settings over, and then puts the kept
//! settings back into the copy ([`restore_after_place`]), so the player gets the settings they had, not the new ones.
//!
//! On disk (docs/kiln/launcher/contracts.md section 3): `<library>/kiln-kept.json` ([`KeptState`]: which entries are
//! kept, per mod, and a settings restore waiting for its placement) and `<library>/kiln-kept/<kind>/<id>/<version>+<hash8>/`
//! (the settings copy, each file at its path relative to the game folder). A kept entry is an ordinary Library entry
//! otherwise: a launcher before dev.29 lists it as another version of the mod and its Clean up may remove it; an entry
//! that is gone is forgotten here. Kept entries are listed after the others ([`crate::library::list`]), so nothing
//! picks one by default; Clean up ([`crate::library::unused`]) leaves them.

use crate::library::LibraryRef;
use crate::manifest::ModKind;
use crate::paths::DataDirs;
use crate::profile::ProfileMod;
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
    fn is(&self, version: &str, hash: &str) -> bool {
        self.version == version && self.hash == hash
    }
}

/// The settings kept with a version: the files of its game-folder copies that were not as Kiln placed them (settings
/// and the other files the mod or the player wrote there; never code or a manifest), and ShroudForge setting values.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptSettings {
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
    /// The game folder they were read in.
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
    #[serde(default)]
    pub settings: KeptSettings,
}

/// Settings waiting for the next placement of a version a roll back put into a profile ([`restore_after_place`]).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PendingRestore {
    pub version: String,
    pub hash: String,
    /// The profile that rolled back.
    pub profile: String,
    pub settings: KeptSettings,
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
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub restore: Option<PendingRestore>,
}

/// `<library>/kiln-kept.json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeptState {
    pub format: u32,
    #[serde(default)]
    pub mods: Vec<KeptMod>,
}

impl Default for KeptState {
    fn default() -> Self {
        KeptState { format: 1, mods: Vec::new() }
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
        Ok(Some(v)) if v.get("format").and_then(serde_json::Value::as_u64) == Some(1) => Some(serde_json::from_value(v).unwrap_or_default()),
        Ok(Some(_)) => None,
        // unreadable: started again (what it named are ordinary Library entries)
        Err(_) => Some(KeptState::default()),
    }
}

fn save(dirs: &DataDirs, st: &KeptState) -> Result<()> {
    crate::util::write_json_atomic(&state_path(dirs), st)
}

/// The state with the entries that are gone from the Library (removed by hand, by a launcher before dev.29, taken
/// over) left out, and their settings copies deleted. `changed`: something was left out.
fn pruned(dirs: &DataDirs, mut st: KeptState) -> (KeptState, bool) {
    let mut changed = false;
    for m in &mut st.mods {
        let (kind, id) = (m.kind, m.id.clone());
        let there = |version: &str, hash: &str| entry_exists(dirs, kind, &id, version, hash);
        let before = m.kept.len();
        let mut gone = Vec::new();
        m.kept.retain(|v| { let keep = there(&v.version, &v.hash); if !keep { gone.push((v.version.clone(), v.hash.clone())); } keep });
        changed |= m.kept.len() != before;
        if m.restore.as_ref().is_some_and(|r| !there(&r.version, &r.hash)) {
            let r = m.restore.take().expect("checked");
            gone.push((r.version, r.hash));
            changed = true;
        }
        for (version, hash) in gone {
            drop_settings(dirs, kind, &id, &version, &hash);
        }
    }
    let before = st.mods.len();
    st.mods.retain(|m| !m.kept.is_empty() || m.restore.is_some());
    changed |= st.mods.len() != before;
    (st, changed)
}

fn entry_exists(dirs: &DataDirs, kind: ModKind, id: &str, version: &str, hash: &str) -> bool {
    crate::library::record(dirs, &LibraryRef { kind, id: id.into(), version: version.into(), hash: hash.into() }).is_ok()
}

/// Changes the state under its lock, with the gone entries pruned first; saved when `f` says so (or the prune
/// changed something). Nothing happens with a state of a newer format.
fn update<T>(dirs: &DataDirs, f: impl FnOnce(&mut KeptState) -> (bool, T)) -> Result<Option<T>> {
    let _lock = lock(dirs)?;
    let Some(st) = load(dirs) else { return Ok(None) };
    let (mut st, pruned) = pruned(dirs, st);
    let before = st.clone();
    let (write, out) = f(&mut st);
    st.mods.retain(|m| !m.kept.is_empty() || m.restore.is_some());
    if (write && st != before) || pruned {
        save(dirs, &st)?;
    }
    Ok(Some(out))
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
    st.mods.push(KeptMod { kind, id: id.to_string(), kept: Vec::new(), restore: None });
    st.mods.last_mut().expect("pushed")
}

/// The kept versions of every mod, read only (`library::list`): by `<kind>/<id>/<hash>`.
pub(crate) fn info(dirs: &DataDirs) -> std::collections::HashMap<String, KeptInfo> {
    let mut out = std::collections::HashMap::new();
    let Some(st) = load(dirs) else { return out };
    for m in st.mods {
        for v in m.kept {
            out.insert(info_key(m.kind, &m.id, &v.hash), KeptInfo {
                kept: v.kept, replaced_by: v.replaced_by.map(|r| r.version), settings: v.settings.files, settings_size: v.settings.size,
            });
        }
    }
    out
}

pub(crate) fn info_key(kind: ModKind, id: &str, hash: &str) -> String {
    format!("{}/{id}/{hash}", kind.as_str())
}

/// The kept versions of one mod, newest kept first (entries gone from the Library left out).
pub fn kept_versions(dirs: &DataDirs, kind: ModKind, id: &str) -> Vec<KeptVersion> {
    let Some(st) = load(dirs) else { return Vec::new() };
    pruned(dirs, st).0.mods.into_iter().find(|m| m.kind == kind && m.id == id).map(|m| m.kept).unwrap_or_default()
}

/// Is this Library entry a kept version?
pub fn is_kept(dirs: &DataDirs, r: &LibraryRef) -> bool {
    kept_versions(dirs, r.kind, &r.id).iter().any(|v| v.version == r.version && v.hash == r.hash)
}

/// A Library entry was removed: it is no longer kept, and its settings copy goes.
pub(crate) fn forget(dirs: &DataDirs, r: &LibraryRef) {
    if nothing_kept(dirs) { return; }
    let _ = update(dirs, |st| {
        let Some(m) = st.mods.iter_mut().find(|m| m.kind == r.kind && m.id == r.id) else { return (false, ()) };
        let before = m.kept.len();
        m.kept.retain(|v| !(v.version == r.version && v.hash == r.hash));
        let restore = m.restore.as_ref().is_some_and(|x| x.version == r.version && x.hash == r.hash);
        if restore { m.restore = None; }
        let changed = restore || m.kept.len() != before;
        if changed { drop_settings(dirs, r.kind, &r.id, &r.version, &r.hash); }
        (changed, ())
    });
}

/// Settings > "Previous versions to keep" changed: every mod keeps at most that many now (the newest kept); the
/// others stay in the Library as ordinary versions (Clean up removes them), their settings copies deleted.
pub fn apply_count(dirs: &DataDirs) -> Result<()> {
    if nothing_kept(dirs) { return Ok(()); }
    let keep = keep_count(dirs) as usize;
    update(dirs, |st| {
        let mut changed = false;
        for m in &mut st.mods {
            while m.kept.len() > keep {
                let v = m.kept.pop().expect("longer than keep");
                if !m.restore.as_ref().is_some_and(|r| r.version == v.version && r.hash == v.hash) {
                    drop_settings(dirs, m.kind, &m.id, &v.version, &v.hash);
                }
                changed = true;
            }
        }
        (changed, ())
    })?;
    Ok(())
}

// ------------------------------------------------------------------------------------------------ settings copies

/// `<library>/kiln-kept/<kind>/<id>/<version>+<hash8>`.
fn settings_dir(dirs: &DataDirs, kind: ModKind, id: &str, version: &str, hash: &str) -> Result<PathBuf> {
    let entry = dirs.library_entry(kind, id, version, hash)?;
    let name = entry.file_name().ok_or_else(|| Error::invalid("library entry without a name"))?;
    if !crate::manifest::is_mod_id(id) || version.contains(['/', '\\', ':']) || version.starts_with('.') {
        return Err(Error::invalid(format!("invalid kept version {id} {version}")));
    }
    let path = dirs.library().join(SETTINGS_DIR).join(kind.as_str()).join(id).join(name);
    crate::engine_mods::refuse_symlink_path(&dirs.root, &path)?;
    Ok(path)
}

fn drop_settings(dirs: &DataDirs, kind: ModKind, id: &str, version: &str, hash: &str) {
    if let Ok(d) = settings_dir(dirs, kind, id, version, hash) {
        let _ = std::fs::remove_dir_all(&d);
        // the mod's and the kind's folders go when they are empty
        for up in d.ancestors().skip(1).take(2) {
            if std::fs::remove_dir(up).is_err() { break; }
        }
    }
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
            let rel = c.path.strip_prefix(game).ok()?.to_str()?.replace('\\', "/");
            (!rel.is_empty() && crate::bundle::safe_rel_path(&rel).is_ok()).then_some((c, rel))
        })
        .collect()
}

/// Only known settings extensions and logs are eligible, excluding engine manifests and markers.
/// Unknown extensions may contain executable scripts and are never restored as settings.
fn not_settings(rel: &str) -> bool {
    let low = rel.to_ascii_lowercase();
    let ext = low.rsplit_once('.').map(|(_, x)| x).unwrap_or("");
    let top = !low.contains('/');
    low.starts_with("src/") || !matches!(ext, "txt" | "ini" | "cfg" | "json" | "toml" | "yaml" | "yml" | "log")
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

/// Copies the settings of `version` from its game-folder copies in the first of `games` that has one into `into`
/// (emptied first): each file not as Kiln placed it (changed or added since) that is no code, at its path relative to
/// the game folder, and a ShroudForge package's setting values.
fn take_settings(dirs: &DataDirs, games: &[PathBuf], kind: ModKind, id: &str, v: &VersionRef, into: &Path) -> std::result::Result<KeptSettings, String> {
    let version_size = crate::library::record(dirs, &LibraryRef { kind, id: id.into(), version: v.version.clone(), hash: v.hash.clone() })
        .map(|r| r.size).unwrap_or(0);
    if let Some(why) = disk_refusal(dirs, version_size, 1) { return Err(why); }
    let mut out = KeptSettings::default();
    let _ = std::fs::remove_dir_all(into);
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
                if crate::bundle::safe_rel_path(rel).is_err()
                    || crate::engine_mods::refuse_symlink_path(game, &src).is_err() { continue; }
                let Ok(meta) = std::fs::metadata(&src) else { continue };
                if meta.len() > MAX_FILE || out.size.saturating_add(meta.len()) > MAX_TOTAL {
                    out.left_out.push(format!("{rel_copy}/{rel}: too large to keep ({})", mib(meta.len())));
                    continue;
                }
                let Ok((sha, size)) = crate::hash::sha256_file(&src) else {
                    out.left_out.push(format!("{rel_copy}/{rel}: not readable"));
                    continue;
                };
                if copy.files.get(rel) == Some(&sha) { continue; } // as Kiln placed it
                if size > MAX_FILE || out.size + size > MAX_TOTAL {
                    out.left_out.push(format!("{rel_copy}/{rel}: too large to keep ({})", mib(size)));
                    continue;
                }
                let game_rel = format!("{rel_copy}/{rel}");
                let dst = into.join(&game_rel);
                if let Some(why) = disk_refusal(dirs, version_size + out.size + size, out.files.len() as u64 + 2) {
                    return Err(why);
                }
                let copied = (|| -> Result<u64> {
                    crate::engine_mods::refuse_symlink_path(into, &dst)?;
                    let bytes = read_bounded(&src)?;
                    if out.size + bytes.len() as u64 > MAX_TOTAL { return Err(Error::invalid("settings copy too large")); }
                    crate::util::write_atomic_exclusive(&dst, &bytes)?;
                    Ok(bytes.len() as u64)
                })();
                match copied {
                    Ok(bytes) => { out.files.push(game_rel); out.size += bytes; }
                    Err(e) => out.left_out.push(format!("{game_rel}: {e}")),
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

// ------------------------------------------------------------------------------------------------ following changes

/// The name a mod is shown with.
fn shown(dirs: &DataDirs, kind: ModKind, m: &ProfileMod) -> String {
    crate::library::record(dirs, &LibraryRef { kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() })
        .ok().and_then(|r| r.name).filter(|n| !n.trim().is_empty()).unwrap_or_else(|| m.id.clone())
}

/// What follows a profile change that replaced mod entries by other versions of the same mods (`changes`: kind, the
/// entry before, the entry now), in `profile_id`: per change, the version left becomes a kept one (with its settings
/// as its game-folder copies hold them now, in the games where the profile is active; `game`: one more game to look
/// in), unless `keepVersions` is 0 or the disk check refuses; a kept version that came back leaves the kept ones, and
/// its settings wait for its next placement ([`restore_after_place`]); each mod keeps at most `keepVersions`.
/// Never fails the change it follows: what could not be done is in the sentences returned.
pub(crate) fn follow(dirs: &DataDirs, profile_id: &str, game: Option<&Path>, changes: &[(ModKind, ProfileMod, ProfileMod)]) -> Vec<String> {
    let changes: Vec<_> = changes.iter().filter(|(_, a, b)| a.id == b.id && (a.version != b.version || a.hash != b.hash)).collect();
    if changes.is_empty() && nothing_kept(dirs) { return Vec::new(); }
    let saved = crate::profile::get(dirs, profile_id).ok();
    let games = active_games(dirs, profile_id, game);
    let keep = keep_count(dirs) as usize;
    let mut notices = Vec::new();
    let done = update(dirs, |st| {
        for (kind, from, to) in &changes {
            follow_one(dirs, st, profile_id, &games, keep, *kind, from, to, &mut notices);
        }
        // A removal can be saved and undone before any placement. Cancel its restore at the save,
        // rather than letting a later, unrelated add of the same version consume it.
        if let Some(saved) = &saved {
            for m in &mut st.mods {
                let stale = m.restore.as_ref().is_some_and(|r| r.profile == profile_id
                    && !saved.mods.list(m.kind).iter().any(|e| e.id == m.id && e.version == r.version && e.hash == r.hash));
                if stale {
                    let r = m.restore.take().expect("checked");
                    if !m.kept.iter().any(|v| v.version == r.version && v.hash == r.hash) {
                        drop_settings(dirs, m.kind, &m.id, &r.version, &r.hash);
                    }
                }
            }
        }
        (true, ())
    });
    match done {
        Ok(Some(())) => {}
        Ok(None) => notices.push("Kiln did not keep the earlier versions: their list (library/kiln-kept.json) is of a newer Kiln.".into()),
        Err(e) => notices.push(format!("Kiln could not keep the earlier versions for a roll back: {}", e.message)),
    }
    notices
}

#[allow(clippy::too_many_arguments)]
fn follow_one(dirs: &DataDirs, st: &mut KeptState, profile_id: &str, games: &[PathBuf], keep: usize, kind: ModKind,
              from: &ProfileMod, to: &ProfileMod, notices: &mut Vec<String>) {
    let name = shown(dirs, kind, from);
    let (from_v, to_v) = (VersionRef::of(from), VersionRef::of(to));
    let m = mod_of(st, kind, &from.id);
    // a roll back to `from` that was never placed: the settings waiting for it are still the ones it has
    let waiting = m.restore.take();
    let mut from_settings = None;
    if let Some(r) = waiting {
        if r.version == from.version && r.hash == from.hash {
            from_settings = Some(r.settings);
        } else if !m.kept.iter().any(|v| v.version == r.version && v.hash == r.hash) && !to_v.is(&r.version, &r.hash) {
            // another version's, left behind by a change Kiln did not follow: no longer wanted
            drop_settings(dirs, kind, &from.id, &r.version, &r.hash);
        }
    }
    // a kept version comes back: its settings go back at its next placement
    if let Some(i) = m.kept.iter().position(|v| v.version == to.version && v.hash == to.hash) {
        let v = m.kept.remove(i);
        if v.settings.is_empty() {
            drop_settings(dirs, kind, &from.id, &v.version, &v.hash);
        } else {
            m.restore = Some(PendingRestore { version: v.version, hash: v.hash, profile: profile_id.to_string(), settings: v.settings });
        }
    }
    // the version left is kept
    let Ok(dir) = settings_dir(dirs, kind, &from.id, &from.version, &from.hash) else { return };
    if keep == 0 {
        if !m.kept.iter().any(|v| v.version == from.version && v.hash == from.hash) {
            drop_settings(dirs, kind, &from.id, &from.version, &from.hash);
        }
    } else if entry_exists(dirs, kind, &from.id, &from.version, &from.hash) {
        let settings = from_settings.map(Ok).unwrap_or_else(|| take_settings(dirs, games, kind, &from.id, &from_v, &dir));
        let size = crate::library::record(dirs, &LibraryRef { kind, id: from.id.clone(), version: from.version.clone(), hash: from.hash.clone() })
            .map(|r| r.size).unwrap_or(0);
        let settings = settings.and_then(|settings| match disk_refusal(dirs, size + settings.size, settings.files.len() as u64 + 1) {
            Some(why) => Err(why),
            None => Ok(settings),
        });
        match settings {
            Err(why) => {
                drop_settings(dirs, kind, &from.id, &from.version, &from.hash);
                m.kept.retain(|v| !(v.version == from.version && v.hash == from.hash));
                notices.push(format!("{name} {} was not kept for a roll back: {why}. It stays in your Library as an earlier version.", from.version));
            }
            Ok(settings) => {
                for left in &settings.left_out {
                    notices.push(format!("{name} {}: kept without {left}", from.version));
                }
                m.kept.retain(|v| !(v.version == from.version && v.hash == from.hash));
                m.kept.insert(0, KeptVersion {
                    version: from.version.clone(), hash: from.hash.clone(), kept: crate::util::now_rfc3339(), replaced_by: Some(to_v.clone()), settings,
                });
            }
        }
    }
    while m.kept.len() > keep {
        let v = m.kept.pop().expect("longer than keep");
        drop_settings(dirs, kind, &from.id, &v.version, &v.hash);
    }
}

/// [`follow`] for the changes of an add or its Undo ([`ProfileChange`]: `before` -> `after`, or back with `undo`).
pub(crate) fn follow_profile_changes(dirs: &DataDirs, profile_id: &str, game: Option<&Path>, changes: &[ProfileChange], undo: bool) -> Vec<String> {
    let list: Vec<(ModKind, ProfileMod, ProfileMod)> = changes.iter()
        .filter_map(|c| c.before.clone().map(|b| if undo { (c.kind, c.after.clone(), b) } else { (c.kind, b, c.after.clone()) }))
        .collect();
    follow(dirs, profile_id, game, &list)
}

/// A profile saved as a whole (the launcher's `profile_save`: "Use <version>" in a mod's menu, the Deck; `kilnctl profile
/// add`): every entry that is another version of the same mod than in `before` follows as at an update ([`follow`]).
/// -> sentences for the player (a version not kept, and why).
pub fn follow_saved(dirs: &DataDirs, before: &crate::profile::Profile, after: &crate::profile::Profile) -> Vec<String> {
    if before.id != after.id { return Vec::new(); }
    let mut changes = Vec::new();
    for kind in ModKind::ALL {
        for m in after.mods.list(kind) {
            if let Some(b) = before.mods.list(kind).iter().find(|b| b.id == m.id) {
                changes.push((kind, b.clone(), m.clone()));
            }
        }
    }
    follow(dirs, &after.id, None, &changes)
}

// ------------------------------------------------------------------------------------------------ roll back

/// "Roll back to <version>": in profile `profile_id`, the mod `kind`/`id` uses the kept version `to` (None: the newest
/// kept one) instead of its current entry, switched on or off as it was. The profile is saved, nothing else: the
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
    let kept: Vec<KeptVersion> = kept_versions(dirs, kind, id).into_iter().filter(|v| !(v.version == before.version && v.hash == before.hash)).collect();
    let pick = match to {
        Some(t) => kept.iter().find(|v| v.version == t.version && v.hash == t.hash).ok_or_else(|| Error::not_found(format!(
            "{name} {} is not kept for a roll back (any more): it was removed, or a newer roll back took its place.", t.version)))?,
        None => kept.first().ok_or_else(|| Error::not_found(format!("No earlier version of {name} is kept for a roll back.")))?,
    };
    let after = ProfileMod { id: id.to_string(), version: pick.version.clone(), hash: pick.hash.clone(), enabled: before.enabled };
    let settings = pick.settings.files.clone();
    p.mods.list_mut(kind)[i] = after.clone();
    let saved = crate::profile::save(dirs, &p)?;
    let notices = follow(dirs, &saved.id, game.map(|g| g.path.as_path()), &[(kind, before.clone(), after.clone())]);
    let is_kept = keep_count(dirs) > 0 && kept_versions(dirs, kind, id).iter().any(|v| v.version == before.version && v.hash == before.hash);
    let mut notices = notices;
    if keep_count(dirs) == 0 {
        notices.push(format!("{name} {} is not kept (Settings keeps no earlier versions): it stays in your Library as an earlier version.", before.version));
    }
    Ok(RollbackReport {
        profile_id: saved.id.clone(), profile_name: saved.name.clone(), kind, id: id.to_string(), name,
        from: VersionRef::of(&before), to: VersionRef::of(&after),
        changes: vec![ProfileChange { kind, before: Some(before), after }],
        kept: is_kept, settings, notices,
    })
}

// ------------------------------------------------------------------------------------------------ restore

/// After `engine_mods::place` placed `profile` in `game`: the settings a roll back kept go back into the copies of the
/// version it put into the profile (each file written over the one the placement carried over from the newer version;
/// ShroudForge values through ShroudForge's setter), once. A restore for a version the profile no longer lists is
/// dropped; one whose copy is not there yet (the mod is switched off) waits. -> one sentence per mod restored, and one
/// per file or value that could not be put back.
pub fn restore_after_place(dirs: &DataDirs, profile: &crate::profile::Profile, game: &Path) -> Vec<String> {
    let mut notes = Vec::new();
    if nothing_kept(dirs) { return notes; }
    let done = update(dirs, |st| {
        let mut changed = false;
        for m in &mut st.mods {
            let Some(r) = m.restore.clone().filter(|r| r.profile == profile.id) else { continue };
            let entry = profile.mods.list(m.kind).iter().find(|e| e.id == m.id);
            let wanted = entry.is_some_and(|e| e.version == r.version && e.hash == r.hash);
            if !wanted {
                m.restore = None;
                if !m.kept.iter().any(|v| v.version == r.version && v.hash == r.hash) {
                    drop_settings(dirs, m.kind, &m.id, &r.version, &r.hash);
                }
                changed = true;
                continue;
            }
            let copies = copies_of(game, m.kind, &m.id, &r.version, &r.hash);
            if copies.is_empty() { continue; }
            let Ok(dir) = settings_dir(dirs, m.kind, &m.id, &r.version, &r.hash) else { continue };
            let tag = format!("{}:{}", m.kind.as_str(), m.id);
            let mut back = Vec::new();
            for rel in &r.settings.files {
                if crate::bundle::safe_rel_path(rel).is_err() { continue; }
                let Some((copy, c)) = copies.iter().find(|(_, c)| rel.starts_with(&format!("{c}/"))) else {
                    notes.push(format!("{tag}: {rel} was not put back: the mod's copy is somewhere else now"));
                    continue;
                };
                match put_back(game, &copy.path, &rel[c.len() + 1..], &dir, &dir.join(rel)) {
                    Ok(true) => back.push(rel.clone()),
                    Ok(false) => {}
                    Err(e) => notes.push(format!("{tag}: {rel} was not put back: {}", e.message)),
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
                    if now.settings.iter().any(|s| s.key == *key && s.value != *value) {
                        match crate::shroudforge::set_setting(&copy.path, key, value) {
                            Ok(_) => back.push(format!("{rel_copy} setting {key}")),
                            Err(e) => notes.push(format!("{tag}: its setting {key} was not put back to {value}: {}", e.message)),
                        }
                    }
                }
            }
            if !back.is_empty() {
                notes.push(format!("{tag}: version {} has the settings it had before the update again ({})", r.version, back.join(", ")));
            }
            m.restore = None;
            drop_settings(dirs, m.kind, &m.id, &r.version, &r.hash);
            changed = true;
        }
        (changed, ())
    });
    if let Err(e) = done {
        notes.push(format!("Kiln could not put back the settings of a rolled back mod: {}", e.message));
    }
    notes
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
    crate::util::write_atomic_exclusive(&to, &bytes)?;
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
        let into = dirs.library().join(SETTINGS_DIR).join("greeneye/map/1.0.0+00000000");
        let saved = with_free_space_for_tests(Some(0), || take_settings(&dirs, &[game], ModKind::Greeneye, "map",
            &VersionRef { version: "1.0.0".into(), hash }, &into));
        assert!(saved.is_err(), "a refused snapshot must not first write files to a full disk");
        assert!(!into.exists());
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

    #[cfg(unix)]
    #[test]
    fn review_forget_does_not_delete_through_kept_ancestor_link() {
        let root = std::env::temp_dir().join(format!("km-review-delete-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.join("data"));
        let outside = root.join("outside");
        let victim = outside.join("1.0.0+00000000");
        std::fs::create_dir_all(&victim).unwrap();
        std::fs::write(victim.join("precious"), b"save").unwrap();
        let parent = dirs.library().join(SETTINGS_DIR).join("greeneye");
        std::fs::create_dir_all(&parent).unwrap();
        std::os::unix::fs::symlink(&outside, parent.join("map")).unwrap();
        drop_settings(&dirs, ModKind::Greeneye, "map", "1.0.0", &format!("kdh1:{}", "0".repeat(64)));
        assert!(victim.join("precious").is_file());
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
        assert_eq!(example.mods[1].restore.as_ref().unwrap().settings.values["mods/sfpack"]["speed"], 7);
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
        let settings = KeptSettings { files: vec!["a/b.ini".into()], size: 1, values: [("a".to_string(), serde_json::Map::new())].into_iter().collect(),
            game: Some("g".into()), left_out: vec!["x".into()] };
        let v = VersionRef { version: "1.0.0".into(), hash: "kdh1:00".into() };
        let st = KeptState { format: 1, mods: vec![KeptMod { kind: ModKind::Greeneye, id: "m".into(),
            kept: vec![KeptVersion { version: "1.0.0".into(), hash: "kdh1:00".into(), kept: "t".into(), replaced_by: Some(v.clone()), settings: settings.clone() }],
            restore: Some(PendingRestore { version: "1.0.0".into(), hash: "kdh1:00".into(), profile: "p".into(), settings }) }] };
        let json = serde_json::to_value(&st).unwrap();
        assert_eq!(keys(&schema), written(&json));
        assert_eq!(keys(&schema["$defs"]["mod"]), written(&json["mods"][0]));
        assert_eq!(keys(&schema["$defs"]["version"]), written(&json["mods"][0]["kept"][0]));
        assert_eq!(keys(&schema["$defs"]["versionRef"]), written(&json["mods"][0]["kept"][0]["replacedBy"]));
        assert_eq!(keys(&schema["$defs"]["restore"]), written(&json["mods"][0]["restore"]));
        assert_eq!(keys(&schema["$defs"]["settings"]), written(&json["mods"][0]["restore"]["settings"]));
    }

    #[test]
    fn a_state_of_a_newer_format_is_read_as_nothing_and_never_written() {
        let root = std::env::temp_dir().join(format!("km-kept-format-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.clone());
        std::fs::create_dir_all(dirs.library()).unwrap();
        std::fs::write(state_path(&dirs), r#"{"format": 2, "mods": "later"}"#).unwrap();
        assert!(load(&dirs).is_none());
        assert_eq!(update(&dirs, |_| (true, ())).unwrap(), None);
        assert_eq!(std::fs::read_to_string(state_path(&dirs)).unwrap(), r#"{"format": 2, "mods": "later"}"#);
        let _ = std::fs::remove_dir_all(root);
    }
}
