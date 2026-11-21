// SPDX-License-Identifier: MIT
//! The activation journal of a kiln-mode activation, through a driver that runs the steps in the order
//! the profile code wires them into `profile::activate`: `sfi_activation::begin`, `game_files::place` (a plugin loader in the game
//! folder), `chain::write_profile_plan_staged`, `engine_mods::place_routed` (a ShroudForge mod on the interpreter's
//! route), the launch plan published through `Staging::write_plan`, and the profile committed last.
//!
//! - **Real process kills**: a child process (this test binary again) runs the activation, or a recovery, and kills
//!   itself (SIGKILL, TerminateProcess) at one boundary, every boundary in turn, game_files' own intent and commit and
//!   engine_mods' own publication steps included, for a first activation and for a replacement activation. The parent
//!   then recovers twice: the first recovery ends in the earlier activation whole or the new one whole (or a refusal of
//!   the ShroudForge family's planned start, when the copies changed), the second changes nothing, the player's files
//!   are never touched, and a profile never pins a plan of another state.
//! - In-process errors abort to the same states; the commit refuses a profile that pins another plan or generation.
//! - The lock serializes activations and recoveries; links and foreign temporary files are never followed or replaced;
//!   a plan name that is taken is never written over; an orphan plan is harmless; a stale plan fails the planned start
//!   of ShroudForge-format mods only; hand-edited files are kept, with instructions.
//!
//! Every package here is synthetic.

mod sfi_routing_common;

use kiln_manage::library::LibraryRef;
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile;
use kiln_manage::sfi_activation::{self, Begin, Boundary, Recovered};
use kiln_manage::sfi_plan::{Mode, ProfileIdentity};
use kiln_manage::{chain, engine_mods, game_files, sfi_routing};
use serde_json::{Value, json};
use sfi_routing_common::*;
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

// ---- the world ----

struct World {
    _t: Option<Tmp>,
    root: PathBuf,
    dirs: DataDirs,
    game: PathBuf,
    profile: String,
    /// The plugin loader (game_files) and the ShroudForge mod, version 1 and 2.
    loader: Vec<LibraryRef>,
    sf: Vec<LibraryRef>,
}

const PLAYER_DLL: &[u8] = b"the player's own dll";
const PLAYER_SF: &[u8] = br#"{"id":"players-own","name":"Mine","version":"1.0.0"}"#;
const PLAYER_CHAIN: &[u8] = b"[chain]\r\nplayer.dll\r\n";

/// A plugin loader under the name the game loads itself, placed in the game folder (game_files).
fn loader_package(root: &Path, version: &str) -> PathBuf {
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

/// A ShroudForge mod (copied into <game>/mods on the interpreter's route).
fn sf_versioned(root: &Path, version: &str) -> PathBuf {
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
        br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"settings":{}}"#,
    );
    put(
        &d,
        "src/mod.lua",
        format!("return {{ v = '{version}' }}").as_bytes(),
    );
    d
}

fn world(tag: &str) -> World {
    let t = tmp(&format!("act-{tag}"));
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
    let mut w = World {
        _t: Some(t),
        root,
        dirs,
        game,
        profile: String::new(),
        loader,
        sf,
    };
    let p = profile::create(&w.dirs, "kill", None).unwrap();
    w.profile = p.id.clone();
    std::fs::write(w.root.join("profile-id"), &w.profile).unwrap();
    w.select(0);
    w
}

impl World {
    /// The child's view of a world the parent made.
    fn open(root: &Path) -> World {
        World {
            _t: None,
            root: root.to_path_buf(),
            dirs: DataDirs::at(root.join("data")),
            game: root.join("game"),
            profile: std::fs::read_to_string(root.join("profile-id")).unwrap(),
            loader: Vec::new(),
            sf: Vec::new(),
        }
    }

    /// The profile with version \`v\` (0, 1) of the loader and the ShroudForge mod, both on.
    fn select(&self, v: usize) {
        let mut p = profile::get(&self.dirs, &self.profile).unwrap();
        p.mods.native = vec![pm(&self.loader[v], true)];
        p.mods.shroudforge = vec![pm(&self.sf[v], true)];
        profile::save(&self.dirs, &p).unwrap();
    }

    fn file(&self, rel: &str) -> PathBuf {
        rel.split('/').fold(self.game.clone(), |p, c| p.join(c))
    }
}

/// The new kiln/profile.json: what it selects (a profile.json shape the readers take).
fn profile_doc(p: &profile::Profile, generation: u64) -> Value {
    let list = |v: &[profile::ProfileMod]| {
        v.iter()
            .map(|m| format!("{}@{}", m.id, m.version))
            .collect::<Vec<_>>()
    };
    json!({"format": 1, "id": p.id, "generation": generation, "mods": {},
        "selection": {"native": list(&p.mods.native), "shroudforge": list(&p.mods.shroudforge)}})
}

/// A launch plan naming the profile it is made for (what the journal and the start check read of a plan).
fn plan_doc(id: &str, ident: &ProfileIdentity) -> Vec<u8> {
    let mut b = serde_json::to_vec_pretty(&json!({"format": 1, "plan_id": id, "profile": {"id": ident.id,
        "generation": ident.generation, "mode": "active", "semantic_sha256": ident.semantic_sha256}})).unwrap();
    b.push(b'\n');
    b
}

fn pretty(v: &Value) -> Vec<u8> {
    let mut b = serde_json::to_vec_pretty(v).unwrap();
    b.push(b'\n');
    b
}

/// Where the driver stops with an error (in this process).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FailAfter {
    GameFiles,
    Chain,
    Placements,
    Plan,
}

/// One kiln-mode activation, in `profile::activate`'s order.
fn activate(w: &World) -> kiln_manage::Result<sfi_activation::Committed> {
    activate_with(w, None, |_, doc| pretty(doc))
}

fn activate_with(
    w: &World,
    fail: Option<FailAfter>,
    profile_bytes: impl FnOnce(&sfi_activation::Committed, &Value) -> Vec<u8>,
) -> kiln_manage::Result<sfi_activation::Committed> {
    let p = profile::get(&w.dirs, &w.profile)?;
    let route = sfi_routing::route_of(&w.game);
    assert!(route.runs(), "{route:?}");
    let generation = profile::read_game_profile(&w.game)?
        .map(|f| f.generation)
        .unwrap_or(0)
        + 1;
    let (mut tx, _) = sfi_activation::begin(
        &w.dirs,
        &w.game,
        Begin {
            profile_id: &p.id,
            generation,
            route: &route,
        },
    )?;
    let injected = |at| {
        if fail == Some(at) {
            Err(kiln_manage::Error::invalid(format!(
                "injected after {at:?}"
            )))
        } else {
            Ok(())
        }
    };
    let staged = (|| {
        tx.game_files(|| game_files::place(&w.dirs, &p, &w.game))?;
        injected(FailAfter::GameFiles)?;
        let plan = chain::ProfilePlan {
            profile: vec![format!("..\\gen{generation}.dll")],
            ..Default::default()
        };
        tx.chain(|stage| chain::write_profile_plan_staged(&w.game, &plan, stage))?;
        injected(FailAfter::Chain)?;
        let route = tx.route();
        tx.placements(|| engine_mods::place_routed(&w.dirs, &p, &w.game, &route))?;
        injected(FailAfter::Placements)?;
        let mut doc = profile_doc(&p, generation);
        let ident = ProfileIdentity::of(&doc, Mode::Active).map_err(kiln_manage::Error::invalid)?;
        let id = kiln_manage::util::uuid_v4()?;
        let r = tx
            .write_plan(&id, &plan_doc(&id, &ident))
            .map_err(kiln_manage::Error::invalid)?
            .expect("a fresh plan id");
        injected(FailAfter::Plan)?;
        doc["sfLaunchPlan"] = r.profile_value();
        Ok((doc, r))
    })();
    match staged {
        Ok((doc, r)) => {
            let bytes = profile_bytes(
                &sfi_activation::Committed {
                    generation,
                    plan: Some(r),
                },
                &doc,
            );
            tx.commit(&bytes)
        }
        Err(e) => {
            tx.abort()?;
            Err(e)
        }
    }
}

// ---- what the game holds ----

#[derive(Debug, Clone, PartialEq, Eq)]
struct GameState {
    profile: Option<Vec<u8>>,
    chain: Option<Vec<u8>>,
    placements: Option<Vec<u8>>,
    /// game_files' receipts: (id, version, hash) placed, and whether a change is pending.
    game_files: Vec<(String, String, String)>,
    pending: bool,
    plans: BTreeSet<String>,
    player: (Vec<u8>, Vec<u8>),
    journal: bool,
    /// The folders in <game>/mods (Kiln's temporary folders aside) and the version of Kiln's copy in each (None: no
    /// marker, a folder of the player's).
    copies: std::collections::BTreeMap<String, Option<String>>,
}

impl GameState {
    fn of(w: &World) -> GameState {
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
        let plans = std::fs::read_dir(w.file("kiln/launch-plans"))
            .into_iter()
            .flatten()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        GameState {
            profile: read("kiln/profile.json"),
            chain: read("kiln/chain.ini"),
            placements: read("kiln/state/library-placements.json"),
            game_files,
            pending: gf["pending"].as_array().is_some_and(|p| !p.is_empty()),
            plans,
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
                    let v = engine_mods::marker(&w.file(&format!("mods/{n}"))).map(|m| m.version);
                    (n, v)
                })
                .collect(),
        }
    }

    fn generation(&self) -> u64 {
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

/// The SHA-256 of a file's bytes, for messages.
fn short(b: &Option<Vec<u8>>) -> String {
    b.as_ref()
        .map(|b| kiln_manage::hash::sha256_hex(b)[..12].to_owned())
        .unwrap_or_else(|| "none".into())
}

/// What one recovery left, against the state before the activation (\`before\`): the earlier activation whole, or the
/// new one whole, or (\`refused\`) the earlier one with the copies of ShroudForge-format mods changed and the planned
/// start refused. A profile never pins a plan of another state.
fn assert_settled(w: &World, before: &GameState, r: &Recovered, what: &str) {
    let now = GameState::of(w);
    assert_eq!(now.player, before.player, "{what}: the player's files");
    assert!(!now.pending, "{what}: game_files settled its intents");
    // a profile never pins a plan of another state (start_check reads the pin, the plan's bytes and its profile)
    let check = sfi_activation::start_check(&w.game);
    if now.profile != before.profile {
        assert!(
            matches!(r, Recovered::Completed { .. } | Recovered::Nothing),
            "{what}: {r:?}"
        );
        assert_eq!(now.generation(), before.generation() + 1, "{what}");
        assert!(
            !now.journal && check.is_none(),
            "{what}: the new activation, whole: {check:?}"
        );
        assert_ne!(now.chain, before.chain, "{what}: the new chain.ini");
        assert_eq!(
            now.plans.len(),
            before.plans.len() + 1,
            "{what}: {:?} {:?}",
            now.plans,
            before.plans
        );
        return;
    }
    assert_eq!(
        now.chain,
        before.chain,
        "{what}: chain.ini put back ({} -> {})",
        short(&before.chain),
        short(&now.chain)
    );
    assert_eq!(
        now.game_files, before.game_files,
        "{what}: the earlier game-folder files"
    );
    assert_eq!(
        now.plans, before.plans,
        "{what}: the plan this activation published goes, every other stays"
    );
    match r {
        Recovered::RolledBack { .. } => {
            assert!(!now.journal, "{what}");
            assert_eq!(
                now.placements, before.placements,
                "{what}: the earlier receipts"
            );
            assert_eq!(
                now.copies, before.copies,
                "{what}: the earlier copies, and no copy without its receipt"
            );
            assert_eq!(check, sfi_activation::start_check(&w.game), "{what}");
            if before.profile.is_some() {
                assert!(
                    check.is_none(),
                    "{what}: the earlier profile and its plan: {check:?}"
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
                check.starts_with("ShroudForge-format mods do not start")
                    && check.contains("Every other mod starts"),
                "{check}"
            );
        }
        other => panic!("{what}: {other:?}"),
    }
}

// ---- the child: this binary again, stopped at one boundary ----

const CHILD: &str = "KILN_SFI_ACTIVATION_CHILD";
/// The child's exit code when it never reached its boundary.
const NOT_REACHED: i32 = 3;
#[cfg(windows)]
const KILLED: u32 = 0xDEAD;

static TARGET: OnceLock<Boundary> = OnceLock::new();

const BOUNDARIES: [Boundary; 21] = [
    Boundary::JournalBegun,
    Boundary::GameFilesStaged,
    Boundary::GameFilesIntent,
    Boundary::GameFilesOp,
    Boundary::GameFilesCommitted,
    Boundary::ChainStaged,
    Boundary::ChainWritten,
    Boundary::PlacementsStaged,
    Boundary::PlacementParked,
    Boundary::PlacementPublished,
    Boundary::PlacementReceipt,
    Boundary::PlacementsDone,
    Boundary::PlanStaged,
    Boundary::PlanWritten,
    Boundary::ProfileStaged,
    Boundary::ProfileWritten,
    Boundary::Committed,
    Boundary::RecoverPlanRemoved,
    Boundary::RecoverChainRestored,
    Boundary::RecoverGameFilesRestored,
    Boundary::RecoverSettled,
];

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
}

/// The child (a no-op in a normal run): `<root>\n<boundary>\n<activate|recover>` in [`CHILD`].
#[test]
fn kill_child() {
    let Ok(spec) = std::env::var(CHILD) else {
        return;
    };
    let mut lines = spec.lines();
    let (root, at, what) = (
        lines.next().unwrap(),
        lines.next().unwrap(),
        lines.next().unwrap(),
    );
    let b = *BOUNDARIES
        .iter()
        .find(|b| format!("{b:?}") == at)
        .unwrap_or_else(|| panic!("{at}"));
    TARGET.set(b).unwrap();
    assert!(sfi_activation::set_crash_test_hook(hook));
    let w = World::open(Path::new(root));
    match what {
        "activate" => drop(activate(&w)),
        "recover" => drop(sfi_activation::recover(&w.dirs, &w.game)),
        other => panic!("{other}"),
    }
    std::process::exit(NOT_REACHED);
}

/// Runs the child on \`w\` until it kills itself at \`b\`: true, or false when it never got there. Waits at most 300 s.
fn run_child(w: &World, b: Boundary, what: &str) -> bool {
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

/// Recovers twice: the second finds nothing more to do and changes nothing. Returns the first outcome.
fn recover_twice(w: &World, what: &str) -> Recovered {
    let r1 = sfi_activation::recover(&w.dirs, &w.game).unwrap_or_else(|e| panic!("{what}: {e:?}"));
    let s1 = snapshot(&w.game);
    let r2 = sfi_activation::recover(&w.dirs, &w.game).unwrap();
    assert_eq!(
        snapshot(&w.game),
        s1,
        "{what}: the second recovery changed something ({r1:?}, then {r2:?})"
    );
    match &r1 {
        Recovered::Refused { reason, .. } => assert_eq!(
            r2,
            Recovered::Refused {
                reason: reason.clone(),
                notes: Vec::new()
            },
            "{what}"
        ),
        _ => assert_eq!(r2, Recovered::Nothing, "{what}"),
    }
    r1
}

/// What the first recovery after a kill at \`b\` is, for a replacement (\`first\`: false) or a first activation.
fn expected(b: Boundary, first: bool) -> &'static str {
    use Boundary::*;
    match b {
        ProfileWritten => "completed",
        Committed => "nothing",
        // the copy of the ShroudForge mod is in the game under its new receipt: the earlier plan no longer describes it
        PlacementReceipt | PlacementsDone | PlanStaged | PlanWritten | ProfileStaged => "refused",
        // a replaced copy published before its receipt: engine_mods finishes it, the copies changed. A first copy
        // published before its receipt is taken out again (engine_mods::recover_placements): copies and receipts as before
        PlacementPublished if !first => "refused",
        _ => "rolled-back",
    }
}

fn outcome(r: &Recovered) -> &'static str {
    match r {
        Recovered::Nothing => "nothing",
        Recovered::Completed { .. } => "completed",
        Recovered::RolledBack { .. } => "rolled-back",
        Recovered::Refused { .. } => "refused",
    }
}

fn kill_matrix(first: bool) {
    for b in &BOUNDARIES[..17] {
        let what = format!(
            "{} activation killed at {b:?}",
            if first { "first" } else { "replacement" }
        );
        let w = world(&format!("{}-{b:?}", if first { "first" } else { "repl" }));
        if !first {
            activate(&w).unwrap();
            w.select(1);
        }
        let before = GameState::of(&w);
        assert!(run_child(&w, *b, "activate"), "{what}: never reached");
        let r = recover_twice(&w, &what);
        assert_eq!(outcome(&r), expected(*b, first), "{what}: {r:#?}");
        assert_settled(&w, &before, &r, &what);
        if !first && outcome(&r) == "rolled-back" {
            let m = engine_mods::marker(&w.file("mods/sfpack")).unwrap();
            assert_eq!(m.version, "1.0.0", "{what}: the earlier copy is back");
        }
        // the next activation is the explicit recovery: it commits whole
        let c = activate(&w).unwrap_or_else(|e| panic!("{what}: activating again: {e:?}"));
        assert_eq!(c.generation, GameState::of(&w).generation());
        assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
        assert_eq!(
            sfi_activation::recover(&w.dirs, &w.game).unwrap(),
            Recovered::Nothing
        );
        assert!(!w.file(sfi_activation::JOURNAL).exists());
    }
}

#[test]
fn a_first_activation_killed_at_every_boundary_recovers_to_the_earlier_or_the_new_state() {
    kill_matrix(true);
}

#[test]
fn a_replacement_activation_killed_at_every_boundary_recovers_to_the_earlier_or_the_new_state() {
    kill_matrix(false);
}

/// A recovery killed at each of its own steps (game_files' intent and commit while it puts the earlier files back
/// among them) is finished by the next one, to the same end.
#[test]
fn a_recovery_killed_at_every_step_is_finished_by_the_next() {
    use Boundary::*;
    for b in [
        RecoverPlanRemoved,
        RecoverChainRestored,
        GameFilesIntent,
        GameFilesOp,
        GameFilesCommitted,
        RecoverGameFilesRestored,
        RecoverSettled,
    ] {
        let what = format!("recovery killed at {b:?}");
        let w = world(&format!("rec-{b:?}"));
        activate(&w).unwrap();
        w.select(1);
        let before = GameState::of(&w);
        assert!(run_child(&w, ProfileStaged, "activate"), "{what}");
        assert!(run_child(&w, b, "recover"), "{what}: never reached");
        let r = recover_twice(&w, &what);
        assert_eq!(outcome(&r), "refused", "{what}: {r:#?}");
        assert_settled(&w, &before, &r, &what);
        activate(&w).unwrap();
        assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
    }
}

// ---- in-process errors ----

#[test]
fn an_error_after_each_step_is_aborted_to_the_earlier_state() {
    for (fail, want) in [
        (FailAfter::GameFiles, "rolled-back"),
        (FailAfter::Chain, "rolled-back"),
        (FailAfter::Placements, "refused"),
        (FailAfter::Plan, "refused"),
    ] {
        let what = format!("an error after {fail:?}");
        let w = world(&format!("err-{fail:?}"));
        activate(&w).unwrap();
        w.select(1);
        let before = GameState::of(&w);
        let e = activate_with(&w, Some(fail), |_, doc| pretty(doc)).unwrap_err();
        assert!(e.message.contains("injected"), "{e:?}");
        // the abort settled it already: a recovery finds the same refusal, or nothing
        let r = recover_twice(&w, &what);
        let r = match r {
            Recovered::Nothing => Recovered::RolledBack {
                generation: 0,
                notes: Vec::new(),
            },
            r => r,
        };
        assert_eq!(outcome(&r), want, "{what}: {r:?}");
        assert_settled(&w, &before, &r, &what);
    }
}

/// The commit refuses a profile that is not the one the activation staged: another generation, no pin, another plan's
/// pin, a pin of this plan from a profile it was not made for. Nothing is written, the activation is rolled back.
#[test]
fn the_commit_refuses_a_profile_that_pins_another_plan_or_state() {
    type Bad = fn(&sfi_activation::Committed, &Value) -> Vec<u8>;
    let cases: [(&str, Bad); 5] = [
        ("another generation", |_, d| {
            let mut d = d.clone();
            d["generation"] = json!(d["generation"].as_u64().unwrap() + 1);
            pretty(&d)
        }),
        ("no pin", |_, d| {
            let mut d = d.clone();
            d.as_object_mut().unwrap().remove("sfLaunchPlan");
            pretty(&d)
        }),
        ("another plan", |_, d| {
            let mut d = d.clone();
            d["sfLaunchPlan"]["path"] = json!("launch-plans/other.json");
            pretty(&d)
        }),
        ("another hash", |_, d| {
            let mut d = d.clone();
            d["sfLaunchPlan"]["sha256"] = json!("0".repeat(64));
            pretty(&d)
        }),
        ("another state", |_, d| {
            let mut d = d.clone();
            d["selection"] = json!("edited after the plan");
            pretty(&d)
        }),
    ];
    for (what, bad) in cases {
        let w = world(&format!("commit-{}", what.replace(' ', "-")));
        activate(&w).unwrap();
        w.select(1);
        let before = GameState::of(&w);
        let e = activate_with(&w, None, bad).unwrap_err();
        assert_eq!(e.code, kiln_manage::ErrorCode::Conflict, "{what}: {e:?}");
        let now = GameState::of(&w);
        assert_eq!(now.profile, before.profile, "{what}: nothing written");
        assert_eq!(
            now.plans, before.plans,
            "{what}: its plan went with the rollback"
        );
        assert_eq!(now.chain, before.chain, "{what}");
        assert!(
            sfi_activation::start_check(&w.game).is_some(),
            "{what}: the copies changed: refused until activated again"
        );
        activate(&w).unwrap();
        assert_eq!(sfi_activation::start_check(&w.game), None);
    }
}

// ---- the lock ----

/// One activation of a game at a time: a recovery and a second activation wait for the one that holds the lock, then
/// see it committed.
#[test]
fn activations_and_recoveries_wait_for_the_one_that_runs() {
    let w = world("lock");
    let p = profile::get(&w.dirs, &w.profile).unwrap();
    let route = sfi_routing::route_of(&w.game);
    let (tx, _) = sfi_activation::begin(
        &w.dirs,
        &w.game,
        Begin {
            profile_id: &p.id,
            generation: 1,
            route: &route,
        },
    )
    .unwrap();
    let (done, wait) = std::sync::mpsc::channel();
    let recovering = {
        let (dirs, game, done) = (w.dirs.clone(), w.game.clone(), done.clone());
        std::thread::spawn(move || {
            let r = sfi_activation::recover(&dirs, &game);
            done.send(("recover", Instant::now())).unwrap();
            r
        })
    };
    let beginning = {
        let (dirs, game, id) = (w.dirs.clone(), w.game.clone(), p.id.clone());
        std::thread::spawn(move || {
            let route = sfi_routing::route_of(&game);
            let r = sfi_activation::begin(
                &dirs,
                &game,
                Begin {
                    profile_id: &id,
                    generation: 2,
                    route: &route,
                },
            )
            .map(|(s, r)| (s.generation(), r));
            done.send(("begin", Instant::now())).unwrap();
            r
        })
    };
    std::thread::sleep(Duration::from_millis(300));
    assert!(
        wait.try_recv().is_err(),
        "both wait while the activation holds the lock"
    );
    let committed = Instant::now();
    tx.commit(&pretty(
        &json!({"format": 1, "id": p.id, "generation": 1, "mods": {}}),
    ))
    .unwrap();
    for _ in 0..2 {
        let (_, at) = wait.recv_timeout(Duration::from_secs(300)).unwrap();
        assert!(at >= committed);
    }
    // the second activation found the first committed; it was dropped unfinished (its staging let go of the lock): the
    // recovery that got the lock after it settled it (nothing was changed), or the one now does, never both
    let (generation, earlier) = beginning.join().unwrap().unwrap();
    assert_eq!((generation, earlier), (2, Recovered::Nothing));
    let dropped = Recovered::RolledBack {
        generation: 2,
        notes: Vec::new(),
    };
    let waited = recovering.join().unwrap().unwrap();
    let now = sfi_activation::recover(&w.dirs, &w.game).unwrap();
    assert!(
        (waited == Recovered::Nothing && now == dropped)
            || (waited == dropped && now == Recovered::Nothing),
        "{waited:?} then {now:?}"
    );
    assert_eq!(
        std::fs::read(w.file("kiln/profile.json")).unwrap(),
        pretty(&json!({"format": 1, "id": p.id, "generation": 1, "mods": {}}))
    );
}

// ---- names that are not Kiln's ----

/// A plan name that is taken is never written over: the other file stays, the journal forgets the name, another id
/// is drawn.
#[test]
fn a_taken_plan_name_is_never_written_over() {
    let w = world("taken");
    let p = profile::get(&w.dirs, &w.profile).unwrap();
    let route = sfi_routing::route_of(&w.game);
    let (mut tx, _) = sfi_activation::begin(
        &w.dirs,
        &w.game,
        Begin {
            profile_id: &p.id,
            generation: 1,
            route: &route,
        },
    )
    .unwrap();
    let doc = profile_doc(&p, 1);
    let ident = ProfileIdentity::of(&doc, Mode::Active).unwrap();
    let id = kiln_manage::util::uuid_v4().unwrap();
    put(
        &w.game,
        &format!("kiln/launch-plans/{id}.json"),
        b"another writer's file",
    );
    assert_eq!(tx.write_plan(&id, &plan_doc(&id, &ident)), Ok(None));
    assert_eq!(
        std::fs::read(w.file(&format!("kiln/launch-plans/{id}.json"))).unwrap(),
        b"another writer's file"
    );
    let journal = std::fs::read_to_string(w.file(sfi_activation::JOURNAL)).unwrap();
    assert!(
        !journal.contains(&id),
        "the journal forgets the taken name: {journal}"
    );
    // an id that is no file name, and a plan for another profile state, are refused before anything is written
    assert!(tx.write_plan("../profile", &plan_doc("x", &ident)).is_err());
    let mut other = ident.clone();
    other.generation = 7;
    assert!(
        tx.write_plan(
            &kiln_manage::util::uuid_v4().unwrap(),
            &plan_doc("x", &other)
        )
        .is_err()
    );
    let id2 = kiln_manage::util::uuid_v4().unwrap();
    let r = tx
        .write_plan(&id2, &plan_doc(&id2, &ident))
        .unwrap()
        .unwrap();
    let mut doc = doc;
    doc["sfLaunchPlan"] = r.profile_value();
    tx.commit(&pretty(&doc)).unwrap();
    assert_eq!(
        std::fs::read(w.file(&format!("kiln/launch-plans/{id}.json"))).unwrap(),
        b"another writer's file"
    );
    assert_eq!(sfi_activation::start_check(&w.game), None);
}

/// A plan no profile pins (an earlier activation's, another tool's) is harmless: the start check ignores it, an
/// activation and a recovery never touch it.
#[test]
fn an_orphan_plan_is_harmless() {
    let w = world("orphan");
    let orphan = "kiln/launch-plans/00000000-0000-4000-8000-000000000000.json";
    put(
        &w.game,
        orphan,
        b"{\"profile\": {\"id\": \"kill\", \"generation\": 1}}\n",
    );
    activate(&w).unwrap();
    assert_eq!(sfi_activation::start_check(&w.game), None);
    w.select(1);
    assert!(run_child(&w, Boundary::PlanWritten, "activate"));
    recover_twice(&w, "orphan");
    activate(&w).unwrap();
    assert_eq!(
        std::fs::read(w.file(orphan)).unwrap(),
        b"{\"profile\": {\"id\": \"kill\", \"generation\": 1}}\n"
    );
    assert_eq!(sfi_activation::start_check(&w.game), None);
}

/// The journal and the state folder are never reached through a link, and a foreign file beside the journal (a
/// temporary file of another name, a link) is never followed or replaced.
#[cfg(unix)]
#[test]
fn links_and_foreign_temporary_files_are_never_followed_or_replaced() {
    let w = world("links");
    let p = profile::get(&w.dirs, &w.profile).unwrap();
    let route = sfi_routing::route_of(&w.game);
    let target = w.root.join("elsewhere.json");
    std::fs::write(&target, b"not Kiln's").unwrap();
    std::fs::create_dir_all(w.file("kiln/state")).unwrap();
    std::os::unix::fs::symlink(&target, w.file(sfi_activation::JOURNAL)).unwrap();
    let e = sfi_activation::begin(
        &w.dirs,
        &w.game,
        Begin {
            profile_id: &p.id,
            generation: 1,
            route: &route,
        },
    )
    .err()
    .unwrap();
    assert_eq!(e.code, kiln_manage::ErrorCode::Refused, "{e:?}");
    assert!(
        sfi_activation::start_check(&w.game).is_some(),
        "a journal Kiln cannot read refuses the planned start"
    );
    assert_eq!(std::fs::read(&target).unwrap(), b"not Kiln's");
    std::fs::remove_file(w.file(sfi_activation::JOURNAL)).unwrap();
    // foreign files beside the journal
    put(
        &w.game,
        "kiln/state/sfi-activation.json.tmp",
        b"a foreign temporary file",
    );
    std::os::unix::fs::symlink(&target, w.file("kiln/state/.sfi-activation.json.kiln-tmp"))
        .unwrap();
    activate(&w).unwrap();
    assert_eq!(
        std::fs::read(w.file("kiln/state/sfi-activation.json.tmp")).unwrap(),
        b"a foreign temporary file"
    );
    assert!(
        std::fs::symlink_metadata(w.file("kiln/state/.sfi-activation.json.kiln-tmp"))
            .unwrap()
            .file_type()
            .is_symlink()
    );
    assert_eq!(std::fs::read(&target).unwrap(), b"not Kiln's");
    // a linked state folder
    let moved = w.root.join("state-elsewhere");
    std::fs::rename(w.file("kiln/state"), &moved).unwrap();
    std::os::unix::fs::symlink(&moved, w.file("kiln/state")).unwrap();
    let before = snapshot(&moved);
    assert!(
        sfi_activation::begin(
            &w.dirs,
            &w.game,
            Begin {
                profile_id: &p.id,
                generation: 2,
                route: &route
            }
        )
        .is_err()
    );
    assert_eq!(snapshot(&moved), before, "nothing written through the link");
    // a linked profile.json is never written through by a commit
    std::fs::remove_file(w.file("kiln/state")).unwrap();
    std::fs::rename(&moved, w.file("kiln/state")).unwrap();
    let pj = w.file("kiln/profile.json");
    let real = w.root.join("profile-elsewhere.json");
    std::fs::rename(&pj, &real).unwrap();
    std::os::unix::fs::symlink(&real, &pj).unwrap();
    let held = std::fs::read(&real).unwrap();
    assert!(activate(&w).is_err());
    assert_eq!(std::fs::read(&real).unwrap(), held);
}

/// A stop inside the plan's publication (its temporary file written and the plan not published yet, or the plan
/// published by a hard link and the temporary name not gone yet) leaves no file of that plan behind once recovered; a
/// file of another shape there, or a link, is not this activation's to remove.
#[test]
fn a_stop_inside_the_plans_publication_leaves_no_file_of_it() {
    for at in [Boundary::PlanStaged, Boundary::PlanWritten] {
        let what = format!("killed at {at:?}");
        let w = world(&format!("plantmp-{at:?}"));
        activate(&w).unwrap();
        w.select(1);
        assert!(run_child(&w, at, "activate"), "{what}");
        let j: Value =
            serde_json::from_slice(&std::fs::read(w.file(sfi_activation::JOURNAL)).unwrap())
                .unwrap();
        let rel = j["stages"]
            .as_array()
            .unwrap()
            .iter()
            .find(|s| s["stage"] == "plan")
            .unwrap()["path"]
            .as_str()
            .unwrap()
            .to_owned();
        let name = rel.strip_prefix("launch-plans/").unwrap();
        // sfi_plan::write_plan's temporary name: .<name>.<16 hex digits>.kiln-tmp
        let ours = format!("kiln/launch-plans/.{name}.0123456789abcdef.kiln-tmp");
        put(&w.game, &ours, b"the plan's bytes, not published");
        let foreign = [
            format!("kiln/launch-plans/.{name}.kiln-tmp"),
            // (another name than ours on a file system that ignores case too)
            format!("kiln/launch-plans/.{name}.ABCDEF0123456789.kiln-tmp"),
            "kiln/launch-plans/.00000000-0000-4000-8000-000000000000.json.0123456789abcdef.kiln-tmp"
                .to_owned(),
        ];
        for f in &foreign {
            put(&w.game, f, b"not this activation's");
        }
        #[cfg(unix)]
        let link = {
            let target = w.root.join("elsewhere");
            std::fs::write(&target, b"not Kiln's").unwrap();
            let link = format!("kiln/launch-plans/.{name}.fedcba9876543210.kiln-tmp");
            std::os::unix::fs::symlink(&target, w.file(&link)).unwrap();
            (link, target)
        };
        let r = recover_twice(&w, &what);
        assert_eq!(outcome(&r), expected(at, false), "{what}: {r:?}");
        assert!(
            !w.file(&format!("kiln/{rel}")).exists(),
            "{what}: the plan goes"
        );
        assert!(!w.file(&ours).exists(), "{what}: its temporary file goes");
        for f in &foreign {
            assert_eq!(
                std::fs::read(w.file(f)).unwrap(),
                b"not this activation's",
                "{what}: {f}"
            );
        }
        #[cfg(unix)]
        {
            assert!(
                std::fs::symlink_metadata(w.file(&link.0))
                    .unwrap()
                    .file_type()
                    .is_symlink()
            );
            assert_eq!(std::fs::read(&link.1).unwrap(), b"not Kiln's");
        }
    }
}

/// A first copy (nothing was at its path) published before its receipt was written: exactly the copy published, it
/// goes with the rollback (the kill matrix, PlacementPublished); with a file the mod wrote into it since, it is Kiln's
/// copy again with its receipt (the copies changed: the planned start stays refused until the next activation, which
/// keeps that file); with a file of the Library's changed it is not Kiln's to touch and stays, as main leaves it.
#[test]
fn a_first_copy_published_before_its_receipt_is_taken_out_or_claimed() {
    let what = "a first copy the mod wrote into";
    let w = world("first-wrote");
    let before = GameState::of(&w);
    assert!(run_child(&w, Boundary::PlacementPublished, "activate"));
    put(&w.game, "mods/sfpack/saved.txt", b"written by the mod");
    let r = recover_twice(&w, what);
    assert_eq!(outcome(&r), "refused", "{what}: {r:?}");
    assert_settled(&w, &before, &r, what);
    let receipts = std::fs::read_to_string(w.file("kiln/state/library-placements.json")).unwrap();
    assert!(receipts.contains("sfpack"), "{what}: {receipts}");
    activate(&w).unwrap();
    assert_eq!(
        std::fs::read(w.file("mods/sfpack/saved.txt")).unwrap(),
        b"written by the mod"
    );
    assert_eq!(sfi_activation::start_check(&w.game), None);

    let what = "a first copy with a Library file changed";
    let w = world("first-changed");
    let before = GameState::of(&w);
    assert!(run_child(&w, Boundary::PlacementPublished, "activate"));
    put(
        &w.game,
        "mods/sfpack/src/mod.lua",
        b"return { edited = true }",
    );
    let held = snapshot(&w.file("mods/sfpack"));
    let r = recover_twice(&w, what);
    assert_eq!(outcome(&r), "rolled-back", "{what}: {r:?}");
    assert_eq!(snapshot(&w.file("mods/sfpack")), held, "{what}");
    assert_eq!(GameState::of(&w).placements, before.placements, "{what}");
}

// ---- the planned start ----

/// A stale plan refuses the planned start of ShroudForge-format mods, and only that: the plan changed, gone, made for
/// another state of the profile, or an activation unfinished. A start without a plan (main's routes) is not refused.
#[test]
fn a_stale_plan_refuses_the_planned_start_of_shroudforge_mods_only() {
    let w = world("stale");
    activate(&w).unwrap();
    assert_eq!(sfi_activation::start_check(&w.game), None);
    let doc: Value =
        serde_json::from_slice(&std::fs::read(w.file("kiln/profile.json")).unwrap()).unwrap();
    let plan = w.file(&format!(
        "kiln/{}",
        doc["sfLaunchPlan"]["path"].as_str().unwrap()
    ));
    let bytes = std::fs::read(&plan).unwrap();
    let refused = |why: &str| {
        let c =
            sfi_activation::start_check(&w.game).unwrap_or_else(|| panic!("{why}: not refused"));
        assert!(
            c.starts_with("ShroudForge-format mods do not start")
                && c.ends_with("Every other mod starts as it is"),
            "{why}: {c}"
        );
        c
    };
    std::fs::write(&plan, [&bytes[..], b" "].concat()).unwrap();
    assert!(refused("changed").contains("changed since the activation"));
    std::fs::remove_file(&plan).unwrap();
    assert!(refused("gone").contains("is gone"));
    std::fs::write(&plan, &bytes).unwrap();
    assert_eq!(sfi_activation::start_check(&w.game), None);
    // the profile hand-edited after its plan was made
    let mut edited = doc.clone();
    edited["selection"] = json!("edited by hand");
    std::fs::write(w.file("kiln/profile.json"), pretty(&edited)).unwrap();
    assert!(refused("another state").contains("made for another state"));
    std::fs::write(w.file("kiln/profile.json"), pretty(&doc)).unwrap();
    // an activation that did not finish
    w.select(1);
    assert!(run_child(&w, Boundary::ChainWritten, "activate"));
    assert!(refused("unfinished").contains("did not finish"));
    sfi_activation::recover(&w.dirs, &w.game).unwrap();
    assert_eq!(sfi_activation::start_check(&w.game), None);
    // main's routes: a profile without a plan has no planned start to refuse
    std::fs::write(
        w.file("kiln/profile.json"),
        pretty(&json!({"format": 1, "id": "kill", "generation": 9, "mods": {}})),
    )
    .unwrap();
    assert_eq!(sfi_activation::start_check(&w.game), None);
}

// ---- hand-edited files ----

/// A file Kiln was to put back that was changed by hand stays as it is; the planned start is refused with what to do,
/// until the next activation (which keeps the player's lines).
#[test]
fn hand_edited_files_are_kept_with_instructions() {
    let w = world("hand");
    activate(&w).unwrap();
    w.select(1);
    let before = GameState::of(&w);
    assert!(run_child(&w, Boundary::ChainWritten, "activate"));
    // the player adds a [chain] line of their own
    let hand = String::from_utf8(std::fs::read(w.file("kiln/chain.ini")).unwrap())
        .unwrap()
        .replace("[chain]\r\n", "[chain]\r\nmine.dll\r\n");
    assert!(hand.contains("mine.dll"), "{hand}");
    let hand = hand.into_bytes();
    std::fs::write(w.file("kiln/chain.ini"), &hand).unwrap();
    let r = recover_twice(&w, "hand-edited chain.ini");
    let Recovered::Refused { reason, .. } = &r else {
        panic!("{r:?}")
    };
    assert!(
        reason.contains("chain.ini was changed by hand") && reason.contains("activate"),
        "{reason}"
    );
    assert_eq!(
        std::fs::read(w.file("kiln/chain.ini")).unwrap(),
        hand,
        "kept as the player left it"
    );
    let now = GameState::of(&w);
    assert_eq!(
        (now.profile, now.game_files),
        (before.profile.clone(), before.game_files.clone()),
        "the rest went back"
    );
    assert!(sfi_activation::start_check(&w.game).is_some());
    activate(&w).unwrap();
    assert!(
        String::from_utf8(std::fs::read(w.file("kiln/chain.ini")).unwrap())
            .unwrap()
            .contains("mine.dll"),
        "the player's line stays"
    );
    assert_eq!(sfi_activation::start_check(&w.game), None);

    // kiln/profile.json written by something else: nothing is put back at all
    w.select(0);
    let before = GameState::of(&w);
    assert!(run_child(&w, Boundary::PlanWritten, "activate"));
    let foreign = pretty(&json!({"format": 1, "id": "kill", "generation": 99, "mods": {}}));
    std::fs::write(w.file("kiln/profile.json"), &foreign).unwrap();
    let held = snapshot(&w.game);
    let r = recover_twice(&w, "foreign profile.json");
    assert!(
        matches!(&r, Recovered::Refused { reason, .. } if reason.contains("changed by something else")),
        "{r:?}"
    );
    // (snapshot keys use the system's separator)
    let without_journal = |mut s: std::collections::BTreeMap<String, String>| {
        s.retain(|k, _| !k.ends_with("sfi-activation.json"));
        s
    };
    assert_eq!(
        without_journal(snapshot(&w.game)),
        without_journal(held),
        "nothing but the journal changed"
    );
    assert_ne!(GameState::of(&w).chain, before.chain);
}

// ---- no fallback after the freeze ----

/// Recovery uses the route its activation froze: kiln.json switched to hosted ShroudForge after the stop does not make
/// it read ShroudForge's own folder (Linux: an inotify tripwire over <game>/shroudforge sees nothing).
#[cfg(target_os = "linux")]
#[test]
fn recovery_uses_the_route_its_activation_froze() {
    let w = world("frozen");
    sf_install(&w.game, Some("SFMODS"));
    activate(&w).unwrap();
    w.select(1);
    assert!(run_child(&w, Boundary::PlacementParked, "activate"));
    select(&w.game, Some(HOSTED));
    let mut wire = Tripwire::arm(&w.game.join("shroudforge"));
    let r = sfi_activation::recover(&w.dirs, &w.game).unwrap();
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "recovery read below <game>/shroudforge"
    );
    assert_eq!(
        sfi_activation::recover(&w.dirs, &w.game).unwrap(),
        Recovered::Nothing
    );
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "the second recovery read below <game>/shroudforge"
    );
    assert_eq!(outcome(&r), "rolled-back", "{r:?}");
    assert_eq!(
        engine_mods::marker(&w.file("mods/sfpack")).unwrap().version,
        "1.0.0",
        "the copy went back where it was parked"
    );
    assert!(!w.game.join("SFMODS").exists());
}

// ---- the journal's shape ----

/// The journal is the closed shape its schema describes (schema/sfi/sfi-activation.schema.json).
#[test]
fn the_journal_has_the_shape_of_its_schema() {
    let schema: Value = serde_json::from_str(include_str!(
        "../../../schema/sfi/sfi-activation.schema.json"
    ))
    .unwrap();
    let w = world("schema");
    activate(&w).unwrap();
    w.select(1);
    assert!(run_child(&w, Boundary::ProfileStaged, "activate"));
    let j: Value =
        serde_json::from_slice(&std::fs::read(w.file(sfi_activation::JOURNAL)).unwrap()).unwrap();
    fn closed(v: &Value, s: &Value, root: &Value, at: &str) {
        let s = match s["$ref"].as_str() {
            Some(r) => &root["$defs"][r.trim_start_matches("#/$defs/")],
            None => s,
        };
        // (values other than objects: their types are the reader's to check)
        if !v.is_object() {
            return;
        }
        if let Some(any) = s["oneOf"].as_array() {
            let fits: Vec<_> = any
                .iter()
                .filter(|alt| {
                    let alt = match alt["$ref"].as_str() {
                        Some(r) => &root["$defs"][r.trim_start_matches("#/$defs/")],
                        None => alt,
                    };
                    alt["properties"]["stage"]["const"] == v["stage"]
                })
                .collect();
            assert_eq!(fits.len(), 1, "{at}: {v}");
            return closed(v, fits[0], root, at);
        }
        let obj = v.as_object().expect("an object");
        assert_eq!(
            s["additionalProperties"],
            json!(false),
            "{at}: the schema is closed"
        );
        for k in obj.keys() {
            assert!(
                s["properties"].get(k).is_some(),
                "{at}: {k} is not in the schema"
            );
        }
        for k in s["required"].as_array().into_iter().flatten() {
            assert!(
                obj.contains_key(k.as_str().unwrap()),
                "{at}: {k} is required"
            );
        }
        for (k, x) in obj {
            let p = &s["properties"][k];
            if p["type"] == "array" || p["items"].is_object() {
                for (i, item) in x.as_array().into_iter().flatten().enumerate() {
                    closed(item, &p["items"], root, &format!("{at}.{k}[{i}]"));
                }
            } else {
                closed(x, p, root, &format!("{at}.{k}"));
            }
        }
    }
    closed(&j, &schema, &schema, "journal");
    let stages: Vec<&str> = j["stages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|s| s["stage"].as_str().unwrap())
        .collect();
    assert_eq!(
        stages,
        ["game-files", "chain", "placements", "plan", "profile"]
    );
    assert_eq!(
        (j["state"].as_str(), j["route"].as_str()),
        (Some("staging"), Some("interpreter"))
    );
}
