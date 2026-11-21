// SPDX-License-Identifier: GPL-3.0-or-later
// Write-ahead scratch ownership. Only the installation-lock holder may create or consume these records.
use std::{fs, io, path::{Path, PathBuf}};
use serde::{Deserialize, Serialize};
use crate::{ownership::{bounded, checked_path, check_stem, directory, is_uuid, temp_of, valid_name}, transaction};
use crate::transaction::FileIdentity;

// Format 2 binds a record to the identities it was written for: `root` (the game folder, at record time) and, for a
// stage, `stage` (the directory create_writer made, bound right after creating it). A path alone is never permission
// to delete: an unbound record (format 1, as dev.29 wrote, or a stage not bound yet) or one whose identities no longer
// match is kept and reported with a reason code, never consumed against a directory that may not be ours.
const FORMAT: u32 = 2;
/// Reason codes in the startup report and the log (stable; the launcher may offer manual cleanup on them).
pub(crate) const CLEANUP_UNBOUND: &str = "eml-cleanup-unbound";
pub(crate) const CLEANUP_IDENTITY_CHANGED: &str = "eml-cleanup-identity-changed";
/// Log only: a stage emptied where no handle-based removal exists (non-Windows builds); never removed by path.
pub(crate) const CLEANUP_KEPT_EMPTY: &str = "eml-cleanup-kept-empty";

#[derive(Serialize, Deserialize)]
struct Record {
    format: u32,
    stem: String,
    name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    root: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    stage: Option<String>,
}

/// Why consume kept a record: an I/O or shape problem, or a recorded path it has no authority over (code + path).
enum Kept { Io(io::Error), Authority(&'static str, PathBuf) }
impl From<io::Error> for Kept { fn from(e: io::Error) -> Self { Kept::Io(e) } }
impl From<serde_json::Error> for Kept { fn from(e: serde_json::Error) -> Self { Kept::Io(e.into()) } }

// Test seams for the racy windows (a directory replaced at its path while Kiln works on it). Never set in production.
#[cfg(test)]
pub(crate) type TestPoint = std::cell::Cell<Option<fn(&Path)>>;
#[cfg(test)]
thread_local! {
    /// In consume (prune, and Drop's held stage), after the stage's authority was checked and before its children are
    /// enumerated.
    pub(crate) static AFTER_STAGE_AUTHORITY: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) };
    /// In create_writer, right after the stage directory was created.
    pub(crate) static AFTER_STAGE_CREATED: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) };
    /// In create_writer, after the record was bound to the stage and before the first staging copy.
    pub(crate) static AFTER_STAGE_BOUND: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) };
    /// In consume, after the emptied stage passed its last check and before it is removed.
    pub(crate) static BEFORE_STAGE_REMOVAL: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) };
}
#[cfg(test)]
pub(crate) fn test_point(point: &'static std::thread::LocalKey<TestPoint>, path: &Path) {
    point.with(|hook| { if let Some(hook) = hook.take() { hook(path); } });
}

fn identity(path: &Path) -> io::Result<String> { Ok(FileIdentity::directory(path)?.token()) }

/// Whether a stage record may delete inside the directory with identity `current`: only a record bound to this root
/// and exactly this directory (the writer's own held stage is bound before any byte is staged in it).
fn stage_authority(root: &Path, r: &Record, current: &FileIdentity) -> io::Result<Result<(), &'static str>> {
    let (Some(root_id), Some(stage_id)) = (&r.root, &r.stage) else { return Ok(Err(CLEANUP_UNBOUND)) };
    if *root_id != identity(root)? || *stage_id != current.token() { return Ok(Err(CLEANUP_IDENTITY_CHANGED)); }
    Ok(Ok(()))
}

fn stage(name: &str) -> Option<&str> { name.strip_prefix(".kiln-eml-stage-").filter(|id| is_uuid(id)) }
fn temp(stem: &str, name: &str) -> bool {
    temp_of(name).is_some_and(|base| [stem.to_owned(), format!("{stem}.kfc"), format!("{stem}.kfc_resources"), format!("{stem}.kfc.bak")].iter().any(|s| s == base))
}
fn record_path(root: &Path, name: &str) -> Option<PathBuf> {
    let id = stage(name).or_else(|| temp_of(name).and_then(|_| name.rsplit_once(".kiln-").map(|(_, id)| id)))?;
    Some(root.join("kiln/eml/ownership").join(format!("cleanup-{id}.json")))
}

/// Flush the exact future name before creating a directory or writing the first byte of a game-folder temp.
pub(crate) fn record(root: &Path, stem: &str, name: &str) -> io::Result<PathBuf> {
    check_stem(stem)?;
    if stage(name).is_none() && !temp(stem, name) { return Err(io::Error::other("invalid scratch name")); }
    let target = root.join(name);
    checked_path(root, &target)?;
    match fs::symlink_metadata(&target) {
        Err(e) if e.kind() == io::ErrorKind::NotFound => {},
        Err(e) => return Err(e),
        Ok(_) => return Err(io::Error::other("scratch destination already exists")),
    }
    directory(root)?;
    let path = record_path(root, name).unwrap();
    if path.try_exists()? { return Err(io::Error::other("scratch record already exists")); }
    let record = Record { format: FORMAT, stem: stem.into(), name: name.into(), root: Some(identity(root)?), stage: None };
    transaction::atomic_write(&path, &serde_json::to_vec(&record)?)?;
    Ok(path)
}

/// Bind a stage's record to the directory just created for it (the identity PatchWriter captured), before any byte
/// is staged. Only an unbound format-2 record of exactly this stage is bound, once.
pub(crate) fn bind_stage(root: &Path, name: &str, stage_id: &FileIdentity) -> io::Result<()> {
    let path = record_path(root, name).filter(|_| stage(name).is_some()).ok_or_else(|| io::Error::other("invalid stage name"))?;
    let mut r: Record = serde_json::from_slice(&bounded(root, &path)?)?;
    if r.format != FORMAT || r.name != name || r.root.is_none() || r.stage.is_some() {
        return Err(io::Error::other("stage cleanup record cannot be bound"));
    }
    if FileIdentity::directory(&root.join(name))? != *stage_id {
        return Err(io::Error::other("stage directory changed before its cleanup record was bound"));
    }
    r.stage = Some(stage_id.token());
    transaction::atomic_write(&path, &serde_json::to_vec(&r)?)
}

/// Stage temps are covered by their folder's record; ownership/cache/SF state temps stay in their own state folders.
pub(crate) fn record_temp(dst: &Path, tmp: &Path) -> io::Result<Option<PathBuf>> {
    let Some(root) = dst.parent() else { return Ok(None) };
    if root.file_name().and_then(|n| n.to_str()).is_some_and(|n| stage(n).is_some()) { return Ok(None); }
    let Some(name) = dst.file_name().and_then(|n| n.to_str()) else { return Ok(None) };
    let stem = [".kfc.bak.integrity", ".kfc_resources.bak", ".kfc.bak", ".kfc_resources", ".kfc"].iter().find_map(|s| name.strip_suffix(s));
    let Some(stem) = stem else { return Ok(None) };
    record(root, stem, tmp.file_name().and_then(|n| n.to_str()).ok_or_else(|| io::Error::other("invalid temp name"))?).map(Some)
}

fn staged(stem: &str, name: &str) -> bool {
    valid_name(stem, name) || name == transaction::VANILLA_HEADER
        || temp(stem, name) || temp_of(name) == Some(transaction::VANILLA_HEADER)
}

/// `own`: the writer's own stage, held since create_writer made it (Drop). Without it, a stage is held here.
fn consume(root: &Path, path: &Path, own: Option<transaction::HeldDir>) -> Result<(), Kept> {
    let r: Record = serde_json::from_slice(&bounded(root, path)?)?;
    check_stem(&r.stem)?;
    if !(r.format == 1 || r.format == FORMAT) || record_path(root, &r.name).as_deref() != Some(path)
        || (stage(&r.name).is_none() && !temp(&r.stem, &r.name)) {
        return Err(io::Error::other("unknown scratch record; retained").into());
    }
    let target = root.join(&r.name);
    checked_path(root, &target)?;
    if let Some(held) = own {
        // Cleaned through the writer's own hold, wherever the stage's path points now: what is at the path is not looked at.
        if stage(&r.name).is_none() { return Err(io::Error::other("a held stage with a temp record; retained").into()); }
        if let Err(code) = stage_authority(root, &r, &held.identity()?)? { return Err(Kept::Authority(code, target)); }
        #[cfg(test)]
        test_point(&AFTER_STAGE_AUTHORITY, &target);
        clean_held(&r.stem, held, &target)?;
    } else {
        match fs::symlink_metadata(&target) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => {},
            Err(e) => return Err(e.into()),
            Ok(m) if stage(&r.name).is_some() && m.is_dir() => {
                // Held from the identity check through the last deletion (transaction::HeldDir): the directory checked is
                // the one enumerated and cleaned, even if something else appears at its path meanwhile.
                let held = transaction::HeldDir::open(&target)?;
                if let Err(code) = stage_authority(root, &r, &held.identity()?)? { return Err(Kept::Authority(code, target)); }
                #[cfg(test)]
                test_point(&AFTER_STAGE_AUTHORITY, &target);
                clean_held(&r.stem, held, &target)?;
            },
            Ok(_) if stage(&r.name).is_none() => {
                // a temp record keeps its file-level behaviour; a bound one must still be for this game folder
                if r.root.as_ref().is_some_and(|id| identity(root).ok().as_ref() != Some(id)) {
                    return Err(Kept::Authority(CLEANUP_IDENTITY_CHANGED, target));
                }
                transaction::regular(&target)?;
                fs::remove_file(&target)?;
                failpoint!("cleanup.temp-removed", target);
            },
            Ok(_) => return Err(io::Error::other("recorded scratch changed type; retained").into()),
        }
    }
    transaction::sync_dir(root)?;
    fs::remove_file(path)?;
    transaction::sync_dir(path.parent().unwrap())?;
    failpoint!("cleanup.record-removed", path);
    Ok(())
}

fn report(kept: Kept, record: &Path, message: &str) {
    match kept {
        Kept::Io(error) => tracing::warn!(%error, record = %record.display(), "{message}"),
        Kept::Authority(code, path) => tracing::warn!(code, path = %path.display(), record = %record.display(),
            "EML cleanup record kept [{code}]: it is not proof for {}; nothing there was deleted", path.display()),
    }
}

/// Empty a stage Kiln holds of what it staged, through the hold, then remove it through the same handle. Foreign
/// children stay (inventory names them even after the record is dropped). Nothing here goes by `target`, the stage's
/// path, which is only named in the log: the path may name another directory by now.
fn clean_held(stem: &str, held: transaction::HeldDir, target: &Path) -> io::Result<()> {
    // Listed through the hold (HeldDir::children): Wine's directory listing by path conflicts with the hold's DELETE access.
    for path in held.children()? {
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if staged(stem, &name) && transaction::regular(&path).is_ok() {
            fs::remove_file(&path)?;
            failpoint!("cleanup.stage-file-removed", path);
        }
    }
    transaction::sync_dir(held.path())?;
    if !held.children()?.is_empty() { return Ok(()); }
    #[cfg(test)]
    test_point(&BEFORE_STAGE_REMOVAL, target);
    let removed = held.remove()?;
    failpoint!("cleanup.stage-removed", target);
    if !removed {
        // Non-Windows builds (host tests, kiln-eml-run): no directory can be removed through its handle, and one is never
        // removed by path. The empty stage stays; recovery guidance does not list empty folders (nothing to inspect).
        tracing::info!(code = CLEANUP_KEPT_EMPTY, path = %target.display(),
            "EML stage emptied and kept [{CLEANUP_KEPT_EMPTY}]: this platform cannot remove it through its handle");
    }
    Ok(())
}

pub(crate) fn finish_stage(root: &Path, name: &str, own: Option<transaction::HeldDir>) {
    if let Some(path) = record_path(root, name) {
        if path.exists() && let Err(kept) = consume(root, &path, own) {
            report(kept, &path, "Could not finish recorded EML scratch cleanup; next start will retry");
        }
    }
}

/// Next-start cleanup under the installation lock. Unrecorded game-folder copies are never deletion candidates.
pub fn prune(root: &Path, stem: &str) {
    crate::displacement::reset();
    if check_stem(stem).is_err() { return; }
    prune_startup_temps(root, stem);
    let dir = root.join("kiln/eml/ownership");
    if checked_path(root, &dir).is_err() { return; }
    let Ok(entries) = fs::read_dir(&dir) else { return };
    let mut journals = Vec::new();
    for e in entries.flatten() {
        let path = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if transaction::regular(&path).is_err() { continue; }
        if name.strip_prefix("cleanup-").and_then(|s| s.strip_suffix(".json")).is_some_and(is_uuid) {
            if let Err(kept) = consume(root, &path, None) { report(kept, &path, "EML scratch record retained"); }
        } else if temp_of(&name).is_some_and(|base| ["enshrouded", "enshrouded_server", stem].contains(&base)
            // kiln-eml: the owed ShroudForge handoff's marker
            || base == format!("{stem}.sf-handoff-owed")
            // kiln-eml: the startup change journal
            || base == format!("{stem}.change-journal")
            || ["sf-handoff-", "cleanup-"].iter().any(|prefix| base.strip_prefix(prefix).is_some_and(is_uuid))) {
            if let Err(error) = fs::remove_file(&path) { tracing::warn!(%error, "Could not remove EML state temp"); }
        } else if name.strip_prefix("sf-handoff-").and_then(|s| s.strip_suffix(".json")).is_some_and(is_uuid) {
            journals.push((e.metadata().and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH), path));
        }
    }
    journals.sort_by(|a, b| b.cmp(a));
    for (_, path) in journals.into_iter().skip(16) {
        if let Err(error) = fs::remove_file(&path) { tracing::warn!(%error, "Could not prune old SF handoff journal"); }
    }
}

/// kiln-eml: the startup cache record's temps (`kiln/eml/cache/sf-startup-<stem>.json`), by their name.
fn prune_startup_temps(root: &Path, stem: &str) {
    let cache = root.join("kiln/eml/cache");
    if checked_path(root, &cache).is_err() { return; }
    let Ok(entries) = fs::read_dir(&cache) else { return };
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if temp_of(&name) == Some(format!("sf-startup-{stem}").as_str()) && transaction::regular(&e.path()).is_ok()
            && let Err(error) = fs::remove_file(e.path()) {
            tracing::warn!(%error, "Could not remove a startup cache temp");
        }
    }
}

// Inventory never follows links, and reads no file contents. A size is the sum of regular files, not allocated blocks.
fn inventory(root: &Path, path: &Path, out: &mut Vec<String>, depth: usize) -> io::Result<u64> {
    checked_path(root, path)?;
    let m = fs::symlink_metadata(path)?;
    let name = path.strip_prefix(root).map_err(io::Error::other)?.to_string_lossy().replace('\\', "/");
    if m.is_file() { out.push(format!("{name} ({} bytes)", m.len())); return Ok(m.len()); }
    if !m.is_dir() || depth > 32 { return Err(io::Error::other("recovery path type or depth cannot be inventoried")); }
    let mut bytes = 0;
    for e in fs::read_dir(path)? { bytes += inventory(root, &e?.path(), out, depth + 1)?; }
    out.push(format!("{name}/ ({bytes} bytes total)"));
    Ok(bytes)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    #[test]
    fn malformed_cleanup_records_never_authorize_a_live_file_or_an_escape() {
        for name in ["g.kfc", "../g.kfc", "sub/g.kiln-0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"] {
            let root = std::env::temp_dir().join(format!("kiln-invalid-cleanup-{}", uuid::Uuid::new_v4()));
            fs::create_dir(&root).unwrap();
            fs::write(root.join("g.kfc"), "live archive").unwrap();
            let dir = directory(&root).unwrap();
            let record = dir.join(format!("cleanup-{}.json", uuid::Uuid::new_v4()));
            fs::write(&record, serde_json::to_vec(&Record { format: 1, stem: "g".into(), name: name.into(), root: None, stage: None }).unwrap()).unwrap();
            prune(&root, "g");
            assert!(record.exists(), "unrecognized records stay, not consumed");
            assert_eq!(fs::read(root.join("g.kfc")).unwrap(), b"live archive");
            fs::remove_dir_all(root).unwrap();
        }
    }
    // A stage record cleans only the directory it was bound to: unbound (dev.29's format 1, or never bound) and
    // re-created directories are kept with a reason code; the bound original is reclaimed.
    #[test]
    fn stage_records_clean_only_the_directory_they_are_bound_to() {
        for case in ["unbound-format-1", "unbound-format-2", "replaced", "bound"] {
            let root = std::env::temp_dir().join(format!("kiln-bound-cleanup-{}", uuid::Uuid::new_v4()));
            fs::create_dir(&root).unwrap();
            let name = format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4());
            let stage = root.join(&name);
            let proof = if case == "unbound-format-1" {
                let path = record_path(&root, &name).unwrap();
                directory(&root).unwrap();
                fs::write(&path, serde_json::to_vec(&serde_json::json!({"format": 1, "stem": "g", "name": name})).unwrap()).unwrap();
                fs::create_dir(&stage).unwrap();
                path
            } else {
                let path = record(&root, "g", &name).unwrap();
                fs::create_dir(&stage).unwrap();
                if case != "unbound-format-2" { bind_stage(&root, &name, &FileIdentity::directory(&stage).unwrap()).unwrap(); }
                path
            };
            if case == "replaced" {
                fs::rename(&stage, root.join("moved-away")).unwrap();
                fs::create_dir(&stage).unwrap();
            }
            fs::write(stage.join("g.kfc"), "same-name child").unwrap();
            for _ in 0..2 { prune(&root, "g"); }
            let guidance = recovery_guidance(&root, "g").unwrap().join(" ");
            match case {
                "bound" => assert!(reclaimed(&stage) && !proof.exists() && guidance.is_empty(), "{case}: {guidance}"),
                _ => {
                    assert_eq!(fs::read(stage.join("g.kfc")).unwrap(), b"same-name child", "{case}");
                    assert!(proof.exists(), "{case}: the record is kept, not consumed");
                    let code = if case == "replaced" { CLEANUP_IDENTITY_CHANGED } else { CLEANUP_UNBOUND };
                    assert!(guidance.contains(&format!("[{code}] {name}")), "{case}: {guidance}");
                }
            }
            fs::remove_dir_all(root).unwrap();
        }
    }
    /// Replace `path` by a new directory holding a foreign `g.kfc`. Returns false when the platform refused the rename
    /// (a held Windows directory handle without FILE_SHARE_DELETE): then nothing was replaced.
    pub(crate) fn replace_with_foreign(path: &Path) -> bool {
        let moved = PathBuf::from(format!("{}-moved", path.display()));
        if fs::rename(path, &moved).is_err() { return false; }
        fs::create_dir(path).unwrap();
        fs::write(path.join("g.kfc"), "foreign data").unwrap();
        true
    }
    thread_local! { static REPLACED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }
    pub(crate) fn replace_hook(path: &Path) { let r = replace_with_foreign(path); REPLACED.with(|c| c.set(r)); }
    /// As replace_hook, with a foreign resource file too: the pair a staging copy would overwrite.
    pub(crate) fn replace_pair_hook(path: &Path) {
        let r = replace_with_foreign(path);
        if r { fs::write(path.join("g.kfc_resources"), "foreign resources").unwrap(); }
        REPLACED.with(|c| c.set(r));
    }
    /// Replace `path` by a foreign EMPTY directory (what a final removal by path could delete).
    pub(crate) fn replace_empty_hook(path: &Path) {
        let r = fs::rename(path, format!("{}-moved", path.display())).is_ok();
        if r { fs::create_dir(path).unwrap(); }
        REPLACED.with(|c| c.set(r));
    }
    pub(crate) fn replaced() -> bool { REPLACED.with(|c| c.replace(false)) }
    /// A cleaned stage: gone, or kept empty on non-Windows builds (no removal through a handle there, never by path).
    pub(crate) fn reclaimed(stage: &Path) -> bool {
        !stage.exists() || (cfg!(not(windows)) && fs::read_dir(stage).is_ok_and(|mut d| d.next().is_none()))
    }
    // Review of b53862f0 (race a): the stage is replaced between the authority check and the enumeration. The
    // replacement's files are never touched (on Windows the held handle refuses the rename in the first place).
    #[test]
    fn a_stage_replaced_after_its_authority_check_is_not_cleaned() {
        let root = std::env::temp_dir().join(format!("kiln-race-cleanup-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let name = format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4());
        let stage = root.join(&name);
        record(&root, "g", &name).unwrap();
        fs::create_dir(&stage).unwrap();
        bind_stage(&root, &name, &FileIdentity::directory(&stage).unwrap()).unwrap();
        fs::write(stage.join("g.kfc"), "staged by Kiln").unwrap();
        AFTER_STAGE_AUTHORITY.with(|h| h.set(Some(replace_hook)));
        prune(&root, "g");
        AFTER_STAGE_AUTHORITY.with(|h| h.set(None));
        if replaced() {
            for _ in 0..2 {
                assert_eq!(fs::read(stage.join("g.kfc")).unwrap(), b"foreign data", "the replacement's file was deleted");
                prune(&root, "g");
            }
        } else {
            assert!(reclaimed(&stage), "the held stage itself was cleaned");
        }
        fs::remove_dir_all(root).unwrap();
    }
    // Round 3 (review of a07c2624, medium): after the emptied stage passed its last check, it is moved away and a foreign
    // EMPTY directory takes its path. Only the held directory is ever removed (on Windows through its own handle, the
    // rename being refused meanwhile; elsewhere it is kept): the foreign directory survives, in prune as in Drop.
    #[test]
    fn a_stage_replaced_before_its_removal_is_never_removed() {
        let root = std::env::temp_dir().join(format!("kiln-race-removal-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let name = format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4());
        let stage = root.join(&name);
        let proof = record(&root, "g", &name).unwrap();
        fs::create_dir(&stage).unwrap();
        bind_stage(&root, &name, &FileIdentity::directory(&stage).unwrap()).unwrap();
        fs::write(stage.join("g.kfc"), "staged by Kiln").unwrap();
        BEFORE_STAGE_REMOVAL.with(|h| h.set(Some(replace_empty_hook)));
        prune(&root, "g");
        BEFORE_STAGE_REMOVAL.with(|h| h.set(None));
        if replaced() {
            for _ in 0..2 {
                assert!(stage.is_dir(), "the foreign directory swapped in at the stage's path was removed");
                prune(&root, "g");
            }
            let moved = PathBuf::from(format!("{}-moved", stage.display()));
            assert!(fs::read_dir(&moved).unwrap().next().is_none(), "Kiln's own (moved) stage was not emptied");
        } else {
            assert!(!stage.exists(), "the held stage was not removed through its handle");
        }
        assert!(!proof.exists(), "the record of a cleaned stage stayed");
        fs::remove_dir_all(root).unwrap();
    }
    // Windows (the real target): a held stage cannot be renamed away, so its path keeps naming the held directory.
    #[cfg(windows)]
    #[test]
    fn a_held_stage_cannot_be_renamed_on_windows() {
        let root = std::env::temp_dir().join(format!("kiln-held-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let stage = root.join(format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&stage).unwrap();
        fs::write(stage.join("g.kfc"), "staged").unwrap();
        let held = transaction::HeldDir::open(&stage).unwrap();
        assert!(fs::rename(&stage, root.join("moved")).is_err(), "a held stage was renamed");
        assert!(!replace_with_foreign(&stage), "a held stage was replaced");
        fs::remove_file(stage.join("g.kfc")).unwrap();  // its children can still be deleted
        drop(held);
        fs::rename(&stage, root.join("moved")).unwrap();  // released: a rename works again
        fs::remove_dir_all(root).unwrap();
    }
    // Windows: a held stage is removed through its own handle (delete-on-close), and only once it is empty.
    #[cfg(windows)]
    #[test]
    fn a_held_stage_is_removed_through_its_own_handle_on_windows() {
        let root = std::env::temp_dir().join(format!("kiln-held-remove-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let stage = root.join(format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&stage).unwrap();
        fs::write(stage.join("g.kfc"), "staged").unwrap();
        let _ = transaction::HeldDir::open(&stage).unwrap().remove();
        assert_eq!(fs::read(stage.join("g.kfc")).unwrap(), b"staged", "a stage that is not empty was removed");
        fs::remove_file(stage.join("g.kfc")).unwrap();
        let held = transaction::HeldDir::open(&stage).unwrap();
        assert!(transaction::HeldDir::open(&stage).is_err(), "a second hold on a held stage");
        assert!(fs::rename(&stage, root.join("moved")).is_err(), "a held stage was renamed");
        assert!(held.remove().unwrap(), "no removal through the handle on Windows");
        assert!(!stage.exists() && !root.join("moved").exists(), "the held stage was not removed");
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn a_stage_record_binds_once_and_only_to_its_own_directory() {
        let root = std::env::temp_dir().join(format!("kiln-bind-once-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let name = format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4());
        record(&root, "g", &name).unwrap();
        fs::create_dir(root.join(&name)).unwrap();
        fs::create_dir(root.join("other")).unwrap();
        assert!(bind_stage(&root, &name, &FileIdentity::directory(&root.join("other")).unwrap()).is_err(), "another directory's identity");
        let id = FileIdentity::directory(&root.join(&name)).unwrap();
        bind_stage(&root, &name, &id).unwrap();
        assert!(bind_stage(&root, &name, &id).is_err(), "a bound record is never rebound");
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn a_bound_temp_record_requires_its_game_folder() {
        let root = std::env::temp_dir().join(format!("kiln-temp-root-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let temp_name = format!("g.kfc_resources.kiln-{}", uuid::Uuid::new_v4());
        let proof = record(&root, "g", &temp_name).unwrap();
        // the game folder is replaced by a copy (same path, new identity) holding a same-name file
        let copy = root.with_extension("moved");
        fs::rename(&root, &copy).unwrap();
        fs::create_dir(&root).unwrap();
        directory(&root).unwrap();
        fs::copy(copy.join(proof.strip_prefix(&root).unwrap()), &proof).unwrap();
        fs::write(root.join(&temp_name), "someone else's file").unwrap();
        prune(&root, "g");
        assert_eq!(fs::read(root.join(&temp_name)).unwrap(), b"someone else's file");
        assert!(proof.exists());
        fs::remove_dir_all(root).unwrap();
        fs::remove_dir_all(copy).unwrap();
    }
    #[cfg(any(unix, windows))]
    #[test]
    fn a_recorded_stage_replaced_by_a_link_never_cleans_its_target() {
        let root = std::env::temp_dir().join(format!("kiln-linked-cleanup-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        let name = format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4());
        let proof = record(&root, "g", &name).unwrap();
        assert!(!root.join(&name).exists(), "proof is flushed before the future stage exists");
        fs::create_dir(root.join("foreign")).unwrap();
        fs::write(root.join("foreign/g.kfc"), "foreign data").unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(root.join("foreign"), root.join(&name)).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(root.join("foreign"), root.join(&name)).unwrap();
        prune(&root, "g");
        assert_eq!(fs::read(root.join("foreign/g.kfc")).unwrap(), b"foreign data");
        assert!(proof.exists());
        assert!(recovery_guidance(&root, "g").unwrap().join(" ").contains(&name));
        fs::remove_dir_all(root).unwrap();
    }
}

/// The reason code a stage's cleanup record is kept with (it exists but has no authority over the folder now).
fn kept_code(root: &Path, name: &str) -> Option<&'static str> {
    let path = record_path(root, name)?;
    let r: Record = serde_json::from_slice(&bounded(root, &path).ok()?).ok()?;
    if r.name != name { return None; }
    let target = root.join(name);
    if !fs::symlink_metadata(&target).is_ok_and(|m| m.is_dir()) { return None; }
    // A report only, so no hold: on Windows a second hold (DELETE access) would fail while a live writer holds the stage.
    let current = FileIdentity::directory(&target).ok()?;
    stage_authority(root, &r, &current).ok()?.err()
}

/// Player-facing state/log guidance. A recorded stage with foreign children becomes an unrecorded retained folder.
pub fn recovery_guidance(root: &Path, stem: &str) -> io::Result<Vec<String>> {
    check_stem(stem)?;
    let mut paths = Vec::new();
    let mut coded = Vec::new();
    for e in fs::read_dir(root)? {
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        if let Some(code) = stage(&name).and_then(|_| kept_code(root, &name)) {
            // machine-readable: "[<code>] <folder>" (CLEANUP_UNBOUND / CLEANUP_IDENTITY_CHANGED)
            coded.push(format!("[{code}] {name}"));
        }
        // An empty stage folder holds nothing to recover (non-Windows builds keep each emptied stage: clean_held).
        let empty_stage = stage(&name).is_some() && fs::symlink_metadata(e.path()).is_ok_and(|m| m.is_dir())
            && fs::read_dir(e.path()).is_ok_and(|mut d| d.next().is_none());
        if !empty_stage && (stage(&name).is_some() || [stem, "enshrouded", "enshrouded_server"].iter().any(|s| temp(s, &name))) {
            if let Err(error) = inventory(root, &e.path(), &mut paths, 0) {
                paths.push(format!("{name} (size unavailable: {error})"));
            }
        }
    }
    paths.sort();
    coded.sort();
    let mut messages = Vec::new();
    if !coded.is_empty() {
        messages.push(format!("EML cleanup records kept without authority over their folders: {}. Kiln will not delete these folders; inspect them before manual cleanup", coded.join(", ")));
    }
    if !paths.is_empty() {
        messages.push(format!("Recovery files retained without cleanup proof: {}. Inspect them before manual cleanup; Kiln will not delete these files", paths.join(", ")));
    }
    match crate::displacement::guidance(root) {
        Ok(displaced) => messages.extend(displaced),
        Err(error) => messages.push(format!("Archive recovery folders could not be inventoried: {error}. Inspect kiln/eml/displaced manually; no recovery files were deleted")),
    }
    Ok(messages)
}
