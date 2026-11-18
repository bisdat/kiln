//! The vendor files of one game folder: `<game>/kiln/vendor/<vendor>/<version>/`, each version folder with its
//! receipt `kiln-vendor.json` (`schema/vendor-receipt.schema.json`), written last.
//!
//! - [`Store::status`]: one [`ComponentStatus`] per manifest component, from the receipts and the files' presence,
//!   sizes and SHA-256 (a file's hash is kept for this process while its size and modification time stay, so only the
//!   first look after a start or a change reads the files). The checks that count are the download's (never cached)
//!   and Kiln's in the game before it loads a file; the status is what the page shows.
//! - [`Store::remove`]: a component's version folders, by the names Kiln put there (the receipt's and the manifest's
//!   files, their `.part` files and the receipt). Anything else in a folder stays, named, and so does the folder.
//! - The download is [`super::download`].
//!
//! Nothing here follows a link: a link anywhere below the game folder on the way to `kiln/vendor/` refuses the change.
//! `<game>/kiln/vendor/.lock` serializes downloads and removals of one game folder across launchers; it is never
//! removed.

use super::agreements::Agreements;
use super::manifest::{file_name_ok, version_ok, Component, VendorManifest};
use crate::game::{GameInstall, GameKind};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// The store's folder, relative to the game folder.
pub const DIR: &str = "kiln/vendor";
/// The receipt in a version folder.
pub const RECEIPT: &str = "kiln-vendor.json";
/// A file being downloaded: `<name>.part`, next to where it goes.
pub const PART_SUFFIX: &str = ".part";
/// The lock file in `kiln/vendor/`.
pub const LOCK: &str = ".lock";
pub const RECEIPT_FORMAT: u32 = 1;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Receipt {
    pub format: u32,
    pub component: String,
    pub vendor: String,
    pub version: String,
    /// RFC 3339 UTC.
    pub installed: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub installer: Option<String>,
    pub license: ReceiptLicense,
    pub files: Vec<ReceiptFile>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReceiptLicense {
    pub name: String,
    pub sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReceiptFile {
    pub name: String,
    pub size: u64,
    pub sha256: String,
    pub url: String,
}

/// What the page shows for a component.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum VendorState {
    /// Pinned, nothing of this component in the game folder (`partial` may hold an interrupted download).
    NotDownloaded,
    /// The manifest's version is there, complete.
    Downloaded,
    /// An earlier version is there; the manifest's (pinned) version is not yet.
    UpdateAvailable,
    /// A file of the manifest's version is not pinned in this build: never downloaded.
    Unavailable,
    /// The manifest's version has a receipt, but a file is missing, is not a plain file, has another size or another
    /// SHA-256, or the receipt's hashes are not this build's.
    Damaged,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentStatus {
    pub id: String,
    pub vendor: String,
    pub vendor_name: String,
    pub name: String,
    pub enables: String,
    /// The manifest's version (what a download gets).
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<String>,
    pub license: LicenseInfo,
    /// The hosts the files come from (raw.githubusercontent.com).
    pub hosts: Vec<String>,
    /// Bytes a download fetches (every file).
    pub size: u64,
    pub files: Vec<FileInfo>,
    pub state: VendorState,
    /// The newest version of this component in the game folder.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub installed: Option<Installed>,
    /// Bytes of an interrupted download of the manifest's version, kept to go on from.
    pub partial: u64,
    /// For `unavailable` and `damaged`: one sentence.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LicenseInfo {
    pub name: String,
    pub sha256: String,
    pub from: String,
    /// The player agreed to this text (for this vendor).
    pub agreed: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub agreed_at: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileInfo {
    pub name: String,
    pub size: u64,
    pub pinned: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Installed {
    pub version: String,
    /// The folder, relative to the game folder (`kiln/vendor/amd/2.4.0`).
    pub folder: String,
    /// Bytes of its files that are there.
    pub size_on_disk: u64,
    /// The receipt's time.
    pub installed: String,
    /// The files that are missing, not plain files, or of another size or SHA-256 than the receipt's.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub problems: Vec<String>,
}

/// What [`Store::remove`] did.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveReport {
    /// The version folders removed (relative to the game folder).
    pub removed: Vec<String>,
    /// Bytes freed.
    pub freed: u64,
    /// What stayed, one sentence each (a file Kiln did not put there, a folder that is not empty).
    pub kept: Vec<String>,
}

/// One game folder's vendor store.
#[derive(Debug, Clone)]
pub struct Store {
    game: PathBuf,
}

/// The guard of the store's lock (held while a download or a removal runs).
pub struct StoreLock(#[allow(dead_code)] std::fs::File);

impl Store {
    /// The store of a game folder (tests, tools). The launcher uses [`Store::for_game`].
    pub fn at(game_dir: impl Into<PathBuf>) -> Store {
        Store { game: game_dir.into() }
    }

    /// The store of a client install. A dedicated server draws no frames: refused.
    pub fn for_game(g: &GameInstall) -> Result<Store> {
        if g.kind != GameKind::Client {
            return Err(Error::new(ErrorCode::Unsupported, "A dedicated server draws no frames: upscalers are for the game client only."));
        }
        Ok(Store::at(&g.path))
    }

    pub fn game_dir(&self) -> &Path {
        &self.game
    }

    /// `<game>/kiln/vendor`
    pub fn root(&self) -> PathBuf {
        self.game.join(DIR)
    }

    /// `<game>/kiln/vendor/<vendor>/<version>`
    pub fn folder(&self, vendor: &str, version: &str) -> PathBuf {
        self.root().join(vendor).join(version)
    }

    /// The folder relative to the game folder, with forward slashes.
    pub fn rel(vendor: &str, version: &str) -> String {
        format!("{DIR}/{vendor}/{version}")
    }

    /// Refuses a path below the game folder that passes through a link (and a game folder without `kiln/`).
    pub(crate) fn guard(&self, path: &Path) -> Result<()> {
        crate::engine_mods::refuse_symlink_path(&self.game, path)
    }

    /// Takes the store's lock, or fails at once when another download or removal holds it.
    pub fn lock(&self) -> Result<StoreLock> {
        let kiln = self.game.join(crate::paths::game::KILN);
        if !kiln.is_dir() {
            return Err(Error::not_found(format!("{} has no kiln/ folder: install Kiln first.", self.game.display())));
        }
        let root = self.root();
        self.guard(&root)?;
        std::fs::create_dir_all(&root).map_err(crate::util::io_at(&root, "creating"))?;
        let path = root.join(LOCK);
        let f = std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(&path).map_err(crate::util::io_at(&path, "opening"))?;
        match f.try_lock() {
            Ok(()) => Ok(StoreLock(f)),
            Err(std::fs::TryLockError::WouldBlock) => Err(Error::new(ErrorCode::Busy, "Kiln is already downloading or removing vendor files for this game. Wait for it to finish.")),
            Err(std::fs::TryLockError::Error(e)) => Err(crate::util::io_at(&path, "locking")(e)),
        }
    }

    /// Every version folder of `vendor` with a receipt of `component`, newest version first. Folders without a
    /// readable receipt are not installs (an interrupted download, another build's files), and neither are folders
    /// whose receipt has a version that is no version or a file name that is not a plain one ([`file_name_ok`]): the
    /// receipt sits in the game folder where anything can write it, and removal joins its names to the folder.
    pub fn receipts(&self, vendor: &str, component: &str) -> Vec<(PathBuf, Receipt)> {
        let dir = self.root().join(vendor);
        let mut out: Vec<(PathBuf, Receipt)> = std::fs::read_dir(&dir).into_iter().flatten().flatten()
            .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
            .filter_map(|e| {
                let p = e.path();
                let r: Receipt = crate::util::read_json(&p.join(RECEIPT)).ok().flatten()?;
                (r.format == RECEIPT_FORMAT && r.component == component && r.vendor == vendor && e.file_name().to_string_lossy() == r.version
                    && receipt_names_ok(&r)).then_some((p, r))
            })
            .collect();
        out.sort_by(|a, b| version_cmp(&b.1.version, &a.1.version));
        out
    }

    /// The files of a receipt that are not there as the receipt says (missing, not a plain file, another size or
    /// another SHA-256).
    pub fn problems(folder: &Path, r: &Receipt) -> Vec<String> {
        r.files.iter()
            .filter(|f| {
                let p = folder.join(&f.name);
                !std::fs::symlink_metadata(&p).is_ok_and(|m| m.is_file() && m.len() == f.size && sha256_seen(&p, &m).is_some_and(|h| h == f.sha256))
            })
            .map(|f| f.name.clone()).collect()
    }

    /// Bytes of `.part` files of `c`'s files in its manifest version's folder (when that folder has no receipt).
    pub fn partial(&self, c: &Component) -> u64 {
        let folder = self.folder(&c.vendor, &c.version);
        if folder.join(RECEIPT).exists() {
            return 0;
        }
        c.files.iter().filter_map(|f| std::fs::symlink_metadata(folder.join(format!("{}{PART_SUFFIX}", f.name))).ok())
            .filter(|m| m.is_file()).map(|m| m.len()).sum()
    }

    /// One status per component of the manifest, in its order.
    pub fn status(&self, m: &VendorManifest, agreements: &Agreements) -> Vec<ComponentStatus> {
        m.components.iter().map(|c| self.component_status(c, agreements)).collect()
    }

    pub fn component_status(&self, c: &Component, agreements: &Agreements) -> ComponentStatus {
        let newest = self.receipts(&c.vendor, &c.id).into_iter().next();
        let installed = newest.as_ref().map(|(folder, r)| {
            let problems = Store::problems(folder, r);
            Installed {
                version: r.version.clone(),
                folder: Store::rel(&c.vendor, &r.version),
                size_on_disk: r.files.iter().filter(|f| !problems.contains(&f.name)).map(|f| f.size).sum(),
                installed: r.installed.clone(),
                problems,
            }
        });
        let pinned = c.pinned();
        let (state, reason) = match (&installed, &newest) {
            (Some(i), Some((_, r))) if i.version == c.version => {
                // the receipt's files are this build's files (a receipt from a build with other hashes is not)
                let same = c.files.iter().all(|f| !f.pinned() || r.files.iter().any(|x| x.name == f.name && x.sha256 == f.sha256 && x.size == f.size));
                if !i.problems.is_empty() {
                    (VendorState::Damaged, Some(format!("{} of it {} missing or changed. Download it again.", i.problems.join(", "), if i.problems.len() == 1 { "is" } else { "are" })))
                } else if !same {
                    (VendorState::Damaged, Some("Its files are not the ones this build of Kiln expects. Download it again.".to_string()))
                } else {
                    (VendorState::Downloaded, None)
                }
            }
            (Some(i), _) if pinned && version_cmp(&c.version, &i.version).is_gt() => (VendorState::UpdateAvailable, None),
            (Some(_), _) => (VendorState::Downloaded, None),
            (None, _) if !pinned => (VendorState::Unavailable, Some("This build of Kiln does not have the files' checksums yet, so it does not download them.".to_string())),
            (None, _) => (VendorState::NotDownloaded, None),
        };
        ComponentStatus {
            id: c.id.clone(),
            vendor: c.vendor.clone(),
            vendor_name: c.vendor_name.clone(),
            name: c.name.clone(),
            enables: c.enables.clone(),
            version: c.version.clone(),
            source: c.source.clone(),
            license: LicenseInfo {
                name: c.license.name.clone(),
                sha256: c.license.sha256.clone(),
                from: c.license.from.clone(),
                agreed: agreements.agreed(c),
                agreed_at: agreements.agreed_at(c).map(str::to_string),
            },
            hosts: c.hosts(),
            size: c.size(),
            files: c.files.iter().map(|f| FileInfo { name: f.name.clone(), size: f.size, pinned: f.pinned() }).collect(),
            state,
            installed,
            partial: if pinned { self.partial(c) } else { 0 },
            reason,
        }
    }

    /// Removes every version folder of `c` (and an interrupted download of its manifest version), by the names Kiln
    /// put there. Takes the store's lock.
    pub fn remove(&self, c: &Component) -> Result<RemoveReport> {
        let _lock = self.lock()?;
        self.remove_locked(c, None)
    }

    /// [`Store::remove`] under a lock the caller holds, sparing the version `keep` (an update's new one).
    pub(crate) fn remove_locked(&self, c: &Component, keep: Option<&str>) -> Result<RemoveReport> {
        let mut report = RemoveReport::default();
        let mut folders: Vec<(String, Vec<String>)> = self.receipts(&c.vendor, &c.id).into_iter()
            .map(|(_, r)| (r.version.clone(), r.files.iter().map(|f| f.name.clone()).collect())).collect();
        if !folders.iter().any(|(v, _)| *v == c.version) {
            folders.push((c.version.clone(), Vec::new()));
        }
        for (version, mut names) in folders {
            if keep == Some(version.as_str()) {
                continue;
            }
            let folder = self.folder(&c.vendor, &version);
            if !folder.exists() {
                continue;
            }
            self.guard(&folder)?;
            if version == c.version {
                names.extend(c.files.iter().map(|f| f.name.clone()));
            }
            // plain names only, whatever a receipt says ([`Store::receipts`] already skips one that is not)
            names.retain(|n| file_name_ok(n));
            names.sort();
            names.dedup();
            let mut ours: Vec<String> = names.iter().flat_map(|n| [n.clone(), format!("{n}{PART_SUFFIX}")]).collect();
            ours.extend([RECEIPT.to_string(), format!("{RECEIPT}.tmp")]);
            // the receipt goes first: a folder that lost some files is never read as complete
            ours.sort_by_key(|n| n != RECEIPT);
            let mut any = false;
            for n in &ours {
                let p = folder.join(n);
                match std::fs::symlink_metadata(&p) {
                    Ok(m) if m.is_file() => {
                        std::fs::remove_file(&p).map_err(crate::util::io_at(&p, "removing"))?;
                        report.freed += m.len();
                        any = true;
                    }
                    Ok(_) => report.kept.push(format!("{}/{n} is not a plain file, so Kiln left it.", Store::rel(&c.vendor, &version))),
                    Err(_) => {}
                }
            }
            let rel = Store::rel(&c.vendor, &version);
            let left: Vec<String> = std::fs::read_dir(&folder).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
            if left.is_empty() {
                std::fs::remove_dir(&folder).map_err(crate::util::io_at(&folder, "removing"))?;
                report.removed.push(rel);
            } else {
                for n in left.iter().filter(|n| !ours.contains(n)) {
                    report.kept.push(format!("{rel}/{n} is not a file Kiln put there, so Kiln left it and the folder."));
                }
                if any {
                    report.removed.push(rel);
                }
            }
        }
        // the vendor's folder when nothing is left in it (kiln/vendor/ itself keeps its lock file)
        let vendor_dir = self.root().join(&c.vendor);
        if std::fs::read_dir(&vendor_dir).is_ok_and(|mut d| d.next().is_none()) {
            let _ = std::fs::remove_dir(&vendor_dir);
        }
        Ok(report)
    }
}

/// SHA-256 of files the status read, by path, with the size and modification time they had: the page asks often and
/// the files change rarely (150 MB is a fraction of a second to hash, every time).
static SEEN: std::sync::Mutex<std::collections::BTreeMap<PathBuf, (u64, std::time::SystemTime, String)>> =
    std::sync::Mutex::new(std::collections::BTreeMap::new());

/// A file's SHA-256 for the status, from [`SEEN`] while its size and modification time are the ones hashed. A file
/// changed in the last 2 seconds is hashed every time: a change within the file system's time step would keep its time.
fn sha256_seen(p: &Path, m: &std::fs::Metadata) -> Option<String> {
    let modified = m.modified().ok();
    let settled = modified.filter(|t| t.elapsed().is_ok_and(|age| age >= std::time::Duration::from_secs(2)));
    if let Some(t) = settled
        && let Some((len, at, sha)) = SEEN.lock().ok()?.get(p)
        && *len == m.len()
        && *at == t
    {
        return Some(sha.clone());
    }
    let (sha, n) = crate::hash::sha256_file(p).ok()?;
    if let (Some(t), true) = (settled, n == m.len())
        && let Ok(mut seen) = SEEN.lock()
    {
        seen.insert(p.to_path_buf(), (n, t, sha.clone()));
    }
    Some(sha)
}

/// A receipt's version is a version and its files plain names, each once (case aside, as on Windows).
fn receipt_names_ok(r: &Receipt) -> bool {
    let mut seen = std::collections::BTreeSet::new();
    version_ok(&r.version) && r.files.iter().all(|f| file_name_ok(&f.name) && seen.insert(f.name.to_ascii_lowercase()))
}

/// Orders two dotted versions by their numbers (a part that is not a number orders as 0; more parts order after).
pub fn version_cmp(a: &str, b: &str) -> std::cmp::Ordering {
    let parts = |s: &str| s.split('.').map(|p| p.parse::<u64>().unwrap_or(0)).collect::<Vec<_>>();
    parts(a).cmp(&parts(b))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_status_keeps_a_settled_files_hash_until_its_size_or_time_changes() {
        let dir = std::env::temp_dir().join(format!("kiln-vendor-seen-{}", crate::hash::hex(&crate::util::random_bytes::<6>().unwrap())));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("a.dll");
        let meta = || std::fs::symlink_metadata(&p).unwrap();
        let set_time = |t: std::time::SystemTime| std::fs::File::options().write(true).open(&p).unwrap().set_modified(t).unwrap();
        let (one, two) = (crate::hash::sha256_hex(b"one"), crate::hash::sha256_hex(b"two"));
        // just written: hashed every time, nothing kept
        std::fs::write(&p, b"one").unwrap();
        assert_eq!(sha256_seen(&p, &meta()).as_deref(), Some(one.as_str()));
        assert!(!SEEN.lock().unwrap().contains_key(&p));
        // settled: kept
        let old = std::time::SystemTime::now() - std::time::Duration::from_secs(3600);
        set_time(old);
        assert_eq!(sha256_seen(&p, &meta()).as_deref(), Some(one.as_str()));
        assert!(SEEN.lock().unwrap().contains_key(&p));
        // the same size and time with other bytes still reads as kept: the status trusts them, the download and the
        // game's check never do
        std::fs::write(&p, b"two").unwrap();
        set_time(old);
        assert_eq!(sha256_seen(&p, &meta()).as_deref(), Some(one.as_str()));
        // another time: read again
        set_time(old + std::time::Duration::from_secs(1));
        assert_eq!(sha256_seen(&p, &meta()).as_deref(), Some(two.as_str()));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn versions_order_by_their_numbers() {
        assert!(version_cmp("2.4.0", "2.10.0").is_lt());
        assert!(version_cmp("310.9.16", "310.9.2").is_gt());
        assert!(version_cmp("3.0.2", "3.0.2").is_eq());
        assert!(version_cmp("3.0", "3.0.0").is_lt());
    }
}
