//! The boot snapshot's Play decision reads the profile of the start it is made for. At Play the launcher's `cook_run`
//! calls `cook::cook_due` (which writes the game's `kiln/profile.json` again when the active profile changed since its
//! activation: `profile::refresh_active`), then decides with `boot_snapshot::boot_mods` (launcher/src-tauri/src/
//! commands/cook.rs). The case that must not go wrong: the player switched an OnBoot mod on after the last start, so the
//! profile.json on disk is the old one without it; the first Play must still make the snapshot.
#![cfg(feature = "cooktools")]
use kiln_manage::boot_snapshot::{self, BootSnapshot};
use kiln_manage::cook;
use kiln_manage::cooktools::Locate;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

fn kiln_mod(root: &Path, id: &str, extra: &str) -> PathBuf {
    let d = root.join(format!("src/{id}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both"{extra} }}"#).as_bytes());
    put(&d, "M.dll", id.as_bytes());
    d
}

fn pm(r: &library::ImportReport, id: &str, enabled: bool) -> ProfileMod {
    let x: &LibraryRef = r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Kiln && x.id == id).unwrap();
    ProfileMod { id: x.id.clone(), version: x.version.clone(), hash: x.hash.clone(), enabled }
}

#[test]
fn the_first_play_after_an_onboot_mod_is_switched_on_makes_the_snapshot() {
    let t = Tmp(std::env::temp_dir().join(format!("km-bootsnap-play-{}", std::process::id())));
    let _ = std::fs::remove_dir_all(&t.0);
    std::fs::create_dir_all(&t.0).unwrap();
    let root = t.0.canonicalize().unwrap();
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let inst = GameInstall {
        id: "g-bootsnap".into(), kind: GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    };
    let r = library::import(&dirs, &[kiln_mod(&root, "plain", ""), kiln_mod(&root, "booter", r#", "boot": true"#)], &mut NoProgress).unwrap();
    // the last start: the OnBoot mod was off, and that is the profile.json in the game folder
    let mut p = profile::create(&dirs, "Boot", None).unwrap();
    p.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &inst).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "the last start had no OnBoot mod");
    // the player switches it on in the launcher: the profile changes, the game folder's profile.json does not yet
    let mut p = profile::get(&dirs, &p.id).unwrap();
    p.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    profile::save(&dirs, &p).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "read before Play's cook, the old profile.json would skip it");

    // Play, as cook_run runs it: the cook (which writes the active profile again first), then the snapshot step
    let locate = Locate::default();  // nothing to cook: no tool is located
    let run = cook::cook_due(&dirs, &inst, None, &locate, &mut |_, _, _, _| {}, &|| false).unwrap();
    assert!(run.activation.is_some(), "the cook wrote the changed profile");
    assert_eq!(boot_snapshot::boot_mods(&inst), ["booter"], "the step reads the profile of this start");
    let calls = std::cell::Cell::new(0);
    let s = boot_snapshot::make_if_needed(&boot_snapshot::boot_mods(&inst), || {
        calls.set(calls.get() + 1);
        BootSnapshot { status: "made".into(), ..Default::default() }
    });
    assert_eq!((s.status.as_str(), calls.get()), ("made", 1));
}

/// Play once with another profile (launch mode `once`): the active profile has no OnBoot mod, the one started once
/// has. The step decides from that profile before the start writes its file (kiln/profile.once.json, which kiln.boot
/// reads at that start), and it decides as kiln.boot then finds that file; the active profile stays as it was.
#[test]
fn a_single_start_with_another_profile_decides_from_that_profile() {
    let t = Tmp(std::env::temp_dir().join(format!("km-bootsnap-once-{}", std::process::id())));
    let _ = std::fs::remove_dir_all(&t.0);
    std::fs::create_dir_all(&t.0).unwrap();
    let root = t.0.canonicalize().unwrap();
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    // a hand-installed drop-in that boots: loaded only by a profile that loads Kiln drop-ins
    put(&game, "kiln/mods/dropboot/kiln.mod.json", br#"{ "id": "dropboot", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "D.dll", "boot": true }"#);
    let inst = GameInstall {
        id: "g-bootsnap-once".into(), kind: GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    };
    let r = library::import(&dirs, &[kiln_mod(&root, "plain", ""), kiln_mod(&root, "booter", r#", "boot": true"#)], &mut NoProgress).unwrap();
    let mut main = profile::create(&dirs, "Main", None).unwrap();
    main.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    main.dropins = vec![];
    let main = profile::save(&dirs, &main).unwrap();
    profile::activate(&dirs, &main.id, &inst).unwrap();
    let mut once = profile::create(&dirs, "Once", None).unwrap();
    once.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    once.dropins = vec![ModKind::Kiln];
    let once = profile::save(&dirs, &once).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "Play's rule reads the active profile: no OnBoot mod there");
    assert_eq!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap(), ["booter", "dropboot"]);
    // the drop-ins only by the profile's own rule; a switched-off OnBoot mod does not count
    let mut off = profile::get(&dirs, &once.id).unwrap();
    off.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    off.dropins = vec![];
    profile::save(&dirs, &off).unwrap();
    assert!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap().is_empty());
    let mut back = profile::get(&dirs, &once.id).unwrap();
    back.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    back.dropins = vec![ModKind::Kiln];
    profile::save(&dirs, &back).unwrap();
    assert_eq!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap(), ["booter", "dropboot"]);
    assert!(boot_snapshot::boot_mods_once(&dirs, &inst, "no-such-profile").is_err());

    // the start itself: kiln.boot reads kiln/profile.once.json, which names the same OnBoot mod, enabled
    let req = kiln_manage::launch::LaunchRequest { mode: kiln_manage::launch::LaunchMode::Once, profile_id: Some(once.id.clone()), skip_backup: true };
    kiln_manage::launch::play_with(&dirs, &inst, &req, Some(&[]), &|_| Ok(())).unwrap();
    let text = std::fs::read_to_string(game.join("kiln/profile.once.json")).unwrap();
    let file: profile::ProfileFile = serde_json::from_str(&text).unwrap();
    let booting: Vec<String> = kiln_manage::launch::kiln_folders(&inst, &file).into_iter()
        .filter(|(_, folder)| kiln_manage::manifest::read_kiln(folder).is_ok_and(|m| m.boot)).map(|(m, _)| m.id.clone()).collect();
    assert_eq!(booting, ["booter"]);
    assert!(file.dropins.names(ModKind::Kiln), "the once file loads Kiln drop-ins too");
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "a single start leaves the active profile as it was");
}

// ---------------------------------------------------------------- the start's timeline while the tool runs

mod start_bar {
    use kiln_manage::boot_snapshot::{self, Run, Runner};
    use kiln_manage::start_progress::{self as sp, Inputs, LauncherStart, StepState};
    use std::cell::{Cell, RefCell};
    use std::ffi::OsString;
    use std::time::Duration;

    const PLAY: i64 = 1_794_590_812_000;

    /// The tool as Play meets it on a game build without a snapshot: its lines one per `step_ms` of the clock, then the
    /// result. `look` sees the start after each line.
    struct Tool<'a> {
        lines: Vec<String>,
        result: &'static str,
        clock: &'a Cell<i64>,
        step_ms: i64,
        look: RefCell<Box<dyn FnMut(&str, i64) + 'a>>,
    }

    impl Runner for Tool<'_> {
        fn run(&self, _args: &[OsString], _limit: Option<Duration>, line: &mut dyn FnMut(&str), _stop: &dyn Fn() -> bool) -> std::io::Result<Run> {
            for l in &self.lines {
                self.clock.set(self.clock.get() + self.step_ms);
                line(l);
                (self.look.borrow_mut())(l, self.clock.get());
            }
            self.clock.set(self.clock.get() + self.step_ms);
            Ok(Run { code: Some(0), stopped: None, stdout: format!("{}\n", self.result), last_err: Vec::new() })
        }
    }

    /// This PC's last start of the game, without the boot game-data step (it runs once per game update).
    fn history() -> Vec<sp::KeptStart> {
        let step = |id: &str, ms: u64| sp::KeptStep { id: id.into(), ms, total: None, parts: Vec::new() };
        vec![sp::KeptStart { kiln: "0.2.0-dev.28+20261114.aaaaaaa".into(), runtime: Some("0.2.0-dev.28".into()), fingerprint: "kms1:x".into(),
            finished: String::new(), started: "2026-11-13T17:26:52.000Z".into(),
            steps: vec![step("cook", 400), step("profile", 1_000), step("steam", 5_000), step("start", 100), step("game", 10_000)] }]
    }

    fn timeline(l: &LauncherStart, now: i64) -> sp::Timeline {
        let h = history();
        sp::timeline(&Inputs { launcher: Some(l), now, history: &h, ..Default::default() }).expect("the start has a timeline")
    }

    /// The step `cook_run` gives Play's start: the cook ended, then the boot game-data tool, its lines to the start.
    fn play_with_tool(lines: &[&str], result: &'static str, mut look: impl FnMut(&LauncherStart, &str, i64) + 'static) -> (LauncherStart, boot_snapshot::BootSnapshot, i64) {
        let clock = Cell::new(PLAY);
        let start = std::rc::Rc::new(RefCell::new(LauncherStart::new(PLAY, Some(Vec::new()), Some("0.2.0-dev.28".into()), Some("kms1:x".into()))));
        clock.set(PLAY + 400);
        start.borrow_mut().cook_done(clock.get());
        start.borrow_mut().boot_snapshot_check();
        let seen = start.clone();
        let tool = Tool {
            lines: lines.iter().map(|l| format!("boot-snapshot: {l}")).collect(),
            result,
            clock: &clock,
            step_ms: 6_000,
            look: RefCell::new(Box::new(move |l, now| look(&seen.borrow(), l, now))),
        };
        let s = boot_snapshot::make(&tool, std::path::Path::new("/game"), false, None,
            &mut |l| start.borrow_mut().boot_snapshot_line(clock.get(), l), &|| false);
        start.borrow_mut().boot_snapshot_done(clock.get());
        let end = clock.get();
        drop(tool);
        let l = std::rc::Rc::try_unwrap(start).ok().expect("one owner").into_inner();
        (l, s, end)
    }

    const MADE: &str = r#"{"status":"made","key":"k","path":"/game/kiln/data/boot-gamedata/k","emptyKinds":[],"bytes":209000000,"seconds":62}"#;
    const REUSED: &str = r#"{"status":"reused","key":"k","path":"/game/kiln/data/boot-gamedata/k","emptyKinds":[]}"#;

    #[test]
    fn play_shows_the_boot_game_data_step_with_the_tools_lines_then_the_next_step() {
        let lines = ["needed by more-flame-altars", "checking the game data snapshot", "making the game data snapshot (about 300 MB, once per game build)",
            "items 1200/4100", "templates 900/52000", "templates 52000/52000", "skills 300/300", "materials 120/120"];
        let looks: std::rc::Rc<RefCell<Vec<(String, sp::Timeline, bool)>>> = Default::default();
        let keep = looks.clone();
        let (mut l, s, end) = play_with_tool(&lines, MADE, move |l, line, now| {
            let mut copy = l.clone();
            let tracked = copy.keep_tracking(false, now);
            keep.borrow_mut().push((line.to_string(), timeline(l, now), tracked));
        });
        assert_eq!(s.status, "made");
        let looks = looks.borrow();
        // before the tool says it builds: no step of its own yet, and the start is not lost
        let (_, before, tracked) = &looks[1];
        assert!(before.steps.iter().all(|x| x.id != "boot-snapshot"), "{:?}", before.steps);
        assert!(*tracked);
        // it builds: a running step of its own, right after the cook, with the tool's line, and the bar has a time
        for (line, t, tracked) in &looks[2..] {
            let r = &t.steps[t.running.expect("a running step")];
            assert_eq!(r.id, "boot-snapshot", "{line}");
            assert_eq!(r.label, "Making the game data for OnBoot mods (once per game update)");
            assert!(r.launcher);
            assert!(*tracked, "a start whose tool runs 40 s after the cook is not stalled ({line})");
            let ids: Vec<&str> = t.steps.iter().map(|x| x.id.as_str()).take(4).collect();
            assert_eq!(ids, ["cook", "boot-snapshot", "profile", "steam"]);
        }
        let (_, t, _) = looks.iter().find(|(l, ..)| l.ends_with("templates 900/52000")).unwrap();
        let r = &t.steps[t.running.unwrap()];
        assert_eq!(r.detail.as_deref(), Some("templates 900/52000"));
        assert_eq!(r.usual_ms, Some(sp::BOOT_SNAPSHOT_USUAL_MS), "no start with it yet: its known cost");
        // the bar moves while it runs
        let fractions: Vec<f64> = looks[2..].iter().map(|(_, t, _)| t.estimate.fraction).collect();
        assert!(fractions.windows(2).all(|w| w[1] > w[0]), "{fractions:?}");
        // it ended when the tool did; the next step runs
        l.begin("profile", end + 50);
        let t = timeline(&l, end + 100);
        let snap = t.steps.iter().find(|x| x.id == "boot-snapshot").unwrap();
        assert_eq!(snap.state, StepState::Done);
        assert_eq!(snap.took_ms, Some((end - (PLAY + 400) - 18_000) as u64), "from the build line to the tool's end");
        assert_eq!(t.steps[t.running.unwrap()].id, "profile");
    }

    #[test]
    fn a_snapshot_found_current_or_not_needed_shows_no_step() {
        let (l, s, end) = play_with_tool(&["needed by more-flame-altars", "checking the game data snapshot"], REUSED, |_, _, _| {});
        assert_eq!(s.status, "reused");
        let t = timeline(&l, end);
        assert!(t.steps.iter().all(|x| x.id != "boot-snapshot"), "{:?}", t.steps);
        assert!(!l.checking);
        assert!(!sp::is_snapshot_build_line("checking the game data snapshot"));
        assert!(!sp::is_snapshot_build_line("needed by a, b"));
        assert!(sp::is_snapshot_build_line("templates 1/2"));
    }
}
