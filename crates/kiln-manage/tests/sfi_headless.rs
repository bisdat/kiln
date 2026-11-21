// SPDX-License-Identifier: MIT
//! The headless starts in kiln mode: a local dedicated server's start (the launcher's `server::run::start`
//! through every runner, `kilnctl run`'s foreground runner) writes the server folder's active profile again the way Play
//! does, before its runner starts anything, and says every warning the check before Play would show (each package that
//! does not run, the do-not-save text, a refused plan or a held interpreter) in the head of its capture and its
//! progress; nobody is asked and nothing waits: the start goes on. Only an infrastructure failure (the cook lock, a
//! profile that cannot be applied) refuses it, and then no runner is asked. A server whose kiln.json asks for no Kiln
//! interpreter starts exactly as before: nothing is written, no note is added.
//!
//! Every package is synthetic and every runner a
//! fake one: nothing here starts a server, Wine or the game.

mod sfi_routing_common;

use kiln_manage::game::GameInstall;
use kiln_manage::launch::{LaunchMode, LaunchRequest};
use kiln_manage::library::LibraryRef;
use kiln_manage::manifest::{ModKind, RunSide};
use kiln_manage::paths::DataDirs;
use kiln_manage::profile;
use kiln_manage::server::instance::{InstanceKind, ServerInstance, ServerRunner};
use kiln_manage::server::run;
use kiln_manage::sfi_inspection::{Inspect, ORIGINS};
use kiln_manage::sfi_launch::{self, Headless, SkippedPackage, Warning};
use kiln_manage::sfi_plan::Mode;
use kiln_manage::task::Progress;
use kiln_manage::{Error, ErrorCode};
use serde_json::{Value, json};
use sfi_routing_common::*;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

// ---- a stand-in inspector that answers for a server ----

const EXE: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const BASE: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const INPUT: &str = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
const VERSION: &str = "0.2.0-test";

fn answer(side: RunSide) -> Value {
    let (s, stem) = match side {
        RunSide::Client => ("client", "enshrouded"),
        RunSide::Server => ("server", "enshrouded_server"),
    };
    let label = ORIGINS.iter().find(|(t, _)| *t == "first-managed-unclassified").unwrap().1;
    json!({"schema_version":1,"inspector":{"component":"kiln-eml-inspect","version":VERSION},"status":"ok",
        "target":{"side":s,"stem":stem,"executable":{"file":format!("{stem}.exe"),"size":10,"sha256":EXE}},
        "proof":{"framing":"kiln-eml-inspect archive-set v1","containers":"size"},
        "input":{"class":"unclassified","sha256":INPUT,"files":[]},
        "base":{"evidence":"current-set","sha256":BASE,"origin":{"token":"first-managed-unclassified","label":label},"baseline":null,"files":[]},
        "receipt":{"state":"absent","file":format!("kiln/eml/ownership/{stem}.json")},
        "inventory":[{"role":"executable","file":format!("{stem}.exe"),"size":10},{"role":"header","file":format!("{stem}.kfc"),"size":5}],
        "reasons":[],"notes":[],"cost":{"bytesRead":15,"filesRead":2}})
}

fn calls() -> &'static Mutex<BTreeMap<PathBuf, usize>> {
    static C: OnceLock<Mutex<BTreeMap<PathBuf, usize>>> = OnceLock::new();
    C.get_or_init(Default::default)
}
fn refusing() -> &'static Mutex<BTreeSet<PathBuf>> {
    static R: OnceLock<Mutex<BTreeSet<PathBuf>>> = OnceLock::new();
    R.get_or_init(Default::default)
}

struct Fake;
impl Inspect for Fake {
    fn run(&self, game: &Path, side: RunSide) -> Result<Vec<u8>, String> {
        *calls().lock().unwrap().entry(game.to_path_buf()).or_default() += 1;
        if refusing().lock().unwrap().contains(game) {
            return Err("the stand-in inspector refuses this game".into());
        }
        Ok(format!("{}\n", answer(side)).into_bytes())
    }
    fn version(&self) -> &str {
        VERSION
    }
    fn for_game(&self, _: &Path) -> Result<Option<Box<dyn Inspect>>, String> {
        Ok(None)
    }
}

fn inspections(game: &Path) -> usize {
    calls().lock().unwrap().get(game).copied().unwrap_or(0)
}

// ---- a server world ----

struct Server {
    _t: Tmp,
    root: PathBuf,
    dirs: DataDirs,
    dir: PathBuf,
    profile: String,
    sf: Vec<LibraryRef>,
}

/// A ShroudForge package in the Library, version `version`, for both sides.
fn sf_lib(root: &Path, version: &str) -> PathBuf {
    let d = root.join(format!("src-sf-{version}/sfpack"));
    put(&d, "mod.json", format!(r#"{{"id":"sfpack","name":"sfpack","version":"{version}","capabilities":["runtime"]}}"#).as_bytes());
    put(&d, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client","server"]}"#);
    put(&d, "src/mod.lua", format!("return {{ v = '{version}' }}").as_bytes());
    d
}

/// A dedicated server folder whose kiln.json selects Kiln's interpreter (the hold lifted as a preview build lifts it),
/// with Kiln's ShroudForge adapter, its runtime and the interpreter, a Wine prefix of its own (Linux, macOS), and a
/// profile with the Library's ShroudForge mod (version 1) activated on it. The player's own package `needy` in
/// `mods/` needs a dependency that is not there, so the plan skips it.
fn server(tag: &str) -> Server {
    profile::sfi::set_inspector(Some(Arc::new(Fake)));
    let t = tmp(&format!("v811-{tag}"));
    let root = t.0.clone();
    let dirs = DataDirs::at(root.join("data"));
    let dir = root.join("server");
    put(&dir, "enshrouded_server.exe", b"MZ");
    put(&dir, "kiln/plugins/kiln-shroudforge.dll", b"adapter");
    put(&dir, "kiln/plugins/shroudforge/shroudforge-runtime.dll", b"runtime");
    put(&dir, "kiln/plugins/kiln-sfi.dll", b"interpreter");
    #[cfg(unix)]
    {
        std::fs::create_dir_all(root.join("server.pfx/dosdevices")).unwrap();
        std::os::unix::fs::symlink("/", root.join("server.pfx/dosdevices/z:")).unwrap();
    }
    select(&dir, Some(KILN));
    put(&dir, "mods/needy/mod.json",
        br#"{"id":"needy","name":"Needy","version":"1.2.0","capabilities":["runtime"],"dependencies":[{"id":"missing-lib","version":"^1"}]}"#);
    put(&dir, "mods/needy/extended.mod.json", br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#);
    put(&dir, "mods/needy/src/mod.lua", b"return {}\n");
    let sf = ["1.0.0", "2.0.0"].iter().map(|v| import(&dirs, &sf_lib(&root, v), ModKind::ShroudForge, "sfpack")).collect();
    let p = profile::create(&dirs, "srv", None).unwrap();
    let s = Server { _t: t, root, dirs, dir, profile: p.id.clone(), sf };
    s.select(0);
    profile::activate(&s.dirs, &s.profile, &s.install()).unwrap_or_else(|e| panic!("{e:?}"));
    s
}

impl Server {
    /// What a server start makes of the folder (`game::inspect`, as `server::run::prepare_local` does).
    fn install(&self) -> GameInstall {
        kiln_manage::game::inspect(&self.dir).unwrap()
    }
    fn select(&self, v: usize) {
        let mut p = profile::get(&self.dirs, &self.profile).unwrap();
        p.mods.shroudforge = vec![pm(&self.sf[v], true)];
        profile::save(&self.dirs, &p).unwrap();
    }
    fn doc(&self) -> Value {
        std::fs::read(self.dir.join("kiln/profile.json")).ok().map(|b| serde_json::from_slice(&b).unwrap()).unwrap_or(Value::Null)
    }
    fn generation(&self) -> u64 {
        self.doc()["generation"].as_u64().unwrap_or(0)
    }
    fn instance(&self, runner: ServerRunner) -> ServerInstance {
        ServerInstance {
            id: "srv".into(),
            name: "srv".into(),
            kind: InstanceKind::Local,
            dir: Some(self.dir.clone()),
            port: 15636,
            query_port: 15637,
            wine: None,
            policy: "verified-only".into(),
            source: "launcher".into(),
            remote: None,
            runner,
            prefix: None,
            backup_dir: None,
            kilnctl: None,
            stop_timeout_s: 90,
        }
    }
}

/// What the fake runner saw when it was asked.
#[derive(Debug, Default)]
struct Seen {
    asked: usize,
    notes: Vec<String>,
    generation: u64,
    plan_current: bool,
    plan: Value,
}

/// Records every progress line.
#[derive(Default)]
struct Lines(Vec<String>);
impl Progress for Lines {
    fn report(&mut self, _: Option<f32>, message: &str) {
        self.0.push(message.to_owned());
    }
}

const FAKE: &str = "the fake runner starts nothing";

/// `server::run::start` of `s` with `runner`, the runner a fake one that records the server folder as it found it.
fn start(s: &Server, runner: ServerRunner) -> (Result<(), Error>, Seen, Vec<String>) {
    let mut seen = Seen::default();
    let mut lines = Lines::default();
    let r = run::start_via(Some(&s.dirs), &s.instance(runner), &mut lines, &mut |i, _, notes, _| {
        assert_eq!(i.runner, runner);
        seen.asked += 1;
        seen.notes = notes.to_vec();
        seen.generation = s.generation();
        seen.plan_current = profile::sfi::plan_current(&s.dirs, &s.install(), Mode::Active);
        seen.plan = s.doc()["sfLaunchPlan"].clone();
        // the cook lock is free again for the runner's long wait
        if s.dir.join("kiln/state/cook.lock").exists() {
            assert!(kiln_manage::state::lock_game(&s.dir, std::time::Duration::ZERO, &|| false).is_ok(), "the runner found the cook lock free");
        }
        Err(Error::new(ErrorCode::Canceled, FAKE))
    });
    let r = match r {
        Err(e) if e.message == FAKE => Ok(()),
        Err(e) => Err(e),
        Ok(_) => panic!("the fake runner never succeeds"),
    };
    (r, seen, lines.0)
}

const RUNNERS: [ServerRunner; 3] = [ServerRunner::Kilnctl, ServerRunner::Steam, ServerRunner::Windows];

fn has(notes: &[String], needle: &str) -> bool {
    notes.iter().any(|n| n.contains(needle))
}

// ---- the tests ----

/// Every local runner: a profile changed since its activation is written again (a new plan: one inspection) before the
/// runner is asked, the skipped package is said in the capture's notes and the progress, and the start goes on; a
/// second start with nothing changed keeps the profile and its plan (no inspection).
#[test]
fn every_local_runner_starts_after_the_profile_is_written_again_and_the_skips_are_said() {
    for (n, runner) in RUNNERS.into_iter().enumerate() {
        let s = server(&format!("runner-{n}"));
        let before = s.generation();
        let pin = s.doc()["sfLaunchPlan"].clone();
        assert!(pin.is_object(), "the activation pinned a plan: {}", s.doc());
        s.select(1);
        let inspected = inspections(&s.dir);
        let (r, seen, progress) = start(&s, runner);
        r.unwrap_or_else(|e| panic!("{runner:?}: {e:?}"));
        assert_eq!(seen.asked, 1, "{runner:?}: the runner was asked once");
        assert!(seen.generation > before, "{runner:?}: the profile was written again before the runner");
        assert!(seen.plan_current, "{runner:?}: the runner found the plan current");
        assert_ne!(seen.plan, pin, "{runner:?}: a new plan for the changed profile");
        assert_eq!(inspections(&s.dir), inspected + 1, "{runner:?}: one inspection for the changed inputs");
        let needy = seen.notes.iter().find(|l| l.contains("needy")).unwrap_or_else(|| panic!("{runner:?}: {:?}", seen.notes));
        assert!(needy.starts_with("WARNING: ShroudForge package needy 1.2.0 (mods/needy) does not run this start"), "{needy}");
        assert!(needy.contains("missing-lib"), "{needy}");
        assert!(has(&seen.notes, "a headless start asks nobody"), "{:?}", seen.notes);
        for l in &seen.notes {
            assert!(progress.contains(l), "{runner:?}: the progress says {l:?} too: {progress:?}");
        }
        assert!(!has(&seen.notes, "sfpack"), "the package that runs is not named: {:?}", seen.notes);
        // nothing changed: the same file and plan, no inspection
        let g = s.generation();
        let (r, seen, _) = start(&s, runner);
        r.unwrap();
        assert_eq!((seen.generation, inspections(&s.dir)), (g, inspected + 1), "{runner:?}: kept");
        assert!(seen.plan_current);
    }
}

/// A package that is off in its own folder, or only for the client, is no skip of a server start.
#[test]
fn a_package_off_or_for_the_other_side_is_no_skip() {
    let s = server("sides");
    put(&s.dir, "mods/offpack/mod.json", br#"{"id":"offpack","name":"Off","version":"1.0.0","capabilities":["runtime"]}"#);
    put(&s.dir, "mods/offpack/extended.mod.json", br#"{"schemaVersion":1,"enabled":false,"targets":["client","server"]}"#);
    put(&s.dir, "mods/offpack/src/mod.lua", b"return {}\n");
    put(&s.dir, "mods/clientpack/mod.json", br#"{"id":"clientpack","name":"Client","version":"1.0.0","capabilities":["runtime"]}"#);
    put(&s.dir, "mods/clientpack/extended.mod.json", br#"{"schemaVersion":1,"enabled":true,"targets":["client"]}"#);
    put(&s.dir, "mods/clientpack/src/mod.lua", b"return {}\n");
    let (r, seen, _) = start(&s, ServerRunner::Kilnctl);
    r.unwrap();
    assert!(has(&seen.notes, "needy"), "{:?}", seen.notes);
    assert!(!has(&seen.notes, "offpack") && !has(&seen.notes, "clientpack"), "{:?}", seen.notes);
}

/// A plan the producer refuses, and a held interpreter: said, and the start goes on (no token, no wait).
#[test]
fn a_refused_plan_and_a_held_interpreter_are_said_and_the_start_goes_on() {
    let s = server("refused");
    s.select(1);
    refusing().lock().unwrap().insert(s.dir.clone());
    let (r, seen, _) = start(&s, ServerRunner::Kilnctl);
    refusing().lock().unwrap().remove(&s.dir);
    r.unwrap();
    assert_eq!(seen.asked, 1);
    let l = seen.notes.iter().find(|l| l.contains("no ShroudForge package runs this start")).unwrap_or_else(|| panic!("{:?}", seen.notes));
    assert!(l.starts_with("WARNING: ") && l.contains("sfpack"), "{l}");

    // held only while this build's gate list holds the interpreter; without its row kiln is a normal choice
    let s = server("held");
    select(&s.dir, Some(KILN_HELD));
    let (r, seen, _) = start(&s, ServerRunner::Steam);
    r.unwrap();
    assert_eq!(seen.asked, 1);
    assert_eq!(has(&seen.notes, "(interpreter-held)"), interpreter_is_a_preview(), "{:?}", seen.notes);
}

/// ShroudForge's own loader in the folder: both Kiln backends stand aside; no interpreter pass is said.
#[test]
fn shroudforges_own_loader_stands_the_interpreter_aside() {
    let s = server("own-loader");
    sf_own_loader(&s.dir);
    s.select(1);
    let before = s.generation();
    let (r, seen, _) = start(&s, ServerRunner::Kilnctl);
    r.unwrap();
    assert_eq!(seen.asked, 1);
    assert!(!has(&seen.notes, "does not run this start") && !has(&seen.notes, "no ShroudForge package runs"), "{:?}", seen.notes);
    assert!(seen.generation > before, "the changed profile was written again (main's route) before the runner");
}

/// The cook lock held elsewhere, or its file a link: the start is refused, nothing is written, no runner is asked.
#[test]
fn an_infrastructure_failure_refuses_and_asks_no_runner() {
    let s = server("lock");
    s.select(1);
    let held = kiln_manage::state::lock_game(&s.dir, std::time::Duration::from_secs(1), &|| false).unwrap();
    let (r, seen, _) = start(&s, ServerRunner::Kilnctl);
    drop(held);
    let e = r.expect_err("refused while the lock is held");
    assert_eq!(seen.asked, 0, "{e:?}");
    // the lock file a link out of the folder: refused as the client's start refuses it
    #[cfg(unix)]
    {
        let s = server("lock-link");
        s.select(1);
        std::fs::create_dir_all(s.dir.join("kiln/state")).unwrap();
        let _ = std::fs::remove_file(s.dir.join("kiln/state/cook.lock"));
        std::os::unix::fs::symlink(s.root.join("elsewhere.lock"), s.dir.join("kiln/state/cook.lock")).unwrap();
        let g = s.generation();
        let (r, seen, _) = start(&s, ServerRunner::Windows);
        r.expect_err("a linked lock file refuses the start");
        assert_eq!((seen.asked, s.generation()), (0, g), "nothing written, no runner asked");
    }
}

/// Hosted, installed or no setting: nothing in the server folder or the data folder changes, no note is added, though
/// the profile changed since its activation (main's start).
#[test]
fn a_server_without_the_interpreter_starts_as_before() {
    for (n, setting) in [Some(HOSTED), Some(INSTALLED), None].into_iter().enumerate() {
        let s = server(&format!("hosted-{n}"));
        select(&s.dir, setting);
        s.select(1);
        let before = snapshot(&s.root);
        let inspected = inspections(&s.dir);
        let (r, seen, progress) = start(&s, ServerRunner::Kilnctl);
        r.unwrap();
        assert_eq!(seen.asked, 1);
        assert!(seen.notes.is_empty() && progress.is_empty(), "{setting:?}: {:?} {progress:?}", seen.notes);
        assert_eq!(snapshot(&s.root), before, "{setting:?}: nothing written");
        assert_eq!(inspections(&s.dir), inspected);
    }
}

/// A remote server's start is kilnctl's there: nothing local is read for it (it fails here on the missing SSH details,
/// before any runner).
#[test]
fn a_remote_start_prepares_nothing_here() {
    let s = server("remote");
    s.select(1);
    let g = s.generation();
    let mut i = s.instance(ServerRunner::Remote);
    i.kind = InstanceKind::Remote;
    let mut asked = 0;
    let r = run::start_via(Some(&s.dirs), &i, &mut Lines::default(), &mut |_, _, _, _| {
        asked += 1;
        Err(Error::new(ErrorCode::Canceled, FAKE))
    });
    assert!(r.is_err() && asked == 0);
    assert_eq!(s.generation(), g, "the local folder was not touched");
}

/// `kilnctl run`: the profile is written again before Wine is asked (a fake Wine that is never run).
#[cfg(unix)]
#[test]
fn the_foreground_runner_writes_the_profile_again_before_wine() {
    use std::os::unix::fs::PermissionsExt;
    let s = server("foreground");
    s.select(1);
    let before = s.generation();
    let bin = s.root.join("fake-wine/bin");
    for f in ["wine", "wineserver"] {
        put(&bin, f, b"#!/bin/false\n");
        std::fs::set_permissions(bin.join(f), std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    let inst = kiln_manage::server::instance::Instance {
        name: "srv".into(),
        file: s.root.join("srv.toml"),
        dir: s.dir.clone(),
        prefix: s.root.join("server.pfx"),
        wine: kiln_manage::wine::WineSpec::Path(bin.join("wine")),
        query_port: 15637,
        backup_dir: s.root.join("backups"),
        require_ntsync: false,
        stop_timeout: std::time::Duration::from_secs(5),
        kiln: kiln_manage::server::instance::Kiln {
            source: s.root.join("src"),
            build: s.root.join("out"),
            policy: Default::default(),
            samples: false,
        },
        env: Default::default(),
    };
    let mut seen = Seen::default();
    let r = run::run_foreground_via(Some(&s.dirs), &inst, &mut |_, _| {
        seen.asked += 1;
        seen.generation = s.generation();
        seen.plan_current = profile::sfi::plan_current(&s.dirs, &s.install(), Mode::Active);
        // kilnctl run only spawns: the cook lock is still held while Wine is asked
        assert!(kiln_manage::state::lock_game(&s.dir, std::time::Duration::ZERO, &|| false).is_err(), "held at the spawn");
        Err(Error::new(ErrorCode::Canceled, FAKE))
    });
    assert_eq!(r.expect_err("the fake Wine starts nothing").message, FAKE);
    assert_eq!(seen.asked, 1);
    assert!(seen.generation > before && seen.plan_current, "{seen:?}");
}

/// The console lines: a package's own text cannot add or rewrite a line, every warning has its level, the save
/// text is there for a save-sensitive package, and a start with nothing to say has no lines.
#[test]
fn console_lines_escape_control_characters_and_carry_the_save_text() {
    assert!(Headless::default().lines().is_empty());
    let mut h = Headless::default();
    h.warnings.push(Warning {
        kind: "package-skipped".into(),
        code: "dependency-missing".into(),
        message: "needs \u{1b}[2Jlib\nFAKE: line\r\u{202e}gnp".into(),
        severity: "save-warning".into(),
        caused_by: vec!["a\u{7}".into(), "b".into()],
        package: Some(SkippedPackage {
            id: "ammo\u{1b}]0;x\u{7}".into(),
            version: "0.8.2".into(),
            root: "mods/ammo".into(),
            save_sensitive: true,
            data: true,
            lua: true,
            dlls: vec!["ammo.dll".into()],
            reasons: Vec::new(),
        }),
        packages: Vec::new(),
        previous: None,
    });
    let lines = h.lines();
    assert_eq!(lines.len(), 2, "{lines:?}");
    for l in &lines {
        assert!(!l.chars().any(|c| c.is_control() || c == '\u{202e}'), "{l:?}");
    }
    let l = &lines[0];
    assert!(l.starts_with("SAVE WARNING: ShroudForge package ammo\\u{1b}]0;x\\u{7} 0.8.2 (mods/ammo) does not run"), "{l}");
    assert!(l.contains("\\u{1b}[2Jlib\\u{a}FAKE: line\\u{d}\\u{202e}gnp"), "{l}");
    assert!(l.contains("startup data, Lua, DLL ammo.dll") && l.contains("do not save"), "{l}");
    assert!(l.contains("(because of a\\u{7} <- b)"), "{l}");
    assert!(lines[1].contains("do not save a world"), "{}", lines[1]);
    assert_eq!(sfi_launch::console_safe("plain text: ok"), "plain text: ok");
}

/// `kilnctl client`'s read of the warnings (after its own activation or play-once write): a held interpreter is said,
/// a hosted game says nothing.
#[test]
fn the_cli_warnings_of_a_client_follow_its_setting() {
    let t = tmp("v811-client");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0.join("game"), true);
    let inst = install(&t.0, &g, kiln_manage::game::PathMode::Copy);
    let req = LaunchRequest { mode: LaunchMode::Profile, profile_id: None, skip_backup: true };
    select(&g, Some(KILN_HELD));
    let lines = sfi_launch::headless_warnings(&dirs, &inst, &req).lines();
    // held only while this build's gate list holds the interpreter
    assert_eq!(has(&lines, "(interpreter-held)"), interpreter_is_a_preview(), "{lines:?}");
    select(&g, Some(HOSTED));
    assert!(sfi_launch::headless_warnings(&dirs, &inst, &req).lines().is_empty());
}
