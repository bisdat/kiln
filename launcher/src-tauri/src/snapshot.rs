//! (0.2.0-dev.29) The "before beta" copy: the small files a test build of the launcher may change in a way the release
//! reads differently, copied before its first start writes anything, so that a switch back to the release can put them
//! back (`update_switch` with `restore`; docs/kiln/launcher/releasing.md, "Going back from the beta").
//!
//! - **What:** `settings.json`, `update.json`, each profile's `profiles/<pid>/meta.json` (its mods, their switches and
//!   order) and the Library's `library/kiln-kept.json` (the versions kept for a roll back). Never the mods themselves (the
//!   Library's folders and their records), the profiles' mod settings (`config/`, `data/`), game folders or saves.
//! - **Where:** `<data>/snapshots/before-beta/`: the files at their paths relative to the data folder, and `snapshot.json`
//!   ([`Info`]: when, the build that wrote the folder last, the test build that took it, the files).
//! - **When:** at the start of a test build ([`at_start`], before anything else of the start writes in the data
//!   folder), when the build that ran before was no test build: `<data>/state/launcher-build.json` ([`LastBuild`])
//!   records the build of every start from dev.29 on (a launcher before dev.29 writes none: no file counts as a release
//!   build). A test build after a test build keeps the copy it has; a switch back records the release it goes to
//!   ([`left_beta`]), so the next test build takes a new copy. A data folder with none of the files gets no copy.
//! - **Restore** ([`restore`], at "Restart now" of a switch back that asked for it): each file of the copy goes back in
//!   place (written atomically); a profile made in the beta stays (its meta.json is not in the copy), and so does every mod
//!   added in the beta (the Library has no index file: each entry's record is in its own folder).

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
}

fn io(p: &Path, what: &str) -> impl FnOnce(std::io::Error) -> Error {
    let (what, p) = (what.to_string(), p.display().to_string());
    move |e| Error::new(ErrorCode::Io, format!("{what} {p}: {e}"))
}

/// The files a copy takes from `root` (those that are there), relative and '/' separated, sorted.
pub fn candidates(root: &Path) -> Vec<String> {
    let mut out: Vec<String> = ["settings.json", "update.json", "library/kiln-kept.json"]
        .iter()
        .filter(|r| root.join(r).is_file())
        .map(|r| r.to_string())
        .collect();
    let profiles = root.join("profiles");
    let mut ids: Vec<String> = std::fs::read_dir(&profiles)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .filter_map(|e| e.file_name().to_str().map(String::from))
        .filter(|id| !id.starts_with('.') && profiles.join(id).join("meta.json").is_file())
        .collect();
    ids.sort();
    out.extend(ids.into_iter().map(|id| format!("profiles/{id}/meta.json")));
    out.retain(|r| std::fs::metadata(root.join(r)).is_ok_and(|m| m.len() <= MAX_FILE));
    out.sort();
    out
}

/// The copy that is there, if any.
pub fn info(root: &Path) -> Option<Info> {
    kiln_manage::util::read_json::<Info>(&root.join(DIR).join(INFO)).ok().flatten().filter(|i| i.format == 1)
}

/// Takes a new copy of `root`'s files ([`candidates`]), replacing the one there. None when there is nothing to copy.
pub fn take(root: &Path, from: &str, by: &str) -> Result<Option<Info>, Error> {
    let files = candidates(root);
    if files.is_empty() {
        return Ok(None);
    }
    let dir = root.join(DIR);
    let stage = root.join(format!("{DIR}.new"));
    let _ = std::fs::remove_dir_all(&stage);
    for rel in &files {
        let to = stage.join(rel);
        if let Some(p) = to.parent() {
            std::fs::create_dir_all(p).map_err(io(p, "creating"))?;
        }
        std::fs::copy(root.join(rel), &to).map_err(io(&to, "copying to"))?;
    }
    let info = Info { format: 1, taken: kiln_manage::util::now_rfc3339(), from: from.into(), by: by.into(), files };
    kiln_manage::util::write_json_atomic(&stage.join(INFO), &info)?;
    // the old copy goes only when the new one is whole
    let old = root.join(format!("{DIR}.old"));
    let _ = std::fs::remove_dir_all(&old);
    if dir.exists() {
        std::fs::rename(&dir, &old).map_err(io(&dir, "moving aside"))?;
    }
    std::fs::rename(&stage, &dir).map_err(io(&dir, "putting in place"))?;
    let _ = std::fs::remove_dir_all(&old);
    Ok(Some(info))
}

fn read_last(root: &Path) -> Option<LastBuild> {
    kiln_manage::util::read_json::<LastBuild>(&root.join(LAST_BUILD)).ok().flatten()
}

fn write_last(root: &Path, last: &LastBuild) -> Result<(), Error> {
    kiln_manage::util::write_json_atomic(&root.join(LAST_BUILD), last)
}

/// At the start (before anything else writes in the data folder): a test build after a build that was none takes the
/// copy; every build records itself. `version`: the package version; `channel` the build's own (feed::Channel's
/// word); `beta` its beta number. Returns the copy it took.
pub fn at_start(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> Result<Option<Info>, Error> {
    let last = read_last(root);
    let mut took = None;
    if let Some(n) = beta
        && last.as_ref().is_none_or(|l| l.beta.is_none())
    {
        let from = last.as_ref().map(|l| l.version.clone()).unwrap_or_default();
        took = take(root, &from, &format!("{version}+beta.{n}"))?;
    }
    let now = LastBuild { format: 1, version: version.into(), channel: channel.into(), beta };
    if last.as_ref() != Some(&now) {
        write_last(root, &now)?;
    }
    Ok(took)
}

/// A switch back to the release line is ready to restart: the next start is no test build (a release before dev.29
/// records nothing itself), so the next test build after it takes a new copy.
pub fn left_beta(root: &Path, to_version: &str, channel: &str) -> Result<(), Error> {
    write_last(root, &LastBuild { format: 1, version: to_version.into(), channel: channel.into(), beta: None })
}

/// Puts the copy back: each of its files to its place (atomically). Returns the files put back.
pub fn restore(root: &Path) -> Result<Vec<String>, Error> {
    let info = info(root).ok_or_else(|| Error::not_found("There is no copy from before the beta."))?;
    let dir = root.join(DIR);
    let mut done = Vec::new();
    for rel in &info.files {
        // only the kinds of file a copy takes, never a path out of the data folder
        let ok = rel == "settings.json" || rel == "update.json" || rel == "library/kiln-kept.json"
            || (rel.starts_with("profiles/") && rel.ends_with("/meta.json") && rel.split('/').count() == 3
                && rel.split('/').all(|p| !p.is_empty() && p != "." && p != ".."));
        if !ok {
            continue;
        }
        let from = dir.join(rel);
        let bytes = std::fs::read(&from).map_err(io(&from, "reading"))?;
        let to: PathBuf = root.join(rel);
        if let Some(p) = to.parent() {
            std::fs::create_dir_all(p).map_err(io(p, "creating"))?;
        }
        kiln_manage::util::write_atomic(&to, &bytes)?;
        done.push(rel.clone());
    }
    Ok(done)
}

#[cfg(test)]
mod tests;
