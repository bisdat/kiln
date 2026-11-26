//! ShroudForge's failed startup pass on the launcher's engine list (`engine_mods::status`, the `engine_status`
//! command), whatever other loaders are in the game folder. Fixtures are made up.

use kiln_manage::engine_mods;
use kiln_manage::pe::build_test_dll;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn game(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-sf-startup-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// 2026-11-10T11:00:00Z.
const T0: u64 = 1_794_308_400;

/// Kiln with its own ShroudForge, a profile that lists one ShroudForge mod (so Kiln hosts ShroudForge), and the start
/// of T0 in which the hosted pass failed.
fn hosted_failure(g: &Path) {
    put(g, "enshrouded.exe", b"MZ");
    put(g, "kiln/plugins/kiln-shroudforge.dll", b"adapter");
    put(g, "kiln/plugins/shroudforge/shroudforge-runtime.dll", b"runtime");
    put(g, "kiln/plugins/shroudforge/version.json", br#"{"version":"1.7.9","build":"kiln.63b8862","host":"kiln"}"#);
    put(g, "mods/bone-caller/mod.json", br#"{"id":"bone-caller","name":"Bone Caller","version":"1.0.0","capabilities":["patch","runtime"]}"#);
    put(g, "mods/bone-caller/extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"targets":["client"],"settings":{}}"#);
    put(g, "mods/bone-caller/src/mod.lua", b"return {}");
    put(g, "kiln/profile.json", serde_json::to_vec(&serde_json::json!({
        "format": 1, "id": "my-mods", "name": "My mods", "generation": 3, "dropins": false,
        "configDir": r"Z:\var\home\frostling\.local\share\kiln\profiles\my-mods\config",
        "mods": {"shroudforge": [{"id": "bone-caller", "version": "1.0.0", "path": r"Z:\var\home\frostling\Enshrouded\mods\bone-caller"}]},
    })).unwrap().as_slice());
    put(g, "kiln/state/boot.json", serde_json::to_vec(&serde_json::json!({
        "format": 1, "kiln": "0.2.0-dev", "pid": 412, "started": "2026-11-10T11:00:00Z", "written": "2026-11-10T11:00:02Z", "side": "client",
        "hooks": true, "profile": {"mode": "active", "id": "my-mods", "generation": 3}, "chain": [], "plugins": [], "errors": [],
    })).unwrap().as_slice());
    put(g, "kiln/state/shroudforge.json", format!(r#"{{"format":1,"mode":"hosted","source":"bundled","pid":412,"side":"client","updated":{},
        "bundled":{{"version":"1.7.9","build":"kiln.63b8862"}},"startupPrepared":true}}"#, T0 + 5).as_bytes());
    put(g, "shroudforge/state.json", format!(r#"{{"schemaVersion":1,"assets":{{"status":"failed","phase":"startup","target":"enshrouded",
        "completedAt":{},"error":"pregame phase failed: could not create a new enshrouded.kfc working baseline"}}}}"#, T0 + 3).as_bytes());
}

fn rows(g: &Path) -> Vec<serde_json::Value> {
    serde_json::to_value(engine_mods::status(g)).unwrap().as_array().unwrap().clone()
}

fn row<'a>(rows: &'a [serde_json::Value], kind: &str) -> &'a serde_json::Value {
    rows.iter().find(|r| r["kind"] == kind).unwrap_or_else(|| panic!("no {kind} row"))
}

/// Review of 6e231b37, finding 4. EML's own `dbghelp.dll` is still in the game folder (under Proton it does not load
/// without an override) while Kiln hosts ShroudForge. The failure used to ride on the own-loader note of a ShroudForge
/// loader: here the only note is EML's, of EML's proxy, so no line of the engine list said that the pass failed. It
/// is on ShroudForge's own line now, whatever notes there are.
#[test]
fn a_hosted_failure_is_on_shroudforges_line_beside_emls_proxy() {
    let t = game("eml-proxy");
    let g = &t.0;
    hosted_failure(g);
    put(g, "dbghelp.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
    let rows = rows(g);
    let said: Vec<&str> = rows.iter().filter(|r| r.to_string().contains("could not write its mods' changes")).filter_map(|r| r["kind"].as_str()).collect();
    assert!(said.contains(&"shroudforge"), "ShroudForge's line says it; the lines that do: {said:?}");
    let sf = row(&rows, "shroudforge");
    let text = sf["startupFailed"]["text"].as_str().unwrap_or_else(|| panic!("no startupFailed.text on ShroudForge's line: {sf}"));
    assert!(text.starts_with("ShroudForge 1.7.9, hosted by Kiln, could not write its mods' changes into the game's data files"), "{text}");
    assert!(text.contains("(bone-caller)"), "{text}");
    // the EML line points there while ShroudForge is what runs the EML mods
    let eml = row(&rows, "eml");
    assert_eq!(eml["startupFailed"]["ownLoader"], false, "the EML line carries it too: {eml}");
    // no other engine's line has it
    for r in rows.iter().filter(|r| r["kind"] != "shroudforge" && r["kind"] != "eml") {
        assert!(r.get("startupFailed").is_none_or(|v| v.is_null()), "{r}");
    }
}

/// The same without any other loader, and nothing once the pass applied.
#[test]
fn shroudforges_line_has_the_failure_only_while_the_record_says_failed() {
    let t = game("plain");
    let g = &t.0;
    hosted_failure(g);
    assert!(row(&rows(g), "shroudforge")["startupFailed"]["text"].is_string());
    put(g, "shroudforge/state.json", format!(r#"{{"schemaVersion":1,"assets":{{"status":"applied","phase":"startup","target":"enshrouded","completedAt":{}}}}}"#, T0 + 3).as_bytes());
    let rows = rows(g);
    assert!(rows.iter().all(|r| r.get("startupFailed").is_none_or(|v| v.is_null())), "{rows:?}");
}

/// kiln-win1 (0.2.0-dev.31.11, 2026-11-25): the pass failed, and so did the recovery after it (0.2.0-dev.31.13's
/// ShroudForge records why in `recoveryError`), so ShroudForge's preparation returned false and Kiln's adapter wrote
/// `failed` (with `startupPrepared` false since 31.13). The engine list said nothing of it: it read the record only
/// while the adapter said `hosted`. Now ShroudForge's line says that no ShroudForge mod ran, why, and how to get a clean
/// copy of the game data back; and the line never claims the game data was put back.
#[test]
fn a_failed_recovery_says_no_shroudforge_mod_ran_and_how_to_get_a_clean_copy() {
    let t = game("recovery-failed");
    let g = &t.0;
    hosted_failure(g);
    put(g, "kiln/state/shroudforge.json", format!(r#"{{"format":1,"mode":"failed","source":"bundled","pid":412,"side":"client","updated":{},
        "bundled":{{"version":"1.7.9","build":"kiln.ba755a7"}},"startupPrepared":false,"runtime":"stopped",
        "reason":"ShroudForge's startup preparation failed (startup asset mods or its DLL Manager)"}}"#, T0 + 5).as_bytes());
    let refusal = "pregame phase failed: ShroudForge: the clean copy of the game data (enshrouded.kfc.bak, enshrouded.kfc_resources.bak) \
        cannot be used: it is of game version 1273043|^/game38/branches/release_1.0|2026-10-20T14:55:29.904125Z, the game's files are of \
        1287369|^/game38/branches/release_1.0|2026-11-04T11:17:54.051725Z; and the game's current files are not the files Steam installed \
        for build 26430625 (enshrouded.kfc has other bytes than Steam's manifest lists (SHA-1)), so no new copy can be made from them. \
        Verify the game files in Steam (or update the server files with SteamCMD's validate), then start again";
    put(g, "shroudforge/state.json", serde_json::to_vec(&serde_json::json!({"schemaVersion": 1, "assets": {
        "status": "failed", "phase": "startup", "target": "enshrouded", "completedAt": T0 + 3,
        "error": "pregame phase failed: debugConsole.toggleKey: Hotkey must be an SDL binding string",
        "recoveryError": refusal,
    }})).unwrap().as_slice());
    let sf = row(&rows(g), "shroudforge").clone();
    let text = sf["startupFailed"]["text"].as_str().unwrap_or_else(|| panic!("no startupFailed.text on ShroudForge's line: {sf}"));
    assert!(text.contains("debugConsole.toggleKey"), "{text}");
    assert!(text.contains("It could not put the game data back either"), "{text}");
    assert!(text.contains("no ShroudForge mod ran in that start"), "{text}");
    assert!(!text.contains("It put the game data back and the game started without them"), "{text}");
    assert!(text.contains("Verify integrity of game files"), "the way out survives the cut of ShroudForge's long words: {text}");
    assert_eq!(sf["startupFailed"]["recoveryError"].as_str().map(|r| r.starts_with("pregame phase failed: ShroudForge: the clean copy")), Some(true), "{sf}");

    // The same refusal ended the pass and its recovery: said once.
    put(g, "shroudforge/state.json", serde_json::to_vec(&serde_json::json!({"schemaVersion": 1, "assets": {
        "status": "failed", "phase": "startup", "target": "enshrouded", "completedAt": T0 + 3, "error": refusal, "recoveryError": refusal,
    }})).unwrap().as_slice());
    let sf = row(&rows(g), "shroudforge").clone();
    let text = sf["startupFailed"]["text"].as_str().unwrap();
    assert!(text.contains("It changed no game file, and no ShroudForge mod ran in that start"), "{text}");
    assert!(!text.contains("could not put the game data back either"), "{text}");

    // An adapter that failed before it prepared anything (no startupPrepared): the record is not this start's word.
    put(g, "kiln/state/shroudforge.json", format!(r#"{{"format":1,"mode":"failed","source":"bundled","pid":412,"side":"client","updated":{},
        "reason":"EML writer is already active or incompatible; refusing a second KFC writer"}}"#, T0 + 5).as_bytes());
    assert!(row(&rows(g), "shroudforge").get("startupFailed").is_none_or(|v| v.is_null()));
}

/// Review (0.2.0-dev.31.13): after a game update the pass failed and its recovery kept the game's own files (they are a
/// clean copy the pass did not change; ShroudForge's record says `recovery: kept`). The line said "It put the game data
/// back", which it did not: it changed no game file. A pair that went back (`recovery: restored`, or a record without
/// the word, from before 31.13) is still said to be put back.
#[test]
fn a_recovery_that_kept_the_game_files_does_not_say_the_game_data_was_put_back() {
    let t = game("recovery-kept");
    let g = &t.0;
    hosted_failure(g);
    let record = |recovery: Option<&str>| {
        let mut assets = serde_json::json!({"status": "failed", "phase": "startup", "target": "enshrouded", "completedAt": T0 + 3,
            "error": "pregame phase failed: debugConsole.toggleKey: Hotkey must be an SDL binding string"});
        if let Some(recovery) = recovery {
            assets["recovery"] = recovery.into();
        }
        serde_json::to_vec(&serde_json::json!({"schemaVersion": 1, "assets": assets})).unwrap()
    };
    put(g, "shroudforge/state.json", &record(Some("kept")));
    let sf = row(&rows(g), "shroudforge").clone();
    let text = sf["startupFailed"]["text"].as_str().unwrap_or_else(|| panic!("no startupFailed.text on ShroudForge's line: {sf}"));
    assert!(!text.contains("put the game data back"), "{text}");
    assert!(text.contains("It changed no game file, and the game started without them"), "{text}");
    assert!(text.contains("(bone-caller)"), "the mods of the pass are still named: {text}");
    for recovery in [Some("restored"), None] {
        put(g, "shroudforge/state.json", &record(recovery));
        let sf = row(&rows(g), "shroudforge").clone();
        let text = sf["startupFailed"]["text"].as_str().unwrap();
        assert!(text.contains("It put the game data back and the game started without them"), "{recovery:?}: {text}");
    }
}
