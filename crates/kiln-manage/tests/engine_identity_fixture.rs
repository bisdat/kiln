//! The hash a server advertises for another engine's mod (modpack::required, engine_identity) against the shared
//! fixture schema/fixtures/engine-identity/cases.json: trees of Kiln's placed and synced copies, the same after a run
//! that writes logs and changes settings, a changed code file, a forged marker, the player's own folder, odd names and
//! line endings. A Python reference (engine_identity.py) wrote each case's hash; Kiln.Network's EngineHashTests read the
//! same file, so the server's advertised hash and the launcher's are the same for every case.

use kiln_manage::manifest::ModKind;
use kiln_manage::modpack;
use serde_json::Value;
use std::path::{Path, PathBuf};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap()
}

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-engine-identity-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}

/// `v` with the fixture's root placeholder in its strings (keys too) replaced by `root`.
fn rooted(v: &Value, placeholder: &str, root: &str) -> Value {
    match v {
        Value::String(s) => Value::String(s.replace(placeholder, root)),
        Value::Array(a) => Value::Array(a.iter().map(|x| rooted(x, placeholder, root)).collect()),
        Value::Object(o) => Value::Object(o.iter().map(|(k, x)| (k.replace(placeholder, root), rooted(x, placeholder, root))).collect()),
        other => other.clone(),
    }
}

/// Writes a case's tree into `root` and returns its mod folder.
fn materialize(case: &Value, placeholder: &str, root: &Path) -> PathBuf {
    let dir = root.join(case["folder"].as_str().unwrap());
    for (rel, v) in case["files"].as_object().unwrap() {
        let bytes = match (v.get("text"), v.get("hex")) {
            (Some(t), _) => t.as_str().unwrap().as_bytes().to_vec(),
            (None, Some(h)) => {
                let h = h.as_str().unwrap();
                (0..h.len()).step_by(2).map(|i| u8::from_str_radix(&h[i..i + 2], 16).unwrap()).collect()
            }
            _ => panic!("{rel}: neither text nor hex"),
        };
        let p = dir.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, bytes).unwrap();
    }
    let state = root.join("kiln/state");
    for (name, v) in case["state"].as_object().unwrap() {
        std::fs::create_dir_all(&state).unwrap();
        std::fs::write(state.join(name), serde_json::to_vec(&rooted(v, placeholder, &root.to_string_lossy())).unwrap()).unwrap();
    }
    dir
}

#[test]
fn every_fixture_tree_is_advertised_with_the_reference_hash() {
    let doc: Value = serde_json::from_slice(&std::fs::read(repo().join("schema/fixtures/engine-identity/cases.json")).unwrap()).unwrap();
    let placeholder = doc["root"].as_str().unwrap();
    let cases = doc["cases"].as_array().unwrap();
    assert!(cases.len() >= 30, "{} cases", cases.len());
    let mut failed = Vec::new();
    for c in cases {
        let name = c["name"].as_str().unwrap();
        let t = tmp(name);
        let dir = materialize(c, placeholder, &t.0);
        let kind = ModKind::parse(c["kind"].as_str().unwrap()).unwrap();
        let id = dir.file_name().unwrap().to_str().unwrap();
        let expect = c["expect"].as_str().unwrap();
        let listed = c.get("listed").and_then(Value::as_bool).unwrap_or(true);
        let required = modpack::required(&t.0).unwrap();
        match (listed, required.iter().find(|r| r.kind == Some(kind) && r.id == id)) {
            (true, Some(r)) if r.hash == expect => {}
            (true, Some(r)) => failed.push(format!("{name}: advertised {}, the reference says {expect}", r.hash)),
            (true, None) => {
                failed.push(format!("{name}: not advertised ({required:?})"));
                continue;
            }
            (false, Some(r)) => {
                failed.push(format!("{name}: advertised {} though it does not run", r.hash));
                continue;
            }
            (false, None) => continue,
        }
        // the launcher's modpack of that server carries what it advertises: packed as placed (no Library here), the
        // advertised hash is its hash or its identity, and every file a player needs is in it
        #[cfg(feature = "zip")]
        {
            let built = modpack::build(&t.0, &t.0.join("out"), "Isles", "1", &mut kiln_manage::task::NoProgress).unwrap();
            let verified = modpack::verify(&built.file, Some(&built.sha256)).unwrap();
            if let Err(e) = modpack::check_against(&verified, &required) {
                failed.push(format!("{name}: its modpack: {}", e.message));
            }
            let m = verified.mods.iter().find(|m| m.kind == kind && m.id == id).unwrap();
            if m.hash != expect && m.identity.as_deref() != Some(expect) {
                failed.push(format!("{name}: the modpack has {} / {:?}, the server advertises {expect}", m.hash, m.identity));
            }
            // the logs the mod wrote while it ran are not packed (a log the package itself ships is one of its files)
            let names = zip_names(&built.file);
            let run_log = |n: &String| ["ut_native.log", "run.log", "crash.log"].iter().any(|l| n.to_ascii_lowercase().ends_with(l));
            if !names.iter().any(|n| n.ends_with("/mod.json")) || names.iter().any(run_log) {
                failed.push(format!("{name}: packed {names:?}"));
            }
        }
    }
    assert!(failed.is_empty(), "{failed:#?}");
}

#[cfg(feature = "zip")]
fn zip_names(file: &Path) -> Vec<String> {
    let mut a = zip::ZipArchive::new(std::fs::File::open(file).unwrap()).unwrap();
    (0..a.len()).map(|i| a.by_index(i).unwrap().name().unwrap().to_string()).collect()
}

/// The Python reference gives the hashes the fixture holds (skipped where there is no Python 3). Windows' `python3`
/// may be the Store's placeholder, which only says to install Python and fails: the interpreter is the first of
/// `python3` and `python` that answers `--version`.
#[test]
fn the_reference_agrees_with_the_fixture() {
    let script = repo().join("schema/fixtures/engine-identity/engine_identity.py");
    let works = |p: &&&str| std::process::Command::new(p).arg("--version").output().is_ok_and(|o| o.status.success() && String::from_utf8_lossy(&o.stdout).starts_with("Python 3"));
    let Some(python) = ["python3", "python"].iter().find(works) else { return };
    let Ok(out) = std::process::Command::new(python).arg("-I").arg(&script).arg("--check").output() else { return };
    assert!(out.status.success(), "{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
}
