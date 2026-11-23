// SPDX-License-Identifier: MIT
//! Offline storage return disk boundary. The browser's exact codec prepares the character and the
//! planned storage value; this module restricts targets, compares both originals, builds the new
//! sidecar from the original bytes (only the revision and `mods.kiln-items.storage` change, with
//! the return marker), backs up, then publishes SIDECAR FIRST.
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
    /// The planned `mods["kiln-items"].storage` value. Only its `revision`, `current` and `history` are used:
    /// the return marker is added here from the original bytes, and the rest of the sidecar is kept byte for byte,
    /// so other mods' data is never re-encoded (JSON.parse rounds integers past 2^53 and writes 1.0 as 1).
    pub storage: String,
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
        let local = root.join("0");
        // Canonicalizing first would bless a LOCAL folder redirected into Steam Cloud.
        // Saved Games itself may be a platform-managed alias; the game's save folders may not.
        root.parent().is_some_and(|base| crate::engine_mods::refuse_symlink_path(base, &local).is_ok())
            && std::fs::canonicalize(local).is_ok_and(|local| path.parent() == Some(local.as_path()))
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

const MAX_RETURNS: usize = 8;
const UNPAIRED: &str = "this character changed since Kiln last saw its storage";

/// The document's return markers `{revision, fromWitness, toWitness}`, checked as Kiln.Saves checks them. Servers
/// read them to hold their own copy instead of handing returned items back, so a malformed one is refused, not dropped.
fn markers(doc: &Value) -> Result<Vec<Value>> {
    let bad = || Error::invalid("invalid storage return marker");
    let list = match &doc["returns"] { Value::Null => return Ok(Vec::new()), Value::Array(list) => list, _ => return Err(bad()) };
    for m in list {
        let text = |key: &str| m[key].as_str().filter(|s| (1..=64).contains(&s.encode_utf16().count()));
        match (text("fromWitness"), text("toWitness")) {
            (Some(from), Some(to)) if from != to && rev(&m["revision"]).is_ok_and(|r| r > 0) => {}
            _ => return Err(bad()),
        }
    }
    Ok(list.clone())
}

/// Newest revision first, exact duplicates once, at most eight: the order Kiln.Saves keeps (ordinal UTF-16 ties).
fn bounded_returns(list: Vec<Value>) -> Vec<Value> {
    let key = |m: &Value| (m["revision"].as_u64().unwrap_or(0), m["fromWitness"].as_str().unwrap_or("").to_owned(),
        m["toWitness"].as_str().unwrap_or("").to_owned());
    let mut out: Vec<Value> = Vec::new();
    for m in list { if !out.iter().any(|o| key(o) == key(&m)) { out.push(m); } }
    out.sort_by(|a, b| {
        let ((ra, fa, ta), (rb, fb, tb)) = (key(a), key(b));
        rb.cmp(&ra).then_with(|| fa.encode_utf16().cmp(fb.encode_utf16())).then_with(|| ta.encode_utf16().cmp(tb.encode_utf16()))
    });
    out.truncate(MAX_RETURNS);
    out
}

/// Byte spans inside JSON text that serde_json has already accepted, so only valid syntax reaches the scanner.
/// Structural bytes are ASCII and never occur inside a multi-byte UTF-8 sequence.
mod span {
    pub(super) fn ws(b: &[u8], mut i: usize) -> usize {
        while i < b.len() && matches!(b[i], b' ' | b'\t' | b'\n' | b'\r') { i += 1; }
        i
    }
    pub(super) fn string_end(b: &[u8], mut i: usize) -> usize {
        i += 1;
        while b[i] != b'"' { i += if b[i] == b'\\' { 2 } else { 1 }; }
        i + 1
    }
    pub(super) fn value_end(b: &[u8], mut i: usize) -> usize {
        match b[i] {
            b'"' => string_end(b, i),
            b'{' | b'[' => {
                let mut depth = 0usize;
                loop {
                    match b[i] {
                        b'"' => { i = string_end(b, i); continue; }
                        b'{' | b'[' => depth += 1,
                        b'}' | b']' => { depth -= 1; if depth == 0 { return i + 1; } }
                        _ => {}
                    }
                    i += 1;
                }
            }
            _ => {
                while i < b.len() && !matches!(b[i], b',' | b'}' | b']' | b' ' | b'\t' | b'\n' | b'\r') { i += 1; }
                i
            }
        }
    }
}

/// The value span of the one `key` member of the object starting at `at`. A repeated key is refused: which copy
/// counts would depend on the reader.
fn member(text: &str, at: usize, key: &str) -> Result<(usize, usize)> {
    let b = text.as_bytes();
    let missing = || Error::invalid(format!("sidecar has no single \"{key}\" member"));
    if b.get(at) != Some(&b'{') { return Err(missing()); }
    let (mut i, mut found) = (span::ws(b, at + 1), None);
    while b[i] == b'"' {
        let name_end = span::string_end(b, i);
        let name: String = serde_json::from_str(&text[i..name_end])?;
        let start = span::ws(b, span::ws(b, name_end) + 1); // past the ':'
        let end = span::value_end(b, start);
        if name == key {
            if found.is_some() { return Err(missing()); }
            found = Some((start, end));
        }
        i = span::ws(b, end);
        if b[i] == b',' { i = span::ws(b, i + 1); }
    }
    found.ok_or_else(missing)
}

/// The new sidecar: the original bytes with only the top-level `revision` and `mods.kiln-items.storage` replaced.
/// The storage value is the original document with the planned revision, current pair and history, plus the return
/// marker (the new revision, the actual pre-write witness and the witness of the character about to be written).
fn build_sidecar(text: &str, old: &Value, planned: &Value, r: &ReturnRequest) -> Result<String> {
    let d = storage(old);
    if !d.is_object() || !planned.is_object() { return Err(Error::invalid("invalid storage document")); }
    let mut doc = d.clone();
    for key in ["revision", "current", "history"] { doc[key] = planned[key].clone(); }
    let revision = rev(&doc["revision"])?;
    let marker = serde_json::json!({ "revision": revision, "fromWitness": r.old_witness, "toWitness": r.new_witness });
    doc["returns"] = Value::Array(bounded_returns(std::iter::once(marker).chain(markers(d)?).collect()));
    // CharacterSync's own counter: it decides whether a server takes the other mods' progress from this copy, so it
    // rises by one. The storage document's revision is unrelated (storage never follows this one).
    let envelope = rev(&old["revision"])? + 1;
    rev(&envelope.into())?;
    let root = span::ws(text.as_bytes(), 0);
    let mods = member(text, root, "mods")?;
    let items = member(text, mods.0, "kiln-items")?;
    let mut parts = [(member(text, root, "revision")?, envelope.to_string()), (member(text, items.0, "storage")?, serde_json::to_string(&doc)?)];
    parts.sort_by_key(|((start, _), _)| *start);
    let mut out = String::with_capacity(text.len() + 1024);
    let mut at = 0;
    for ((start, end), value) in parts {
        out.push_str(&text[at..start]);
        out.push_str(&value);
        at = end;
    }
    out.push_str(&text[at..]);
    Ok(out)
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
    // A character no pair matches changed outside Kiln (or is a backup beyond history): returning could duplicate.
    let selected = all.iter().filter(|(_, _, w)| *w == r.old_witness).max_by_key(|(_, rev, _)| *rev)
        .ok_or_else(|| Error::new(ErrorCode::Refused, UNPAIRED))?.0;
    let hist = n["history"].as_array().ok_or_else(|| Error::invalid("missing pre-return pair"))?;
    if hist.len() > 8 || !hist.iter().any(|e| e["witness"] == r.old_witness && e["storages"] == selected["storages"] && e["parked"] == selected["parked"]) {
        return Err(Error::invalid("return must keep the full storage paired to the original character"));
    }
    // The marker carries the new revision: it must outrank every pair and every older marker.
    let marked = markers(d)?.iter().map(|m| rev(&m["revision"])).collect::<Result<Vec<_>>>()?;
    let max = all.iter().map(|(_, rev, _)| *rev).chain(marked).max().unwrap_or(0);
    if rev(&n["revision"])? <= max || rev(&next["revision"])? <= rev(&old["revision"])? {
        return Err(Error::invalid("return revision must increase"));
    }
    let rest = |v: &Value| -> Result<Value> {
        let mut v = v.clone();
        v.as_object_mut().ok_or_else(|| Error::invalid("invalid sidecar"))?.remove("revision");
        v["mods"]["kiln-items"].as_object_mut().ok_or_else(|| Error::invalid("missing item data"))?.remove("storage");
        Ok(v)
    };
    if rest(old)? != rest(next)? { return Err(Error::invalid("return cannot change other mod data")); }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Step { BeforeSidecar, AfterSidecar, AfterCharacter }
#[cfg(test)]
thread_local! {
    static FAULT: std::cell::Cell<Option<Step>> = const { std::cell::Cell::new(None) };
    static RUNNING: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static PROCESS_NAMES: std::cell::RefCell<Vec<String>> = const { std::cell::RefCell::new(Vec::new()) };
    static AFTER_SIDECAR: std::cell::RefCell<Option<(PathBuf, Vec<u8>)>> = const { std::cell::RefCell::new(None) };
}
fn check_running() -> Result<()> {
    #[cfg(test)]
    let running = RUNNING.with(|v| v.get());
    #[cfg(not(test))]
    let running = false;
    if running || editor::game_running()? { return Err(Error::new(ErrorCode::GameRunning, "quit Enshrouded before returning storage")); }
    #[cfg(test)]
    let names = PROCESS_NAMES.with(|v| v.borrow().clone());
    #[cfg(not(test))]
    let names: Vec<String> = crate::proc::list()?.into_iter().flat_map(|p| {
        let image = p.exe.as_deref().and_then(|e| e.file_name()).map(|n| n.to_string_lossy().into_owned());
        [Some(p.name), image].into_iter().flatten()
    }).collect();
    if names.iter().any(|name| name.eq_ignore_ascii_case("steam") || name.eq_ignore_ascii_case("steam.exe")) {
        return Err(Error::new(ErrorCode::GameRunning, "quit Steam before returning storage"));
    }
    Ok(())
}
fn boundary(step: Step) -> Result<()> {
    #[cfg(test)]
    if step == Step::AfterSidecar {
        AFTER_SIDECAR.with(|v| {
            if let Some((path, bytes)) = v.borrow_mut().take() { std::fs::write(path, bytes).unwrap(); }
        });
    }
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
    let sidecar = build_sidecar(&r.expected_sidecar, &old, &serde_json::from_str(&r.storage)?, r)?;
    let next: Value = serde_json::from_str(&sidecar)?;
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
    crate::durable::replace(&side, sidecar.as_bytes())?;
    boundary(Step::AfterSidecar)?;
    check_running()?;
    // The sidecar on disk must be exactly what was built, and another writer may have changed either file while
    // it was being flushed. Leave its bytes alone; our historical full pair still covers the original character.
    if std::fs::read(&p)? != actual || std::fs::read(&side)? != sidecar.as_bytes() {
        return Err(Error::new(ErrorCode::Refused, "character or storage changed during return; read it again"));
    }
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
            // The planned storage value the frontend sends: reduced current, the full pre-return pair in history.
            let mut doc = sidecar["mods"]["kiln-items"]["storage"].clone();
            doc["revision"] = 6.into(); doc["current"]["witness"] = "xxh3:0000000000000002".into();
            doc["current"]["storages"]["mod:bag"]["records"] = json!([]);
            let mut history = full.clone(); history["revision"] = 5.into(); doc["history"] = json!([history]);
            let text = sidecar.to_string();
            let path = sidecar_path(&install, &id).unwrap(); std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(&path, &text).unwrap(); std::fs::write(&target, &old).unwrap();
            super::super::editor::tests::set_io(root.join("backups/editor"));
            let request = ReturnRequest { path: target.display().to_string(), game_id: install.id.clone(), expected_save: old, expected_sidecar: text,
                data: new, storage: doc.to_string(), old_witness: "xxh3:0000000000000001".into(), new_witness: "xxh3:0000000000000002".into() };
            Self { root, install, target, request }
        }
        fn sidecar(&self) -> Vec<u8> { std::fs::read(sidecar_path(&self.install, &character_id(&self.request.data).unwrap()).unwrap()).unwrap() }
        /// The sidecar a successful return writes (the ordering tests compare against it).
        fn produced(&self) -> Vec<u8> {
            let old: serde_json::Value = serde_json::from_str(&self.request.expected_sidecar).unwrap();
            build_sidecar(&self.request.expected_sidecar, &old, &serde_json::from_str(&self.request.storage).unwrap(), &self.request).unwrap().into_bytes()
        }
        fn planned(&mut self, change: impl FnOnce(&mut serde_json::Value)) {
            let mut doc: serde_json::Value = serde_json::from_str(&self.request.storage).unwrap();
            change(&mut doc);
            self.request.storage = doc.to_string();
        }
        fn set_expected_sidecar(&mut self, text: &str) {
            std::fs::write(sidecar_path(&self.install, &character_id(&self.request.data).unwrap()).unwrap(), text).unwrap();
            self.request.expected_sidecar = text.into();
        }
    }

    fn side_doc(text: &[u8]) -> serde_json::Value { serde_json::from_slice(text).unwrap() }

    #[test]
    fn storage_return_never_reencodes_other_mod_data() {
        // What Kiln.Saves may hold for another mod: integers past 2^53, float-formatted integers, odd spacing, an
        // escaped key; a sibling of the storage value in kiln-items' own data; the revision after the mods.
        let mut f = Fixture::new();
        let id = character_id(&f.request.expected_save).unwrap();
        let storage = side_doc(f.request.expected_sidecar.as_bytes())["mods"]["kiln-items"]["storage"].to_string();
        let other = r#""other": {"keep": 1, "ticks": 638712345678901234567, "steam": 76561198000000001, "ratio": 1.0, "e": 1E2, "key": [ 1.50, -0.0 ]}"#;
        let head = format!("{{\"format\": 1, \"character\": \"{id}\",\n  \"mods\": {{\"kiln-items\": {{\"unrelated\": 1.0 , \"storage\" :");
        let tail = format!("}},\n  {other}}},\n  \"revision\" : 10 }}\n");
        f.set_expected_sidecar(&format!("{head}{storage}{tail}"));
        return_for_install(&f.install, &f.request).expect("return succeeds");
        let written = String::from_utf8(f.sidecar()).unwrap();
        // Everything outside the storage value and the top-level revision is the original text.
        let tail = tail.replace("\"revision\" : 10 }", "\"revision\" : 11 }");
        assert!(written.starts_with(&head) && written.ends_with(&tail), "{written}");
        let doc: serde_json::Value = serde_json::from_str(&written[head.len()..written.len() - tail.len()]).unwrap();
        assert_eq!(doc["current"]["witness"], "xxh3:0000000000000002");
        assert_eq!(doc["history"][0]["witness"], "xxh3:0000000000000001");
        assert_eq!(std::fs::read(&f.target).unwrap(), f.request.data);
    }

    #[test]
    fn storage_return_keeps_older_markers_newest_first_at_most_eight() {
        let mut f = Fixture::new();
        let old_markers: Vec<_> = (10..19).map(|r| json!({"revision": r, "fromWitness": format!("from-{r}"), "toWitness": format!("to-{r}")})).collect();
        let mut old = side_doc(f.request.expected_sidecar.as_bytes());
        old["mods"]["kiln-items"]["storage"]["returns"] = json!(old_markers.clone());
        f.set_expected_sidecar(&old.to_string());
        // The frontend's own list is not trusted: the markers come from the original bytes.
        f.planned(|doc| { doc["revision"] = 20.into(); doc["returns"] = json!([]); });
        return_for_install(&f.install, &f.request).unwrap();
        let side = side_doc(&f.sidecar());
        let returns = side["mods"]["kiln-items"]["storage"]["returns"].as_array().unwrap().clone();
        assert_eq!(returns[0], json!({"revision": 20, "fromWitness": "xxh3:0000000000000001", "toWitness": "xxh3:0000000000000002"}));
        assert_eq!(returns[1..], old_markers.iter().rev().take(7).cloned().collect::<Vec<_>>()[..]);
        assert_eq!(side["revision"], 11); // CharacterSync's counter: one more than before, not the storage revision
    }

    #[test]
    fn storage_return_refuses_malformed_markers_a_revision_below_a_marker_and_duplicate_keys() {
        for case in 0..3 {
            let mut f = Fixture::new();
            let mut old = side_doc(f.request.expected_sidecar.as_bytes());
            let text = match case {
                0 => { old["mods"]["kiln-items"]["storage"]["returns"] = json!([{"revision": 3, "fromWitness": "a", "toWitness": "a"}]); old.to_string() }
                1 => { old["mods"]["kiln-items"]["storage"]["returns"] = json!([{"revision": 6, "fromWitness": "a", "toWitness": "b"}]); old.to_string() }
                _ => f.request.expected_sidecar.replacen("\"format\":1", "\"revision\":4,\"format\":1", 1),
            };
            f.set_expected_sidecar(&text);
            assert!(return_for_install(&f.install, &f.request).is_err(), "case {case}");
            assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save);
            assert_eq!(f.sidecar(), text.as_bytes());
        }
    }

    /// The planner's real request (launcher/unit/player-storage-golden.test.ts) through this disk boundary: the exact
    /// sidecar a return writes, pinned as tests/fixtures/player-storage/return-sidecar.json for Kiln.Saves' pairing tests.
    /// KILN_UPDATE_FIXTURES=1 rewrites it after an intended change.
    #[test]
    fn storage_return_golden_is_the_sidecar_kiln_saves_reads() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/player-storage");
        let request: serde_json::Value = serde_json::from_slice(&std::fs::read(dir.join("return-request.json")).unwrap()).unwrap();
        let text = request["expectedSidecar"].as_str().unwrap();
        let r = ReturnRequest { path: String::new(), game_id: String::new(), expected_save: Vec::new(), expected_sidecar: text.into(),
            data: Vec::new(), storage: request["storage"].as_str().unwrap().into(),
            old_witness: request["oldWitness"].as_str().unwrap().into(), new_witness: request["newWitness"].as_str().unwrap().into() };
        let old: serde_json::Value = serde_json::from_str(text).unwrap();
        let id = old["character"].as_str().unwrap().to_owned();
        let built = build_sidecar(text, &old, &serde_json::from_str(&r.storage).unwrap(), &r).unwrap();
        let next: serde_json::Value = serde_json::from_str(&built).unwrap();
        validate_pair(&old, &next, &r, &id).unwrap();
        // Other mods' bytes (and a sibling of the storage value) are the original text.
        assert!(built.contains(r#""other-mod":{"ticks":638712345678901234567,"steam":76561198000000001,"ratio":1.0,"e":1E2}"#), "{built}");
        assert!(built.ends_with(r#","note":1.0}}}"#), "{built}");
        assert_eq!(next["revision"], 18);
        assert_eq!(next["mods"]["kiln-items"]["storage"]["returns"][0],
            json!({"revision": 43, "fromWitness": r.old_witness, "toWitness": r.new_witness}));
        let golden = dir.join("return-sidecar.json");
        if std::env::var("KILN_UPDATE_FIXTURES").as_deref() == Ok("1") { std::fs::write(&golden, &built).unwrap(); }
        assert_eq!(std::fs::read_to_string(&golden).unwrap(), built);
    }

    #[test]
    fn storage_return_raises_the_sidecar_revision_by_one_whatever_the_storage_revision() {
        // The top-level revision is CharacterSync's: it decides whether a server takes this sidecar's OTHER mods' progress.
        // Jumping it to the storage document's (unrelated, faster) counter would let this copy's older progress replace a
        // server's newer one. Storage itself never follows that revision.
        let mut f = Fixture::new();
        let mut old = side_doc(f.request.expected_sidecar.as_bytes());
        old["mods"]["kiln-items"]["storage"]["revision"] = 500.into();
        f.set_expected_sidecar(&old.to_string());
        f.planned(|doc| { doc["revision"] = 501.into(); doc["history"][0]["revision"] = 500.into(); });
        return_for_install(&f.install, &f.request).unwrap();
        let side = side_doc(&f.sidecar());
        assert_eq!(side["revision"], 11);
        assert_eq!(side["mods"]["kiln-items"]["storage"]["revision"], 501);
    }

    #[test]
    fn storage_return_writes_the_return_marker_with_the_reduced_pair() {
        let f = Fixture::new();
        return_for_install(&f.install, &f.request).unwrap();
        let d = side_doc(&f.sidecar());
        let d = &d["mods"]["kiln-items"]["storage"];
        assert_eq!(d["returns"][0], json!({"revision": d["revision"], "fromWitness": "xxh3:0000000000000001", "toWitness": "xxh3:0000000000000002"}));
        assert_eq!(d["current"]["witness"], "xxh3:0000000000000002");
    }

    #[test]
    fn storage_return_refuses_a_character_that_pairs_with_no_storage() {
        let mut f = Fixture::new();
        // The frontend would pin the unmatched file's witness; the original sidecar never saw it.
        f.request.old_witness = "xxh3:00000000000000ff".into();
        f.planned(|doc| doc["history"][0]["witness"] = "xxh3:00000000000000ff".into());
        let err = return_for_install(&f.install, &f.request).unwrap_err();
        assert!(err.to_string().contains("changed since Kiln last saw its storage"), "{err}");
        assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save);
        assert_eq!(f.sidecar(), f.request.expected_sidecar.as_bytes());
    }

    #[test]
    fn storage_return_refuses_steam_even_without_the_game() {
        for name in ["steam", "Steam.exe", "steam.exe"] {
            let f = Fixture::new();
            PROCESS_NAMES.with(|v| *v.borrow_mut() = vec![name.into()]);
            let result = return_for_install(&f.install, &f.request);
            PROCESS_NAMES.with(|v| v.borrow_mut().clear());
            assert!(result.is_err(), "must refuse while {name} is running");
            assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save);
            assert_eq!(f.sidecar(), f.request.expected_sidecar.as_bytes());
        }
    }

    #[test]
    #[cfg(unix)]
    fn storage_return_refuses_local_root_redirected_to_cloud() {
        let mut f = Fixture::new();
        let local = f.target.parent().unwrap().to_path_buf();
        let cloud = f.root.join("userdata/1/1203620/remote");
        std::fs::create_dir_all(cloud.parent().unwrap()).unwrap();
        std::fs::rename(&local, &cloud).unwrap();
        std::os::unix::fs::symlink(&cloud, &local).unwrap();
        f.target = cloud.join("character0.ksd");
        f.request.path = f.target.display().to_string();
        assert!(!read_for_install(&f.install, &f.request.path).unwrap().local);
        assert!(return_for_install(&f.install, &f.request).is_err());
        assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save);
    }

    #[test]
    fn storage_return_rechecks_both_files_after_sidecar_publication() {
        for change_sidecar in [false, true] {
            let f = Fixture::new();
            let path = if change_sidecar { sidecar_path(&f.install, &character_id(&f.request.data).unwrap()).unwrap() } else { f.target.clone() };
            let changed = if change_sidecar { b"new storage from another writer".to_vec() } else { let mut b = f.request.expected_save.clone(); b[48] = 9; b };
            AFTER_SIDECAR.with(|v| *v.borrow_mut() = Some((path.clone(), changed.clone())));
            assert!(return_for_install(&f.install, &f.request).is_err(), "must refuse a concurrent change after the first write");
            assert_eq!(std::fs::read(path).unwrap(), changed);
            assert_eq!(std::fs::read(&f.target).unwrap(), if change_sidecar { f.request.expected_save.clone() } else { changed });
        }
    }

    #[test]
    fn storage_return_backs_up_both_originals_then_commits_sidecar_before_character() {
        let f = Fixture::new();
        let result = return_for_install(&f.install, &f.request).unwrap();
        assert_eq!(std::fs::read(&f.target).unwrap(), f.request.data);
        assert_eq!(f.sidecar(), f.produced());
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
            let storage_new = f.sidecar() == f.produced();
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
                f.planned(|doc| doc["history"] = json!([]));
            }
            assert!(return_for_install(&f.install, &f.request).is_err()); RUNNING.with(|v| v.set(false));
            assert_eq!(std::fs::read(&f.target).unwrap(), f.request.expected_save); assert_eq!(f.sidecar(), f.request.expected_sidecar.as_bytes());
        }
    }
}
