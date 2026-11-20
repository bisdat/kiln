//! (0.2.0-dev.29) The "before beta" copy: the small files a test build of the launcher may change in a way the release
//! reads differently, copied before its first start writes anything, so that a switch back to the release can put them
//! back (`update_switch` with `restore`; docs/kiln/launcher/releasing.md, "Going back from the beta").
//!
//! - **What:** `settings.json` and each profile's `profiles/<pid>/meta.json` (its mods, their switches and order).
//!   The live kept journal and its settings folders stay together. Never the mods themselves (the Library's
//!   folders and their records), the profiles' mod settings (`config/`, `data/`), game folders or saves, nor
//!   `update.json` (the self-update's own record: the switch back writes it for the release's "Updated to").
//! - **Where:** `<data>/snapshots/before-beta/`: the files at their paths relative to the data folder, and `snapshot.json`
//!   ([`Info`]: when, the build that wrote the folder last, the test build that took it, the files).
//! - **When:** at the start of a test build ([`at_start`], before anything else of the start writes in the data
//!   folder), when the build that ran before was no test build: `<data>/state/launcher-build.json` ([`LastBuild`])
//!   records the build of every start from dev.29 on (a launcher before dev.29 writes none: no file counts as a release
//!   build). A test build after a test build keeps the copy it has; a switch back records the release it goes to
//!   ([`plan_leave_beta`]). Only a release that knows this marker acknowledges its actual start; until then
//!   the next beta retains the original whole copy. A legacy release cannot provide that acknowledgement. A data folder with none of the files gets no copy.
//! - **Restore** ([`restore`], at "Restart now" of a switch back that asked for it): each file of the copy goes back in
//!   place (written atomically); a profile made in the beta stays (its meta.json is not in the copy), and so does every mod
//!   added in the beta (the Library has no index file: each entry's record is in its own folder).

pub mod startup;

use kiln_manage::{Error, ErrorCode};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// `<data>/snapshots/before-beta/`.
pub const DIR: &str = "snapshots/before-beta";
/// `<data>/state/launcher-build.json`.
pub const LAST_BUILD: &str = "state/launcher-build.json";
const INFO: &str = "snapshot.json";
/// A file larger than this is not copied (none of these is anywhere near it).
const MAX_FILE: u64 = 8 * 1024 * 1024;

/// `<data>/state/launcher-build.json`: the build of the last start.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LastBuild {
    pub format: u32,
    pub version: String,
    /// "stable", "dev" or "beta".
    pub channel: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub beta: Option<u32>,
    #[serde(default)]
    pub without_restore: Vec<String>,
    #[serde(default)]
    pub pending_release: bool,
    #[serde(default)]
    pub copy_complete: Option<bool>,
}

/// `snapshot.json`, and `UpdateStatus.beforeBeta`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Info {
    pub format: u32,
    /// RFC 3339.
    pub taken: String,
    /// The build that wrote the data folder last before the test build ("" when not known: a launcher before dev.29).
    pub from: String,
    /// The test build that took it (its id, `0.2.0-dev.29+beta.3`).
    pub by: String,
    /// The files in it, relative to the data folder, '/' separated.
    pub files: Vec<String>,
    #[serde(default)]
    pub hashes: std::collections::BTreeMap<String, String>,
}

fn io(p: &Path, what: &str) -> impl FnOnce(std::io::Error) -> Error {
    let (what, p) = (what.to_string(), p.display().to_string());
    move |e| Error::new(ErrorCode::Io, format!("{what} {p}: {e}"))
}

/// The files a copy takes from `root` (those that are there), relative and '/' separated, sorted.
pub fn candidates(root: &Path) -> Vec<String> {
    checked_candidates(root).unwrap_or_default()
}

fn checked_candidates(root: &Path) -> Result<Vec<String>, Error> {
    // The kept journal owns mutable settings folders. It cannot be rewound alone.
    let mut paths = vec!["settings.json".to_string()];
    let mut issues = Vec::new();
    let profiles = root.join("profiles");
    if let Err(e) = check_components(root, "profiles") { issues.push(e.message); }
    else {
        match std::fs::read_dir(&profiles) {
            Ok(entries) => for entry in entries {
                match entry {
                    Ok(e) => {
                        let name = e.file_name().to_string_lossy().to_string();
                        if name.starts_with('.') { continue; }
                        match e.file_type() {
                            Ok(t) if t.is_dir() => paths.push(format!("profiles/{name}/meta.json")),
                            Ok(t) if t.is_symlink() => issues.push(format!("profiles/{name}: symbolic link was not copied")),
                            Ok(_) => {},
                            Err(e) => issues.push(format!("profiles/{name}: {e}")),
                        }
                    },
                    Err(e) => issues.push(format!("profiles: {e}")),
                }
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
            Err(e) => issues.push(format!("profiles: {e}")),
        }
    }
    let mut files = Vec::new();
    for rel in paths {
        if !allowed(&rel) { issues.push(format!("{rel}: path cannot be restored")); continue; }
        if let Err(e) = check_components(root, &rel) { issues.push(e.message); continue; }
        match std::fs::symlink_metadata(root.join(&rel)) {
            Ok(m) if !m.is_file() => issues.push(format!("{rel}: not a regular file")),
            Ok(m) if m.len() > MAX_FILE => issues.push(format!("{rel}: larger than {MAX_FILE} bytes")),
            Ok(_) => match std::fs::File::open(root.join(&rel)) {
                Ok(_) => files.push(rel),
                Err(e) => issues.push(format!("{rel}: unreadable ({e})")),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound && !rel.starts_with("profiles/") => {},
            Err(e) => issues.push(format!("{rel}: {e}")),
        }
    }
    if !issues.is_empty() { return Err(Error::new(ErrorCode::Refused, issues.join("\n"))); }
    files.sort();
    Ok(files)
}

pub(super) fn sync_dir(path: &Path) -> Result<(), Error> {
    #[cfg(unix)]
    std::fs::File::open(path).and_then(|f| f.sync_all()).map_err(io(path, "flushing directory"))?;
    Ok(())
}

pub(super) fn durable_write(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    let parent = path.parent().ok_or_else(|| Error::invalid("No snapshot parent folder"))?;
    let mut missing = Vec::new();
    let mut next = parent;
    while !next.exists() {
        missing.push(next.to_path_buf());
        next = next.parent().ok_or_else(|| Error::invalid("No snapshot ancestor"))?;
    }
    std::fs::create_dir_all(parent).map_err(io(parent, "creating"))?;
    for dir in missing.into_iter().rev() { sync_dir(&dir)?; sync_dir(dir.parent().unwrap())?; }
    kiln_manage::util::write_atomic_durable_exclusive(path, bytes)
}

fn durable_json(path: &Path, value: &impl Serialize) -> Result<(), Error> {
    durable_write(path, &serde_json::to_vec_pretty(value).map_err(|e| Error::new(ErrorCode::Internal, e.to_string()))?)
}

/// The copy that is there, if any.
pub fn info(root: &Path) -> Option<Info> {
    if read_last(root).is_some_and(|l| !l.without_restore.is_empty() || l.copy_complete == Some(false)) { return None; }
    let i = kiln_manage::util::read_json::<Info>(&root.join(DIR).join(INFO)).ok().flatten().filter(|i| i.format == 1)?;
    for rel in &i.files {
        if !allowed(rel) { continue; }
        let bytes = std::fs::read(root.join(DIR).join(rel)).ok()?;
        if i.hashes.get(rel).is_some_and(|hash| *hash != kiln_manage::hash::sha256_hex(&bytes)) { return None; }
    }
    Some(i)
}

/// Takes a new copy of `root`'s files ([`candidates`]), replacing the one there. None when there is nothing to copy.
pub fn take(root: &Path, from: &str, by: &str) -> Result<Option<Info>, Error> {
    for rel in [DIR, "snapshots/before-beta.new", "snapshots/before-beta.old", "snapshots/before-beta.pending.json"] { check_components(root, rel)?; }
    let by_pending = root.join("snapshots/before-beta.pending.json");
    let pending: Option<(String, String)> = kiln_manage::util::read_json(&by_pending)?;
    if pending.as_ref() == Some(&(from.to_string(), by.to_string())) {
        if let Some(i) = info(root).filter(|i| i.from == from && i.by == by) { return Ok(Some(i)); }
    }
    let files = checked_candidates(root)?;
    if files.is_empty() {
        return Ok(None);
    }
    durable_json(&by_pending, &(from, by))?;
    let dir = root.join(DIR);
    let stage = root.join(format!("{DIR}.new"));
    let _ = std::fs::remove_dir_all(&stage);
    let mut hashes = std::collections::BTreeMap::new();
    for rel in &files {
        let to = stage.join(rel);
        if let Some(p) = to.parent() {
            std::fs::create_dir_all(p).map_err(io(p, "creating"))?;
        }
        let bytes = std::fs::read(root.join(rel)).map_err(io(&to, "copying to"))?;
        durable_write(&to, &bytes)?;
        hashes.insert(rel.clone(), kiln_manage::hash::sha256_hex(&bytes));
    }
    let info = Info { format: 1, taken: kiln_manage::util::now_rfc3339(), from: from.into(), by: by.into(), files, hashes };
    durable_json(&stage.join(INFO), &info)?;
    sync_tree_dirs(&stage)?;
    // the old copy goes only when the new one is whole
    let old = root.join(format!("{DIR}.old"));
    let _ = std::fs::remove_dir_all(&old);
    if dir.exists() {
        durable_rename(&dir, &old)?;
    }
    durable_rename(&stage, &dir)?;
    sync_dir(dir.parent().unwrap())?;
    let _ = std::fs::remove_dir_all(&old);
    Ok(Some(info))
}

fn read_last(root: &Path) -> Option<LastBuild> {
    kiln_manage::util::read_json::<LastBuild>(&root.join(LAST_BUILD)).ok().flatten()
}

fn write_last(root: &Path, last: &LastBuild) -> Result<(), Error> {
    check_components(root, LAST_BUILD)?;
    durable_json(&root.join(LAST_BUILD), last)
}

/// At the start (before anything else writes in the data folder): a test build after a build that was none takes the
/// copy; every build records itself. `version`: the package version; `channel` the build's own (feed::Channel's
/// word); `beta` its beta number. Returns the copy it took.
pub fn at_start(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> Result<Option<Info>, Error> {
    let last = read_last(root);
    let mut took = None;
    if let Some(n) = beta
        && last.as_ref().is_none_or(|l| l.beta.is_none() && !l.pending_release)
    {
        let from = last.as_ref().map(|l| l.version.clone()).unwrap_or_default();
        let by = format!("{version}+beta.{n}");
        took = match info(root).filter(|i| last.is_none() || i.by == by) {
            Some(i) => Some(i),
            None => take(root, &from, &by)?,
        };
    }
    let copy_complete = beta.map(|_| {
        last.as_ref().filter(|l| l.beta.is_some() || l.pending_release)
            .and_then(|l| l.copy_complete).unwrap_or_else(|| took.is_some() || info(root).is_some())
    });
    let copy_complete = if beta.is_some() && last.as_ref().is_none_or(|l| l.beta.is_none() && !l.pending_release) { Some(took.is_some()) } else { copy_complete };
    let now = LastBuild { format: 1, version: version.into(), channel: channel.into(), beta, without_restore: last.as_ref().filter(|l| (l.beta.is_some() || l.pending_release) && beta.is_some()).map(|l| l.without_restore.clone()).unwrap_or_default(), pending_release: false, copy_complete };
    if last.as_ref() != Some(&now) {
        write_last(root, &now)?;
    }
    let _ = std::fs::remove_file(root.join("snapshots/before-beta.pending.json"));
    Ok(took)
}

/// Records an acknowledged release start. A planned restart uses `plan_leave_beta` instead.
pub fn left_beta(root: &Path, to_version: &str, channel: &str) -> Result<(), Error> {
    write_last(root, &LastBuild { format: 1, version: to_version.into(), channel: channel.into(), beta: None, without_restore: vec![], pending_release: false, copy_complete: None })
}

/// Puts the copy back: each of its files to its place (atomically). Returns the files put back. `channel`: the channel
/// the switch goes to. The copy was taken at the test build's first start, after the release build saved the player's
/// choice of the beta (`updateChannel: "beta"`): a restored settings.json that says "beta" says `channel` instead, or
/// the release would offer the test build again at its first check. Any other value (none, "dev", "stable") stays.
pub fn restore(root: &Path, channel: Option<&str>) -> Result<Vec<String>, Error> {
    let info = info(root).ok_or_else(|| Error::not_found("There is no copy from before the beta."))?;
    let dir = root.join(DIR);
    let mut prepared = Vec::new();
    for rel in &info.files {
        // only the kinds of file a copy takes, never a path out of the data folder
        if !allowed(rel) { continue; }
        // Preflight the whole copy before replacing anything. A symlinked root is fine;
        // links below it must not redirect either the copy or the destination.
        check_components(root, &format!("{DIR}/{rel}"))?;
        check_components(root, rel)?;
        let from = dir.join(rel);
        let mut bytes = std::fs::read(&from).map_err(io(&from, "reading"))?;
        if rel == "settings.json"
            && let Some(ch) = channel
            && let Ok(mut v) = serde_json::from_slice::<serde_json::Value>(&bytes)
            && v.get("updateChannel").and_then(|c| c.as_str()) == Some("beta")
            && ch != "beta"
        {
            v["updateChannel"] = ch.into();
            bytes = serde_json::to_vec_pretty(&v).map_err(|e| Error::new(ErrorCode::Internal, e.to_string()))?;
            bytes.push(b'\n');
        }
        check_profile_packages(root, rel, &bytes)?;
        prepared.push((rel.clone(), bytes));
    }
    restore_transaction(root, prepared)
}

// Metadata-only restore cannot bring back packages removed by a legacy launcher
// or by hand. Check all entries, even disabled ones, before any destination writes.
fn check_profile_packages(root: &Path, rel: &str, bytes: &[u8]) -> Result<(), Error> {
    if !rel.starts_with("profiles/") { return Ok(()); }
    let profile: kiln_manage::profile::Profile = serde_json::from_slice(bytes)
        .map_err(|e| Error::invalid(format!("Cannot restore {rel}: {e}")))?;
    let dirs = kiln_manage::paths::DataDirs::at(root);
    let mut missing = Vec::new();
    for kind in kiln_manage::manifest::ModKind::ALL {
        for m in profile.mods.list(kind) {
            let r = kiln_manage::library::LibraryRef { kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
            let there = kiln_manage::library::record(&dirs, &r).is_ok()
                && kiln_manage::library::path_of(&dirs, &r).is_ok_and(|p| if kind == kiln_manage::manifest::ModKind::Plugin { p.is_file() } else { p.is_dir() });
            if !there { missing.push(format!("{} {} {} ({})", kind.as_str(), m.id, m.version, m.hash)); }
        }
    }
    if !missing.is_empty() {
        return Err(Error::new(ErrorCode::NotFound, format!("Cannot restore {rel}. Reimport these missing Library packages first: {}.", missing.join(", "))));
    }
    Ok(())
}

// Inspect existing components without following a link. Missing destination components
// are made later; missing source components fail when the copy is read.
fn check_components(root: &Path, rel: &str) -> Result<(), Error> {
    let mut path = root.to_path_buf();
    for component in rel.split('/') {
        path.push(component);
        match std::fs::symlink_metadata(&path) {
            Ok(m) if m.file_type().is_symlink() => return Err(Error::new(ErrorCode::Refused,
                format!("Cannot restore through a symbolic link: {}", path.display()))),
            Ok(_) => {},
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
            Err(e) => return Err(io(&path, "checking")(e)),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;

fn sync_tree_dirs(dir: &Path) -> Result<(), Error> {
    for e in std::fs::read_dir(dir).map_err(io(dir, "reading"))? {
        let e = e.map_err(io(dir, "reading"))?;
        if e.file_type().map_err(io(&e.path(), "reading"))?.is_dir() { sync_tree_dirs(&e.path())?; }
    }
    sync_dir(dir)
}

pub fn without_restore(root: &Path) -> Vec<String> {
    read_last(root).map(|l| l.without_restore).unwrap_or_default()
}

pub fn continue_without_restore(root: &Path, version: &str, channel: &str, beta: Option<u32>, reasons: Vec<String>) -> Result<(), Error> {
    let reasons = if reasons.is_empty() { vec!["The player continued without a copy from before the beta.".into()] } else { reasons };
    write_last(root, &LastBuild { format: 1, version: version.into(), channel: channel.into(), beta, without_restore: reasons, pending_release: false, copy_complete: Some(false) })
}

fn allowed(rel: &str) -> bool {
    !rel.contains('\\') && (rel == "settings.json" ||
        (rel.starts_with("profiles/") && rel.ends_with("/meta.json") && rel.split('/').count() == 3 && rel.split('/').all(|p| !p.is_empty() && p != "." && p != "..")))
}

#[cfg(test)]
thread_local! { static RESTORE_FAIL: std::cell::Cell<Option<usize>> = const { std::cell::Cell::new(None) }; }
fn restore_step() -> Result<(), Error> {
    #[cfg(test)]
    RESTORE_FAIL.with(|f| match f.get() {
        Some(0) => Err(Error::new(ErrorCode::Io, "injected restore interruption")),
        Some(n) => { f.set(Some(n - 1)); Ok(()) },
        None => Ok(()),
    })?;
    Ok(())
}
pub const RESTORE_DIR: &str = "snapshots/restore-beta";
const JOURNAL: &str = "snapshots/restore-beta/journal.json";

#[derive(Serialize, Deserialize)]
struct RestoreJournal {
    format: u32,
    files: std::collections::BTreeMap<String, String>,
}

fn restore_transaction(root: &Path, prepared: Vec<(String, Vec<u8>)>) -> Result<Vec<String>, Error> {
    recover_restore(root)?;
    check_components(root, RESTORE_DIR)?;
    let dir = root.join(RESTORE_DIR);
    if dir.exists() { std::fs::remove_dir_all(&dir).map_err(io(&dir, "removing abandoned stage"))?; }
    let mut files = std::collections::BTreeMap::new();
    for (rel, bytes) in prepared {
        restore_step()?;
        durable_write(&dir.join("new").join(&rel), &bytes)?;
        restore_step()?;
        files.insert(rel, kiln_manage::hash::sha256_hex(&bytes));
    }
    if files.is_empty() { return Ok(vec![]); }
    sync_tree_dirs(&dir)?;
    restore_step()?;
    durable_json(&root.join(JOURNAL), &RestoreJournal { format: 1, files })?;
    sync_dir(root.join("snapshots").as_path())?;
    restore_step()?;
    finish_restore(root)
}

pub fn recover_restore(root: &Path) -> Result<(), Error> {
    check_components(root, JOURNAL)?;
    if !root.join(JOURNAL).try_exists().map_err(io(&root.join(JOURNAL), "checking"))? { return Ok(()); }
    finish_restore(root).map(|_| ())
}

fn finish_restore(root: &Path) -> Result<Vec<String>, Error> {
    let journal: RestoreJournal = kiln_manage::util::read_json(&root.join(JOURNAL))?
        .ok_or_else(|| Error::invalid("The beta restore journal is missing."))?;
    if journal.format != 1 { return Err(Error::invalid("Unknown beta restore journal format.")); }
    // Verify every staged file before the first replacement, including on recovery.
    let mut prepared = Vec::new();
    for (rel, hash) in journal.files {
        // Older stages included the journal without its settings payload.
        if rel == "library/kiln-kept.json" { continue; }
        if !allowed(&rel) { return Err(Error::invalid("Unsafe path in beta restore journal.")); }
        check_components(root, &rel)?;
        check_components(root, &format!("{RESTORE_DIR}/new/{rel}"))?;
        let bytes = std::fs::read(root.join(RESTORE_DIR).join("new").join(&rel)).map_err(io(&root.join(&rel), "reading restore stage"))?;
        if kiln_manage::hash::sha256_hex(&bytes) != hash { return Err(Error::invalid("The staged beta restore file is incomplete.")); }
        check_profile_packages(root, &rel, &bytes)?;
        prepared.push((rel, bytes));
    }
    let mut done = Vec::new();
    for (rel, bytes) in prepared {
        restore_step()?;
        durable_write(&root.join(&rel), &bytes)?;
        restore_step()?;
        done.push(rel);
    }
    restore_step()?;
    std::fs::remove_file(root.join(JOURNAL)).map_err(io(&root.join(JOURNAL), "completing restore"))?;
    sync_dir(&root.join(RESTORE_DIR))?;
    restore_step()?;
    // The journal's absence is the commit. Keep its durable staging bytes until
    // another restore starts; old launchers ignore this folder.
    Ok(done)
}

fn durable_rename(from: &Path, to: &Path) -> Result<(), Error> {
    #[cfg(not(windows))]
    {
        std::fs::rename(from, to).map_err(io(to, "renaming"))?;
        sync_dir(to.parent().unwrap())
    }
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        unsafe extern "system" { fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32; }
        let a: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
        let b: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(a.as_ptr(), b.as_ptr(), 8) } == 0 { return Err(io(to, "renaming")(std::io::Error::last_os_error())); }
        Ok(())
    }
}

pub fn plan_leave_beta(root: &Path, version: &str, channel: &str) -> Result<(), Error> {
    // An installer may fail or be canceled. Until a release that knows the
    // marker actually starts, retain the whole original copy across *all* betas.
    write_last(root, &LastBuild { format: 1, version: version.into(), channel: channel.into(), beta: None,
        without_restore: without_restore(root), pending_release: true, copy_complete: read_last(root).and_then(|l| l.copy_complete) })
}
