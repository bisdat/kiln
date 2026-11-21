// SPDX-License-Identifier: MIT
//! Shared by the kiln-mode profile tests: a world (a client game whose kiln.json selects Kiln's interpreter,
//! the interpreter installed, a plugin loader that game_files places in the game folder, a ShroudForge mod in two
//! versions, an EML package that needs ShroudForge's runtime, the player's own files), a stand-in archive inspector
//! (a synthetic answer, counted per game), the game's state for comparisons, and the crash-test kill harness.
//! Every package and DLL here is synthetic.
#![allow(dead_code)]

use crate::sfi_routing_common::*;
use kiln_manage::game::{GameInstall, PathMode};
use kiln_manage::library::LibraryRef;
use kiln_manage::manifest::{ModKind, RunSide};
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile;
use kiln_manage::sfi_activation::{self, Boundary, Recovered};
use kiln_manage::sfi_inspection::{Inspect, ORIGINS};
use kiln_manage::sfi_plan::{Mode, ProfileIdentity};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

// ---- the stand-in inspector ----

pub const EXE: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
pub const BASE: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
pub const INPUT: &str = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
pub const VERSION: &str = "0.2.0-test";

/// An inspection of a client as kiln-eml-inspect prints it (status ok, first managed): the executable and the
/// header are its inventory.
pub fn answer() -> Value {
    let label = ORIGINS
        .iter()
        .find(|(t, _)| *t == "first-managed-unclassified")
        .unwrap()
        .1;
    json!({"schema_version":1,"inspector":{"component":"kiln-eml-inspect","version":VERSION},"status":"ok",
        "target":{"side":"client","stem":"enshrouded","executable":{"file":"enshrouded.exe","size":10,"sha256":EXE}},
        "proof":{"framing":"kiln-eml-inspect archive-set v1","containers":"size"},
        "input":{"class":"unclassified","sha256":INPUT,"files":[]},
        "base":{"evidence":"current-set","sha256":BASE,"origin":{"token":"first-managed-unclassified","label":label},"baseline":null,"files":[]},
        "receipt":{"state":"absent","file":"kiln/eml/ownership/enshrouded.json"},
        "inventory":[{"role":"executable","file":"enshrouded.exe","size":10},{"role":"header","file":"enshrouded.kfc","size":5}],
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
fn unresolvable() -> &'static Mutex<BTreeSet<PathBuf>> {
    static U: OnceLock<Mutex<BTreeSet<PathBuf>>> = OnceLock::new();
    U.get_or_init(Default::default)
}

/// Why the stand-in cannot be resolved for a game in [`unresolvable_inspector`] (as an installed one that is not there).
pub const UNRESOLVED: &str = "the stand-in's installed inspector cannot be resolved for this game";

/// The inspector every test of this process runs: one synthetic answer for any game, counted per game; a game in
/// [`refuse_inspection`] gets no answer.
struct Fake;
impl Inspect for Fake {
    fn run(&self, game: &Path, _: RunSide) -> Result<Vec<u8>, String> {
        *calls()
            .lock()
            .unwrap()
            .entry(game.to_path_buf())
            .or_default() += 1;
        if refusing().lock().unwrap().contains(game) {
            return Err("the stand-in inspector refuses this game".into());
        }
        Ok(format!("{}\n", answer()).into_bytes())
    }
    fn version(&self) -> &str {
        VERSION
    }
    fn for_game(&self, game: &Path) -> Result<Option<Box<dyn Inspect>>, String> {
        if unresolvable().lock().unwrap().contains(game) {
            return Err(UNRESOLVED.into());
        }
        Ok(None)
    }
}

/// The stand-in cannot be resolved for `game` from now on (as an installed inspector that is gone), or can again.
pub fn unresolvable_inspector(game: &Path, on: bool) {
    let mut u = unresolvable().lock().unwrap();
    if on {
        u.insert(game.to_path_buf());
    } else {
        u.remove(game);
    }
}

/// Sets the stand-in inspector for this process (every test does; the same one).
pub fn fake_inspector() {
    profile::sfi::set_inspector(Some(Arc::new(Fake)));
}

/// How often the inspector ran for `game`.
pub fn inspections(game: &Path) -> usize {
    calls().lock().unwrap().get(game).copied().unwrap_or(0)
}

/// The inspector refuses `game` from now on (or answers again).
pub fn refuse_inspection(game: &Path, refuse: bool) {
    let mut r = refusing().lock().unwrap();
    if refuse {
        r.insert(game.to_path_buf());
    } else {
        r.remove(game);
    }
}

// ---- the world ----

pub const PLAYER_DLL: &[u8] = b"the player's own dll";
pub const PLAYER_SF: &[u8] = br#"{"id":"players-own","name":"Mine","version":"1.0.0"}"#;
pub const PLAYER_CHAIN: &[u8] = b"[chain]\r\nplayer.dll\r\n";

pub struct World {
    pub _t: Option<Tmp>,
    pub root: PathBuf,
    pub dirs: DataDirs,
    pub game: PathBuf,
    pub inst: GameInstall,
    pub profile: String,
    /// The plugin loader (game_files), version 1 and 2.
    pub loader: Vec<LibraryRef>,
    /// The ShroudForge mod, version 1 and 2.
    pub sf: Vec<LibraryRef>,
    /// An EML package that needs ShroudForge's runtime.
    pub wardrobe: Option<LibraryRef>,
}

/// A plugin loader under the name the game loads itself, placed in the game folder (game_files).
pub fn loader_package(root: &Path, version: &str) -> PathBuf {
    let p = root.join(format!("src-loader-{version}/AutolootCoop {version}"));
    put(
        &p,
        "powrprof.dll",
        &build_test_dll(
            &["PowerGetActiveScheme", "PowerReadACValue"],
            format!("PluginLoader.log plugins\\ {version}").as_bytes(),
        ),
    );
    put(
        &p,
        "plugins/AutolootCoop.dll",
        &build_test_dll(&[], format!("AutolootCoop {version}").as_bytes()),
    );
    put(&p, "AutolootCoop.ini", b"[Autoloot]\nEnabled=1\n");
    p
}

/// A ShroudForge mod with a runtime and a setting (copied into <game>/mods on the interpreter's route).
pub fn sf_versioned(root: &Path, version: &str) -> PathBuf {
    let d = root.join(format!("src-sf-{version}/sfpack"));
    put(
        &d,
        "mod.json",
        format!(
            r#"{{"id":"sfpack","name":"sfpack","version":"{version}","capabilities":["runtime"]}}"#
        )
        .as_bytes(),
    );
    put(
        &d,
        "extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client","server"],
        "settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#,
    );
    put(
        &d,
        "src/mod.lua",
        format!("return {{ v = '{version}' }}").as_bytes(),
    );
    d
}

/// The install record of `game` (Z mode through its own prefix on Linux and macOS, native paths on Windows).
pub fn install_of(root: &Path, game: &Path) -> GameInstall {
    #[cfg(unix)]
    {
        install(root, game, PathMode::Z)
    }
    #[cfg(windows)]
    {
        use kiln_manage::game::{GameKind, Platform};
        let _ = root;
        GameInstall {
            id: format!("g-{}", game.file_name().unwrap().to_string_lossy()),
            kind: GameKind::Client,
            path: game.to_path_buf(),
            exe: "enshrouded.exe".into(),
            app_id: kiln_manage::CLIENT_APP_ID,
            library: None,
            platform: Platform::Windows,
            prefix: None,
            build_id: None,
            exe_sha256: None,
            steam_flavor: None,
            path_mode: PathMode::Native,
            source: "manual".into(),
        }
    }
}

/// A world in a fresh temporary folder; `eml`: the profile has the EML package that needs ShroudForge's runtime too.
pub fn world(tag: &str, eml: bool) -> World {
    fake_inspector();
    let t = tmp(&format!("v89-{tag}"));
    let root = t.0.clone();
    let dirs = DataDirs::at(root.join("data"));
    let game = game(&root.join("game"), true);
    select(&game, Some(KILN));
    put(&game, "player.dll", PLAYER_DLL);
    put(&game, "mods/players-own/mod.json", PLAYER_SF);
    put(&game, "kiln/chain.ini", PLAYER_CHAIN);
    let loader = ["1.6.0", "1.7.0"]
        .iter()
        .map(|v| import_kind(&dirs, &loader_package(&root, v), ModKind::Native))
        .collect();
    let sf = ["1.0.0", "2.0.0"]
        .iter()
        .map(|v| {
            import(
                &dirs,
                &sf_versioned(&root, v),
                ModKind::ShroudForge,
                "sfpack",
            )
        })
        .collect();
    let wardrobe = eml.then(|| {
        import(
            &dirs,
            &eml_runtime_package(&root, "wardrobe"),
            ModKind::Eml,
            "wardrobe",
        )
    });
    let inst = install_of(&root, &game);
    let mut w = World {
        _t: Some(t),
        root,
        dirs,
        game,
        inst,
        profile: String::new(),
        loader,
        sf,
        wardrobe,
    };
    let p = profile::create(&w.dirs, "kill", None).unwrap();
    w.profile = p.id.clone();
    std::fs::write(w.root.join("profile-id"), &w.profile).unwrap();
    std::fs::write(w.root.join("eml"), if eml { "1" } else { "0" }).unwrap();
    w.select(0);
    w
}

impl World {
    /// The child's view of a world the parent made.
    pub fn open(root: &Path) -> World {
        fake_inspector();
        let game = root.join("game");
        World {
            _t: None,
            root: root.to_path_buf(),
            dirs: DataDirs::at(root.join("data")),
            inst: install_of(root, &game),
            game,
            profile: std::fs::read_to_string(root.join("profile-id")).unwrap(),
            loader: Vec::new(),
            sf: Vec::new(),
            wardrobe: None,
        }
    }

    /// The profile with version `v` (0, 1) of the loader and the ShroudForge mod, both on (and the EML package).
    pub fn select(&self, v: usize) {
        let mut p = profile::get(&self.dirs, &self.profile).unwrap();
        p.mods.native = vec![pm(&self.loader[v], true)];
        p.mods.shroudforge = vec![pm(&self.sf[v], true)];
        p.mods.eml = self.wardrobe.iter().map(|r| pm(r, true)).collect();
        profile::save(&self.dirs, &p).unwrap();
    }

    pub fn file(&self, rel: &str) -> PathBuf {
        rel.split('/').fold(self.game.clone(), |p, c| p.join(c))
    }

    /// The game's profile file of `mode` as JSON (Null: none).
    pub fn doc(&self, mode: Mode) -> Value {
        let rel = match mode {
            Mode::Active => "kiln/profile.json",
            Mode::Once => "kiln/profile.once.json",
        };
        std::fs::read(self.file(rel))
            .ok()
            .map(|b| serde_json::from_slice(&b).unwrap())
            .unwrap_or(Value::Null)
    }

    /// The plans in kiln/launch-plans (their file names).
    pub fn plans(&self) -> BTreeSet<String> {
        std::fs::read_dir(self.file("kiln/launch-plans"))
            .into_iter()
            .flatten()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| !n.starts_with('.'))
            .collect()
    }
}

/// The plan the profile file `doc` pins, checked: there as a plain file with the pinned SHA-256, naming exactly this
/// profile file (its id, generation, mode and semantic hash). Its JSON.
pub fn pinned_plan(game: &Path, doc: &Value, mode: Mode) -> Value {
    let pin = &doc["sfLaunchPlan"];
    let rel = pin["path"]
        .as_str()
        .unwrap_or_else(|| panic!("no plan pinned: {doc}"));
    let bytes = std::fs::read(rel.split('/').fold(game.join("kiln"), |p, c| p.join(c))).unwrap();
    assert_eq!(
        pin["sha256"].as_str().unwrap(),
        kiln_manage::hash::sha256_hex(&bytes),
        "the pinned bytes"
    );
    let plan: Value = serde_json::from_slice(&bytes).unwrap();
    let ident = ProfileIdentity::of(doc, mode).unwrap();
    let want = json!({"id": ident.id, "generation": ident.generation, "mode": match mode { Mode::Active => "active", Mode::Once => "once" },
        "semantic_sha256": ident.semantic_sha256});
    assert_eq!(plan["profile"], want, "the plan names this profile file");
    plan
}

// ---- what the game holds ----

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameState {
    pub profile: Option<Vec<u8>>,
    pub once: Option<Vec<u8>>,
    pub chain: Option<Vec<u8>>,
    pub placements: Option<Vec<u8>>,
    /// game_files' receipts (id, version, hash), and whether a change is pending.
    pub game_files: Vec<(String, String, String)>,
    pub pending: bool,
    pub plans: BTreeSet<String>,
    pub player: (Vec<u8>, Vec<u8>),
    pub journal: bool,
    /// The folders in <game>/mods (Kiln's temporary folders aside) and the version of Kiln's copy in each.
    pub copies: BTreeMap<String, Option<String>>,
}

impl GameState {
    pub fn of(w: &World) -> GameState {
        let read = |rel: &str| std::fs::read(w.file(rel)).ok();
        let gf: Value = read("kiln/state/game-files.json")
            .map(|b| serde_json::from_slice(&b).unwrap())
            .unwrap_or(Value::Null);
        let mut game_files: Vec<_> = gf["mods"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|m| {
                (
                    m["id"].as_str().unwrap().to_owned(),
                    m["version"].as_str().unwrap().to_owned(),
                    m["hash"].as_str().unwrap().to_owned(),
                )
            })
            .collect();
        game_files.sort();
        GameState {
            profile: read("kiln/profile.json"),
            once: read("kiln/profile.once.json"),
            chain: read("kiln/chain.ini"),
            placements: read("kiln/state/library-placements.json"),
            game_files,
            pending: gf["pending"].as_array().is_some_and(|p| !p.is_empty()),
            plans: w.plans(),
            player: (
                read("player.dll").unwrap(),
                read("mods/players-own/mod.json").unwrap(),
            ),
            journal: w.file(sfi_activation::JOURNAL).exists(),
            copies: std::fs::read_dir(w.file("mods"))
                .into_iter()
                .flatten()
                .flatten()
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .filter(|n| !n.starts_with('.'))
                .map(|n| {
                    let v = kiln_manage::engine_mods::marker(&w.file(&format!("mods/{n}")))
                        .map(|m| m.version);
                    (n, v)
                })
                .collect(),
        }
    }

    pub fn generation(&self) -> u64 {
        self.profile
            .as_ref()
            .map(|b| {
                serde_json::from_slice::<Value>(b).unwrap()["generation"]
                    .as_u64()
                    .unwrap()
            })
            .unwrap_or(0)
    }
}

pub fn outcome(r: &Recovered) -> &'static str {
    match r {
        Recovered::Nothing => "nothing",
        Recovered::Completed { .. } => "completed",
        Recovered::RolledBack { .. } => "rolled-back",
        Recovered::Refused { .. } => "refused",
    }
}

/// Recovers twice: the second finds nothing more to do and changes nothing. The first outcome.
pub fn recover_twice(w: &World, what: &str) -> Recovered {
    let r1 = sfi_activation::recover(&w.dirs, &w.game).unwrap_or_else(|e| panic!("{what}: {e:?}"));
    let s1 = snapshot(&w.game);
    let r2 = sfi_activation::recover(&w.dirs, &w.game).unwrap();
    assert_eq!(
        snapshot(&w.game),
        s1,
        "{what}: the second recovery changed something ({r1:?}, then {r2:?})"
    );
    match &r1 {
        Recovered::Refused { reason, .. } => {
            assert_eq!(
                r2,
                Recovered::Refused {
                    reason: reason.clone(),
                    notes: Vec::new()
                },
                "{what}"
            )
        }
        _ => assert_eq!(r2, Recovered::Nothing, "{what}"),
    }
    r1
}

/// What one recovery of an interrupted operation on the profile file of `mode` left, against the state before
/// (`before`): the earlier state whole, the new one whole, or (refused) the earlier one with the copies changed and the
/// planned start refused. A profile file never pins a plan of another state.
pub fn assert_settled(w: &World, before: &GameState, r: &Recovered, mode: Mode, what: &str) {
    let now = GameState::of(w);
    assert_eq!(now.player, before.player, "{what}: the player's files");
    assert!(!now.pending, "{what}: game_files settled its intents");
    let check = sfi_activation::start_check_mode(&w.game, mode);
    let (file_now, file_before) = match mode {
        Mode::Active => (&now.profile, &before.profile),
        Mode::Once => (&now.once, &before.once),
    };
    if file_now != file_before {
        assert!(
            matches!(r, Recovered::Completed { .. } | Recovered::Nothing),
            "{what}: {r:?}"
        );
        assert!(
            !now.journal && check.is_none(),
            "{what}: the new state, whole: {check:?}"
        );
        let doc = w.doc(mode);
        if doc.get("sfLaunchPlan").is_some() {
            pinned_plan(&w.game, &doc, mode);
        }
        return;
    }
    // the plan the profile file pinned before still holds it
    if !before.journal
        && file_before.is_some()
        && w.doc(mode).get("sfLaunchPlan").is_some()
        && !now.journal
    {
        pinned_plan(&w.game, &w.doc(mode), mode);
    }
    assert_eq!(now.chain, before.chain, "{what}: chain.ini put back");
    assert_eq!(
        now.game_files, before.game_files,
        "{what}: the earlier game-folder files"
    );
    assert_eq!(
        now.plans, before.plans,
        "{what}: the plan this operation published goes, every other stays"
    );
    match r {
        Recovered::RolledBack { .. } | Recovered::Nothing => {
            assert!(!now.journal, "{what}");
            assert_eq!(
                now.placements, before.placements,
                "{what}: the earlier receipts"
            );
            assert_eq!(now.copies, before.copies, "{what}: the earlier copies");
            assert_eq!(
                check,
                sfi_activation::start_check_mode(&w.game, mode),
                "{what}"
            );
            if file_before.is_some() {
                assert!(
                    check.is_none(),
                    "{what}: the earlier profile file and its plan: {check:?}"
                );
            }
        }
        Recovered::Refused { reason, .. } => {
            assert!(
                now.journal,
                "{what}: the refusal stays until the next activation"
            );
            assert_ne!(
                now.placements, before.placements,
                "{what}: refused only when the copies changed: {reason}"
            );
            let check = check.unwrap_or_else(|| panic!("{what}: the planned start is refused"));
            assert!(
                check.starts_with("ShroudForge-format mods do not start"),
                "{check}"
            );
        }
        other => panic!("{what}: {other:?}"),
    }
}

// ---- the kill harness: this test binary again, stopped at one boundary ----

pub const CHILD: &str = "KILN_SFI_V89_CHILD";
/// The child's exit code when it never reached its boundary.
pub const NOT_REACHED: i32 = 3;
#[cfg(windows)]
pub const KILLED: u32 = 0xDEAD;

static TARGET: OnceLock<Boundary> = OnceLock::new();

std::thread_local! {
    /// The boundaries this thread passed while it records them.
    static PASSED: std::cell::RefCell<Option<Vec<Boundary>>> = const { std::cell::RefCell::new(None) };
}

fn die() -> ! {
    #[cfg(unix)]
    unsafe {
        libc::kill(libc::getpid(), libc::SIGKILL);
    }
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::Threading::{GetCurrentProcess, TerminateProcess};
        TerminateProcess(GetCurrentProcess(), KILLED);
    }
    loop {
        std::thread::park();
    }
}

fn hook(b: Boundary) {
    if TARGET.get() == Some(&b) {
        die();
    }
    PASSED.with(|p| {
        if let Some(v) = p.borrow_mut().as_mut() {
            v.push(b);
        }
    });
}

/// The boundaries `f` passes on this thread, in order (the crash-test hook records them).
pub fn passed<T>(f: impl FnOnce() -> T) -> (T, Vec<Boundary>) {
    let _ = sfi_activation::set_crash_test_hook(hook);
    PASSED.with(|p| *p.borrow_mut() = Some(Vec::new()));
    let out = f();
    let seen = PASSED.with(|p| p.borrow_mut().take()).unwrap_or_default();
    (out, seen)
}

/// The child's side (called by each test binary's `kill_child` test): `<root>\n<boundary>\n<operation>` in [`CHILD`],
/// the operation run by `run` on the world at `<root>` until it stops at the boundary.
pub fn child(all: &[Boundary], run: impl FnOnce(&World, &str)) {
    let Ok(spec) = std::env::var(CHILD) else {
        return;
    };
    let mut lines = spec.lines();
    let (root, at, what) = (
        lines.next().unwrap(),
        lines.next().unwrap(),
        lines.next().unwrap(),
    );
    let b = *all
        .iter()
        .find(|b| format!("{b:?}") == at)
        .unwrap_or_else(|| panic!("{at}"));
    TARGET.set(b).unwrap();
    assert!(sfi_activation::set_crash_test_hook(hook));
    let w = World::open(Path::new(root));
    run(&w, what);
    std::process::exit(NOT_REACHED);
}

/// Runs this binary's `kill_child` on `w` until it kills itself at `b` running `what`: true, or false when it never got
/// there. Waits at most 300 s.
pub fn run_child(w: &World, b: Boundary, what: &str) -> bool {
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["kill_child", "--exact", "--nocapture", "--test-threads=1"])
        .env(CHILD, format!("{}\n{b:?}\n{what}", w.root.display()))
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let start = Instant::now();
    let status = loop {
        if let Some(s) = child.try_wait().unwrap() {
            break s;
        }
        if start.elapsed() > Duration::from_secs(300) {
            let _ = child.kill();
            panic!("{b:?}: the child did not end in 300 s");
        }
        std::thread::sleep(Duration::from_millis(10));
    };
    let mut err = String::new();
    use std::io::Read as _;
    child
        .stderr
        .take()
        .unwrap()
        .read_to_string(&mut err)
        .unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt as _;
        if status.signal() == Some(libc::SIGKILL) {
            return true;
        }
    }
    #[cfg(windows)]
    if status.code() == Some(KILLED as i32) {
        return true;
    }
    assert_eq!(
        status.code(),
        Some(NOT_REACHED),
        "{b:?} {what}: {status:?}\n{err}"
    );
    false
}

/// Each boundary once, in the order first passed.
pub fn first_passed(seen: &[Boundary]) -> Vec<Boundary> {
    let mut out = Vec::new();
    for b in seen {
        if !out.contains(b) {
            out.push(*b);
        }
    }
    out
}

// ---- uninstalling Kiln over an unfinished activation ----

/// Kiln's install receipt in the world's game folder (no file of its own to remove), so `install::uninstall_report`
/// runs there.
pub fn kiln_receipt(w: &World) {
    put(
        &w.game,
        "kiln/.install.json",
        br#"{"format":1,"installId":"sfi-v89fu","installed":"2026-11-20T00:00:00Z","runtime":{"version":"0.0.0-test"},"side":"client","files":[]}"#,
    );
}

/// Uninstalls Kiln from the world's game folder: the report's warnings.
pub fn uninstall(w: &World, what: &str) -> Vec<String> {
    kiln_manage::install::uninstall_report(&w.dirs, &w.inst, &mut |_: Option<f32>, _: &str| {})
        .unwrap_or_else(|e| panic!("{what}: uninstall: {e:?}"))
        .warnings
}

/// `text` with the world's folder (host, Windows, Z: and JSON-escaped spellings) as `<T>`.
fn norm(text: &str, t: &Path) -> String {
    let host = t.to_string_lossy().into_owned();
    let plain = [
        format!("Z:{}", host.replace('/', "\\")),
        host.clone(),
        host.replace('\\', "/"),
    ];
    // the random name of engine_mods' parking folder
    let mut out = String::new();
    let mut rest = text;
    while let Some(i) = rest.find(".kiln-place-") {
        let (head, tail) = rest.split_at(i + ".kiln-place-".len());
        out.push_str(head);
        let n = tail.bytes().take_while(u8::is_ascii_hexdigit).count();
        out.push_str(if n > 0 { "X" } else { "" });
        rest = &tail[n..];
    }
    out.push_str(rest);
    for f in plain
        .iter()
        .map(|f| f.replace('\\', "\\\\"))
        .chain(plain.iter().cloned())
    {
        if !f.is_empty() {
            out = out.replace(&f, "<T>");
        }
    }
    out
}

/// A JSON file of the game folder without the members that differ between two runs of the same steps (times, random
/// ids, the plan reference with its random plan id, fingerprints over host paths), with `<T>`; else its text.
fn left_file(w: &World, rel: &str) -> Option<String> {
    fn strip(v: &mut Value) {
        match v {
            Value::Object(m) => {
                for k in [
                    "activated",
                    "updated",
                    "created",
                    "written",
                    "fingerprint",
                    "time",
                    "at",
                    "stamp",
                    "pid",
                    "launchId",
                    "placed",
                    "txn",
                    "sfLaunchPlan",
                ] {
                    m.remove(k);
                }
                m.values_mut().for_each(strip);
            }
            Value::Array(a) => a.iter_mut().for_each(strip),
            _ => {}
        }
    }
    let bytes = std::fs::read(w.file(rel)).ok()?;
    Some(match serde_json::from_slice::<Value>(&bytes) {
        Ok(mut v) => {
            if rel == sfi_activation::JOURNAL {
                // the journal's state, not its hashes of files that hold times or its random plan name
                v = json!({"state": v["state"], "refused": v["refused"], "generation": v["generation"],
                    "profileId": v["profileId"], "route": v["route"], "mode": v["mode"],
                    "stages": v["stages"].as_array().map(|a| a.iter().map(|s| s["stage"].clone()).collect::<Vec<_>>())});
            }
            strip(&mut v);
            norm(&v.to_string(), &w.root)
        }
        Err(_) => norm(&String::from_utf8_lossy(&bytes), &w.root),
    })
}

/// What an uninstall left in the game folder, comparable between two worlds that ran the same steps.
#[derive(Debug, PartialEq, Eq)]
pub struct Left {
    pub files: BTreeMap<String, Option<String>>,
    pub pinned: (bool, bool),
    pub plans: usize,
    pub state: GameStateLeft,
    /// Everything left under `<game>/mods` that is not a package folder (engine_mods' parking and staging folders).
    pub hidden: Vec<String>,
}

/// [`GameState`] without its bytes that hold times or host paths.
#[derive(Debug, PartialEq, Eq)]
pub struct GameStateLeft {
    pub game_files: Vec<(String, String, String)>,
    pub pending: bool,
    pub player: (Vec<u8>, Vec<u8>),
    pub journal: bool,
    pub copies: BTreeMap<String, Option<String>>,
}

/// The entries under `<game>/mods` whose names start with a dot, each file and folder in them, with `<T>`.
pub fn hidden(w: &World) -> Vec<String> {
    fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
        for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            out.push(
                e.path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/"),
            );
            if e.file_type().is_ok_and(|t| t.is_dir()) {
                walk(root, &e.path(), out);
            }
        }
    }
    let mods = w.file("mods");
    let mut out = Vec::new();
    for e in std::fs::read_dir(&mods).into_iter().flatten().flatten() {
        if e.file_name().to_string_lossy().starts_with('.') {
            out.push(e.file_name().to_string_lossy().into_owned());
            walk(&mods, &e.path(), &mut out);
        }
    }
    let mut out: Vec<String> = out.iter().map(|p| norm(p, &w.root)).collect();
    out.sort();
    out
}

impl Left {
    pub fn of(w: &World) -> Left {
        let s = GameState::of(w);
        Left {
            files: [
                "kiln/profile.json",
                "kiln/profile.once.json",
                "kiln/chain.ini",
                "kiln/chain/once.ini",
                "kiln/state/library-placements.json",
                "kiln/state/placement-journal.json",
                "kiln/state/game-files.json",
                sfi_activation::JOURNAL,
                "kiln/.install.json",
            ]
            .into_iter()
            .map(|rel| (rel.to_owned(), left_file(w, rel)))
            .collect(),
            pinned: (
                w.doc(Mode::Active).get("sfLaunchPlan").is_some(),
                w.doc(Mode::Once).get("sfLaunchPlan").is_some(),
            ),
            plans: s.plans.len(),
            state: GameStateLeft {
                game_files: s.game_files,
                pending: s.pending,
                player: s.player,
                journal: s.journal,
                copies: s.copies,
            },
            hidden: hidden(w),
        }
    }
}

/// One boundary of an uninstall matrix: `a` and `b` were stopped at the same boundary of the same operation (`before`:
/// their state before it). `a` is uninstalled as it is: the uninstall settles the journal itself. `b` is recovered first,
/// as the next activation recovers it, then uninstalled. Both leave the same game folder; Kiln's copies and game-folder
/// files are gone whole (none half placed, none left as a folder of the player's), the player's files are as they were,
/// and no journal is left, a refused one neither (it goes with Kiln, as a deactivation removes it: a reinstall does not
/// start with the planned start refused). The recovery's outcome.
pub fn uninstall_settles_as_the_next_activation(
    a: &World,
    b: &World,
    before: &GameState,
    what: &str,
) -> Recovered {
    let notes = uninstall(a, what);
    let r = sfi_activation::recover(&b.dirs, &b.game).unwrap_or_else(|e| panic!("{what}: {e:?}"));
    let _ = uninstall(b, what);
    assert_eq!(
        Left::of(a),
        Left::of(b),
        "{what}: the uninstall settles as the next activation does ({r:?})"
    );
    let s = GameState::of(a);
    assert_eq!(s.player, before.player, "{what}: the player's files");
    assert!(
        s.game_files.is_empty() && !s.pending,
        "{what}: the game-folder files went whole: {s:?}"
    );
    assert_eq!(
        s.copies,
        BTreeMap::from([("players-own".to_owned(), None)]),
        "{what}: Kiln's copies went, the player's own folder stays"
    );
    assert!(!s.journal, "{what}: no journal left ({r:?})");
    assert!(
        sfi_activation::start_check(&a.game).is_none_or(|why| !why.contains("did not finish")),
        "{what}: a reinstall does not start refused"
    );
    let again = sfi_activation::recover(&a.dirs, &a.game).unwrap();
    assert_eq!(outcome(&again), "nothing", "{what}: nothing left to settle");
    if matches!(r, Recovered::RolledBack { .. }) {
        assert!(
            notes.iter().any(|n| n.contains("did not finish")),
            "{what}: the uninstall says what it settled: {notes:?}"
        );
    }
    r
}

// ---- a plan that cannot be written ----

/// From now on no plan can be written in the world's game folder: kiln/launch-plans is moved aside (the plans it holds
/// with it) and a plain file takes its name (the plan writer refuses a plans folder that is no plain folder). No input of
/// a plan changes. [`unblock_plans`] puts the folder back.
pub fn block_plans(w: &World) {
    let dir = w.file("kiln/launch-plans");
    if dir.exists() {
        std::fs::rename(&dir, w.file("kiln/launch-plans.aside")).unwrap();
    }
    std::fs::write(&dir, b"not a folder").unwrap();
}

/// Undoes [`block_plans`].
pub fn unblock_plans(w: &World) {
    std::fs::remove_file(w.file("kiln/launch-plans")).unwrap();
    let aside = w.file("kiln/launch-plans.aside");
    if aside.exists() {
        std::fs::rename(aside, w.file("kiln/launch-plans")).unwrap();
    }
}
