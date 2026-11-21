// SPDX-License-Identifier: GPL-3.0-or-later
//! Every kiln.launch record but `kiln` (and `kiln` under `KILN_SHROUDFORGE=0`) leaves kiln-eml's whole
//! start as it is without the table, through the plugin (`kiln_plugin_main`, `run_now`, the runner): kiln/state/eml.json,
//! every kiln.log line, kiln/logs/eml.log and every file of the install.
//!
//! Each start runs in a child process (the plugin's statics are process-wide), from a copy of this test binary named
//! `enshrouded`, so that the run goes past the runtime step into the EML run and the install lock. The fakes here use
//! nothing of kiln_mode.rs, so this file also builds into a kiln-eml from before kiln.launch: with
//! `KILN_EML_RECORDS_KEEP=<dir>` the dumps stay in `<dir>` for comparing the two builds.

use super::*;
use camino::{Utf8Path, Utf8PathBuf};
use std::sync::Mutex;

const CHILD: &str = "KILN_EML_RECORDS_CHILD";
const SENTINEL: &str = "SF-STATE-REASON-sentinel";
const PLAN_ID: &str = "0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0";

/// The records served (none: no kiln.launch table at all, which is what an older core and the old kiln-eml see).
const CASES: &[&str] = &["none", "v0", "short", "nostatus", "unfrozen", "frozen0", "backend4", "none001", "idle200",
    "idle204", "refused101", "refused106", "hosted", "installed", "kiln"];

static GAME: Mutex<String> = Mutex::new(String::new());
static PROFILE: Mutex<String> = Mutex::new(String::new());
static LOGGED: Mutex<Vec<(i32, String)>> = Mutex::new(Vec::new());
static RECORD: Mutex<Option<(u32, u32, i32, FakeStatus)>> = Mutex::new(None);

/// kiln_launch.h KilnLaunchStatus v1 (192 bytes), kept here so that this file needs nothing of the change it checks.
#[repr(C)]
#[derive(Clone, Copy)]
struct FakeStatus {
    size: u32,
    backend: u32,
    frozen: u32,
    reason_code: u32,
    launch_id: u64,
    plan_hash: [u8; 32],
    freeze_stamp: u64,
    reason: [u8; 128],
}

#[repr(C)]
struct FakeLaunchApi {
    header: KilnServiceHeader,
    status: Option<unsafe extern "C" fn(*mut FakeStatus) -> i32>,
    notice_copy: *const std::ffi::c_void,
}

unsafe extern "C" fn status(out: *mut FakeStatus) -> i32 {
    let (_, _, rc, record) = RECORD.lock().unwrap().unwrap();
    if unsafe { (*out).size } < 192 { return KILN_ERR_INVALID_ARGUMENT; }
    unsafe { *out = record };
    rc
}

unsafe extern "C" fn service(name: KilnStr, _version: u32) -> *const std::ffi::c_void {
    match unsafe { name.to_string() }.as_str() {
        "kiln.profile" => profile_table(),
        "kiln.launch" => {
            let Some((size, version, rc, _)) = *RECORD.lock().unwrap() else { return std::ptr::null() };
            Box::leak(Box::new(FakeLaunchApi {
                header: KilnServiceHeader { size, version },
                status: if rc == -1 { None } else { Some(status) },
                notice_copy: std::ptr::null(),
            })) as *const FakeLaunchApi as *const std::ffi::c_void
        }
        _ => std::ptr::null(),
    }
}

fn profile_table() -> *const std::ffi::c_void {
    unsafe extern "C" fn mode() -> u32 { KILN_PROFILE_ACTIVE }
    unsafe extern "C" fn text() -> KilnStr { KilnStr::lit("records") }
    unsafe extern "C" fn json() -> KilnStr {
        let s: &'static str = Box::leak(PROFILE.lock().unwrap().clone().into_boxed_str());
        KilnStr::of(s)
    }
    unsafe extern "C" fn zero() -> u64 { 0 }
    unsafe extern "C" fn count(_: u32) -> u32 { 0 }
    unsafe extern "C" fn none() -> u32 { 0 }
    Box::leak(Box::new(KilnProfileApi {
        header: KilnServiceHeader { size: std::mem::size_of::<KilnProfileApi>() as u32, version: 1 },
        mode, id: text, name: text, generation: zero, count, get: std::ptr::null(), dropins: none, config_dir: text,
        data_dir: text, json,
    })) as *const KilnProfileApi as *const std::ffi::c_void
}

fn api() -> *mut KilnApi {
    unsafe extern "C" fn version() -> KilnStr { KilnStr::lit("records") }
    unsafe extern "C" fn log(_: u64, level: i32, text: KilnStr) { LOGGED.lock().unwrap().push((level, unsafe { text.to_string() })); }
    unsafe extern "C" fn path(kind: i32, buf: *mut u8, cap: usize, len: *mut usize) -> i32 {
        let game = GAME.lock().unwrap().clone();
        let sep = std::path::MAIN_SEPARATOR;
        let p = match kind {
            KILN_PATH_GAME => game,
            KILN_PATH_KILN => format!("{game}{sep}kiln"),
            KILN_PATH_LOGS => format!("{game}{sep}kiln{sep}logs"),
            KILN_PATH_STATE => format!("{game}{sep}kiln{sep}state"),
            _ => return KILN_ERR_UNAVAILABLE,
        };
        unsafe { *len = p.len() };
        if cap < p.len() { return KILN_ERR_BUFFER_TOO_SMALL; }
        unsafe { std::ptr::copy_nonoverlapping(p.as_ptr(), buf, p.len()) };
        KILN_OK
    }
    unsafe extern "C" fn register(_: u64, _: KilnStr, _: *const std::ffi::c_void) -> i32 { KILN_OK }
    let null = std::ptr::null();
    Box::leak(Box::new(KilnApi {
        size: std::mem::size_of::<KilnApi>() as u32, abi_version: 1, version, log, build_info: null, feature: null, path,
        owner_create: null, owner_revoke: null, subscribe_tick: null, unsubscribe: null, post: null, on_tick_thread: null,
        tick_info: null, subscribe_log: null, symbol: null, constant: null, service, register_service: register,
    }))
}

/// A file's bytes for the comparison: the text itself when small (every fixture file is), else its length and a hash
/// that is the same for every build of one toolchain (no crate this file could lack).
fn content(bytes: &[u8]) -> String {
    use std::hash::{Hash as _, Hasher as _};
    if bytes.len() <= 4096 { return String::from_utf8_lossy(bytes).into_owned(); }
    let mut h = std::hash::DefaultHasher::new();
    bytes.hash(&mut h);
    format!("{} bytes, {:016x}", bytes.len(), h.finish())
}

/// The install: EML packages in the profile (one patch, one runtime, one handed to ShroudForge by the launcher, one the
/// launcher blocked), a ShroudForge package and an EML drop-in in `<game>/mods`, kiln-shroudforge's state file of this
/// process, and a launch plan the profile names (case `kiln` is compared only under KILN_SHROUDFORGE=0, where it
/// leaves the plan unread).
fn fixture(game: &Utf8Path) {
    let _ = std::fs::remove_dir_all(game);
    std::fs::create_dir_all(game.join("kiln/state")).unwrap();
    std::fs::write(game.join("enshrouded.kfc"), b"KFC fixture").unwrap();
    std::fs::write(game.join("kiln/state/shroudforge.json"), serde_json::json!({
        "pid": std::process::id(), "mode": "off", "reason": SENTINEL }).to_string()).unwrap();
    let package = |dir: &str, id: &str, caps: &[&str], extended: Option<&str>| {
        let p = game.join(dir);
        std::fs::create_dir_all(p.join("src")).unwrap();
        std::fs::write(p.join("mod.json"), serde_json::json!({"id": id, "name": id, "version": "1.0.0", "capabilities": caps}).to_string()).unwrap();
        std::fs::write(p.join("src/mod.lua"), "-- fixture").unwrap();
        if let Some(text) = extended { std::fs::write(p.join("extended.mod.json"), text).unwrap(); }
        p
    };
    let pure = package("kiln/library/pure", "pure", &["patch"], None);
    let rt = package("kiln/library/rt", "rt", &["patch", "runtime"], None);
    let delegated = package("kiln/library/delegated", "delegated", &["runtime"], None);
    let blocked = package("kiln/library/blocked", "blocked", &["runtime-register-dll"], None);
    package("mods/sf", "sf", &["runtime"], Some(r#"{"schemaVersion":1,"launcher":"SF","enabled":true}"#));
    package("mods/dropin", "dropin", &["patch"], None);
    package("mods/dropin-rt", "dropin-rt", &["runtime"], None);
    let plan = serde_json::json!({"startupDialect": "shroudforge", "packages": [
        {"id": "rt", "root": "mods/rt", "decision": "candidate", "data": {"required": true}, "reasons": []},
        {"id": "sf", "root": "mods/sf", "decision": "candidate", "data": {"required": false}, "reasons": []},
        {"id": "dropin", "root": "mods/dropin", "decision": "skip", "data": {"required": true},
         "reasons": [{"message": "needs the missing package rt-core"}]},
    ]}).to_string();
    std::fs::create_dir_all(game.join("kiln/launch-plans")).unwrap();
    std::fs::write(game.join(format!("kiln/launch-plans/{PLAN_ID}.json")), &plan).unwrap();
    *PROFILE.lock().unwrap() = serde_json::json!({
        "mods": {"eml": [{"id": "pure", "path": pure.as_str(), "enabled": true},
                         {"id": "rt", "path": rt.as_str(), "enabled": true},
                         {"id": "delegated", "path": delegated.as_str(), "enabled": false},
                         {"id": "blocked", "path": blocked.as_str(), "enabled": false}]},
        "dropins": true,
        "emlRuntime": ["delegated"],
        "emlRuntimeBlocked": {"blocked": "the launcher's own reason"},
        "sfLaunchPlan": {"path": format!("launch-plans/{PLAN_ID}.json"), "sha256": "0".repeat(64)},
    }).to_string();
}

fn record(case: &str) -> Option<(u32, u32, i32, FakeStatus)> {
    let plan_hash = [7; 32];
    let full = std::mem::size_of::<FakeLaunchApi>() as u32;
    let mut r = FakeStatus { size: 192, backend: 0, frozen: 1, reason_code: 0, launch_id: 0x1234, plan_hash: [0; 32],
        freeze_stamp: 1, reason: [0; 128] };
    r.reason[..6].copy_from_slice(b"reason");
    let with = |backend: u32, code: u32| FakeStatus { backend, reason_code: code, ..r };
    let kiln = FakeStatus { plan_hash, ..with(3, 0x005) };
    Some(match case {
        "none" => return None,
        "v0" => (full, 0, KILN_OK, kiln),
        "short" => (8, 1, KILN_OK, kiln),
        "nostatus" => (full, 1, -1, kiln),
        "unfrozen" => (full, 1, KILN_ERR_UNAVAILABLE, FakeStatus { frozen: 0, ..kiln }),
        "frozen0" => (full, 1, KILN_OK, FakeStatus { frozen: 0, ..kiln }),
        "backend4" => (full, 1, KILN_OK, FakeStatus { plan_hash, ..with(4, 0x005) }),
        "none001" => (full, 1, KILN_OK, with(0, 0x001)),
        "idle200" => (full, 1, KILN_OK, with(0, 0x200)),
        "idle204" => (full, 1, KILN_OK, with(0, 0x204)),
        "refused101" => (full, 1, KILN_OK, with(0, 0x101)),
        "refused106" => (full, 1, KILN_OK, FakeStatus { plan_hash, ..with(0, 0x106) }),
        "hosted" => (full, 1, KILN_OK, with(1, 0x001)),
        "installed" => (full, 1, KILN_OK, with(2, 0x003)),
        "kiln" => (full, 1, KILN_OK, kiln),
        other => panic!("unknown case {other}"),
    })
}

/// Every string in `v` with the install's folder replaced by `<game>`.
fn scrub(v: &mut serde_json::Value, game: &str) {
    match v {
        serde_json::Value::String(s) => *s = s.replace(game, "<game>"),
        serde_json::Value::Array(a) => a.iter_mut().for_each(|v| scrub(v, game)),
        serde_json::Value::Object(o) => o.values_mut().for_each(|v| scrub(v, game)),
        _ => {}
    }
}

/// `text` with the install's folder as `<game>` and every duration (`<number> ms`) as `N ms`: the only parts of a line
/// that differ between two starts of the same install.
fn line(text: &str, game: &str) -> String {
    let text = text.replace(game, "<game>");
    let mut out = String::with_capacity(text.len());
    let mut rest = text.as_str();
    while let Some(at) = rest.find(" ms") {
        let head = &rest[..at];
        let number = head.trim_end_matches(|c: char| c.is_ascii_digit() || c == '.');
        if number.len() < head.len() && head[number.len()..].starts_with(|c: char| c.is_ascii_digit()) {
            out.push_str(number);
            out.push('N');
        } else {
            out.push_str(head);
        }
        out.push_str(" ms");
        rest = &rest[at + 3..];
    }
    out.push_str(rest);
    out
}

/// What the start left: eml.json (without pid and ms), the kiln.log lines, eml.log and every other file of the install.
fn dump(game: &Utf8Path) -> serde_json::Value {
    let g = game.as_str();
    let mut eml: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(game.join("kiln/state/eml.json")).unwrap()).unwrap();
    let o = eml.as_object_mut().unwrap();
    o.remove("pid");
    o.remove("ms");
    scrub(&mut eml, g);
    let log: Vec<(i32, String)> = LOGGED.lock().unwrap().iter().map(|(l, t)| (*l, line(t, g))).collect();
    // eml.log in sorted order: EML's preflight opens the mods' entry modules in parallel, so its debug lines interleave
    let mut eml_log: Vec<String> = std::fs::read_to_string(game.join("kiln/logs/eml.log")).unwrap_or_default().lines()
        .map(|l| line(l, g)).collect();
    eml_log.sort();
    let mut files = Vec::new();
    let mut stack = vec![game.to_owned()];
    while let Some(dir) = stack.pop() {
        for e in std::fs::read_dir(&dir).unwrap() {
            let p = Utf8PathBuf::from_path_buf(e.unwrap().path()).unwrap();
            let rel = p.strip_prefix(game).unwrap().as_str().replace('\\', "/");
            if p.is_dir() {
                files.push(serde_json::json!([rel, "dir"]));
                stack.push(p);
            } else if ["kiln/state/shroudforge.json", "kiln/state/eml.json", "kiln/logs/eml.log"].contains(&rel.as_str()) {
                files.push(serde_json::json!([rel, "compared apart"]));
            } else if rel == ".kiln-eml.lock" {
                // held (locked) by this process until it exits: Windows refuses to read a locked range
                files.push(serde_json::json!([rel, "the install lock"]));
            } else {
                files.push(serde_json::json!([rel, content(&std::fs::read(&p).unwrap())]));
            }
        }
    }
    files.sort_by_key(|f| f[0].as_str().unwrap().to_owned());
    serde_json::json!({"emlJson": eml, "kilnLog": log, "emlLog": eml_log, "files": files})
}

#[test]
fn every_record_but_kiln_runs_as_without_the_table() {
    if let Ok(case) = std::env::var(CHILD) {
        let game = Utf8PathBuf::from(std::env::var("KILN_EML_RECORDS_GAME").unwrap());
        fixture(&game);
        *GAME.lock().unwrap() = game.to_string();
        *RECORD.lock().unwrap() = record(&case);
        let mut info = KilnPluginInfo { size: std::mem::size_of::<KilnPluginInfo>() as u32, id: KilnStr::lit(""),
            version: KilnStr::lit(""), on_unload: None, user: std::ptr::null_mut(), on_ready: None };
        assert_eq!(unsafe { kiln_plugin_main(api(), 1, &mut info) }, KILN_OK);
        let out = Utf8PathBuf::from(std::env::var("KILN_EML_RECORDS_OUT").unwrap());
        std::fs::write(out.join(format!("{case}.json")), serde_json::to_string_pretty(&dump(&game)).unwrap()).unwrap();
        return;
    }
    let root = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap().join(format!("kiln-eml-records-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(root.join("bin")).unwrap();
    // the plugin takes the game's file name from its own executable: `enshrouded` lets the run reach the EML run
    let exe = root.join(if cfg!(windows) { "bin/enshrouded.exe" } else { "bin/enshrouded" });
    // Copied by a separate process on Unix: a file this process opened for writing is shared, until their exec, with
    // every child another test thread creates meanwhile (runner and plugin tests spawn this binary), and running `exe`
    // while any such copy of the writable descriptor exists fails ETXTBSY ("Text file busy"). `cp` holds the only
    // writable descriptor and nothing inherits it; once it has exited, no process can write the file.
    #[cfg(unix)]
    assert!(mod_loader_lua::test_gate::status(std::process::Command::new("cp").arg("--").arg(std::env::current_exe().unwrap()).arg(&exe)).unwrap().success());
    #[cfg(not(unix))]
    std::fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
    let keep = std::env::var("KILN_EML_RECORDS_KEEP").ok().map(Utf8PathBuf::from);
    for env in [None, Some("0")] {
        let out = match &keep {
            Some(k) => k.join(format!("env-{}", env.unwrap_or("unset"))),
            None => root.join(format!("out-{}", env.unwrap_or("unset"))),
        };
        std::fs::create_dir_all(&out).unwrap();
        for case in CASES {
            let mut child = std::process::Command::new(&exe);
            child.args(["--exact", "plugin::records_tests::every_record_but_kiln_runs_as_without_the_table", "--test-threads=1"])
                .env(CHILD, case).env("KILN_EML_RECORDS_GAME", root.join("game").as_str()).env("KILN_EML_RECORDS_OUT", out.as_str());
            match env {
                Some(v) => child.env("KILN_SHROUDFORGE", v),
                None => child.env_remove("KILN_SHROUDFORGE"),
            };
            let o = mod_loader_lua::test_gate::output(&mut child).unwrap();
            assert!(o.status.success() && String::from_utf8_lossy(&o.stdout).contains("1 passed"), "{case} {env:?}: {}{}",
                String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
        }
        let read = |case: &str| std::fs::read_to_string(out.join(format!("{case}.json"))).unwrap();
        let before = read("none");
        // the start reached the runner's legacy rule, the EML run and the install lock
        let rule = if env.is_some() { "which the KILN_SHROUDFORGE=0 environment variable switches off" } else { SENTINEL };
        assert!(before.contains(rule), "the runtime package's reason is the rule from before ({rule}): {before}");
        assert!(before.contains(".kiln-eml.lock"), "the run took the install lock: {before}");
        for case in CASES.iter().filter(|c| **c != "none" && (**c != "kiln" || env == Some("0"))) {
            assert_eq!(read(case), before, "record {case}, KILN_SHROUDFORGE={env:?}: kiln-eml's start differs from one without kiln.launch");
        }
    }
    let _ = std::fs::remove_dir_all(&root);
}
