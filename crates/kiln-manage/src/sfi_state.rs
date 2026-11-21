// SPDX-License-Identifier: MIT
//!
//! The ShroudForge row of the launcher's engine status while a game's ShroudForge-format mods go to Kiln's interpreter
//! ([`crate::sfi_routing::Route::Interpreter`]): read from kiln-sfi's own `kiln/state/sfi.json` (format 1, writer
//! kiln-sfi), never from ShroudForge's `kiln/state/shroudforge.json` or anything below `<game>/shroudforge`, which
//! belong to a hosted ShroudForge and are stale in kiln mode. A file that is not kiln-sfi's format 1 is an error, never a
//! run that did not happen; the row says the interpreter's phase, its reason, and one line per package.
//!
//! A record that is not this start's is never shown as running or done: its phase becomes `unknown`, with the
//! reason as `staleCode` and text, and no package lines. That is a record whose `launch.launchId` is not the one of the
//! core's newest `kiln/state/launch.json` (an older start's: this one's kiln-sfi has not written yet, or could not), and
//! a `starting` or `running` record while the game is not running (that start ended without its final write, or
//! crashed); on Windows also one whose pid is not the running game's (a reused pid is another start). The start before
//! the record's own is said apart (`previous`: `incomplete` or `suspect`), never as a fact of this start.
use serde_json::{json, Value};
use std::path::{Path, PathBuf};

/// `kiln/state/sfi.json` of a game folder.
pub fn file(game: &Path) -> PathBuf {
    game.join("kiln").join("state").join("sfi.json")
}

/// Larger than any record kiln-sfi writes (4096 packages at most, each bounded).
const MAX_BYTES: u64 = 32 * 1024 * 1024;

/// The core's newest launch record (kiln/state/launch.json); its `launchId` names the newest start.
const LAUNCH_JSON: &str = "kiln/state/launch.json";

/// The game as it is now, to tell this start's record from an older one's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Live {
    /// The game (or its server) runs from this folder.
    pub running: bool,
    /// The running game's pid where it is the kind of pid kiln-sfi writes (native Windows; under Wine and Proton the
    /// record holds the Windows pid, which this side cannot see): None, nothing is compared.
    pub pid: Option<u32>,
}

/// [`Live`] of a game folder, from one look at the process table.
pub fn live(game: &Path) -> Live {
    let found = ["enshrouded.exe", "enshrouded_server.exe"]
        .into_iter()
        .find_map(|exe| crate::proc::find_exe(game, exe).ok().flatten());
    Live { running: found.is_some(), pid: if cfg!(windows) { found.map(|p| p.pid) } else { None } }
}

/// The row's state (None: the interpreter has not written a record in this game yet) or why the file cannot be used.
pub fn engine_state(game: &Path) -> (Option<Value>, Option<String>) {
    engine_state_with(game, &|| live(game))
}

/// [`engine_state`] with the game's [`Live`] state from `live` (asked only for a `starting` or `running` record).
pub fn engine_state_with(game: &Path, live: &dyn Fn() -> Live) -> (Option<Value>, Option<String>) {
    let f = file(game);
    let bytes = match std::fs::metadata(&f) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return (None, None),
        Err(e) => return (None, Some(format!("{}: {e}", f.display()))),
        Ok(m) if !m.is_file() => return (None, Some(format!("{}: not a file", f.display()))),
        Ok(m) if m.len() > MAX_BYTES => return (None, Some(format!("{}: larger than {MAX_BYTES} bytes", f.display()))),
        Ok(_) => match std::fs::File::open(&f).and_then(|r| read_capped(r, MAX_BYTES)) {
            Ok(Some(b)) => b,
            Ok(None) => return (None, Some(format!("{}: larger than {MAX_BYTES} bytes", f.display()))),
            Err(e) => return (None, Some(format!("{}: {e}", f.display()))),
        },
    };
    match serde_json::from_slice::<Value>(&bytes) {
        Ok(v) => match row(&v) {
            Some(r) => (Some(stale(game, &v, r, live)), None),
            None => (None, Some(format!("{}: not kiln-sfi's state (format 1, writer kiln-sfi)", f.display()))),
        },
        Err(e) => (None, Some(format!("{}: {e}", f.display()))),
    }
}

/// At most `max` bytes of `r`, or None when it holds more: the bound holds on what is read, so a file that grows after
/// its size was checked cannot make the read larger.
fn read_capped(r: impl std::io::Read, max: u64) -> std::io::Result<Option<Vec<u8>>> {
    use std::io::Read;
    let mut bytes = Vec::new();
    r.take(max + 1).read_to_end(&mut bytes)?;
    Ok((bytes.len() as u64 <= max).then_some(bytes))
}

/// The newest start's launch id from the core's launch.json, when it can be read.
fn newest_launch(game: &Path) -> Option<String> {
    let f = game.join(LAUNCH_JSON);
    let m = std::fs::symlink_metadata(&f).ok()?;
    if !m.is_file() || m.len() > 1024 * 1024 {
        return None;
    }
    let v: Value = serde_json::from_slice(&std::fs::read(&f).ok()?).ok()?;
    v["launchId"].as_str().map(str::to_owned)
}

/// `row` as it is, or as `unknown` when the record is not this start's (module docs).
fn stale(game: &Path, v: &Value, row: Value, live: &dyn Fn() -> Live) -> Value {
    let phase = v["phase"].as_str().unwrap_or("unknown");
    let own = v["launch"]["launchId"].as_str();
    let why = match (own, newest_launch(game)) {
        (Some(own), Some(newest)) if own != newest => Some(("older-start", format!(
            "kiln-sfi's record is of an earlier start ({own}), not of the newest one ({newest}): what the interpreter did in the newest start is not known"))),
        _ if matches!(phase, "starting" | "running") => {
            let now = live();
            let pid = v["launch"]["pid"].as_u64();
            if !now.running {
                Some(("not-running", format!(
                    "kiln-sfi's record says {phase}, but the game is not running: that start ended without its last record (it may have crashed)")))
            } else if let (Some(running), Some(pid)) = (now.pid, pid) {
                (u64::from(running) != pid).then(|| ("other-process", format!(
                    "kiln-sfi's record is of process {pid}, but the running game is process {running}: it is not this start's")))
            } else {
                None
            }
        }
        _ => None,
    };
    let Some((code, text)) = why else { return row };
    let mut out = json!({
        "mode": row["mode"],
        "status": "unknown",
        "recordPhase": phase,
        "staleCode": code,
        "reason": text,
        "backend": row["backend"],
        "launchId": row["launchId"],
        "pid": row["pid"],
        "updatedMs": row["updatedMs"],
        "mods": [],
    });
    if let Some(p) = row.get("previous") {
        out["previous"] = p.clone();
    }
    out
}

/// The engine row of one sfi.json record, or None when it is not kiln-sfi's format 1.
pub fn row(v: &Value) -> Option<Value> {
    if v["format"].as_u64() != Some(1) || v["writer"].as_str() != Some("kiln-sfi") {
        return None;
    }
    let phase = v["phase"].as_str().unwrap_or("unknown");
    let reason = v["reason"]["text"].as_str();
    let mods: Vec<Value> = v["packages"].as_array().into_iter().flatten().filter_map(package).collect();
    let mut row = json!({
        "mode": "kiln interpreter",
        "status": phase,
        "backend": v["launch"]["backend"]["selected"],
        "launchId": v["launch"]["launchId"],
        "pid": v["launch"]["pid"],
        "updatedMs": v["updatedMs"],
        "mods": mods,
    });
    if let Some(r) = reason {
        row["reason"] = json!(r);
    }
    // the start before this record's: said apart, never as this start's fact
    if let Some(d @ ("incomplete" | "suspect")) = v["previousStart"]["disposition"].as_str() {
        row["previous"] = json!(d);
    }
    Some(row)
}

/// One package's line: what runs now, else why not.
fn package(p: &Value) -> Option<Value> {
    let id = p["id"].as_str()?;
    let skip = p["skip"]["text"].as_str();
    let state = if p["runtimeActive"].as_bool() == Some(true) {
        "running"
    } else if p["intent"].as_str().is_some_and(|i| i != "selected") {
        "off"
    } else if skip.is_some() {
        "skipped"
    } else if matches!(p["data"].as_str(), Some("committed" | "cached-verified")) {
        "data applied"
    } else {
        p["lua"].as_str().unwrap_or("unknown")
    };
    let mut line = json!({ "id": id, "version": p["version"], "state": state });
    if let Some(s) = skip {
        line["reason"] = json!(s);
    } else if let Some(w) = p["warning"]["text"].as_str() {
        line["reason"] = json!(w);
    }
    Some(line)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("km-sfi-state-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("kiln/state")).unwrap();
        d
    }

    #[test]
    fn a_refused_start_names_its_reason_and_each_skipped_package() {
        let g = tmp("refused");
        std::fs::write(file(&g), br#"{"format":1,"writer":"kiln-sfi","phase":"refused",
            "reason":{"code":"startup-grant-missing","text":"kiln-eml's startup pass granted no archive generation"},
            "launch":{"launchId":"eca8","pid":388,"backend":{"selected":"kiln"}},"updatedMs":5,
            "packages":[{"id":"combat_tweaks","version":"0.7.2","intent":"selected","data":"excluded","lua":"excluded",
              "runtimeActive":false,"skip":{"code":"startup-pass-failed","text":"the startup pass failed"}},
              {"id":"sf-survival","version":"1.0.0","intent":"disabled","data":"excluded","lua":"excluded","runtimeActive":false}]}"#).unwrap();
        let (s, e) = engine_state(&g);
        assert!(e.is_none(), "{e:?}");
        let s = s.unwrap();
        assert_eq!(s["status"], "refused");
        assert_eq!(s["reason"], "kiln-eml's startup pass granted no archive generation");
        assert_eq!(s["mods"][0], json!({"id":"combat_tweaks","version":"0.7.2","state":"skipped","reason":"the startup pass failed"}));
        assert_eq!(s["mods"][1]["state"], "off");
    }

    #[test]
    fn a_running_start_says_running_per_package() {
        let g = tmp("running");
        std::fs::write(file(&g), br#"{"format":1,"writer":"kiln-sfi","phase":"running","launch":{"backend":{"selected":"kiln"}},
            "packages":[{"id":"skeleton-commander","version":"1.2.0","intent":"selected","data":"committed","lua":"active","runtimeActive":true},
              {"id":"sf-unlock-blueprints","version":"1.0.2","intent":"selected","data":"committed","lua":"excluded","runtimeActive":false}]}"#).unwrap();
        let s = engine_state_with(&g, &|| Live { running: true, pid: None }).0.unwrap();
        assert_eq!(s["status"], "running");
        assert!(s.get("reason").is_none());
        assert_eq!(s["mods"][0]["state"], "running");
        assert_eq!(s["mods"][1]["state"], "data applied");
    }

    const RUNNING: &[u8] = br#"{"format":1,"writer":"kiln-sfi","phase":"running","incomplete":true,
        "previousStart":{"disposition":"incomplete","launchId":"00000000000000a1","pid":7},
        "launch":{"launchId":"00000000000000b2","pid":4242,"backend":{"selected":"kiln"}},
        "packages":[{"id":"skeleton-commander","version":"1.2.0","intent":"selected","data":"committed","lua":"active","runtimeActive":true}]}"#;

    #[test]
    fn a_running_record_while_the_game_is_closed_is_unknown_never_running() {
        let g = tmp("closed");
        std::fs::write(file(&g), RUNNING).unwrap();
        let s = engine_state_with(&g, &|| Live { running: false, pid: None }).0.unwrap();
        assert_eq!(s["status"], "unknown");
        assert_eq!(s["recordPhase"], "running");
        assert_eq!(s["staleCode"], "not-running");
        assert_eq!(s["mods"], json!([]));
        // the start before it is said apart
        assert_eq!(s["previous"], "incomplete");
        // the same record while the game runs is this start's
        let s = engine_state_with(&g, &|| Live { running: true, pid: None }).0.unwrap();
        assert_eq!(s["status"], "running");
        assert_eq!(s["previous"], "incomplete");
        // a running game with another pid (native Windows: a reused pid is another start)
        let s = engine_state_with(&g, &|| Live { running: true, pid: Some(4243) }).0.unwrap();
        assert_eq!((s["status"].as_str(), s["staleCode"].as_str()), (Some("unknown"), Some("other-process")));
        assert_eq!(engine_state_with(&g, &|| Live { running: true, pid: Some(4242) }).0.unwrap()["status"], "running");
    }

    #[test]
    fn a_record_of_an_older_launch_is_unknown_and_the_current_one_is_read_as_it_is() {
        let g = tmp("older");
        let ended = br#"{"format":1,"writer":"kiln-sfi","phase":"ended","incomplete":false,
            "launch":{"launchId":"00000000000000b2","pid":4242,"backend":{"selected":"kiln"}},
            "packages":[{"id":"skeleton-commander","version":"1.2.0","intent":"selected","data":"committed","lua":"active","runtimeActive":false}]}"#;
        std::fs::write(file(&g), ended).unwrap();
        let never = || -> Live { panic!("an ended record needs no process look") };
        // no launch.json: the record stands
        assert_eq!(engine_state_with(&g, &never).0.unwrap()["status"], "ended");
        std::fs::write(g.join(LAUNCH_JSON), br#"{"launchId":"00000000000000b2"}"#).unwrap();
        assert_eq!(engine_state_with(&g, &never).0.unwrap()["status"], "ended");
        // a newer start whose kiln-sfi wrote nothing (yet)
        std::fs::write(g.join(LAUNCH_JSON), br#"{"launchId":"00000000000000c3"}"#).unwrap();
        let s = engine_state_with(&g, &never).0.unwrap();
        assert_eq!((s["status"].as_str(), s["staleCode"].as_str()), (Some("unknown"), Some("older-start")));
        assert!(s["reason"].as_str().unwrap().contains("00000000000000c3"));
        assert_eq!(s["mods"], json!([]));
        // also for a running record, whatever the process table says
        std::fs::write(file(&g), RUNNING).unwrap();
        assert_eq!(engine_state_with(&g, &|| Live { running: true, pid: None }).0.unwrap()["staleCode"], "older-start");
    }

    #[test]
    fn no_record_is_none_and_a_foreign_or_broken_file_is_an_error() {
        let g = tmp("missing");
        assert_eq!(engine_state(&g), (None, None));
        std::fs::write(file(&g), br#"{"format":2,"writer":"kiln-sfi"}"#).unwrap();
        assert!(engine_state(&g).1.unwrap().contains("not kiln-sfi's state"));
        std::fs::write(file(&g), br#"{"format":1,"writer":"someone-else"}"#).unwrap();
        assert!(engine_state(&g).1.is_some());
        std::fs::write(file(&g), b"{ torn").unwrap();
        assert!(engine_state(&g).1.is_some());
    }

    #[test]
    fn the_read_is_bounded_by_what_it_reads_not_by_the_size_seen_before() {
        assert_eq!(read_capped(&b"12345"[..], 5).unwrap().as_deref(), Some(&b"12345"[..]));
        assert_eq!(read_capped(&b"123456"[..], 5).unwrap(), None);
        assert_eq!(read_capped(&b""[..], 5).unwrap().as_deref(), Some(&b""[..]));
        // a source that never ends (a file still growing) stops one byte past the bound
        let mut endless = std::io::repeat(b'x');
        assert_eq!(read_capped(&mut endless, 1024).unwrap(), None);
        let mut counted = std::io::Read::take(std::io::repeat(b'x'), u64::MAX);
        assert_eq!(read_capped(&mut counted, 1024).unwrap(), None);
        assert_eq!(counted.limit(), u64::MAX - 1025);
    }
}
