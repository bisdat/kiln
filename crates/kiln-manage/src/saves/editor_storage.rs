// SPDX-License-Identifier: MIT
//! Offline storage return disk boundary. The browser's exact codec prepares both files;
//! this module restricts targets, compares both originals, backs up, then publishes SIDECAR FIRST.
//! The old inventory witness retains full storage in history until the character write succeeds.

use crate::{Error, ErrorCode, Result};
use crate::game::{GameInstall, GameKind};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::{Path, PathBuf};
use super::editor;

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StorageSnapshot {
    pub character: String,
    pub game_id: String,
    pub text: Option<String>,
    pub local: bool,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReturnRequest {
    pub path: String,
    pub game_id: String,
    pub expected_save: Vec<u8>,
    pub expected_sidecar: String,
    pub data: Vec<u8>,
    pub sidecar: String,
    pub old_witness: String,
    pub new_witness: String,
}

#[derive(Debug, Serialize)]
pub struct ReturnResult { pub backup: String }

fn character_id(bytes: &[u8]) -> Result<String> {
    if bytes.len() < 48 || &bytes[..4] != b"SV2C" { return Err(Error::invalid("not a character save")); }
    let b = &bytes[12..28];
    Ok(format!("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{}-{}",
        b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], crate::hash::hex(&b[8..10]), crate::hash::hex(&b[10..16])))
}

fn sidecar_path(install: &GameInstall, id: &str) -> Result<PathBuf> {
    let base = install.path.join("kiln");
    let path = base.join("data/characters").join(format!("{}.json", id.replace('-', "")));
    crate::engine_mods::refuse_symlink_path(&install.path, &path)?;
    Ok(path)
}

fn local_target(install: &GameInstall, path: &Path) -> bool {
    if install.kind != GameKind::Client || !path.file_name().and_then(|s| s.to_str()).is_some_and(editor::is_character_file) { return false; }
    super::local_settings_dirs(install).iter().any(|root| {
        std::fs::canonicalize(root.join("0")).is_ok_and(|local| path.parent() == Some(local.as_path()))
    })
}

pub fn read_for_install(install: &GameInstall, path: &str) -> Result<StorageSnapshot> {
    let p = editor::target_path(Path::new(path))?;
    let id = character_id(&editor::read_file(path)?)?;
    let sidecar = sidecar_path(install, &id)?;
    let text = match std::fs::read(&sidecar) {
        Ok(bytes) => {
            if bytes.len() > 1024 * 1024 { return Err(Error::invalid("character sidecar is too large")); }
            Some(String::from_utf8(bytes).map_err(|_| Error::invalid("character sidecar is not UTF-8"))?)
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.into()),
    };
    Ok(StorageSnapshot { character: id, game_id: install.id.clone(), text, local: local_target(install, &p) })
}

fn same_guid(value: &Value, id: &str) -> bool {
    value.as_str().is_some_and(|s| s.replace('-', "").eq_ignore_ascii_case(&id.replace('-', "")))
}
fn storage(value: &Value) -> &Value { &value["mods"]["kiln-items"]["storage"] }
fn rev(value: &Value) -> Result<u64> {
    value.as_u64().filter(|n| *n < 9_007_199_254_740_991).ok_or_else(|| Error::invalid("invalid storage revision"))
}
fn valid_witness(w: &str) -> bool {
    w.len() == 21 && w.starts_with("xxh3:") && w[5..].bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn validate_pair(old: &Value, next: &Value, r: &ReturnRequest, id: &str) -> Result<()> {
    if old["format"] != 1 || next["format"] != 1 || !same_guid(&old["character"], id) || !same_guid(&next["character"], id) {
        return Err(Error::invalid("sidecar character or format does not match"));
    }
    let (d, n) = (storage(old), storage(next));
    if d["v"] != 1 || n["v"] != 1 || !same_guid(&d["character"], id) || !same_guid(&n["character"], id) {
        return Err(Error::invalid("storage is read-only or belongs to another character"));
    }
    if serde_json::to_vec(n)?.len() > 65536 { return Err(Error::invalid("storage exceeds 64 KiB")); }
    if !valid_witness(&r.old_witness) || !valid_witness(&r.new_witness) || r.old_witness == r.new_witness || n["current"]["witness"] != r.new_witness {
        return Err(Error::invalid("invalid returned inventory witness"));
    }
    let history = d["history"].as_array().ok_or_else(|| Error::invalid("invalid storage history"))?;
    let mut all = vec![(&d["current"], rev(&d["revision"])?, d["current"]["witness"].as_str().unwrap_or(""))];
    for e in history { all.push((e, rev(&e["revision"])?, e["witness"].as_str().unwrap_or(""))); }
    let primary = all.clone();
    for (e, _, _) in primary {
        if let Some(pairs) = e["pairs"].as_array() {
            for p in pairs { all.push((e, rev(&p["revision"])?, p["witness"].as_str().unwrap_or(""))); }
        }
    }
    let matched = all.iter().any(|(_, _, w)| *w == r.old_witness);
    let selected = all.iter().filter(|(_, _, w)| !matched || *w == r.old_witness).max_by_key(|(_, rev, _)| *rev)
        .ok_or_else(|| Error::invalid("no storage pair"))?.0;
    let hist = n["history"].as_array().ok_or_else(|| Error::invalid("missing pre-return pair"))?;
    if hist.len() > 8 || !hist.iter().any(|e| e["witness"] == r.old_witness && e["storages"] == selected["storages"] && e["parked"] == selected["parked"]) {
        return Err(Error::invalid("return must keep the full storage paired to the original character"));
    }
    let max = all.iter().map(|(_, rev, _)| *rev).max().unwrap_or(0);
    if rev(&n["revision"])? <= max || rev(&next["revision"])? <= rev(&old["revision"])? {
        return Err(Error::invalid("return revision must increase"));
    }
    let mut before = old["mods"].clone(); let mut after = next["mods"].clone();
    for value in [&mut before, &mut after] {
        value["kiln-items"].as_object_mut().ok_or_else(|| Error::invalid("missing item data"))?.remove("storage");
    }
    if before != after { return Err(Error::invalid("return cannot change other mod data")); }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Step { BeforeSidecar, AfterSidecar, AfterCharacter }
#[cfg(test)]
thread_local! {
    static FAULT: std::cell::Cell<Option<Step>> = const { std::cell::Cell::new(None) };
    static RUNNING: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
fn check_running() -> Result<()> {
    #[cfg(test)]
    let running = RUNNING.with(|v| v.get());
    #[cfg(not(test))]
    let running = false;
    if running || editor::game_running()? { return Err(Error::new(ErrorCode::GameRunning, "quit Enshrouded before returning storage")); }
    Ok(())
}
fn boundary(step: Step) -> Result<()> {
    #[cfg(test)]
    if FAULT.with(|v| v.get() == Some(step)) { return Err(Error::new(ErrorCode::Io, format!("injected {step:?} interruption"))); }
    let _ = step;
    Ok(())
}

pub fn return_for_install(install: &GameInstall, r: &ReturnRequest) -> Result<ReturnResult> {
    check_running()?;
    let p = editor::target_path(Path::new(&r.path))?;
    if r.game_id != install.id || !local_target(install, &p) { return Err(Error::new(ErrorCode::Refused, "storage can only be returned to LOCAL characters")); }
    // Reject aliases into a Steam/cloud save or out of the selected local root.
    crate::engine_mods::refuse_symlink_path(p.parent().unwrap(), Path::new(&r.path))?;
    let _save_lock = editor::lock_target(&p)?;
    let actual = std::fs::read(&p)?;
    if actual != r.expected_save { return Err(Error::new(ErrorCode::Refused, "character changed; read it again")); }
    let id = character_id(&actual)?;
    if character_id(&r.data)? != id { return Err(Error::invalid("returned save changed the character identity")); }
    let side = sidecar_path(install, &id)?;
    let _side_lock = editor::lock_target(&side)?;
    if std::fs::read(&side)? != r.expected_sidecar.as_bytes() { return Err(Error::new(ErrorCode::Refused, "storage changed; read it again")); }
    let old: Value = serde_json::from_str(&r.expected_sidecar)?;
    let next: Value = serde_json::from_str(&r.sidecar)?;
    validate_pair(&old, &next, r, &id)?;
    let backup = editor::make_backup(&p)?;
    crate::durable::replace(&backup.join("storage-sidecar.json"), r.expected_sidecar.as_bytes())?;
    check_running()?;
    // CAS is checked again after the potentially slow backup (external editors/cloud sync).
    if std::fs::read(&p)? != actual || std::fs::read(&side)? != r.expected_sidecar.as_bytes() {
        return Err(Error::new(ErrorCode::Refused, "character or storage changed during backup; read it again"));
    }
    boundary(Step::BeforeSidecar)?;
    crate::durable::replace(&side.with_extension("json.prev"), r.expected_sidecar.as_bytes())?;
    crate::durable::replace(&side, r.sidecar.as_bytes())?;
    boundary(Step::AfterSidecar)?;
    check_running()?;
    // The sidecar's historical full pair is already durable before the character can contain returned stacks.
    editor::replace_save(&p, &r.data, None)?;
    boundary(Step::AfterCharacter)?;
    Ok(ReturnResult { backup: backup.display().to_string() })
}


#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::{GameInstall, GameKind, PathMode, Platform};
    use serde_json::json;
    use std::sync::atomic::{AtomicU64, Ordering};

    struct Fixture { root: std::path::PathBuf, install: GameInstall, target: std::path::PathBuf, request: ReturnRequest }
    impl Drop for Fixture { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.root); } }
    impl Fixture {
        fn new() -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let root = std::env::temp_dir().join(format!("kiln-storage-return-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            let install = GameInstall { id: "storage-test".into(), kind: GameKind::Client, path: root.join("game"), exe: "enshrouded.exe".into(),
                app_id: crate::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(root.join("prefix")),
                build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into() };
            let target = install.prefix.as_ref().unwrap().join("drive_c/users/steamuser/Saved Games/keen games/Enshrouded/0/character0.ksd");
            std::fs::create_dir_all(target.parent().unwrap()).unwrap();
            let mut old = vec![0; 49]; old[..4].copy_from_slice(b"SV2C"); old[12..28].fill(1); old[48] = 1;
            let mut new = old.clone(); new[48] = 2;
            let id = character_id(&old).unwrap();
            let full = json!({"witness":"xxh3:0000000000000001","worldRev":10,"storages":{"mod:bag":{"slots":8,"records":[{"s":0,"b64":"AA==","item":1,"n":1}]}},"parked":{}});
            let sidecar = json!({"format":1,"character":id,"revision":10,"mods":{"other":{"keep":1},"kiln-items":{"storage":{"v":1,"character":id,"revision":5,"current":full,"history":[],"quarantine":[]}}}});
            let mut next = sidecar.clone();
            next["revision"] = 11.into();
            let doc = &mut next["mods"]["kiln-items"]["storage"];
            doc["revision"] = 6.into(); doc["current"]["witness"] = "xxh3:0000000000000002".into();
            doc["current"]["storages"]["mod:bag"]["records"] = json!([]);
            let mut history = full.clone(); history["revision"] = 5.into(); doc["history"] = json!([history]);
            let text = sidecar.to_string();
            let path = sidecar_path(&install, &id).unwrap(); std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(&path, &text).unwrap(); std::fs::write(&target, &old).unwrap();
            super::super::editor::tests::set_io(root.join("backups/editor"));
            let request = ReturnRequest { path: target.display().to_string(), game_id: install.id.clone(), expected_save: old, expected_sidecar: text,
                data: new, sidecar: next.to_string(), old_witness: "xxh3:0000000000000001".into(), new_witness: "xxh3:0000000000000002".into() };
            Self { root, install, target, request }
        }
        fn sidecar(&self) -> Vec<u8> { std::fs::read(sidecar_path(&self.install, &character_id(&self.request.data).unwrap()).unwrap()).unwrap() }
    }

    #[test]
    fn storage_return_backs_up_both_originals_then_commits_sidecar_before_character() {
        let f = Fixture::new();
        let result = return_for_install(&f.install, &f.request).unwrap();
        assert_eq!(std::fs::read(&f.target).unwrap(), f.request.data);
        assert_eq!(f.sidecar(), f.request.sidecar.as_bytes());
        assert_eq!(std::fs::read(std::path::Path::new(&result.backup).join("character0.ksd")).unwrap(), f.request.expected_save);
        assert_eq!(std::fs::read(std::path::Path::new(&result.backup).join("storage-sidecar.json")).unwrap(), f.request.expected_sidecar.as_bytes());
    }

    #[test]
    fn storage_return_crash_boundaries_never_publish_character_before_reduced_storage() {
        for at in [Step::BeforeSidecar, Step::AfterSidecar, Step::AfterCharacter] {
            let f = Fixture::new(); FAULT.with(|v| v.set(Some(at)));
            assert!(return_for_install(&f.install, &f.request).is_err());
            FAULT.with(|v| v.set(None));
            let char_new = std::fs::read(&f.target).unwrap() == f.request.data;
            let storage_new = f.sidecar() == f.request.sidecar.as_bytes();
            assert!(!char_new || storage_new, "no duplicate: new character requires reduced storage");
            assert_eq!(char_new, at == Step::AfterCharacter);
            assert_eq!(storage_new, at != Step::BeforeSidecar);
            // New sidecar retains the full old pair: an old character always has its storage.
            let side: serde_json::Value = serde_json::from_slice(&f.sidecar()).unwrap();
            let d = &side["mods"]["kiln-items"]["storage"];
            let pair = if char_new { &d["current"] } else if storage_new { &d["history"][0] } else { &d["current"] };
            assert_eq!(pair["storages"]["mod:bag"]["records"].as_array().unwrap().len(), usize::from(!char_new));
        }
    }

    #[test]
    fn storage_return_refuses_changed_character_or_sidecar_and_future_versions() {
        for case in 0..3 {
            let mut f = Fixture::new();
            if case == 0 { f.request.expected_save[48] = 7; }
            if case == 1 { f.request.expected_sidecar.push(' '); }
            if case == 2 {
                let mut doc: serde_json::Value = serde_json::from_str(&f.request.expected_sidecar).unwrap();
                doc["mods"]["kiln-items"]["storage"]["v"] = 2.into();
                f.request.expected_sidecar = doc.to_string();
                std::fs::write(sidecar_path(&f.install, &character_id(&f.request.data).unwrap()).unwrap(), &f.request.expected_sidecar).unwrap();
            }
            let old_char = std::fs::read(&f.target).unwrap(); let old_side = f.sidecar();
            assert!(return_for_install(&f.install, &f.request).is_err());
            assert_eq!(std::fs::read(&f.target).unwrap(), old_char); assert_eq!(f.sidecar(), old_side);
        }
    }

    #[test]
    fn storage_return_refuses_running_game_steam_targets_and_missing_old_pair() {
        for case in 0..3 {
            let mut f = Fixture::new();
            if case == 0 { RUNNING.with(|v| v.set(true)); }
            if case == 1 {
                let steam = f.root.join("userdata/1/1203620/remote/character0.ksd");
                std::fs::create_dir_all(steam.parent().unwrap()).unwrap(); std::fs::write(&steam, &f.request.expected_save).unwrap();
                f.request.path = steam.display().to_string();
            }
            if case == 2 {
                let mut next: serde_json::Value = serde_json::from_str(&f.request.sidecar).unwrap();
                next["mods"]["kiln-items"]["storage"]["history"] = json!([]); f.request.sidecar = next.to_string();
            }
            assert!(return_for_install(&f.install, &f.request).is_err()); RUNNING.with(|v| v.set(false));
            assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save); assert_eq!(f.sidecar(), f.request.expected_sidecar.as_bytes());
        }
    }
}
