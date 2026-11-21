// SPDX-License-Identifier: MIT
//!
//! On a game folder whose kiln.json selects Kiln's interpreter:
//! - the launcher's ShroudForge row (`engine_mods::status`) is kiln-sfi's sfi.json, and a record that is not this start's
//!   (the game closed while it says running, an older launch than launch.json's) is `unknown` with its reason, never
//!   running or done; a broken or foreign file is an error, never a start that did not happen;
//! - "Restore the game's data" names the BASE it goes back to (`restore_data::base_origin`) by the exact label kiln-eml's
//!   receipt records, only while that origin is keyed to the backups in the folder; anything else is `unrecorded` or
//!   `unknown`, never vanilla; other routes and a game with no receipt have none.
//!
//! Synthetic files only; no game, Steam or Wine runs.

use kiln_manage::engine_mods;
use kiln_manage::manifest::ModKind;
use kiln_manage::restore_data;
use serde_json::{Value, json};
use std::fs;
use std::path::{Path, PathBuf};

const FIRST: &str = "unclassified: game files as found when Kiln first managed this install";

struct Game(PathBuf);
impl Drop for Game {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn game(name: &str, kiln_mode: bool) -> Game {
    let g = std::env::temp_dir().join(format!("km-sfi-state-it-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&g);
    fs::create_dir_all(g.join("kiln/state")).unwrap();
    fs::create_dir_all(g.join("kiln/eml/ownership")).unwrap();
    let backend = if kiln_mode { "kiln" } else { "hosted" };
    fs::write(g.join("kiln/kiln.json"), json!({"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": backend}}).to_string()).unwrap();
    Game(g)
}

fn sf_row(g: &Path) -> engine_mods::EngineStatus {
    engine_mods::status(g).into_iter().find(|s| s.kind == ModKind::ShroudForge).expect("a ShroudForge row")
}

fn record(phase: &str, launch: &str) -> Value {
    json!({"format": 1, "writer": "kiln-sfi", "phase": phase, "incomplete": phase == "running",
        "previousStart": {"disposition": "clean", "launchId": null, "pid": null},
        "launch": {"launchId": launch, "pid": 900, "backend": {"selected": "kiln"}},
        "packages": [{"id": "skeleton-commander", "version": "1.2.0", "intent": "selected", "data": "committed",
            "lua": "active", "runtimeActive": phase == "running"}]})
}

#[test]
fn the_kiln_mode_row_is_this_starts_record_or_unknown() {
    let g = game("row", true);
    let g = &g.0;
    let sfi = g.join("kiln/state/sfi.json");
    fs::write(g.join("kiln/state/launch.json"), r#"{"launchId":"00000000000000b2"}"#).unwrap();
    // this start's record, ended
    fs::write(&sfi, record("ended", "00000000000000b2").to_string()).unwrap();
    let row = sf_row(g);
    assert_eq!(row.plugin, "kiln-sfi");
    let state = row.state.expect("state");
    assert_eq!(state["status"], "ended");
    assert_eq!(state["mods"][0]["id"], "skeleton-commander");
    // a running record while no game runs from this folder (the test's temp folder): that start died
    fs::write(&sfi, record("running", "00000000000000b2").to_string()).unwrap();
    let state = sf_row(g).state.expect("state");
    assert_eq!((state["status"].as_str(), state["staleCode"].as_str()), (Some("unknown"), Some("not-running")));
    assert_eq!(state["mods"], json!([]));
    // a newer launch than the record's
    fs::write(&sfi, record("ended", "00000000000000a1").to_string()).unwrap();
    let state = sf_row(g).state.expect("state");
    assert_eq!((state["status"].as_str(), state["staleCode"].as_str()), (Some("unknown"), Some("older-start")));
    // broken and foreign files: an error, no state
    for bad in ["{ torn".to_string(), json!({"format": 1, "writer": "kiln-shroudforge"}).to_string(), json!({"format": 2, "writer": "kiln-sfi"}).to_string()] {
        fs::write(&sfi, bad).unwrap();
        let row = sf_row(g);
        assert!(row.state.is_none() && row.error.is_some(), "{:?}", row.error);
    }
}

/// A receipt whose baseOrigin is keyed to the backups written here.
fn write_base(g: &Path, token: &str, label: &str) {
    let bak = b"backup header".as_slice();
    let res = b"backup resources".as_slice();
    fs::write(g.join("enshrouded.kfc.bak"), bak).unwrap();
    fs::write(g.join("enshrouded.kfc_resources.bak"), res).unwrap();
    let d = |b: &[u8]| json!({"size": b.len(), "sha256": kiln_manage::hash::sha256_hex(b)});
    let receipt = json!({"format": 1, "stem": "enshrouded", "files": {}, "backups": {}, "baseOrigin": {"token": token, "label": label,
        "baseline": {"enshrouded.kfc.bak": d(bak), "enshrouded.kfc_resources.bak": d(res), "enshrouded.kfc.bak.integrity": null}}});
    fs::write(g.join("kiln/eml/ownership/enshrouded.json"), receipt.to_string()).unwrap();
}

#[test]
fn the_restore_base_is_the_receipts_exact_label_keyed_to_the_backups() {
    let g = game("base", true);
    let g = &g.0;
    // nothing Kiln wrote: no base to go back to
    assert_eq!(restore_data::base_origin(g).unwrap(), None);
    write_base(g, "first-managed-unclassified", FIRST);
    let b = restore_data::base_origin(g).unwrap().expect("a base");
    assert_eq!((b.state.as_str(), b.token.as_deref(), b.label.as_deref()), ("recorded", Some("first-managed-unclassified"), Some(FIRST)));
    assert_eq!(b.baseline_sha256.as_deref(), Some(kiln_manage::hash::sha256_hex(b"backup header").as_str()));
    assert!(!b.label.unwrap().contains("vanilla"));
    // the backup changed since: the origin is not this baseline's
    fs::write(g.join("enshrouded.kfc_resources.bak"), b"backup resourcez").unwrap();
    let b = restore_data::base_origin(g).unwrap().expect("a base");
    assert_eq!((b.state.as_str(), b.token.as_deref()), ("unrecorded", Some("unrecorded")));
    // a seal the receipt does not list: another baseline too
    write_base(g, "first-managed-unclassified", FIRST);
    fs::write(g.join("enshrouded.kfc.bak.integrity"), b"seal").unwrap();
    assert_eq!(restore_data::base_origin(g).unwrap().unwrap().state, "unrecorded");
    fs::remove_file(g.join("enshrouded.kfc.bak.integrity")).unwrap();
    // never upgraded: a "vanilla" token, a token with the other token's label
    write_base(g, "vanilla", "vanilla");
    assert_eq!(restore_data::base_origin(g).unwrap().unwrap().state, "unrecorded");
    write_base(g, "rebaselined-unclassified", FIRST);
    assert_eq!(restore_data::base_origin(g).unwrap().unwrap().state, "unrecorded");
    // the backups gone: Kiln cannot tell what it would put back
    write_base(g, "first-managed-unclassified", FIRST);
    fs::remove_file(g.join("enshrouded.kfc_resources.bak")).unwrap();
    let b = restore_data::base_origin(g).unwrap().unwrap();
    assert_eq!((b.state.as_str(), b.label.as_deref()), ("unknown", None));
    // a torn receipt
    fs::write(g.join("kiln/eml/ownership/enshrouded.json"), b"{ torn").unwrap();
    assert_eq!(restore_data::base_origin(g).unwrap().unwrap().state, "unknown");
}

#[test]
fn the_restore_base_is_kiln_modes_only() {
    let g = game("hosted", false);
    write_base(&g.0, "first-managed-unclassified", FIRST);
    assert_eq!(restore_data::base_origin(&g.0).unwrap(), None);
}
