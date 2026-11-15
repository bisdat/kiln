//! Read-only, bounded evidence of a deliberately stopped start. Never infer a stop from an exit status alone.
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, io::Read, path::Path};

pub const LIMIT: u64 = 256 * 1024;
/// The codes Kiln's deliberate stops end the process with (Windows; a Linux parent under Wine sees the low byte):
/// kiln.boot's required boot mod, kiln.knowledge's roster, kiln.knowledge's whole build of the mods' NPCs and quests.
pub const BOOT_MOD_EXIT_CODE: u32 = 19201;
pub const ROSTER_EXIT_CODE: u32 = 19202;
pub const CONTENT_EXIT_CODE: u32 = 19203;
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StopKind { Roster, BootMod, Content }

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StoppedStart {
    pub kind: StopKind,
    pub reason: String,
    pub mods: Vec<String>,
    pub names: Vec<String>,
    pub advice: String,
    pub utc: String,
    pub exit_code: u32,
    /// Kiln's Windows pid (also under Proton).
    pub pid: u32,
}

/// Identity retained while the process runs. Proton's host pid is not Kiln's Windows pid: there the process start
/// time is mandatory, and a runtime pid observed in current evidence pins subsequent reads to that same run.
#[derive(Debug, Clone)]
pub struct StartIdentity {
    pub pid: Option<u32>,
    pub began_ms: i64,
    pub ended_ms: Option<i64>,
}

fn json(path: &Path, start: Option<&StartIdentity>) -> Option<serde_json::Value> {
    let f = match std::fs::File::open(path) { Ok(f) => f, Err(e) if e.kind() == std::io::ErrorKind::NotFound => return None,
        Err(e) => { eprintln!("stopped start: {}: {e}", path.display()); return None; } };
    if start.is_some_and(|start| f.metadata().and_then(|m| m.modified()).ok().is_none_or(|t| crate::start_progress::unix_ms(t) < start.began_ms)) {
        eprintln!("stopped start: ignoring file predating process {}", path.display()); return None;
    }
    let result = (|| {
        let mut bytes = Vec::new();
        f.take(LIMIT + 1).read_to_end(&mut bytes).ok()?;
        if bytes.len() as u64 > LIMIT { return None; }
        let v: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
        (v["format"].as_u64() == Some(1)).then_some(v)
    })();
    if result.is_none() { eprintln!("stopped start: ignoring invalid, oversized or unknown format {}", path.display()); }
    result
}

fn belongs(v: &serde_json::Value, time: &str, start: &StartIdentity) -> Option<(u32, String)> {
    let pid = u32::try_from(v["pid"].as_u64()?).ok()?;
    let utc = v[time].as_str()?;
    let ms = crate::start_progress::parse_ms(utc)?;
    // Runtime timestamps have whole-second precision. No broad grace window that could admit an earlier start.
    if start.pid.is_some_and(|p| p != pid) || ms < start.began_ms.div_euclid(1000) * 1000
        || ms > start.ended_ms.unwrap_or_else(|| crate::start_progress::unix_ms(std::time::SystemTime::now())) { return None; }
    Some((pid, utc.into()))
}

/// The launch command's boot summary uses the same bound and ignores malformed/unknown files rather than
/// rejecting the whole query (which would hide a valid roster notice alongside a garbled boot file).
pub fn boot_state(game: &Path) -> Option<crate::state::BootJson> {
    let v = json(&game.join("kiln/state/boot.json"), None)?;
    match serde_json::from_value(v) {
        Ok(boot) => Some(boot),
        Err(e) => { eprintln!("stopped start: ignoring invalid boot.json: {e}"); None }
    }
}

/// A game can stop between process-list polls. Fresh failed progress carries its runtime process identity;
/// require its start time after our request, not just a recently touched refusal from an earlier run.
pub fn failed_identity(game: &Path, asked_ms: i64, now_ms: i64) -> Option<StartIdentity> {
    let request = StartIdentity { pid: None, began_ms: asked_ms, ended_ms: Some(now_ms) };
    let v = json(&game.join("kiln/state/progress.json"), Some(&request))?;
    if v["state"] != "failed" { return None; }
    let began_ms = crate::start_progress::parse_ms(v["started"].as_str()?)?;
    if began_ms < asked_ms || began_ms > now_ms { return None; }
    Some(StartIdentity { pid: Some(u32::try_from(v["pid"].as_u64()?).ok()?), began_ms, ended_ms: Some(now_ms) })
}

/// kiln.knowledge's own state file of a stop (`roster-refused.json`, `content-refused.json`: one shape, its code and
/// status fixed per file), written before kiln.boot's files; the only evidence beside an older core.
fn refused(state: &Path, file: &str, kind: StopKind, code: u32, start: &StartIdentity, names: &BTreeMap<String, String>) -> Option<StoppedStart> {
    let v = json(&state.join(file), Some(start))?;
    let parsed = (|| {
        let (pid, utc) = belongs(&v, "utc", start)?;
        if v["refused"] != true || v["exitCode"] != code || v["unixExitStatus"] != code & 0xff
            || v["restart"] != false || !matches!(v["side"].as_str(), Some("client" | "server"))
            || !v["bootStop"].is_boolean() { return None; }
        let mods: Vec<String> = serde_json::from_value(v["mods"].clone()).ok()?;
        let reason = v["why"].as_str()?.to_string();
        if reason.trim().is_empty() { return None; }
        Some(StoppedStart { kind, reason, names: resolve(&mods, names), mods, advice: v["advice"].as_str()?.into(), utc, exit_code: code, pid })
    })();
    if parsed.is_none() { eprintln!("stopped start: ignoring stale or invalid {file}"); }
    parsed
}

pub fn read(game: &Path, start: &StartIdentity, names: &BTreeMap<String, String>) -> Option<StoppedStart> {
    let state = game.join("kiln/state");
    // .last is deliberately not read: it describes the preceding start.
    if let Some(s) = refused(&state, "roster-refused.json", StopKind::Roster, ROSTER_EXIT_CODE, start, names) { return Some(s); }
    if let Some(s) = refused(&state, "content-refused.json", StopKind::Content, CONTENT_EXIT_CODE, start, names) { return Some(s); }
    let v = json(&state.join("boot.json"), Some(start))?;
    let (pid, utc) = belongs(&v, "written", start)?;
    let b = &v["boot"];
    let code = u32::try_from(b["exitCode"].as_u64()?).ok()?;
    if code == 0 { return None; }
    let stop = &b["stop"];
    let (reason, mods) = if stop.is_object() {
        if stop["code"].as_u64()? != u64::from(code) || !stop["by"].is_string() { return None; }
        (stop["reason"].as_str()?.to_string(), serde_json::from_value::<Vec<String>>(stop["mods"].clone()).ok()?)
    } else if code == BOOT_MOD_EXIT_CODE {
        let mods = b["owners"].as_array().into_iter().flatten().filter(|o| matches!(o["state"].as_str(), Some("excluded-callback" | "excluded-dependency" | "rejected-prepare" | "generation-aborted" | "activation-fault")))
            .filter_map(|o| o["id"].as_str().map(str::to_string)).collect();
        (b["reason"].as_str()?.to_string(), mods)
    } else { return None; };
    if reason.trim().is_empty() { return None; }
    let (kind, advice) = match code {
        ROSTER_EXIT_CODE => (StopKind::Roster, "Check these mods for updates or switch one off before starting again."),
        CONTENT_EXIT_CODE => (StopKind::Content, "Remove or update these mods, then start again. If it still stops, send kiln/logs/kiln.log (the support report has it) to the mods' authors or to Kiln."),
        _ => (StopKind::BootMod, "Check these mods for updates or switch one off before starting again."),
    };
    Some(StoppedStart { kind, reason, names: resolve(&mods, names), mods, advice: advice.into(), utc, exit_code: code, pid })
}
fn resolve(ids: &[String], names: &BTreeMap<String, String>) -> Vec<String> {
    ids.iter().map(|id| names.get(id).unwrap_or(id).clone()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> (crate::library::TempDir, StartIdentity, serde_json::Value) {
        let t = crate::library::TempDir::new(&std::env::temp_dir(), "stop-reader").unwrap();
        std::fs::create_dir_all(t.path().join("kiln/state")).unwrap();
        let v = serde_json::json!({"format":1,"refused":true,"why":"Roster full","mods":["a"],"side":"client","exitCode":19202,"unixExitStatus":2,"restart":false,"advice":"Switch one off","bootStop":true,"pid":42,"utc":"2026-11-15T12:00:01Z"});
        (t, StartIdentity { pid: Some(42), began_ms: crate::start_progress::parse_ms("2026-11-15T12:00:00Z").unwrap(), ended_ms: Some(crate::start_progress::parse_ms("2026-11-15T12:00:02Z").unwrap()) }, v)
    }
    #[test] fn roster_identity_names_and_no_writes() {
        let (t, mut s, v) = fixture(); let path = t.path().join("kiln/state/roster-refused.json");
        let bytes = serde_json::to_vec(&v).unwrap(); std::fs::write(&path, &bytes).unwrap();
        let r = read(t.path(), &s, &BTreeMap::from([("a".into(), "A mod".into())])).unwrap();
        assert_eq!(r.names, ["A mod"]); assert_eq!(r.advice, "Switch one off");
        s.pid = Some(43); assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        s.pid = Some(42); s.began_ms += 3000; assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        s.began_ms -= 3000; s.ended_ms = Some(s.began_ms); assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        assert_eq!(std::fs::read(path).unwrap(), bytes);
    }
    #[test] fn invalid_unknown_oversized_and_last_are_ignored() {
        let (t, s, mut v) = fixture(); let p = t.path().join("kiln/state/roster-refused.json");
        std::fs::write(p.with_file_name("roster-refused.last.json"), serde_json::to_vec(&v).unwrap()).unwrap();
        assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        for bytes in [b"{".to_vec(), vec![b' '; LIMIT as usize + 1], { v["format"] = 2.into(); serde_json::to_vec(&v).unwrap() }] {
            std::fs::write(&p, bytes).unwrap(); assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        }
    }
    #[test] fn stale_file_in_the_same_timestamp_second_is_ignored() {
        let (t, mut s, v) = fixture();
        let p = t.path().join("kiln/state/roster-refused.json");
        std::fs::write(&p, serde_json::to_vec(&v).unwrap()).unwrap();
        s.began_ms += 1500; // embedded timestamp rounds into this same second
        let old = std::time::UNIX_EPOCH + std::time::Duration::from_millis((s.began_ms - 100) as u64);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(old).unwrap();
        assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
    }
    #[test] fn fast_exit_needs_fresh_failed_progress_identity() {
        let (t, s, v) = fixture();
        let p = t.path().join("kiln/state/progress.json");
        let mut progress = serde_json::json!({"format":1,"pid":42,"started":"2026-11-15T12:00:00Z","state":"failed"});
        std::fs::write(&p, serde_json::to_vec(&progress).unwrap()).unwrap();
        let i = failed_identity(t.path(), s.began_ms, s.ended_ms.unwrap()).unwrap();
        std::fs::write(p.with_file_name("roster-refused.json"), serde_json::to_vec(&v).unwrap()).unwrap();
        assert!(read(t.path(), &i, &BTreeMap::new()).is_some());
        assert!(failed_identity(t.path(), s.began_ms + 1, s.ended_ms.unwrap()).is_none());
        progress["state"] = "ready".into();
        std::fs::write(&p, serde_json::to_vec(&progress).unwrap()).unwrap();
        assert!(failed_identity(t.path(), s.began_ms, s.ended_ms.unwrap()).is_none());
    }
    #[test] fn invalid_boot_summary_does_not_hide_roster_evidence() {
        let (t, s, v) = fixture();
        let p = t.path().join("kiln/state/boot.json");
        std::fs::write(p.with_file_name("roster-refused.json"), serde_json::to_vec(&v).unwrap()).unwrap();
        for bytes in [b"{".to_vec(), vec![b' '; LIMIT as usize + 1], b"{\"format\":99}".to_vec()] {
            std::fs::write(&p, bytes).unwrap();
            assert!(boot_state(t.path()).is_none());
            assert!(read(t.path(), &s, &BTreeMap::new()).is_some());
        }
    }
    // 0.2.0-dev.29 contentstop: the whole build of the mods' native quests and NPCs failed (19203, Wine status 3)
    fn content(v: &serde_json::Value) -> serde_json::Value {
        let mut c = v.clone();
        c["why"] = "the build of the mods' native quests failed: boom".into();
        c["exitCode"] = 19203.into(); c["unixExitStatus"] = 3.into(); c["advice"] = "Remove or update the mods named here".into();
        c
    }
    #[test] fn content_stop_file_names_the_mods_and_its_kind() {
        let (t, s, v) = fixture(); let p = t.path().join("kiln/state/content-refused.json");
        std::fs::write(&p, serde_json::to_vec(&content(&v)).unwrap()).unwrap();
        let r = read(t.path(), &s, &BTreeMap::from([("a".into(), "A mod".into())])).expect("content-refused.json is a stop");
        assert_eq!(serde_json::to_value(&r.kind).unwrap(), "content");
        assert_eq!((r.exit_code, r.names.clone(), r.advice.as_str()), (19203, vec!["A mod".to_string()], "Remove or update the mods named here"));
        assert!(r.reason.contains("native quests failed"));
        // the roster's code or status in this file is not this stop; the .last file describes the preceding start
        for (k, bad) in [("exitCode", serde_json::json!(19202)), ("unixExitStatus", serde_json::json!(2))] {
            let mut c = content(&v); c[k] = bad;
            std::fs::write(&p, serde_json::to_vec(&c).unwrap()).unwrap();
            assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
        }
        std::fs::remove_file(&p).unwrap();
        std::fs::write(p.with_file_name("content-refused.last.json"), serde_json::to_vec(&content(&v)).unwrap()).unwrap();
        assert!(read(t.path(), &s, &BTreeMap::new()).is_none());
    }
    #[test] fn content_stop_from_boot_json_alone() {
        let (t, s, _) = fixture(); let p = t.path().join("kiln/state/boot.json");
        let v = serde_json::json!({"format":1,"pid":42,"written":"2026-11-15T12:00:01Z","boot":{"exitCode":19203,"reason":"x","owners":[],
            "stop":{"code":19203,"reason":"kiln.knowledge: the mods' native quests, knowledge and NPCs could not be built at all (boom); mods: b","by":"kiln-quests","mods":["b"]}}});
        std::fs::write(&p, serde_json::to_vec(&v).unwrap()).unwrap();
        let r = read(t.path(), &s, &BTreeMap::new()).unwrap();
        assert_eq!(serde_json::to_value(&r.kind).unwrap(), "content");
        assert_eq!((r.exit_code, r.mods.clone()), (19203, vec!["b".to_string()]));
        assert!(r.advice.contains("kiln.log"), "{}", r.advice);
    }
    #[test] fn required_boot_mod_and_service_stop() {
        let (t, s, _) = fixture(); let p = t.path().join("kiln/state/boot.json");
        let mut v = serde_json::json!({"format":1,"pid":42,"written":"2026-11-15T12:00:01Z","boot":{"exitCode":19201,"reason":"Required mod failed","owners":[{"id":"a","state":"excluded-callback"}]}});
        std::fs::write(&p, serde_json::to_vec(&v).unwrap()).unwrap();
        assert_eq!(read(t.path(), &s, &BTreeMap::new()).unwrap().mods, ["a"]);
        v["boot"]["stop"] = serde_json::json!({"code":19201,"reason":"Cannot prepare content","by":"service:x","mods":["b"]});
        std::fs::write(&p, serde_json::to_vec(&v).unwrap()).unwrap();
        assert_eq!(read(t.path(), &s, &BTreeMap::new()).unwrap().mods, ["b"]);
    }
}
