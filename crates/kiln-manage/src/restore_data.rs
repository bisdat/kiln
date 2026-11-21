//! Explicit, reversible recovery of unproven EML leftovers. Never restores shared backups over live data.
use crate::{Error, Result};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path};
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    pub from: String,
    pub size: u64,
    pub sha256: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Plan {
    pub fingerprint: String,
    pub files: Vec<Entry>,
}
const BASE: &str = "kiln/eml/displaced";
fn refuse(message: &str) -> Error {
    Error::new(crate::ErrorCode::Refused, message)
}
fn checked(root: &Path, name: &str) -> Result<std::path::PathBuf> {
    let p = Path::new(name);
    if p.is_absolute()
        || p.components()
            .any(|c| !matches!(c, std::path::Component::Normal(_)))
        || name.contains('\\')
    {
        return Err(refuse("Invalid recovery path."));
    }
    let p = root.join(p);
    crate::engine_mods::refuse_symlink_path(root, &p)?;
    Ok(p)
}
fn entry(root: &Path, name: &str) -> Result<Entry> {
    let p = checked(root, name)?;
    if !fs::symlink_metadata(&p)?.is_file() {
        return Err(refuse("Recovery accepts regular files only."));
    }
    let (sha256, size) = crate::hash::sha256_file(&p)?;
    Ok(Entry {
        from: name.into(),
        size,
        sha256,
    })
}
fn guard(root: &Path) -> Result<fs::File> {
    #[cfg(test)]
    if RUNNING.with(|r| r.get()) {
        return Err(refuse("Close the game before restoring its data."));
    }

    for exe in ["enshrouded.exe", "enshrouded_server.exe"] {
        if crate::proc::find_exe(root, exe)?.is_some() {
            return Err(refuse("Close the game before restoring its data."));
        }
    }
    let p = checked(root, ".kiln-eml.lock")?;
    let lock = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(p)?;
    lock.try_lock()
        .map_err(|_| refuse("The game data writer is busy."))?;
    match shroudforge_writes(root)? {
        // (Kiln's interpreter: kiln-eml writes the game's data for it at start; whether this restore may
        // run then is not decided yet, so it stays refused, in words about the interpreter)
        Some(crate::own_loaders::Via::Interpreter) => return Err(refuse(INTERPRETER_WRITES)),
        Some(_) => return Err(refuse(
            "ShroudForge prepares the game's data for this profile, so these files may be its own. Switch the profile's ShroudForge mods off (or remove ShroudForge's own loader), start the game through Kiln once, and come back.",
        )),
        None => {}
    }
    Ok(lock)
}
/// ShroudForge is the archive writer of the game's loaded profile: its own loader is installed, or Kiln hosts it for
/// that profile ([`crate::own_loaders::shroudforge_runs_eml`]). Its `shroudforge/` folder alone does not say so: a
/// player who once had a ShroudForge mod keeps that folder, and kiln-eml is then the writer whose start failed.
/// Some: how that loader gets in (one list, one route of the game's ShroudForge-format mods); None: it is not the writer.
fn shroudforge_writes(root: &Path) -> Result<Option<crate::own_loaders::Via>> {
    checked(root, "kiln/profile.json")?;
    let profile = crate::profile::read_game_profile(root)?
        .map(|p| crate::own_loaders::ProfileView::of_file(&p));
    let loaders = crate::own_loaders::find(root, profile.as_ref());
    // (shroudforge_runs_eml: the first of them)
    Ok(loaders
        .iter()
        .find(|o| o.kind == crate::manifest::ModKind::Eml && o.engine == crate::engines::Engine::ShroudForge)
        .map(|o| o.via))
}
/// [`guard`]'s refusal while Kiln's interpreter carries the loaded profile's EML mods.
const INTERPRETER_WRITES: &str = "Kiln's interpreter runs this profile's ShroudForge mods (kiln.json selects it), and Kiln prepares the game's data for them when the game starts, so these files may be that start's. Switch the profile's ShroudForge mods off, start the game through Kiln once, and come back.";
/// Whether there is anything for [`plan`] or [`latest`] to look at: the last start's state names unproven data, or a
/// move is recorded. Read only: a game folder with neither is not touched (no lock file is made in it, nothing is
/// hashed), because the launcher asks whenever it shows the engines.
fn relevant(root: &Path) -> Result<bool> {
    if !journals(root)?.is_empty() {
        return Ok(true);
    }
    let p = checked(root, "kiln/state/eml.json")?;
    let Ok(bytes) = fs::read(&p) else { return Ok(false) };
    if bytes.len() > 16 * 1024 * 1024 {
        return Ok(false);
    }
    let v: serde_json::Value = serde_json::from_slice(&bytes).unwrap_or_default();
    Ok(v["recovery"]["unproven"] == true)
}
const NOTHING: &str = "The last start through Kiln did not report game data it could not prove; there is nothing to restore.";
fn make_plan(root: &Path) -> Result<Plan> {
    let state = entry(root, "kiln/state/eml.json")?;
    let v: serde_json::Value = serde_json::from_slice(&fs::read(checked(root, &state.from)?)?)?;
    let r = &v["recovery"];
    if r["unproven"] == false
        || checked(root, "kiln/eml/ownership/enshrouded.json")?.try_exists()?
    {
        return Err(refuse(
            "Kiln has a record of the game data it wrote, so this is not another loader's data. Switch the EML mods off and use \"Restore vanilla game data\" instead.",
        ));
    }
    if !matches!(v["phase"].as_str(), Some("idle" | "failed"))
        || r["unproven"] != true
        || r["stem"] != "enshrouded"
    {
        return Err(refuse(
            NOTHING,
        ));
    }
    let header = entry(root, "enshrouded.kfc")?;
    if Some(header.sha256.as_str()) != r["headerSha256"].as_str() {
        return Err(refuse(
            "The game's index has changed since Kiln last read it (Steam's file check does that). Start the game through Kiln once, so it reads the list of leftover files again, then come back here. EML mods may fail once more in that start.",
        ));
    }
    let count = r["containers"]
        .as_u64()
        .filter(|n| *n < 1_000_000)
        .ok_or_else(|| refuse("The game's container count is unavailable."))?;
    let mut names = Vec::new();
    for e in fs::read_dir(root)? {
        let name = e?.file_name();
        let Some(name) = name.to_str() else { continue };
        if name
            .strip_prefix("enshrouded_")
            .and_then(|s| s.strip_suffix(".dat"))
            .filter(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
            .and_then(|s| s.parse::<u64>().ok())
            .is_some_and(|n| n >= count)
        {
            names.push(name.to_owned());
        }
    }
    for n in ["enshrouded.kfc.bak", "enshrouded.kfc_resources.bak"] {
        if checked(root, n)?.try_exists()? {
            names.push(n.into());
        }
    }
    if names.is_empty() {
        return Err(refuse(
            "There are no leftover containers or shared backups to move.",
        ));
    }
    // Move invalidated state as well: retain it for undo, never erase it.
    for n in [
        "enshrouded.kfc.bak.integrity",
        "kiln/eml/cache/files.json",
        "kiln/eml/cache/files.partial.json",
    ] {
        if checked(root, n)?.try_exists()? {
            names.push(n.into());
        }
    }
    names.sort();
    let files = names
        .iter()
        .map(|n| entry(root, n))
        .collect::<Result<Vec<_>>>()?;
    let fingerprint = crate::hash::sha256_hex(&serde_json::to_vec(&(&state, &header, &files))?);
    Ok(Plan { fingerprint, files })
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Moved {
    pub folder: String,
    pub files: Vec<Entry>,
}
#[derive(Serialize, Deserialize)]
struct Journal {
    header_sha256: String,
    plan: Plan,
    when: String,
    why: String,
    undo: bool,
    complete: bool,
    completed: usize,
}
fn journals(root: &Path) -> Result<Vec<String>> {
    let base = checked(root, BASE)?;
    if !base.try_exists()? {
        return Ok(vec![]);
    }
    let mut out = vec![];
    for e in fs::read_dir(base)? {
        let name = e?.file_name();
        if let Some(n) = name.to_str().filter(|n| n.starts_with("restore-")) {
            let folder = format!("{BASE}/{n}");
            let p = checked(root, &format!("{folder}/manifest.json"))?;
            if p.try_exists()? {
                out.push(folder);
            }
        }
    }
    out.sort();
    Ok(out)
}
fn read_journal(root: &Path, folder: &str) -> Result<Journal> {
    let p = checked(root, &format!("{folder}/manifest.json"))?;
    if fs::metadata(&p)?.len() > 1024 * 1024 {
        return Err(refuse("Recovery manifest is too large."));
    }
    let j: Journal = serde_json::from_slice(&fs::read(p)?)?;
    let mut seen = std::collections::BTreeSet::new();
    for e in &j.plan.files {
        checked(root, &e.from)?;
        let allowed = matches!(
            e.from.as_str(),
            "enshrouded.kfc.bak"
                | "enshrouded.kfc_resources.bak"
                | "enshrouded.kfc.bak.integrity"
                | "kiln/eml/cache/files.json"
                | "kiln/eml/cache/files.partial.json"
        ) || e
            .from
            .strip_prefix("enshrouded_")
            .and_then(|s| s.strip_suffix(".dat"))
            .is_some_and(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()));
        if !allowed || !seen.insert(&e.from) {
            return Err(refuse("Invalid recovery manifest file list."));
        }
    }
    Ok(j)
}
fn save(root: &Path, folder: &str, j: &Journal) -> Result<()> {
    crate::durable::json(&checked(root, &format!("{folder}/manifest.json"))?, j, &[])
}
// Rename without replacement. Cross-volume moves fail; there is deliberately no copy fallback.
fn rename(from: &Path, to: &Path) -> Result<()> {
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::ffi::OsStrExt;
        unsafe extern "C" {
            fn renameat2(
                a: i32,
                b: *const std::ffi::c_char,
                c: i32,
                d: *const std::ffi::c_char,
                flags: u32,
            ) -> i32;
        }
        let a = std::ffi::CString::new(from.as_os_str().as_bytes())
            .map_err(|_| refuse("Invalid source path."))?;
        let b = std::ffi::CString::new(to.as_os_str().as_bytes())
            .map_err(|_| refuse("Invalid destination path."))?;
        // SAFETY: NUL-terminated paths; RENAME_NOREPLACE atomically rejects any existing destination.
        if unsafe { renameat2(-100, a.as_ptr(), -100, b.as_ptr(), 1) } != 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        crate::durable::flush_dir(from.parent().unwrap())?;
        crate::durable::flush_dir(to.parent().unwrap())?;
    }
    #[cfg(windows)]
    {
        crate::durable::rename_dir(from, to)?;
    }
    #[cfg(not(any(target_os = "linux", windows)))]
    {
        return Err(refuse("Recovery is unavailable on this platform."));
    }
    Ok(())
}
#[cfg(test)]
thread_local! { static STOP:std::cell::Cell<Option<usize>>=const{std::cell::Cell::new(None)}; static RUNNING:std::cell::Cell<bool>=const{std::cell::Cell::new(false)}; }
fn step(_n: usize) -> Result<()> {
    #[cfg(test)]
    if STOP.with(|s| s.get() == Some(_n)) {
        return Err(refuse("Injected interruption."));
    }
    Ok(())
}
fn finish(root: &Path, folder: &str, j: &mut Journal) -> Result<()> {
    if !j.undo && entry(root, "enshrouded.kfc")?.sha256 != j.header_sha256 {
        // A different verified index invalidates a pending forward move. Return only unchanged
        // files to empty original names; the same preflight below refuses every collision.
        j.undo = true;
        j.completed = 0;
        save(root, folder, j)?;
    }
    // Preflight the entire transaction, including already renamed files after a crash before journaling.
    for e in &j.plan.files {
        let saved = format!("{folder}/{}", e.from);
        let (from, to) = if j.undo {
            (&saved, &e.from)
        } else {
            (&e.from, &saved)
        };
        let a = checked(root, from)?;
        let b = checked(root, to)?;
        match (a.try_exists()?, b.try_exists()?) {
            (true, false) if entry(root, from)?.sha256 == e.sha256 => {}
            (false, true) if entry(root, to)?.sha256 == e.sha256 => {}
            _ => {
                return Err(refuse(&format!(
                    "Cannot move {}: a name exists again, is missing, or its contents changed.",
                    e.from
                )));
            }
        }
    }
    for (i, e) in j.plan.files.iter().enumerate() {
        let saved = format!("{folder}/{}", e.from);
        let (from, to) = if j.undo {
            (&saved, &e.from)
        } else {
            (&e.from, &saved)
        };
        step(i * 2)?;
        if checked(root, from)?.try_exists()? {
            if let Some(parent) = checked(root, to)?.parent() {
                crate::durable::ensure_dir(parent)?;
            }
            rename(&checked(root, from)?, &checked(root, to)?)?;
        }
        step(i * 2 + 1)?;
        j.completed = i + 1;
        save(root, folder, j)?;
    }
    step(j.plan.files.len() * 2)?;
    j.complete = true;
    save(root, folder, j)
}
fn recover(root: &Path) -> Result<()> {
    for folder in journals(root)? {
        let mut j = read_journal(root, &folder)?;
        if !j.complete {
            finish(root, &folder, &mut j)?;
        }
    }
    Ok(())
}
/// Pending moves are finished on the next recovery call, under the writer lock; never at launcher startup.
pub fn plan(root: &Path) -> Result<Plan> {
    if !relevant(root)? {
        return Err(refuse(NOTHING));
    }
    let _lock = guard(root)?;
    recover(root)?;
    make_plan(root)
}
/// The caller must supply explicit Steam-verification confirmation and the fingerprint the dialog displayed.
pub fn apply(root: &Path, fingerprint: &str, verified: bool) -> Result<Moved> {
    if !verified {
        return Err(refuse(
            "Verify the game files in Steam and confirm before moving anything.",
        ));
    }
    let _lock = guard(root)?;
    recover(root)?;
    for folder in journals(root)? {
        let j = read_journal(root, &folder)?;
        if j.plan.fingerprint == fingerprint && !j.undo {
            return Ok(Moved {
                folder,
                files: j.plan.files,
            });
        }
    }
    let p = make_plan(root)?;
    if p.fingerprint != fingerprint {
        return Err(refuse("The file plan changed; review it again."));
    }
    let folder = format!(
        "{BASE}/restore-{}-{:09}",
        crate::util::now_unix(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .subsec_nanos()
    );
    let dest = checked(root, &folder)?;
    crate::durable::ensure_dir(&checked(root, BASE)?)?;
    fs::create_dir(&dest)?;
    let mut j=Journal{header_sha256:entry(root,"enshrouded.kfc")?.sha256,plan:p,when:crate::util::now_rfc3339(),why:"Player confirmed Steam verification; set unproven leftovers aside for a fresh EML backup".into(),undo:false,complete:false,completed:0};
    save(root, &folder, &j)?;
    finish(root, &folder, &mut j)?;
    if j.undo {
        return Err(refuse(
            "The game's index changed while the files were being moved, so the move was undone. Start the game through Kiln once and come back.",
        ));
    }
    Ok(Moved {
        folder,
        files: j.plan.files,
    })
}
pub fn undo(root: &Path, folder: &str) -> Result<Moved> {
    let _lock = guard(root)?;
    recover(root)?;
    if !journals(root)?.iter().any(|f| f == folder) {
        return Err(refuse("Unknown recovery folder."));
    }
    let mut j = read_journal(root, folder)?;
    if j.undo {
        return Ok(Moved {
            folder: folder.into(),
            files: j.plan.files,
        });
    }
    // Refuse all collisions before recording undo intent.
    for e in &j.plan.files {
        if checked(root, &e.from)?.try_exists()? {
            return Err(refuse(&format!("Cannot undo: {} exists again.", e.from)));
        }
    }
    j.undo = true;
    j.complete = false;
    j.completed = 0;
    save(root, folder, &j)?;
    finish(root, folder, &mut j)?;
    Ok(Moved {
        folder: folder.into(),
        files: j.plan.files,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Temp(std::path::PathBuf);
    impl Temp {
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn fixture() -> Temp {
        static N: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let d = Temp(std::env::temp_dir().join(format!(
            "restore-data-{}-{}",
            std::process::id(),
            N.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        )));
        let r = d.path();
        fs::create_dir_all(r.join("kiln/state")).unwrap();
        fs::write(r.join("enshrouded.kfc"), b"verified header").unwrap();
        fs::write(r.join("enshrouded.kfc_resources"), b"resources").unwrap();
        fs::write(r.join("kiln/state/eml.json"), serde_json::to_vec(&serde_json::json!({"phase":"failed","recovery":{
            "stem":"enshrouded", "headerSha256":crate::hash::sha256_hex(b"verified header"), "containers":55,"unproven":true
        }})).unwrap()).unwrap();
        for i in 55..64 {
            fs::write(r.join(format!("enshrouded_{i:03}.dat")), b"foreign").unwrap();
        }
        for name in ["enshrouded.kfc.bak", "enshrouded.kfc_resources.bak"] {
            fs::write(r.join(name), b"old").unwrap();
        }
        d
    }
    #[test]
    fn restore_data_running_and_writer_lock() {
        let d = fixture();
        RUNNING.with(|r| r.set(true));
        let e = plan(d.path()).unwrap_err();
        RUNNING.with(|r| r.set(false));
        assert!(e.message.contains("Close the game"));
        let lock = guard(d.path()).unwrap();
        assert!(plan(d.path()).unwrap_err().message.contains("busy"));
        drop(lock);
    }
    #[test]
    fn restore_data_cache_seal_and_latest() {
        let d = fixture();
        let r = d.path();
        fs::create_dir_all(r.join("kiln/eml/cache")).unwrap();
        fs::write(r.join("kiln/eml/cache/files.json"), b"cache").unwrap();
        fs::write(r.join("enshrouded.kfc.bak.integrity"), b"seal").unwrap();
        fs::write(r.join("kiln/eml/cache/files.partial.json"), b"partial").unwrap();
        let p = plan(r).unwrap();
        assert_eq!(p.files.len(), 14);
        let m = apply(r, &p.fingerprint, true).unwrap();
        assert_eq!(latest(r).unwrap().unwrap().folder, m.folder);
        assert!(!r.join("kiln/eml/cache/files.json").exists());
        undo(r, &m.folder).unwrap();
        assert_eq!(
            fs::read(r.join("kiln/eml/cache/files.json")).unwrap(),
            b"cache"
        );
        assert!(latest(r).unwrap().is_none());
    }
    #[test]
    fn restore_data_never_overwrites() {
        let d = fixture();
        let r = d.path();
        let a = r.join("a");
        let b = r.join("b");
        fs::write(&a, b"a").unwrap();
        fs::write(&b, b"b").unwrap();
        assert!(rename(&a, &b).is_err());
        assert_eq!(fs::read(&b).unwrap(), b"b");
        assert!(a.exists());
    }
    #[cfg(unix)]
    #[test]
    fn restore_data_destination_links() {
        let d = fixture();
        let r = d.path();
        fs::create_dir_all(r.join("kiln/eml")).unwrap();
        std::os::unix::fs::symlink(std::env::temp_dir(), r.join("kiln/eml/displaced")).unwrap();
        assert!(plan(r).is_err());
    }
    #[test]
    fn restore_data_player_plan() {
        let d = fixture();
        assert_eq!(plan(d.path()).unwrap().files.len(), 11);
    }

    #[test]
    fn restore_data_refusals_and_fingerprint() {
        let d = fixture();
        let r = d.path();
        let p = plan(r).unwrap();
        assert!(
            apply(r, &p.fingerprint, false)
                .unwrap_err()
                .message
                .contains("Steam")
        );
        fs::write(r.join("enshrouded_055.dat"), b"changed").unwrap();
        assert!(
            apply(r, &p.fingerprint, true)
                .unwrap_err()
                .message
                .contains("plan changed")
        );
    }
    #[test]
    fn restore_data_shroudforge_is_refused_only_while_it_writes() {
        let d = fixture();
        let r = d.path();
        // ShroudForge's folder from an earlier profile: kiln-eml was the writer whose start failed
        fs::create_dir_all(r.join("shroudforge/config")).unwrap();
        fs::write(r.join("shroudforge/state.json"), b"{}").unwrap();
        assert!(shroudforge_writes(r).unwrap().is_none());
        assert_eq!(plan(r).unwrap().files.len(), 11);
        // Kiln hosts ShroudForge for the loaded profile: the files may be ShroudForge's own
        fs::create_dir_all(r.join("kiln/plugins/shroudforge")).unwrap();
        fs::write(r.join("kiln/plugins/kiln-shroudforge.dll"), b"x").unwrap();
        fs::write(r.join("kiln/plugins/shroudforge/shroudforge-runtime.dll"), b"x").unwrap();
        fs::write(
            r.join("kiln/profile.json"),
            br#"{"format": 1, "id": "p", "generation": 1, "mods": {"shroudforge": [{"id": "sfmod", "path": "lib/sfmod", "enabled": true}]}}"#,
        )
        .unwrap();
        assert_eq!(shroudforge_writes(r).unwrap(), Some(crate::own_loaders::Via::HostedShroudForge));
        assert!(plan(r).unwrap_err().message.contains("ShroudForge"));
        // the same profile with the mod switched off
        fs::write(
            r.join("kiln/profile.json"),
            br#"{"format": 1, "id": "p", "generation": 1, "mods": {"shroudforge": [{"id": "sfmod", "path": "lib/sfmod", "enabled": false}]}}"#,
        )
        .unwrap();
        assert!(shroudforge_writes(r).unwrap().is_none());
        assert_eq!(plan(r).unwrap().files.len(), 11);
    }
    /// Kiln's interpreter carries the loaded profile's EML mods (kiln.json selects it): the refusal stays,
    /// in words about the interpreter (Kiln prepares the game's data for it at start), never ShroudForge's own.
    #[cfg(feature = "sfi-scan")]
    #[test]
    fn restore_data_refusal_under_kilns_interpreter_says_so() {
        let d = fixture();
        let r = d.path();
        fs::write(
            r.join("kiln/kiln.json"),
            br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}}"#,
        )
        .unwrap();
        fs::write(
            r.join("kiln/profile.json"),
            br#"{"format": 1, "id": "p", "generation": 1, "mods": {"shroudforge": [{"id": "sfmod", "path": "../mods/sfmod", "enabled": true}]}}"#,
        )
        .unwrap();
        // without the interpreter nothing carries the EML mods: kiln-eml is the writer whose start failed
        assert_eq!(plan(r).unwrap().files.len(), 11);
        fs::create_dir_all(r.join("kiln/plugins")).unwrap();
        fs::write(r.join("kiln/plugins/kiln-sfi.dll"), b"x").unwrap();
        assert_eq!(shroudforge_writes(r).unwrap(), Some(crate::own_loaders::Via::Interpreter));
        let e = plan(r).unwrap_err();
        assert_eq!(e.code, crate::ErrorCode::Refused);
        assert!(e.message.starts_with("Kiln's interpreter runs this profile's ShroudForge mods"), "{}", e.message);
        assert!(!e.message.contains("ShroudForge prepares") && !e.message.contains("own loader"), "{}", e.message);
        // hosted ShroudForge for the same profile: ShroudForge's words, as before
        fs::write(r.join("kiln/kiln.json"), br#"{"shroudforge": {"backend": "hosted"}}"#).unwrap();
        fs::create_dir_all(r.join("kiln/plugins/shroudforge")).unwrap();
        fs::write(r.join("kiln/plugins/kiln-shroudforge.dll"), b"x").unwrap();
        fs::write(r.join("kiln/plugins/shroudforge/shroudforge-runtime.dll"), b"x").unwrap();
        assert!(plan(r).unwrap_err().message.starts_with("ShroudForge prepares the game's data"));
    }
    #[test]
    fn restore_data_leaves_a_game_with_nothing_to_restore_untouched() {
        let d = fixture();
        let r = d.path();
        // the last start said nothing about unproven data: no lock file, no folder, no hashing, also while a game runs
        fs::write(r.join("kiln/state/eml.json"), br#"{"phase":"done"}"#).unwrap();
        RUNNING.with(|x| x.set(true));
        let e = plan(r).unwrap_err();
        let l = latest(r).unwrap();
        RUNNING.with(|x| x.set(false));
        assert!(e.message.contains("nothing to restore"), "{}", e.message);
        assert!(l.is_none());
        assert!(!r.join(".kiln-eml.lock").exists());
        assert!(!r.join("kiln/eml").exists());
        fs::remove_file(r.join("kiln/state/eml.json")).unwrap();
        assert!(plan(r).unwrap_err().message.contains("nothing to restore"));
        assert!(!r.join(".kiln-eml.lock").exists());
    }
    #[test]
    fn restore_data_undo_and_collision() {
        let d = fixture();
        let r = d.path();
        let p = plan(r).unwrap();
        let m = apply(r, &p.fingerprint, true).unwrap();
        assert_eq!(apply(r, &p.fingerprint, true).unwrap().folder, m.folder);
        fs::write(r.join("enshrouded_055.dat"), b"new").unwrap();
        assert!(
            undo(r, &m.folder)
                .unwrap_err()
                .message
                .contains("exists again")
        );
        fs::remove_file(r.join("enshrouded_055.dat")).unwrap();
        undo(r, &m.folder).unwrap();
        assert_eq!(plan(r).unwrap().files.len(), 11);
    }
    #[test]
    fn restore_data_interruptions() {
        for stop in 0..=22 {
            let d = fixture();
            let r = d.path();
            let p = plan(r).unwrap();
            STOP.with(|s| s.set(Some(stop)));
            assert!(apply(r, &p.fingerprint, true).is_err());
            STOP.with(|s| s.set(None));
            let m = apply(r, &p.fingerprint, true).unwrap();
            assert_eq!(m.files.len(), 11);
            STOP.with(|s| s.set(Some(stop)));
            assert!(undo(r, &m.folder).is_err());
            STOP.with(|s| s.set(None));
            undo(r, &m.folder).unwrap();
            assert_eq!(plan(r).unwrap().files.len(), 11);
        }
    }
    #[test]
    fn restore_data_index_change_undoes_interrupted_move() {
        let d = fixture();
        let r = d.path();
        let p = plan(r).unwrap();
        STOP.with(|s| s.set(Some(1)));
        assert!(apply(r, &p.fingerprint, true).is_err());
        STOP.with(|s| s.set(None));
        fs::write(r.join("enshrouded.kfc"), b"updated index").unwrap();
        assert!(plan(r).unwrap_err().message.contains("index has changed"));
        for e in p.files {
            assert!(r.join(e.from).exists());
        }
    }
    #[test]
    fn restore_data_empty_owned_and_changed_index() {
        let d = fixture();
        let r = d.path();
        fs::write(r.join("enshrouded.kfc"), b"updated").unwrap();
        assert!(plan(r).unwrap_err().message.contains("index has changed"));
        fs::write(r.join("enshrouded.kfc"), b"verified header").unwrap();
        let p = plan(r).unwrap();
        for e in p.files {
            fs::remove_file(r.join(e.from)).unwrap();
        }
        assert!(plan(r).unwrap_err().message.contains("no leftover"));
        fs::create_dir_all(r.join("kiln/eml/ownership")).unwrap();
        fs::write(r.join("kiln/eml/ownership/enshrouded.json"), b"{}").unwrap();
        assert!(plan(r).unwrap_err().message.contains("record of the game data it wrote"));
    }
    #[cfg(unix)]
    #[test]
    fn restore_data_links() {
        let d = fixture();
        let r = d.path();
        fs::remove_file(r.join("enshrouded_055.dat")).unwrap();
        std::os::unix::fs::symlink("enshrouded.kfc", r.join("enshrouded_055.dat")).unwrap();
        assert!(plan(r).is_err());
    }
}

/// Most recent completed move, retained across launcher restarts so Undo remains available.
pub fn latest(root: &Path) -> Result<Option<Moved>> {
    if journals(root)?.is_empty() {
        return Ok(None);
    }
    let _lock = guard(root)?;
    recover(root)?;
    for folder in journals(root)?.into_iter().rev() {
        let j = read_journal(root, &folder)?;
        if !j.undo {
            return Ok(Some(Moved {
                folder,
                files: j.plan.files,
            }));
        }
    }
    Ok(None)
}

/// The BASE "Restore the game's data" takes a kiln-mode game back to: what kiln-eml's ownership receipt
/// records as its origin (`baseOrigin`, written by a kiln-mode publication), kept only when it is keyed to exactly the
/// backups in the game folder now. Read only; the restore itself is kiln-eml's at the next start with no mod that
/// changes the game's data (its ownership-checked BASE restore), never a copy from here.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BaseOrigin {
    /// `recorded`: the receipt's origin, keyed to the backups present; `unrecorded`: backups are present but no origin
    /// a receipt carries is keyed to them; `unknown`: Kiln cannot tell (`reason`).
    pub state: String,
    pub token: Option<String>,
    /// The token's exact label (never "vanilla": integrity is not provenance).
    pub label: Option<String>,
    /// SHA-256 of the backup header (`enshrouded.kfc.bak`) the origin is keyed to.
    pub baseline_sha256: Option<String>,
    pub reason: Option<String>,
}

/// The backups a receipt's `baseOrigin.baseline` lists, in order.
#[cfg(feature = "sfi-scan")]
const BASELINE: [&str; 3] = ["enshrouded.kfc.bak", "enshrouded.kfc_resources.bak", "enshrouded.kfc.bak.integrity"];

/// [`BaseOrigin`] of a game on the interpreter's route; None on any other route, or when Kiln has no record of game
/// data it wrote (the game's data is as Kiln found it: nothing to go back to). Hashes the backups (on request only).
pub fn base_origin(root: &Path) -> Result<Option<BaseOrigin>> {
    #[cfg(feature = "sfi-scan")]
    {
        if !crate::sfi_routing::route_of(root).interpreter() {
            return Ok(None);
        }
        Ok(read_base_origin(root).unwrap_or_else(|e| Some(unknown_base(&e.message))))
    }
    #[cfg(not(feature = "sfi-scan"))]
    {
        let _ = root;
        Ok(None)
    }
}

#[cfg(feature = "sfi-scan")]
fn unknown_base(reason: &str) -> BaseOrigin {
    BaseOrigin { state: "unknown".into(), token: None, label: None, baseline_sha256: None, reason: Some(reason.into()) }
}

#[cfg(feature = "sfi-scan")]
fn read_base_origin(root: &Path) -> Result<Option<BaseOrigin>> {
    use serde_json::Value;
    let receipt = checked(root, "kiln/eml/ownership/enshrouded.json")?;
    let meta = match fs::symlink_metadata(&receipt) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        r => r?,
    };
    if !meta.is_file() || meta.len() > 16 * 1024 * 1024 {
        return Ok(Some(unknown_base("Kiln's record of the game data it wrote is not a file it can read")));
    }
    let Ok(v) = serde_json::from_slice::<Value>(&fs::read(&receipt)?) else {
        return Ok(Some(unknown_base("Kiln's record of the game data it wrote cannot be read")));
    };
    // the backups in the game folder now, each with its size and SHA-256 (sizes first: the resources are large)
    let mut present = Vec::new();
    for name in BASELINE {
        let p = checked(root, name)?;
        present.push(match fs::symlink_metadata(&p) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(e.into()),
            Ok(m) if !m.is_file() => return Ok(Some(unknown_base(&format!("{name} is not a regular file")))),
            Ok(m) => Some((p, m.len())),
        });
    }
    if present[0].is_none() || present[1].is_none() {
        return Ok(Some(unknown_base(
            "the game data Kiln backed up is not in the game folder (all of it): Kiln cannot tell what it would put back",
        )));
    }
    let header = || -> Result<String> { Ok(crate::hash::sha256_file(&present[0].as_ref().expect("checked").0)?.0) };
    let unrecorded = |why: &str| -> Result<Option<BaseOrigin>> {
        let (token, label) = crate::sfi_inspection::ORIGINS[2];
        Ok(Some(BaseOrigin { state: "unrecorded".into(), token: Some(token.into()), label: Some(label.into()),
            baseline_sha256: Some(header()?), reason: Some(why.into()) }))
    };
    let o = &v["baseOrigin"];
    let Some(obj) = o.as_object().filter(|o| o.len() == 3) else {
        return unrecorded("Kiln's record of the game data it wrote names no origin for these backups");
    };
    let (Some(token), Some(label), Some(baseline)) = (obj.get("token").and_then(Value::as_str), obj.get("label").and_then(Value::as_str),
        obj.get("baseline").and_then(Value::as_object)) else {
        return unrecorded("the origin in Kiln's record is not one it records");
    };
    // only the two unclassified tokens, each with its own exact label (sfi_inspection::ORIGINS, kiln-eml's RECORDED_ORIGINS)
    if !crate::sfi_inspection::ORIGINS[..2].iter().any(|&(t, l)| t == token && l == label)
        || baseline.len() != BASELINE.len()
    {
        return unrecorded("the origin in Kiln's record is not one it records");
    }
    for (name, now) in BASELINE.iter().zip(&present) {
        let Some(want) = baseline.get(*name) else { return unrecorded("the origin in Kiln's record is not one it records") };
        let keyed = match (want, now) {
            (Value::Null, None) => true,
            (Value::Object(d), Some((p, size))) if d.len() == 2 && d.get("size").and_then(Value::as_u64) == Some(*size) => {
                d.get("sha256").and_then(Value::as_str) == Some(crate::hash::sha256_file(p)?.0.as_str())
            }
            _ => false,
        };
        if !keyed {
            return unrecorded("the origin in Kiln's record is of other backups than the ones in the game folder now");
        }
    }
    Ok(Some(BaseOrigin { state: "recorded".into(), token: Some(token.into()), label: Some(label.into()),
        baseline_sha256: baseline[BASELINE[0]]["sha256"].as_str().map(str::to_owned), reason: None }))
}
