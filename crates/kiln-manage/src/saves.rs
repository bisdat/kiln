//! Save slots of a client (Steam userdata/<id>/1203620/remote/*.ksd; under Proton inside the prefix's Steam
//! folder), automatic backups (before a launch whose mod-set fingerprint changed, before a modpack is applied; keep
//! 10 per game) in `<data>/profiles/<pid>/save-backups/` or `<data>/backups/saves/<gameId>/` without a profile, and
//! restore (refused while the game runs). `editor` carries the Save Editor's commands, Linux-aware.
//!
//! Where a client's saves are (checked on the Steam build of 2026-11): every Steam root's
//! `userdata/<steamid>/1203620/remote/` (`character<N>.ksd`, `world<N>.ksd`, `global.ksd`, `user_settings.json`; the
//! game's own rolling copies in `remote/backup/` are not backed up), plus the local settings in
//! `Saved Games/keen games/Enshrouded/` (`<pfx>/drive_c/users/steamuser/...` under Proton, `%USERPROFILE%` on
//! Windows). A backup is `<backup dir>/<id>/` with one sub folder per source (`steam-<steamid>/`, `local/`) and
//! `backup.json`; restore puts every file back byte-identically.

use crate::game::{GameInstall, GameKind};
use crate::hash;
use crate::paths::DataDirs;
use crate::steam::{self, SteamRoot};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub mod editor_storage;

pub const KEEP: usize = 10;
pub const BACKUP_JSON: &str = "backup.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SlotKind {
    World,
    Character,
    Other,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveSlot {
    pub kind: SlotKind,
    pub file: String,
    pub path: String,
    pub modified: String,
    pub size: u64,
    pub steam_id: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BackupReason {
    Launch,
    Modpack,
    Manual,
    Restore,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveBackup {
    pub id: String,
    pub created: String,
    pub reason: BackupReason,
    pub profile_id: Option<String>,
    pub fingerprint: Option<String>,
    pub note: Option<String>,
    pub files: u32,
    pub bytes: u64,
}

/// One folder holding saves: `label` names its sub folder in a backup (`steam-<steamid>`, `local`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveDir {
    pub label: String,
    pub path: PathBuf,
    pub steam_id: Option<String>,
    /// Walk sub folders (the local settings folder) or only the top level (Steam's remote/, whose backup/ is the
    /// game's own).
    pub recursive: bool,
}

/// One file of a backup (`backup.json` `entries[]`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BackupEntry {
    /// The SaveDir label it came from.
    pub dir: String,
    /// '/' separated, relative to the SaveDir (and to `<backup>/<dir>/`).
    pub rel: String,
    /// The host path it was copied from (restore target when no SaveDir has the label any more).
    pub source: String,
    pub size: u64,
    pub sha256: String,
}

/// `<backup>/backup.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BackupManifest {
    pub format: u32,
    pub game_id: String,
    #[serde(flatten)]
    pub backup: SaveBackup,
    pub entries: Vec<BackupEntry>,
}

// ---- time (UTC) ----

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = (if z >= 0 { z } else { z - 146_096 }) / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    (yoe + era * 400 + i64::from(m <= 2), m, d)
}

fn utc(t: SystemTime) -> (i64, u32, u32, u32, u32, u32) {
    let secs = match t.duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    };
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    let (y, mo, d) = civil_from_days(days);
    (y, mo, d, (rem / 3600) as u32, (rem / 60 % 60) as u32, (rem % 60) as u32)
}

/// `2026-11-04T12:00:00Z`
pub(crate) fn rfc3339(t: SystemTime) -> String {
    let (y, mo, d, h, mi, s) = utc(t);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}Z")
}

/// `20261104T120000Z` (backup folder names).
pub(crate) fn stamp(t: SystemTime) -> String {
    let (y, mo, d, h, mi, s) = utc(t);
    format!("{y:04}{mo:02}{d:02}T{h:02}{mi:02}{s:02}Z")
}

// ---- where saves are ----

/// The slot kind a file name suggests: `character*` -> Character, `world*` / an 8-hex-digit world id -> World.
pub fn slot_kind(file: &str) -> SlotKind {
    let lower = file.to_ascii_lowercase();
    let stem = lower.split('.').next().unwrap_or("");
    if stem.starts_with("character") {
        SlotKind::Character
    } else if stem.starts_with("world") || (stem.len() == 8 && stem.bytes().all(|b| b.is_ascii_hexdigit())) {
        SlotKind::World
    } else {
        SlotKind::Other
    }
}

/// The Windows user's Saved Games folder (its known-folder location, which may have been moved off `%USERPROFILE%`).
fn windows_saved_games() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        crate::steam::win::saved_games()
    }
    #[cfg(not(windows))]
    {
        std::env::var_os("USERPROFILE").map(|h| Path::new(&h).join("Saved Games"))
    }
}

fn local_settings_dirs(install: &GameInstall) -> Vec<PathBuf> {
    let mut v = Vec::new();
    if cfg!(windows) || install.platform == crate::game::Platform::Windows {
        if let Some(base) = windows_saved_games() {
            v.push(base.join("keen games").join("Enshrouded"));
            v.push(base.join("Enshrouded"));
        }
    } else if let Some(pfx) = &install.prefix {
        let base = pfx.join("drive_c/users/steamuser/Saved Games");
        v.push(base.join("keen games/Enshrouded"));
        v.push(base.join("Enshrouded"));
    }
    v
}

/// The folders holding this client's saves, given the Steam roots (servers keep theirs in the server folder: none).
pub fn save_dirs_in(install: &GameInstall, roots: &[SteamRoot]) -> Vec<SaveDir> {
    let mut out: Vec<SaveDir> = Vec::new();
    if install.kind != GameKind::Client {
        return out;
    }
    let preferred: Vec<SteamRoot> = match install.library.as_deref().and_then(|l| steam::root_of_library(roots, l)) {
        Some(r) => vec![r],
        None => roots.to_vec(),
    };
    for r in &preferred {
        let mut users: Vec<PathBuf> = std::fs::read_dir(r.path.join("userdata")).into_iter().flatten().flatten().map(|e| e.path()).collect();
        users.sort();
        for u in users {
            let Some(id) = u.file_name().and_then(|n| n.to_str()).filter(|n| n.bytes().all(|b| b.is_ascii_digit())) else {
                continue;
            };
            let remote = u.join(install.app_id.to_string()).join("remote");
            if remote.is_dir() && !out.iter().any(|d| d.steam_id.as_deref() == Some(id)) {
                out.push(SaveDir { label: format!("steam-{id}"), path: remote, steam_id: Some(id.to_string()), recursive: false });
            }
        }
    }
    for (i, d) in local_settings_dirs(install).into_iter().enumerate() {
        if d.is_dir() {
            out.push(SaveDir { label: if i == 0 { "local".into() } else { format!("local{}", i + 1) }, path: d, steam_id: None, recursive: true });
        }
    }
    let characters = install.path.join("kiln").join("data").join("characters");
    if characters.is_dir() {
        out.push(SaveDir { label: "kiln-characters".into(), path: characters, steam_id: None, recursive: false });
    }
    out
}

pub fn save_dirs(install: &GameInstall) -> Result<Vec<SaveDir>> {
    Ok(save_dirs_in(install, &steam::roots().unwrap_or_default()))
}

/// (relative path, absolute path) of every file in a SaveDir.
fn files_of(d: &SaveDir) -> Result<Vec<(String, PathBuf)>> {
    let mut out = Vec::new();
    walk(&d.path, "", d.recursive, &mut out)?;
    out.sort();
    Ok(out)
}

fn walk(dir: &Path, prefix: &str, recursive: bool, out: &mut Vec<(String, PathBuf)>) -> Result<()> {
    let rd = match std::fs::read_dir(dir) {
        Ok(rd) => rd,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e.into()),
    };
    for e in rd {
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        let rel = if prefix.is_empty() { name.clone() } else { format!("{prefix}/{name}") };
        let ft = std::fs::symlink_metadata(e.path())?.file_type();
        if ft.is_file() {
            if !name.ends_with(".kiln-tmp") && !name.ends_with(".kiln-editor-lock") {
                out.push((rel, e.path()));
            }
        } else if ft.is_dir() && recursive {
            walk(&e.path(), &rel, true, out)?;
        }
    }
    Ok(())
}

pub fn list_in(dirs: &[SaveDir]) -> Result<Vec<SaveSlot>> {
    let mut out = Vec::new();
    for d in dirs {
        for (rel, p) in files_of(d)? {
            let md = std::fs::metadata(&p)?;
            let file = rel.rsplit('/').next().unwrap_or(&rel).to_string();
            out.push(SaveSlot {
                kind: slot_kind(&file),
                file,
                path: p.display().to_string(),
                modified: rfc3339(md.modified().unwrap_or(UNIX_EPOCH)),
                size: md.len(),
                steam_id: d.steam_id.clone(),
            });
        }
    }
    Ok(out)
}

pub fn list(install: &GameInstall) -> Result<Vec<SaveSlot>> {
    list_in(&save_dirs(install)?)
}

// ---- backups ----

/// The profile of this data folder active in the game: another data folder's profile of the same id is not
/// (`profile::game_profile_owner`), so its save backups are not filed under this folder's profile.
fn active_profile(dirs: &DataDirs, install: &GameInstall) -> Option<String> {
    match crate::profile::game_profile_owner(dirs, install) {
        Ok(crate::profile::GameProfileOwner::Here { id }) => Some(id),
        _ => None,
    }
}

/// Where a new backup of this install goes: the active profile's `save-backups/`, else `backups/saves/<gameId>/`.
pub fn backup_root(dirs: &DataDirs, install: &GameInstall, profile: Option<&str>) -> PathBuf {
    match profile {
        Some(pid) => dirs.profile(pid).join("save-backups"),
        None => dirs.backups().join("saves").join(&install.id),
    }
}

/// Every backup folder of this install: `(folder, manifest)`, newest first.
fn all_backups(dirs: &DataDirs, install: &GameInstall) -> Vec<(PathBuf, BackupManifest)> {
    let mut roots = vec![dirs.backups().join("saves").join(&install.id)];
    for e in std::fs::read_dir(dirs.profiles()).into_iter().flatten().flatten() {
        roots.push(e.path().join("save-backups"));
    }
    let mut out = Vec::new();
    for r in roots {
        for e in std::fs::read_dir(&r).into_iter().flatten().flatten() {
            let Ok(text) = std::fs::read_to_string(e.path().join(BACKUP_JSON)) else { continue };
            let Ok(m) = serde_json::from_str::<BackupManifest>(&text) else { continue };
            if m.game_id == install.id {
                out.push((e.path(), m));
            }
        }
    }
    out.sort_by_key(|b| std::cmp::Reverse(order_key(&b.1.backup)));
    out
}

/// Newest first: creation time, then the clash suffix (`<stamp>`, `<stamp>-2`, `<stamp>-3`, ...).
fn order_key(b: &SaveBackup) -> (String, String, u32) {
    let (base, n) = match b.id.rsplit_once('-') {
        Some((base, n)) if n.bytes().all(|c| c.is_ascii_digit()) => (base.to_string(), n.parse().unwrap_or(1)),
        _ => (b.id.clone(), 1),
    };
    (b.created.clone(), base, n)
}

pub fn backups(dirs: &DataDirs, install: &GameInstall) -> Result<Vec<SaveBackup>> {
    Ok(all_backups(dirs, install).into_iter().map(|(_, m)| m.backup).collect())
}

fn copy_verified(src: &Path, dst: &Path) -> Result<(u64, String)> {
    if let Some(p) = dst.parent() {
        std::fs::create_dir_all(p)?;
    }
    let tmp = dst.with_file_name(format!(".{}.kiln-tmp", dst.file_name().unwrap_or_default().to_string_lossy()));
    std::fs::copy(src, &tmp)?;
    let (want, size) = hash::sha256_file(src)?;
    let (got, _) = hash::sha256_file(&tmp)?;
    if want != got {
        let _ = std::fs::remove_file(&tmp);
        return Err(Error::new(ErrorCode::HashMismatch, format!("{} changed while it was copied", src.display())));
    }
    std::fs::rename(&tmp, dst).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })?;
    Ok((size, want))
}

fn write_json<T: Serialize>(path: &Path, v: &T) -> Result<()> {
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, serde_json::to_string_pretty(v)? + "\n")?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

/// Deletes automatic backups beyond KEEP (oldest first); manual ones and `protect` stay.
fn prune(dirs: &DataDirs, install: &GameInstall, protect: Option<&str>) -> Result<()> {
    let auto: Vec<(PathBuf, BackupManifest)> =
        all_backups(dirs, install).into_iter().filter(|(_, m)| m.backup.reason != BackupReason::Manual).collect();
    for (dir, m) in auto.into_iter().skip(KEEP) {
        if Some(m.backup.id.as_str()) == protect {
            continue;
        }
        std::fs::remove_dir_all(&dir)?;
    }
    Ok(())
}

/// `backup` from explicit save folders, with the profile and mod-set fingerprint recorded (launch backups).
pub fn backup_from(
    dirs: &DataDirs, install: &GameInstall, saves: &[SaveDir], reason: BackupReason, note: Option<&str>,
    fingerprint: Option<&str>,
) -> Result<SaveBackup> {
    let b = backup_unpruned(dirs, install, saves, reason, note, fingerprint)?;
    prune(dirs, install, None)?;
    Ok(b)
}

fn backup_unpruned(
    dirs: &DataDirs, install: &GameInstall, saves: &[SaveDir], reason: BackupReason, note: Option<&str>,
    fingerprint: Option<&str>,
) -> Result<SaveBackup> {
    let now = SystemTime::now();
    let profile = active_profile(dirs, install);
    let root = backup_root(dirs, install, profile.as_deref());
    std::fs::create_dir_all(&root)?;
    // `<stamp>`, then `<stamp>-2`, `-3`, ... above every id of that second (also pruned ones' successors), so the
    // order of ids is the order of creation
    let base = stamp(now);
    let mut taken: Vec<String> = all_backups(dirs, install).into_iter().map(|(_, m)| m.backup.id).collect();
    taken.extend(std::fs::read_dir(&root).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned()));
    let max_n = taken
        .iter()
        .filter_map(|t| if *t == base { Some(1) } else { t.strip_prefix(&format!("{base}-"))?.parse::<u32>().ok() })
        .max();
    let id = match max_n {
        None => base.clone(),
        Some(n) => format!("{base}-{}", n + 1),
    };
    let partial = root.join(format!(".{id}.partial"));
    let _ = std::fs::remove_dir_all(&partial);
    std::fs::create_dir_all(&partial)?;
    let mut entries = Vec::new();
    let mut run = || -> Result<()> {
        for d in saves {
            for (rel, src) in files_of(d)? {
                let (size, sha256) = copy_verified(&src, &partial.join(&d.label).join(&rel))?;
                entries.push(BackupEntry { dir: d.label.clone(), rel, source: src.display().to_string(), size, sha256 });
            }
        }
        Ok(())
    };
    if let Err(e) = run() {
        let _ = std::fs::remove_dir_all(&partial);
        return Err(e);
    }
    let backup = SaveBackup {
        id: id.clone(),
        created: rfc3339(now),
        reason,
        profile_id: profile,
        fingerprint: fingerprint.map(str::to_string),
        note: note.map(str::to_string),
        files: entries.len() as u32,
        bytes: entries.iter().map(|e| e.size).sum(),
    };
    let m = BackupManifest { format: 1, game_id: install.id.clone(), backup: backup.clone(), entries };
    write_json(&partial.join(BACKUP_JSON), &m)?;
    std::fs::rename(&partial, root.join(&id))?;
    Ok(backup)
}

/// Copies every save slot; prunes to KEEP (manual backups are never pruned).
pub fn backup(dirs: &DataDirs, install: &GameInstall, reason: BackupReason, note: Option<&str>) -> Result<SaveBackup> {
    backup_from(dirs, install, &save_dirs(install)?, reason, note, None)
}

/// `backup` with the launch's mod-set fingerprint (kms1) recorded.
pub fn backup_with_fingerprint(
    dirs: &DataDirs, install: &GameInstall, reason: BackupReason, note: Option<&str>, fingerprint: Option<&str>,
) -> Result<SaveBackup> {
    backup_from(dirs, install, &save_dirs(install)?, reason, note, fingerprint)
}

fn find_backup(dirs: &DataDirs, install: &GameInstall, id: &str) -> Result<(PathBuf, BackupManifest)> {
    all_backups(dirs, install)
        .into_iter()
        .find(|(_, m)| m.backup.id == id)
        .ok_or_else(|| Error::not_found(format!("no save backup {id} for {}", install.id)))
}

/// `restore` from explicit save folders (the running check stays).
pub fn restore_to(dirs: &DataDirs, install: &GameInstall, saves: &[SaveDir], backup_id: &str) -> Result<()> {
    if let Ok(Some(p)) = crate::proc::find_game(install) {
        return Err(Error::new(ErrorCode::GameRunning, "quit the game before restoring saves").with_detail(serde_json::json!({"pid": p.pid})));
    }
    let (dir, m) = find_backup(dirs, install, backup_id)?;
    // check the backup before touching anything
    for e in &m.entries {
        if e.rel.split('/').any(|c| c == ".." || c.is_empty()) || e.dir.contains(['/', '\\']) || e.dir == ".." {
            return Err(Error::new(ErrorCode::Schema, format!("backup {backup_id}: bad entry {}/{}", e.dir, e.rel)));
        }
        let (sha, _) = hash::sha256_file(&dir.join(&e.dir).join(&e.rel))?;
        if sha != e.sha256 {
            return Err(Error::new(ErrorCode::HashMismatch, format!("backup {backup_id}: {}/{} is damaged", e.dir, e.rel)));
        }
    }
    let before = backup_unpruned(dirs, install, saves, BackupReason::Restore, Some(&format!("before restoring {backup_id}")), None)?;
    for e in &m.entries {
        let target = match saves.iter().find(|d| d.label == e.dir) {
            Some(d) => e.rel.split('/').fold(d.path.clone(), |p, c| p.join(c)),
            None => PathBuf::from(&e.source),
        };
        let (_, sha) = copy_verified(&dir.join(&e.dir).join(&e.rel), &target)?;
        if sha != e.sha256 {
            return Err(Error::new(ErrorCode::HashMismatch, format!("{}: restored copy differs", target.display())));
        }
    }
    prune(dirs, install, Some(&before.id))?;
    Ok(())
}

/// Refuses (GameRunning) while the game runs; backs up the current saves first (reason restore).
pub fn restore(dirs: &DataDirs, install: &GameInstall, backup_id: &str) -> Result<()> {
    restore_to(dirs, install, &save_dirs(install)?, backup_id)
}

pub fn delete(dirs: &DataDirs, install: &GameInstall, backup_id: &str) -> Result<()> {
    let (dir, _) = find_backup(dirs, install, backup_id)?;
    std::fs::remove_dir_all(dir)?;
    Ok(())
}

/// The Save Editor's Tauri commands (upstream src-tauri/src/main.rs), ported.
pub mod editor {
    use crate::{Error, ErrorCode, Result};
    use serde::{Deserialize, Serialize};
    use std::path::{Path, PathBuf};
    use std::time::UNIX_EPOCH;

    #[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct EditorSaveFile {
        pub path: String,
        pub file: String,
        /// ms since the epoch
        pub modified: f64,
    }

    #[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct EditorFileInfo {
        pub modified: f64,
    }

    #[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct EditorBackup {
        /// "game" (remote/backup) | "folder" (the editor's backups)
        pub kind: String,
        pub label: String,
        pub path: String,
        pub modified: f64,
        /// "matching" for this save; "legacy" for older backups whose origin is unknown.
        #[serde(default)]
        pub group: String,
        #[serde(default)]
        pub source: Option<EditorBackupSource>,
    }

    /// Canonical save folder and file, not just a basename shared by multiple accounts/installations.
    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct EditorBackupSource {
        pub folder: String,
        pub file: String,
    }

    const EDITOR_BACKUP_JSON: &str = "editor-backup.json";

    #[derive(Serialize, Deserialize)]
    struct EditorBackupManifest {
        source: EditorBackupSource,
    }

    pub(super) fn target_path(path: &Path) -> Result<PathBuf> {
        match std::fs::canonicalize(path) {
            Ok(p) => Ok(p),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let absolute = std::path::absolute(path)?;
                let parent = absolute.parent().ok_or_else(|| Error::invalid("save has no parent folder"))?;
                Ok(std::fs::canonicalize(parent)?.join(absolute.file_name().ok_or_else(|| Error::invalid("save has no file name"))?))
            }
            Err(e) => Err(e.into()),
        }
    }

    fn source_identity(path: &Path) -> Result<EditorBackupSource> {
        // A launcher's manifest can outlive the original install. Its recorded absolute origin still identifies
        // it; do not require that another account's save folder still exists just to filter that backup out.
        let p = match target_path(path) {
            Ok(p) => p,
            Err(e) if e.code == ErrorCode::NotFound => std::path::absolute(path)?,
            Err(e) => return Err(e),
        };
        let folder = p.parent().ok_or_else(|| Error::invalid("save has no parent folder"))?.to_string_lossy().into_owned();
        let file = p.file_name().ok_or_else(|| Error::invalid("save has no file name"))?.to_string_lossy().into_owned();
        // Canonicalization expands short names and resolves case. Do not lowercase: Windows also supports
        // case-sensitive folders. Reuse the common verbatim-path rules for the persisted spelling.
        let folder = if cfg!(windows) { crate::winpath::plain_path(&folder).into_owned() } else { folder };
        Ok(EditorBackupSource { folder, file })
    }

    fn live_source(source: EditorBackupSource) -> Result<Option<EditorBackupSource>> {
        let path = Path::new(&source.folder).join(&source.file);
        match std::fs::metadata(&path) {
            Ok(md) if md.is_file() => source_identity(&path).map(Some),
            Ok(_) => Ok(None),
            // A move or rename loses the absolute origin, not the recovery copy. Never infer a new match.
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e.into()),
        }
    }

    fn same_source(a: &EditorBackupSource, b: &EditorBackupSource) -> Result<bool> {
        if a == b { return Ok(true); }
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawHandle;
            use windows_sys::Win32::Storage::FileSystem::{BY_HANDLE_FILE_INFORMATION, GetFileInformationByHandle};
            let identity = |source: &EditorBackupSource| -> Result<Option<(u32, u64)>> {
                let path = Path::new(&source.folder).join(&source.file);
                let file = match std::fs::File::open(&path) {
                    Ok(file) => file,
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
                    Err(e) => return Err(e.into()),
                };
                let mut info = std::mem::MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::uninit();
                // SAFETY: the handle is open and info has the required size.
                if unsafe { GetFileInformationByHandle(file.as_raw_handle(), info.as_mut_ptr()) } == 0 {
                    return Err(std::io::Error::last_os_error().into());
                }
                // SAFETY: the successful call initialized info.
                let info = unsafe { info.assume_init() };
                Ok(Some((info.dwVolumeSerialNumber, u64::from(info.nFileIndexHigh) << 32 | u64::from(info.nFileIndexLow))))
            };
            // Canonical UNC and drive names can still name the very same file. Compare live filesystem
            // identity, never contents (two different characters can have identical save bytes).
            return Ok(matches!((identity(a)?, identity(b)?), (Some(a), Some(b)) if a == b));
        }
        #[cfg(not(windows))]
        Ok(false)
    }

    /// The sidecar stays after unlocking: deleting it would let a third caller lock a different inode.
    pub(super) fn lock_target(path: &Path) -> Result<std::fs::File> {
        let name = path.file_name().ok_or_else(|| Error::invalid("save has no file name"))?.to_string_lossy();
        let lock = path.with_file_name(format!(".{name}.kiln-editor-lock"));
        let file = std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(&lock)?;
        match file.try_lock() {
            Ok(()) => Ok(file),
            Err(std::fs::TryLockError::WouldBlock) => Err(Error::new(ErrorCode::Busy, "a save or restore is already in progress for this file; wait for it to finish")),
            Err(std::fs::TryLockError::Error(e)) => Err(e.into()),
        }
    }

    fn modified_ms(p: &Path) -> f64 {
        std::fs::metadata(p)
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as f64)
            .unwrap_or(0.0)
    }

    fn is_save(path: &str) -> Result<PathBuf> {
        let p = PathBuf::from(path);
        match p.extension().and_then(|e| e.to_str()) {
            Some(e) if e.eq_ignore_ascii_case("ksd") => Ok(p),
            _ => Err(Error::invalid("only .ksd files can be read or written")),
        }
    }

    pub(super) fn is_character_file(name: &str) -> bool {
        name.len() > 13
            && name.starts_with("character")
            && name.ends_with(".ksd")
            && name[9..name.len() - 4].bytes().all(|c| c.is_ascii_digit())
    }

    /// `characterN.ksd` in every Steam user's `1203620/remote/` (all Steam roots: native, Flatpak, Snap, Windows).
    pub fn list_saves_in(roots: &[crate::steam::SteamRoot]) -> Vec<EditorSaveFile> {
        let mut out = Vec::new();
        for r in roots {
            let mut users: Vec<PathBuf> = std::fs::read_dir(r.path.join("userdata")).into_iter().flatten().flatten().map(|e| e.path()).collect();
            users.sort();
            for u in users {
                let dir = u.join(crate::CLIENT_APP_ID.to_string()).join("remote");
                let mut files: Vec<PathBuf> = std::fs::read_dir(&dir).into_iter().flatten().flatten().map(|e| e.path()).collect();
                files.sort();
                for f in files {
                    let name = f.file_name().unwrap_or_default().to_string_lossy().into_owned();
                    if is_character_file(&name) {
                        out.push(EditorSaveFile { path: f.display().to_string(), file: name, modified: modified_ms(&f) });
                    }
                }
            }
        }
        out
    }

    pub fn list_saves() -> Result<Vec<EditorSaveFile>> {
        Ok(list_saves_in(&crate::steam::roots().unwrap_or_default()))
    }

    pub fn file_info(path: &str) -> Result<EditorFileInfo> {
        Ok(EditorFileInfo { modified: modified_ms(Path::new(path)) })
    }

    /// Only `.ksd` files.
    pub fn read_file(path: &str) -> Result<Vec<u8>> {
        Ok(std::fs::read(is_save(path)?)?)
    }

    #[cfg(feature = "saves")]
    pub fn zstd_decompress(data: &[u8]) -> Result<Vec<u8>> {
        zstd::stream::decode_all(data).map_err(|e| Error::new(ErrorCode::Parse, format!("zstd: {e}")))
    }

    #[cfg(feature = "saves")]
    pub fn zstd_compress(data: &[u8]) -> Result<Vec<u8>> {
        zstd::bulk::compress(data, 19).map_err(|e| Error::new(ErrorCode::Internal, format!("zstd: {e}")))
    }

    #[cfg(not(feature = "saves"))]
    pub fn zstd_decompress(data: &[u8]) -> Result<Vec<u8>> {
        let _ = data;
        Err(Error::new(ErrorCode::Unsupported, "zstd needs kiln-manage's \"saves\" feature"))
    }

    #[cfg(not(feature = "saves"))]
    pub fn zstd_compress(data: &[u8]) -> Result<Vec<u8>> {
        let _ = data;
        Err(Error::new(ErrorCode::Unsupported, "zstd needs kiln-manage's \"saves\" feature"))
    }

    /// Any `enshrouded.exe` process (the editor does not know which install a save belongs to).
    pub fn game_running() -> Result<bool> {
        #[cfg(test)]
        if tests::configured() { return Ok(false); }
        Ok(crate::proc::list()?.iter().any(|p| {
            p.name.eq_ignore_ascii_case("enshrouded.exe")
                || p.exe.as_deref().and_then(|e| e.file_name()).is_some_and(|n| n.eq_ignore_ascii_case("enshrouded.exe"))
        }))
    }

    /// The editor's backups: `<data>/backups/editor` (Windows: `%USERPROFILE%\Documents\Enshrouded-Backups\editor`, as
    /// the Save Editor keeps them).
    pub fn backups_dir() -> Result<PathBuf> {
        #[cfg(test)]
        if let Some(dir) = tests::backup_dir() { return Ok(dir); }
        if cfg!(windows)
            && let Some(home) = std::env::var_os("USERPROFILE")
        {
            return Ok(Path::new(&home).join("Documents").join("Enshrouded-Backups").join("editor"));
        }
        Ok(crate::paths::DataDirs::locate()?.backups().join("editor"))
    }

    fn backups_home() -> Result<PathBuf> {
        let r = backups_dir()?;
        Ok(r.parent().map(Path::to_path_buf).unwrap_or(r))
    }

    fn copy_dir(src: &Path, dst: &Path) -> std::io::Result<()> {
        std::fs::create_dir_all(dst)?;
        for e in std::fs::read_dir(src)?.flatten() {
            let t = dst.join(e.file_name());
            if e.path().is_dir() { copy_dir(&e.path(), &t)?; } else { std::fs::copy(e.path(), t)?; }
        }
        Ok(())
    }

    pub(super) fn make_backup(file: &Path) -> Result<PathBuf> {
        let secs = std::time::SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        let base = backups_dir()?;
        crate::durable::ensure_dir(&base)?;
        let tag = crate::hash::hex(&crate::util::random_bytes::<16>()?);
        let name = format!("tauri-{secs}-{tag}");
        let partial = base.join(format!(".{name}.partial"));
        let dir = base.join(name);
        std::fs::create_dir(&partial)?;
        let run = || -> Result<()> {
            let name = file.file_name().ok_or_else(|| Error::invalid(format!("{}: no file name", file.display())))?;
            super::copy_verified(file, &partial.join(name))?;
            if let Some(parent) = file.parent() {
                let b = parent.join("backup");
                if b.is_dir() { copy_dir(&b, &partial.join("backup"))?; }
            }
            let manifest = EditorBackupManifest { source: source_identity(file)? };
            crate::durable::replace(&partial.join(EDITOR_BACKUP_JSON), &serde_json::to_vec(&manifest)?)?;
            crate::durable::flush_contents(&partial)?;
            crate::durable::rename_dir(&partial, &dir)
        };
        run().inspect_err(|_| { let _ = std::fs::remove_dir_all(&partial); })?;
        Ok(dir)
    }

    pub fn backup_file(path: &str) -> Result<String> {
        let p = target_path(&is_save(path)?)?;
        let _lock = lock_target(&p)?;
        make_backup(&p).map(|d| d.display().to_string())
    }

    fn running_refusal(what: &str) -> Error {
        Error::new(ErrorCode::GameRunning, format!("quit Enshrouded before {what}"))
    }

    /// Backs up first; refuses while the game runs.
    pub fn write_save(path: &str, data: &[u8]) -> Result<()> {
        if game_running()? {
            return Err(running_refusal("saving"));
        }
        let p = target_path(&is_save(path)?)?;
        let _lock = lock_target(&p)?;
        #[cfg(test)]
        tests::before_change();
        if data.len() < 0x30 || &data[..4] != b"SV2C" {
            return Err(Error::invalid("not a save (SV2C header missing)"));
        }
        if p.is_file() {
            make_backup(&p)?;
        }
        replace_save(&p, data, None)
    }

    /// Resolve editor identity or the launcher's existing manifest; absent metadata means legacy, not a match.
    fn backup_source(path: &Path, home: &Path) -> Result<Option<EditorBackupSource>> {
        for dir in path.ancestors().skip(1).take_while(|d| d.starts_with(home)) {
            if let Some(m) = crate::util::read_json::<EditorBackupManifest>(&dir.join(EDITOR_BACKUP_JSON))? {
                // This manifest describes only the top-level save, not every other slot copied from backup/.
                if path.parent() != Some(dir) || !path.file_name().is_some_and(|file| crate::winpath::same_folder(Path::new(file), Path::new(&m.source.file))) {
                    return Err(Error::new(ErrorCode::Refused, "this file is not the save identified by its backup manifest"));
                }
                return live_source(m.source);
            }
            if let Some(m) = crate::util::read_json::<super::BackupManifest>(&dir.join(super::BACKUP_JSON))? {
                let rel = path.strip_prefix(dir).map_err(|_| Error::invalid("backup path outside its manifest"))?;
                for entry in m.entries {
                    if Path::new(&entry.dir).join(&entry.rel) == rel {
                        let path = Path::new(&entry.source);
                        return live_source(EditorBackupSource {
                            folder: path.parent().unwrap_or_else(|| Path::new("")).to_string_lossy().into_owned(),
                            file: path.file_name().unwrap_or_default().to_string_lossy().into_owned(),
                        });
                    }
                }
                return Err(Error::new(ErrorCode::Refused, "backup has no source entry in its manifest"));
            }
        }
        Ok(None)
    }

    /// A backup that cannot be read or identified (a damaged manifest, a file its manifest does not list, a folder that
    /// cannot be read) is left out of the list: it never hides the others, as the list did before origins were read
    /// (restore_backup still refuses such a file on its own).
    fn collect(dir: &Path, home: &Path, base: &str, source: &EditorBackupSource, depth: u32, out: &mut Vec<EditorBackup>) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            // Do not follow links out of the backup tree or offer an incomplete new backup as legacy.
            let Ok(ft) = e.file_type() else { continue };
            if ft.is_dir() {
                if depth > 0 && !e.file_name().to_string_lossy().ends_with(".partial") {
                    collect(&p, home, base, source, depth - 1, out);
                }
            } else if ft.is_file() && e.file_name().to_string_lossy().eq_ignore_ascii_case(base) {
                let Ok(identity) = backup_source(&p, home) else { continue };
                if let Some(identity) = &identity {
                    if !same_source(identity, source).unwrap_or(false) { continue; }
                }
                let label = p.parent().and_then(|d| d.strip_prefix(home).ok()).map(|r| r.display().to_string()).unwrap_or_default();
                out.push(EditorBackup {
                    kind: "folder".into(), label, path: p.display().to_string(), modified: modified_ms(&p),
                    group: if identity.is_some() { "matching" } else { "legacy" }.into(), source: identity,
                });
            }
        }
    }

    /// Existing array shape; matching backups first, then older backups (origin unknown), never mixed together.
    pub fn list_backups(target: &str) -> Result<Vec<EditorBackup>> {
        let t = target_path(&is_save(target)?)?;
        let source = source_identity(&t)?;
        let base = t.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let mut out = Vec::new();
        if let Some(dir) = t.parent() {
            for e in std::fs::read_dir(dir.join("backup")).into_iter().flatten().flatten() {
                let n = e.file_name().to_string_lossy().into_owned();
                if n.starts_with(&format!("{base}.backup-")) && e.file_type()?.is_file() {
                    out.push(EditorBackup {
                        kind: "game".into(), label: n, path: e.path().display().to_string(), modified: modified_ms(&e.path()),
                        group: "matching".into(), source: Some(source.clone()),
                    });
                }
            }
        }
        let home = backups_home()?;
        // Includes a launcher's backups/saves/<install>/<backup>/<source>/<file> layout.
        collect(&home, &home, &base, &source, 4, &mut out);
        out.sort_by(|a, b| (a.group == "legacy").cmp(&(b.group == "legacy"))
            .then_with(|| b.modified.partial_cmp(&a.modified).unwrap_or(std::cmp::Ordering::Equal)));
        Ok(out)
    }

    /// Stage next to the target, flush and verify it, then replace the name without ever deleting the old file first.
    pub(super) fn replace_save(target: &Path, data: &[u8], copy: Option<&Path>) -> Result<()> {
        use std::io::Write;
        let tag = crate::hash::hex(&crate::util::random_bytes::<16>()?);
        let name = target.file_name().ok_or_else(|| Error::invalid("save has no file name"))?.to_string_lossy();
        let tmp = target.with_file_name(format!(".{name}.{tag}.kiln-tmp"));
        let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(&tmp)?;
        let staged = (|| -> Result<()> {
            #[cfg(test)]
            {
                if copy.is_some() { tests::fail_step(tests::Fault::Copy, &tmp)?; }
                tests::fail_step(tests::Fault::Write, &tmp)?;
            }
            match copy {
                Some(src) => { std::io::copy(&mut std::fs::File::open(src)?, &mut file)?; }
                None => file.write_all(data)?,
            }
            Ok(())
        })();
        drop(file);
        let commit = || -> Result<()> {
            staged?;
            #[cfg(test)]
            {
                if tests::corrupt() { std::fs::write(&tmp, b"SV2Cdamaged")?; }
                tests::fail_step(tests::Fault::Flush, &tmp)?;
            }
            crate::durable::flush_file(&tmp)?;
            let (got, size) = crate::hash::sha256_file(&tmp)?;
            if size != data.len() as u64 || got != crate::hash::sha256_hex(data) {
                return Err(Error::new(ErrorCode::HashMismatch, "the staged save differs; the original save has not been changed"));
            }
            #[cfg(test)]
            tests::fail_step(tests::Fault::Rename, &tmp)?;
            #[cfg(windows)]
            {
                use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH};
                let (src, dst) = (crate::util::win32_path(&tmp)?, crate::util::win32_path(target)?);
                // SAFETY: both are valid NUL-terminated UTF-16 names for the duration of this call.
                // No COPY_ALLOWED: source and target are on the same volume, with NTFS atomic replace semantics.
                if unsafe { MoveFileExW(src.as_ptr(), dst.as_ptr(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) } == 0 {
                    return Err(crate::util::io_at(target, "replacing save")(std::io::Error::last_os_error()));
                }
            }
            #[cfg(not(windows))]
            crate::util::rename_replace(&tmp, target)?;
            if let Some(parent) = target.parent() { crate::durable::flush_dir(parent)?; }
            Ok(())
        };
        commit().inspect_err(|_| { let _ = std::fs::remove_file(&tmp); })
    }

    pub fn restore_backup(src: &str, target: &str) -> Result<()> {
        if game_running()? {
            return Err(running_refusal("restoring"));
        }
        let t = target_path(&is_save(target)?)?;
        let _lock = lock_target(&t)?;
        #[cfg(test)]
        tests::before_change();
        let s = std::fs::canonicalize(src)?;
        let home = backups_home()?;
        let game_backups = t.parent().map(|d| d.join("backup")).unwrap_or_default();
        let under = |dir: &Path| std::fs::canonicalize(dir).is_ok_and(|dir| s.starts_with(dir));
        let in_home = under(&home);
        if !(in_home || under(&game_backups)) || !s.is_file() {
            return Err(Error::not_found("backup not found"));
        }
        let target_source = source_identity(&t)?;
        let origin = if in_home {
            backup_source(&s, &std::fs::canonicalize(&home)?)?
        } else {
            // The current folder supplies the identity of the game's rolling backups, but not of another slot's.
            let prefix = format!("{}.backup-", t.file_name().unwrap_or_default().to_string_lossy());
            if !s.file_name().unwrap_or_default().to_string_lossy().starts_with(&prefix) {
                return Err(Error::new(ErrorCode::Refused, "this game backup belongs to a different save file"));
            }
            Some(target_source.clone())
        };
        if let Some(origin) = origin {
            if !same_source(&origin, &target_source)? {
                return Err(Error::new(ErrorCode::Refused, "this backup belongs to a different save folder or file"));
            }
        }
        let data = std::fs::read(&s)?;
        if data.len() < 4 || &data[..4] != b"SV2C" {
            return Err(Error::invalid("the backup is not a save (SV2C header missing)"));
        }
        if t.is_file() {
            make_backup(&t)?;
        }
        replace_save(&t, &data, Some(&s))
    }

    #[cfg(test)]
    pub(super) mod tests {
        use super::*;

        use std::cell::RefCell;
        use std::sync::{Arc, Barrier};
        use std::sync::atomic::{AtomicU64, Ordering};

        #[derive(Clone, Copy, Debug, PartialEq)]
        pub(super) enum Fault { Copy, Write, Flush, Rename, Corrupt }

        #[derive(Default)]
        struct TestIo {
            backups: Option<PathBuf>,
            fault: Option<Fault>,
            pause: Option<(Arc<Barrier>, Arc<Barrier>)>,
        }
        thread_local! { static IO: RefCell<TestIo> = RefCell::new(TestIo::default()); }
        pub(super) fn corrupt() -> bool { IO.with(|c| c.borrow().fault == Some(Fault::Corrupt)) }
        pub(super) fn configured() -> bool { IO.with(|c| c.borrow().backups.is_some()) }
        pub(super) fn backup_dir() -> Option<PathBuf> { IO.with(|c| c.borrow().backups.clone()) }
        pub(super) fn before_change() {
            let pause = IO.with(|c| c.borrow_mut().pause.take());
            if let Some((entered, resume)) = pause { entered.wait(); resume.wait(); }
        }
        pub(super) fn fail_step(step: Fault, path: &Path) -> Result<()> {
            if IO.with(|c| c.borrow().fault == Some(step)) {
                // A short write/copy must only damage a staging file, never the target.
                if matches!(step, Fault::Write | Fault::Copy) { std::fs::write(path, b"SV2Cpartial")?; }
                return Err(Error::new(ErrorCode::Io, format!("injected {step:?} failure")));
            }
            Ok(())
        }
        pub(in crate::saves) fn set_io(backups: PathBuf) { IO.with(|c| *c.borrow_mut() = TestIo { backups: Some(backups), ..Default::default() }); }

        struct Fixture { root: PathBuf, target: PathBuf, src: PathBuf }
        impl Fixture {
            fn new() -> Self {
                static NEXT: AtomicU64 = AtomicU64::new(0);
                let root = std::env::temp_dir().join(format!("kiln-editor-backups-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
                let target = root.join("account-a/remote/character0.ksd");
                let src = root.join("backups/older/character0.ksd");
                for path in [&target, &src] { std::fs::create_dir_all(path.parent().unwrap()).unwrap(); }
                std::fs::write(&target, Self::bytes(1)).unwrap();
                std::fs::write(&src, Self::bytes(2)).unwrap();
                set_io(root.join("backups/editor"));
                Self { root, target, src }
            }
            fn bytes(value: u8) -> Vec<u8> { let mut b = vec![value; 128]; b[..4].copy_from_slice(b"SV2C"); b }
            fn restore(&self) -> Result<()> { restore_backup(self.src.to_str().unwrap(), self.target.to_str().unwrap()) }
            fn write(&self) -> Result<()> { write_save(self.target.to_str().unwrap(), &Self::bytes(3)) }
            fn backups(&self) -> Vec<EditorBackup> { list_backups(self.target.to_str().unwrap()).unwrap() }
            fn assert_original(&self) { assert_eq!(std::fs::read(&self.target).unwrap(), Self::bytes(1), "failed operation changed the original target"); }
            fn assert_no_staging(&self) {
                assert!(!std::fs::read_dir(self.target.parent().unwrap()).unwrap().flatten().any(|e| e.file_name().to_string_lossy().ends_with(".kiln-tmp")));
            }
        }
        impl Drop for Fixture {
            fn drop(&mut self) { IO.with(|c| *c.borrow_mut() = TestIo::default()); let _ = std::fs::remove_dir_all(&self.root); }
        }

        fn restore_failure(fault: Fault) {
            let f = Fixture::new();
            IO.with(|c| c.borrow_mut().fault = Some(fault));
            let result = f.restore();
            f.assert_original();
            assert!(result.is_err(), "{fault:?} failure was not exercised");
            f.assert_no_staging();
        }
        #[test] fn restore_copy_failure_keeps_original() { restore_failure(Fault::Copy); }
        #[test] fn restore_partial_write_keeps_original() { restore_failure(Fault::Write); }
        #[test] fn restore_flush_failure_keeps_original() { restore_failure(Fault::Flush); }
        #[test] fn restore_rename_failure_keeps_original() { restore_failure(Fault::Rename); }
        #[test] fn restore_verification_failure_keeps_original() { restore_failure(Fault::Corrupt); }

        #[test]
        fn restore_preserves_replaced_save() {
            let f = Fixture::new();
            f.restore().unwrap();
            assert_eq!(std::fs::read(&f.target).unwrap(), Fixture::bytes(2));
            assert!(f.backups().iter().any(|b| std::fs::read(&b.path).unwrap() == Fixture::bytes(1)));
            f.assert_no_staging();
        }

        #[test]
        fn save_flushes_before_replacement() {
            let f = Fixture::new();
            let (result, events) = crate::durable::recording(|| f.write());
            result.unwrap();
            let target = target_path(&f.target).unwrap();
            assert!(events.iter().any(|e| matches!(e, crate::durable::Event::File(p) if p.parent() == target.parent())), "no staged save flush: {events:?}");
            assert!(events.iter().any(|e| matches!(e, crate::durable::Event::Dir(p) if Some(p.as_path()) == target.parent())), "no save directory flush: {events:?}");
            let published = events.iter().position(|e| matches!(e, crate::durable::Event::Rename { to, .. } if to.parent() == backup_dir().as_deref())).unwrap();
            let staged = events.iter().position(|e| matches!(e, crate::durable::Event::File(p) if p.parent() == target.parent())).unwrap();
            assert!(published < staged, "save staged before its durable backup: {events:?}");
            assert!(events[..published].iter().any(|e| matches!(e, crate::durable::Event::File(p) if p.file_name() == target.file_name())), "backup contents not flushed before publication: {events:?}");
            assert_eq!(std::fs::read(&f.target).unwrap(), Fixture::bytes(3));
            f.assert_no_staging();
        }

        #[test]
        fn backups_separate_same_basename_and_label_legacy() {
            let f = Fixture::new();
            let a = backup_file(f.target.to_str().unwrap()).unwrap();
            let other = f.root.join("account-b/remote/character0.ksd");
            std::fs::create_dir_all(other.parent().unwrap()).unwrap();
            // Equal bytes are not proof of the same save or account.
            std::fs::write(&other, Fixture::bytes(1)).unwrap();
            let b = backup_file(other.to_str().unwrap()).unwrap();
            let list = serde_json::to_value(f.backups()).unwrap();
            let entries = list.as_array().unwrap();
            assert!(!entries.iter().any(|e| Path::new(e["path"].as_str().unwrap()).starts_with(&b)), "another account's backup was listed: {list}");
            let matching = entries.iter().find(|e| Path::new(e["path"].as_str().unwrap()).starts_with(&a)).unwrap();
            assert_eq!(matching["group"], "matching");
            assert_eq!(matching["source"]["file"], "character0.ksd");
            let legacy_path = std::fs::canonicalize(&f.src).unwrap();
            let legacy = entries.iter().find(|e| std::fs::canonicalize(e["path"].as_str().unwrap()).is_ok_and(|p| p == legacy_path)).unwrap();
            assert_eq!(legacy["group"], "legacy");
            assert!(legacy.get("groupLabel").is_none(), "headings are translated by the launcher");
            assert!(legacy["source"].is_null());
            assert_eq!(entries.last().unwrap()["group"], "legacy");
            // An old launcher still gets an array and every field it already consumes.
            for e in entries { for key in ["kind", "label", "path", "modified"] { assert!(e.get(key).is_some()); } }
            f.restore().unwrap(); // unidentified backups remain restorable
            let err = restore_backup(Path::new(&b).join("character0.ksd").to_str().unwrap(), f.target.to_str().unwrap()).unwrap_err();
            assert_eq!(err.code, ErrorCode::Refused);
        }

        #[test]
        fn launcher_backups_keep_known_origins_and_recover_missing_ones() {
            let f = Fixture::new();
            let add = |install: &str, source: &Path| {
                let dir = f.root.join("backups/saves").join(install).join("stamp");
                let path = dir.join("steam-1/character0.ksd");
                std::fs::create_dir_all(path.parent().unwrap()).unwrap();
                std::fs::write(&path, Fixture::bytes(4)).unwrap();
                let manifest = serde_json::json!({
                    "format": 1, "gameId": install, "id": "stamp", "created": "2026-11-22T00:00:00Z",
                    "reason": "manual", "files": 1, "bytes": 128,
                    "entries": [{"dir": "steam-1", "rel": "character0.ksd", "source": source.to_str().unwrap(), "size": 128, "sha256": "unused"}]
                });
                std::fs::write(dir.join("backup.json"), serde_json::to_vec(&manifest).unwrap()).unwrap();
                path
            };
            let own = add("own", &f.target);
            let foreign = add("removed-install", &f.root.join("removed-install/remote/character0.ksd"));
            let backups = list_backups(f.target.to_str().unwrap()).expect("a removed installation must not break this save's backup list");
            assert!(backups.iter().any(|b| Path::new(&b.path) == own && b.group == "matching"));
            assert!(backups.iter().any(|b| Path::new(&b.path) == foreign && b.group == "legacy" && b.source.is_none()));
            restore_backup(foreign.to_str().unwrap(), f.target.to_str().unwrap()).unwrap();
            assert_eq!(std::fs::read(&f.target).unwrap(), Fixture::bytes(4));
        }

        #[test]
        fn moved_save_keeps_its_backups_as_legacy_recovery() {
            let f = Fixture::new();
            let backup = backup_file(f.target.to_str().unwrap()).unwrap();
            let copied = Path::new(&backup).join("character0.ksd");
            let moved = f.root.join("moved-remote");
            std::fs::rename(f.target.parent().unwrap(), &moved).unwrap();
            let target = moved.join("character0.ksd");
            std::fs::write(&target, Fixture::bytes(3)).unwrap();
            let list = list_backups(target.to_str().unwrap()).unwrap();
            assert!(list.iter().any(|b| Path::new(&b.path) == copied && b.group == "legacy" && b.source.is_none()), "moved save lost its recovery backup: {list:?}");
            restore_backup(copied.to_str().unwrap(), target.to_str().unwrap()).unwrap();
            assert_eq!(std::fs::read(&target).unwrap(), Fixture::bytes(1));
        }

        #[test]
        fn copied_game_backup_of_another_file_is_refused() {
            let f = Fixture::new();
            let rolling = f.target.parent().unwrap().join("backup/character1.ksd.backup-1");
            std::fs::create_dir_all(rolling.parent().unwrap()).unwrap();
            std::fs::write(&rolling, Fixture::bytes(4)).unwrap();
            let backup = backup_file(f.target.to_str().unwrap()).unwrap();
            let copied = Path::new(&backup).join("backup/character1.ksd.backup-1");
            let result = restore_backup(copied.to_str().unwrap(), f.target.to_str().unwrap());
            f.assert_original();
            assert_eq!(result.unwrap_err().code, ErrorCode::Refused);
        }

        /// One backup whose origin cannot be read (a damaged launcher manifest, a manifest that does not list the file)
        /// is left out; the list still shows every other backup of this save.
        #[test]
        fn an_unreadable_backup_origin_does_not_hide_the_other_backups() {
            let f = Fixture::new();
            let own = backup_file(f.target.to_str().unwrap()).unwrap();
            let put = |install: &str, manifest: &[u8]| {
                let dir = f.root.join("backups/saves").join(install).join("stamp");
                let path = dir.join("steam-1/character0.ksd");
                std::fs::create_dir_all(path.parent().unwrap()).unwrap();
                std::fs::write(&path, Fixture::bytes(4)).unwrap();
                std::fs::write(dir.join("backup.json"), manifest).unwrap();
                path
            };
            let damaged = put("damaged", b"");
            let unlisted = put("unlisted", br#"{"format":1,"gameId":"unlisted","id":"stamp","created":"2026-11-22T00:00:00Z","reason":"manual","files":0,"bytes":0,"entries":[]}"#);
            let list = list_backups(f.target.to_str().unwrap()).expect("a damaged backup must not hide this save's backups");
            assert!(list.iter().any(|b| Path::new(&b.path).starts_with(&own) && b.group == "matching"), "{list:?}");
            let legacy = std::fs::canonicalize(&f.src).unwrap();
            assert!(list.iter().any(|b| std::fs::canonicalize(&b.path).is_ok_and(|p| p == legacy) && b.group == "legacy"), "{list:?}");
            for gone in [&damaged, &unlisted] {
                assert!(!list.iter().any(|b| Path::new(&b.path) == gone.as_path()), "{} listed: {list:?}", gone.display());
                assert!(restore_backup(gone.to_str().unwrap(), f.target.to_str().unwrap()).is_err(), "{} restored", gone.display());
            }
            f.assert_original();
        }

        #[cfg(windows)]
        #[test]
        fn windows_drive_and_unc_aliases_share_backup_identity() {
            let f = Fixture::new();
            let plain = crate::winpath::plain_path(f.target.to_str().unwrap()).replace('/', "\\");
            let bytes = plain.as_bytes();
            assert_eq!(bytes.get(1), Some(&b':'), "test fixture must be on a drive: {plain}");
            let unc = format!(r"\\localhost\{}${}", &plain[..1], &plain[2..]);
            if !Path::new(&unc).is_file() {
                eprintln!("UNC alias unavailable; native runner must provide its existing administrative share");
                return;
            }
            let backup = backup_file(&plain).unwrap();
            let backed_up = Path::new(&backup).join("character0.ksd");
            let aliases = [plain.to_uppercase(), plain.replace('\\', "/"), format!(r"\\?\{plain}"), unc.clone(), format!(r"\\?\UNC\{}", &unc[2..])];
            for alias in aliases {
                let list = list_backups(&alias).unwrap();
                assert!(list.iter().any(|b| Path::new(&b.path) == backed_up && b.group == "matching"), "same save hidden for {alias}: {list:?}");
                restore_backup(backed_up.to_str().unwrap(), &alias).unwrap();
                f.assert_original();
            }
            let _lock = lock_target(&target_path(&f.target).unwrap()).unwrap();
            assert_eq!(write_save(&unc, &Fixture::bytes(3)).unwrap_err().code, ErrorCode::Busy);
        }

        #[cfg(windows)]
        #[test]
        fn windows_case_sensitive_saves_do_not_share_backups() {
            let f = Fixture::new();
            let root = f.root.join("case-sensitive");
            std::fs::create_dir(&root).unwrap();
            let enabled = std::process::Command::new("fsutil").args(["file", "setCaseSensitiveInfo"])
                .arg(&root).arg("enable").output().unwrap();
            if !enabled.status.success() {
                eprintln!("case-sensitive directory unavailable: {} {}", String::from_utf8_lossy(&enabled.stdout), String::from_utf8_lossy(&enabled.stderr));
                return;
            }
            let a = root.join("Account/character0.ksd");
            let b = root.join("account/character0.ksd");
            for (path, value) in [(&a, 4), (&b, 5)] {
                std::fs::create_dir_all(path.parent().unwrap()).unwrap();
                std::fs::write(path, Fixture::bytes(value)).unwrap();
            }
            assert_ne!(std::fs::read(&a).unwrap(), std::fs::read(&b).unwrap());
            let backup = backup_file(a.to_str().unwrap()).unwrap();
            let copied = Path::new(&backup).join("character0.ksd");
            let list = list_backups(b.to_str().unwrap()).unwrap();
            assert!(!list.iter().any(|entry| Path::new(&entry.path) == copied), "distinct case-sensitive save was treated as matching: {list:?}");
            assert_eq!(restore_backup(copied.to_str().unwrap(), b.to_str().unwrap()).unwrap_err().code, ErrorCode::Refused);
            assert_eq!(std::fs::read(&b).unwrap(), Fixture::bytes(5));
        }

        #[cfg(windows)]
        #[test]
        fn windows_short_names_share_backup_identity_and_lock() {
            use std::os::windows::ffi::{OsStrExt, OsStringExt};
            use windows_sys::Win32::Storage::FileSystem::GetShortPathNameW;
            let f = Fixture::new();
            let name: Vec<u16> = f.target.as_os_str().encode_wide().chain(Some(0)).collect();
            let mut short = vec![0u16; 32768];
            // SAFETY: input is terminated and output has the advertised capacity.
            let size = unsafe { GetShortPathNameW(name.as_ptr(), short.as_mut_ptr(), short.len() as u32) };
            assert!(size > 0 && size < short.len() as u32, "{}", std::io::Error::last_os_error());
            let short = PathBuf::from(std::ffi::OsString::from_wide(&short[..size as usize]));
            if !short.to_string_lossy().contains('~') {
                eprintln!("8.3 names are disabled for the fixture; no short alias was available");
                return;
            }
            eprintln!("8.3 alias exercised");
            let backup = backup_file(f.target.to_str().unwrap()).unwrap();
            let copied = Path::new(&backup).join("character0.ksd");
            assert!(list_backups(short.to_str().unwrap()).unwrap().iter().any(|b| Path::new(&b.path) == copied && b.group == "matching"));
            restore_backup(copied.to_str().unwrap(), short.to_str().unwrap()).unwrap();
            let _lock = lock_target(&target_path(&f.target).unwrap()).unwrap();
            assert_eq!(write_save(short.to_str().unwrap(), &Fixture::bytes(3)).unwrap_err().code, ErrorCode::Busy);
        }

        #[test]
        fn game_backup_of_another_file_is_refused() {
            let f = Fixture::new();
            let other = f.target.parent().unwrap().join("backup/character1.ksd.backup-1");
            std::fs::create_dir_all(other.parent().unwrap()).unwrap();
            std::fs::write(&other, Fixture::bytes(4)).unwrap();
            let result = restore_backup(other.to_str().unwrap(), f.target.to_str().unwrap());
            f.assert_original();
            assert_eq!(result.unwrap_err().code, ErrorCode::Refused);
        }

        #[test]
        fn save_failures_keep_original_and_release_lock() {
            for fault in [Fault::Write, Fault::Flush, Fault::Rename, Fault::Corrupt] {
                let f = Fixture::new();
                IO.with(|c| c.borrow_mut().fault = Some(fault));
                assert!(f.write().is_err());
                f.assert_original();
                f.assert_no_staging();
                IO.with(|c| c.borrow_mut().fault = None);
                f.write().unwrap();
            }
        }

        #[test]
        fn target_lock_is_not_a_save_or_backup_entry() {
            let f = Fixture::new();
            f.write().unwrap();
            let dir = crate::saves::SaveDir {
                label: "steam-1".into(), path: f.target.parent().unwrap().to_path_buf(), steam_id: None, recursive: false,
            };
            let saves = crate::saves::list_in(&[dir]).unwrap();
            assert_eq!(saves.len(), 1);
            assert_eq!(saves[0].file, "character0.ksd");
        }

        #[test]
        fn lock_crash_child() {
            let Some(path) = std::env::var_os("KILN_EDITOR_LOCK_CRASH_TARGET") else { return; };
            let _lock = lock_target(Path::new(&path)).unwrap();
            // Exit without running destructors, as with a process crash.
            std::process::exit(0);
        }

        #[test]
        fn stale_lock_after_process_exit_does_not_block_editor_or_game_writes() {
            let f = Fixture::new();
            let target = target_path(&f.target).unwrap();
            let child = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "saves::editor::tests::lock_crash_child", "--nocapture"])
                .env("KILN_EDITOR_LOCK_CRASH_TARGET", &target).output().unwrap();
            assert!(child.status.success(), "{}", String::from_utf8_lossy(&child.stderr));
            assert!(target.with_file_name(".character0.ksd.kiln-editor-lock").is_file());
            f.write().unwrap();
            let _lock = lock_target(&target).unwrap();
            // The editor never locks the game's save handle, only its own sidecar.
            std::fs::write(&target, Fixture::bytes(4)).unwrap();
            assert_eq!(std::fs::read(&target).unwrap(), Fixture::bytes(4));
        }

        #[test]
        fn independent_targets_can_change_together() {
            let f = Fixture::new();
            let _first = lock_target(&target_path(&f.target).unwrap()).unwrap();
            let other = f.target.with_file_name("character1.ksd");
            std::fs::write(&other, Fixture::bytes(4)).unwrap();
            write_save(other.to_str().unwrap(), &Fixture::bytes(3)).unwrap();
            f.assert_original();
        }

        #[cfg(windows)]
        #[test]
        fn windows_sharing_refusal_keeps_original() {
            use std::os::windows::fs::OpenOptionsExt;
            use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_SHARE_WRITE};
            let f = Fixture::new();
            // Another reader allows bytes to be written but refuses renaming/deletion of its file.
            let held = std::fs::OpenOptions::new().read(true).share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE).open(&f.target).unwrap();
            assert!(f.restore().is_err());
            f.assert_original();
            f.assert_no_staging();
            drop(held);
            f.restore().unwrap();
            assert_eq!(std::fs::read(&f.target).unwrap(), Fixture::bytes(2));
        }

        fn concurrent_changes(first_is_save: bool) {
            let f = Fixture::new();
            let entered = Arc::new(Barrier::new(2));
            let resume = Arc::new(Barrier::new(2));
            let target = f.target.clone(); let src = f.src.clone(); let backups = backup_dir().unwrap();
            let e = entered.clone(); let r = resume.clone();
            let worker = std::thread::spawn(move || {
                set_io(backups);
                IO.with(|c| c.borrow_mut().pause = Some((e, r)));
                if first_is_save { write_save(target.to_str().unwrap(), &Fixture::bytes(3)) }
                else { restore_backup(src.to_str().unwrap(), target.to_str().unwrap()) }
            });
            entered.wait();
            let restore = f.restore(); let save = f.write();
            resume.wait();
            worker.join().unwrap().unwrap();
            for result in [restore, save] {
                let err = result.expect_err("a second operation on this save was accepted");
                assert_eq!(err.code, ErrorCode::Busy);
                assert!(err.message.contains("already"));
            }
            // The lock must be released after the first operation returns.
            f.restore().unwrap();
        }
        #[test] fn restore_refuses_overlapping_changes() { concurrent_changes(false); }
        #[test] fn save_refuses_overlapping_changes() { concurrent_changes(true); }

        #[test]
        fn character_names() {
            assert!(is_character_file("character0.ksd"));
            assert!(is_character_file("character12.ksd"));
            assert!(!is_character_file("characters.ksd"));
            assert!(!is_character_file("character.ksd"));
            assert!(!is_character_file("world0.ksd"));
            assert!(is_save("x/a.KSD").is_ok());
            assert!(is_save("x/a.json").is_err());
        }

        #[cfg(feature = "saves")]
        #[test]
        fn zstd_round_trip() {
            let data = b"SV2C".repeat(100);
            let z = zstd_compress(&data).unwrap();
            assert!(z.len() < data.len());
            assert_eq!(zstd_decompress(&z).unwrap(), data);
            assert!(zstd_decompress(b"not zstd").is_err());
        }

        #[cfg(not(feature = "saves"))]
        #[test]
        fn zstd_needs_the_feature() {
            assert_eq!(zstd_compress(b"x").unwrap_err().code, ErrorCode::Unsupported);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kinds_and_times() {
        assert_eq!(slot_kind("character0.ksd"), SlotKind::Character);
        assert_eq!(slot_kind("world2.ksd"), SlotKind::World);
        assert_eq!(slot_kind("3ad85aea"), SlotKind::World);
        assert_eq!(slot_kind("3ad85aea_info"), SlotKind::Other);
        assert_eq!(slot_kind("global.ksd"), SlotKind::Other);
        assert_eq!(slot_kind("user_settings.json"), SlotKind::Other);
        let t = UNIX_EPOCH + std::time::Duration::from_secs(1_793_756_357);
        assert_eq!(rfc3339(t), "2026-11-04T01:39:17Z");
        assert_eq!(stamp(t), "20261104T013917Z");
    }

    /// Windows: the Saved Games known folder from the registry (expanded, absolute), and both of the game's folders in
    /// it for a native client.
    #[cfg(windows)]
    #[test]
    fn windows_saved_games_known_folder() {
        let base = windows_saved_games().expect("Saved Games folder");
        assert!(base.is_absolute() && !base.to_string_lossy().contains('%'), "{}", base.display());
        if let Some(home) = std::env::var_os("USERPROFILE") {
            // the default location unless the user moved it
            eprintln!("Saved Games: {} (USERPROFILE {})", base.display(), Path::new(&home).display());
        }
        let g = GameInstall {
            id: "x".into(),
            kind: GameKind::Client,
            path: PathBuf::from(r"C:\Games\Enshrouded"),
            exe: "enshrouded.exe".into(),
            app_id: crate::CLIENT_APP_ID,
            library: None,
            platform: crate::game::Platform::Windows,
            prefix: None,
            build_id: None,
            exe_sha256: None,
            steam_flavor: None,
            path_mode: crate::game::PathMode::Native,
            source: "manual".into(),
        };
        assert_eq!(local_settings_dirs(&g), vec![base.join("keen games").join("Enshrouded"), base.join("Enshrouded")]);
    }
}
