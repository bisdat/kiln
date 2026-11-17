//! Direct starts (dev.29.17, `kiln_manage::direct`): the command per platform and runner (built, never run), what a
//! custom folder is, the settings in the install record, the launch flow through a recording starter, and whether
//! Kiln knows a game build (a fixture build record, a runtime bundle's index). Nothing here starts a program.

use kiln_manage::direct::{self, DirectCommand, RunnerInfo, RunnerKind, StartMode, StartSettings};
use kiln_manage::game::{self, GameInstall, GameKind, PathMode, Platform};
use kiln_manage::launch::{self, LaunchMode, LaunchRequest, Starter};
use kiln_manage::paths::DataDirs;
use kiln_manage::steam::{SteamFlavor, SteamRoot};
use std::cell::RefCell;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-direct-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}

fn put(p: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, bytes).unwrap();
}

#[cfg(unix)]
fn exec(p: &Path) {
    use std::os::unix::fs::PermissionsExt;
    put(p, b"#!/bin/sh\nexit 0\n");
    std::fs::set_permissions(p, std::fs::Permissions::from_mode(0o755)).unwrap();
}

fn client(path: &Path, source: &str, library: Option<PathBuf>) -> GameInstall {
    GameInstall {
        id: "g-direct".into(), kind: GameKind::Client, path: path.to_path_buf(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library, platform: Platform::Proton, prefix: None, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: source.into(),
    }
}

fn env<'a>(c: &'a DirectCommand, k: &str) -> Option<&'a str> {
    c.env.iter().find(|(n, _)| n == k).map(|(_, v)| v.as_str())
}

#[test]
fn windows_runs_the_exe_in_the_game_folder_with_nothing_set() {
    let g = client(Path::new("/games/Enshrouded-old"), "manual", None);
    let c = direct::command_for(true, &g, None, None, None, false).unwrap();
    assert_eq!(c.program, Path::new("/games/Enshrouded-old").join("enshrouded.exe"));
    assert!(c.args.is_empty());
    assert_eq!(c.cwd, Path::new("/games/Enshrouded-old"));
    assert!(c.env.is_empty(), "dinput8.dll next to the exe is loaded first on Windows: {:?}", c.env);
    assert!(c.runner.is_none() && c.prefix.is_none());
    // the same with Ultimate ASI Loader chained: Windows needs no override
    assert!(direct::command_for(true, &g, None, None, None, true).unwrap().env.is_empty());
}

#[test]
fn proton_runs_the_exe_with_compatdata_steam_and_the_proxy_override() {
    let g = client(Path::new("/games/Enshrouded-old"), "manual", None);
    let proton = RunnerInfo { kind: RunnerKind::Proton, path: "/tools/GE-Proton10-25".into(), label: "GE-Proton10-25".into(), source: "compat".into() };
    let c = direct::command_for(false, &g, Some(&proton), Some(Path::new("/pfx/g-direct")), Some(Path::new("/steam")), false).unwrap();
    assert_eq!(c.program, Path::new("/tools/GE-Proton10-25/proton"));
    assert_eq!(c.args, vec!["run".to_string(), "/games/Enshrouded-old/enshrouded.exe".to_string()]);
    assert_eq!(c.cwd, Path::new("/games/Enshrouded-old"));
    assert_eq!(env(&c, "STEAM_COMPAT_DATA_PATH"), Some("/pfx/g-direct"));
    assert_eq!(env(&c, "STEAM_COMPAT_CLIENT_INSTALL_PATH"), Some("/steam"));
    assert_eq!(env(&c, "WINEDLLOVERRIDES"), Some("dinput8=n,b"), "the advised Steam launch option's override");
    assert_eq!(c.prefix.as_deref(), Some(Path::new("/pfx/g-direct/pfx")));
    // Kiln never tells the Steam API an app id: Steam does that when it starts a game
    for k in ["SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID"] {
        assert!(env(&c, k).is_none(), "{k} must not be set");
        assert!(c.env_remove.iter().any(|r| r == k), "{k} inherited from the launcher is dropped");
    }
    for k in ["WINEPREFIX", "WINEDLLOVERRIDES", "LD_PRELOAD"] {
        assert!(c.env_remove.iter().any(|r| r == k), "{k}");
    }
    // a chained Ultimate ASI Loader needs xinput9_1_0 native first too
    let c = direct::command_for(false, &g, Some(&proton), Some(Path::new("/pfx/g-direct")), Some(Path::new("/steam")), true).unwrap();
    assert_eq!(env(&c, "WINEDLLOVERRIDES"), Some("dinput8,xinput9_1_0=n,b"));
    // Proton cannot run without Steam's folder; nothing at all without a runner
    let e = direct::command_for(false, &g, Some(&proton), Some(Path::new("/pfx/g-direct")), None, false).unwrap_err();
    assert!(e.message.contains("Steam"), "{}", e.message);
    assert!(direct::command_for(false, &g, None, Some(Path::new("/pfx")), Some(Path::new("/steam")), false).is_err());
}

#[test]
fn wine_runs_the_exe_with_its_prefix_and_keeps_mscoree() {
    let g = client(Path::new("/games/Enshrouded-old"), "manual", None);
    let wine = RunnerInfo { kind: RunnerKind::Wine, path: "/usr/bin/wine".into(), label: "Wine".into(), source: "system".into() };
    let c = direct::command_for(false, &g, Some(&wine), Some(Path::new("/pfx/mine")), None, false).unwrap();
    assert_eq!(c.program, Path::new("/usr/bin/wine"));
    assert_eq!(c.args, vec!["/games/Enshrouded-old/enshrouded.exe".to_string()]);
    assert_eq!(c.cwd, Path::new("/games/Enshrouded-old"));
    assert_eq!(env(&c, "WINEPREFIX"), Some("/pfx/mine"));
    let o = env(&c, "WINEDLLOVERRIDES").unwrap();
    assert_eq!(o, "mshtml=;dinput8=n,b");
    assert!(kiln_manage::wine::dinput8_native_first(o) && kiln_manage::wine::check_mscoree(o).is_ok());
    assert!(env(&c, "STEAM_COMPAT_DATA_PATH").is_none());
    assert_eq!(c.prefix.as_deref(), Some(Path::new("/pfx/mine")));
}

#[test]
fn servers_never_start_here() {
    let mut g = client(Path::new("/srv/server"), "manual", None);
    g.kind = GameKind::Server;
    g.exe = "enshrouded_server.exe".into();
    assert!(direct::command_for(true, &g, None, None, None, false).is_err());
    assert_eq!(direct::effective_mode(&g, Some(&StartSettings { mode: Some(StartMode::Direct), ..Default::default() })), StartMode::Steam);
}

#[test]
fn prefix_paths_take_a_compatdata_folder_or_a_wine_prefix() {
    let t = tmp("pfx");
    let own = t.0.join("own");
    assert_eq!(direct::prefix_paths(RunnerKind::Proton, None, &own).unwrap(), (own.clone(), own.join("pfx")));
    // a Steam compatdata folder (pfx inside), and its pfx picked by mistake: the same pair
    let compat = t.0.join("compatdata/1203620");
    put(&compat.join("pfx/system.reg"), b"WINE REGISTRY Version 2\n");
    let want = (compat.clone(), compat.join("pfx"));
    assert_eq!(direct::prefix_paths(RunnerKind::Proton, Some(&compat), &own).unwrap(), want);
    assert_eq!(direct::prefix_paths(RunnerKind::Proton, Some(&compat.join("pfx")), &own).unwrap(), want);
    // a plain Wine prefix: Wine takes it as it is, Proton refuses it with what to do
    let wp = t.0.join("wineprefix");
    put(&wp.join("system.reg"), b"WINE REGISTRY Version 2\n");
    assert_eq!(direct::prefix_paths(RunnerKind::Wine, Some(&wp), &own).unwrap(), (wp.clone(), wp.clone()));
    assert!(direct::prefix_paths(RunnerKind::Proton, Some(&wp), &own).unwrap_err().message.contains("Proton"));
    // Wine given a compatdata folder uses its pfx; an empty folder becomes the prefix
    assert_eq!(direct::prefix_paths(RunnerKind::Wine, Some(&compat), &own).unwrap(), want);
    let empty = t.0.join("fresh");
    assert_eq!(direct::prefix_paths(RunnerKind::Wine, Some(&empty), &own).unwrap(), (empty.clone(), empty.clone()));
    assert!(direct::prefix_paths(RunnerKind::Wine, Some(Path::new("relative/pfx")), &own).is_err());
}

#[test]
fn own_prefixes_live_outside_the_data_folder() {
    use kiln_manage::paths::DataEnv;
    let e = DataEnv { home: Some("/h".into()), ..Default::default() };
    assert_eq!(direct::own_prefix(&e, "abc"), Some(PathBuf::from("/h/.local/share/kiln-prefixes/abc")));
    let e = DataEnv { home: Some("/h".into()), xdg_data_home: Some("/x".into()), ..Default::default() };
    assert_eq!(direct::own_prefix(&e, "abc"), Some(PathBuf::from("/x/kiln-prefixes/abc")));
    assert_ne!(direct::prefixes_root(&e), e.default_root().ok(), "never the data folder itself");
    assert_eq!(direct::own_prefix(&DataEnv { windows: true, ..Default::default() }, "abc"), None);
}

#[cfg(unix)]
#[test]
fn runners_are_found_and_steams_mapping_is_the_default() {
    let t = tmp("runners");
    let root = t.0.join("Steam");
    for ge in ["GE-Proton9-27", "GE-Proton10-4"] {
        let d = root.join("compatibilitytools.d").join(ge);
        exec(&d.join("proton"));
        exec(&d.join("files/bin/wine"));
    }
    let valve = root.join("steamapps/common/Proton 9.0");
    exec(&valve.join("proton"));
    exec(&valve.join("files/bin/wine"));
    std::fs::create_dir_all(root.join("steamapps/common/Enshrouded")).unwrap(); // a game, not a runner
    let roots = vec![SteamRoot { path: root.clone(), flavor: SteamFlavor::Native }];
    let all: Vec<RunnerInfo> = direct::runners(&roots, &t.0.join("nohome")).into_iter().filter(|r| r.source != "system").collect();
    let names: Vec<&str> = all.iter().map(|r| r.label.as_str()).collect();
    assert_eq!(names, ["GE-Proton10-4", "GE-Proton9-27", "Proton 9.0"]);
    assert!(all.iter().all(|r| r.kind == RunnerKind::Proton));
    assert_eq!(all[2].source, "steam");
    // nothing mapped: the first found
    assert_eq!(direct::default_runner(&roots, &all).unwrap().label, "GE-Proton10-4");
    // Steam maps Enshrouded to Proton 9.0: that one
    put(&root.join("config/config.vdf"), br#""InstallConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" { "1203620" { "name" "proton_9" "config" "" "priority" "250" } } } } } }"#);
    assert_eq!(direct::default_runner(&roots, &all).unwrap().path, valve.canonicalize().unwrap());
    // the player's own path: a Proton folder, a wine binary, a wine folder; relative or empty refused
    assert_eq!(direct::runner_at(&valve).unwrap().kind, RunnerKind::Proton);
    assert_eq!(direct::runner_at(&valve.join("proton")).unwrap().path, valve);
    let wine = t.0.join("wine-9/bin/wine");
    exec(&wine);
    assert_eq!(direct::runner_at(&wine).unwrap().kind, RunnerKind::Wine);
    assert_eq!(direct::runner_at(&t.0.join("wine-9")).unwrap().path, wine);
    assert!(direct::runner_at(Path::new("wine")).is_err());
    assert!(direct::runner_at(&t.0.join("nothing")).is_err());
}

#[test]
fn a_folder_no_steam_library_knows_is_custom_and_starts_directly() {
    let t = tmp("custom");
    let data = t.0.join("data");
    // Steam's own copy: a library with the app manifest
    let lib = t.0.join("lib");
    let steam_game = lib.join("steamapps/common/Enshrouded");
    put(&steam_game.join("enshrouded.exe"), b"MZ steam");
    put(&lib.join("steamapps/appmanifest_1203620.acf"), br#""AppState" { "appid" "1203620" "installdir" "Enshrouded" "buildid" "26430625" "StateFlags" "4" }"#);
    let roots = vec![SteamRoot { path: lib.clone(), flavor: SteamFlavor::Native }];
    let s = game::inspect_with(&steam_game, &roots, &data).unwrap();
    assert_eq!(s.source, "steam");
    assert!(!direct::is_custom(&s));
    assert_eq!(direct::effective_mode(&s, None), StartMode::Steam);
    // an older copy next to it in the same library folder, and one on another drive: neither is Steam's
    for dir in [lib.join("steamapps/common/Enshrouded-old"), t.0.join("elsewhere/Enshrouded 2025")] {
        put(&dir.join("enshrouded.exe"), b"MZ old");
        let g = game::inspect_with(&dir, &roots, &data).unwrap();
        assert_eq!((g.source.as_str(), g.library.as_deref()), ("manual", None), "{}", dir.display());
        assert!(direct::is_custom(&g));
        assert_eq!(direct::effective_mode(&g, None), StartMode::Direct, "the default for a custom folder");
        assert_eq!(direct::effective_mode(&g, Some(&StartSettings { mode: Some(StartMode::Steam), ..Default::default() })), StartMode::Steam, "the player's choice wins");
    }
    // a folder without the exe is not a game folder
    std::fs::create_dir_all(t.0.join("empty")).unwrap();
    assert!(game::inspect_with(&t.0.join("empty"), &roots, &data).is_err());
}

#[test]
fn the_start_choice_lives_in_the_record_and_survives_rewrites() {
    let t = tmp("record");
    let dirs = DataDirs::at(t.0.join("data"));
    let folder = t.0.join("old-build");
    put(&folder.join("enshrouded.exe"), b"MZ old");
    let roots: Vec<SteamRoot> = Vec::new();
    let g = game::add_with(&dirs, &folder, &roots).unwrap();
    assert!(direct::is_custom(&g));
    assert_eq!(direct::mode_of(&dirs, &g), StartMode::Direct);
    assert!(game::record(&dirs, &g.id).unwrap().unwrap().start.is_none(), "defaults are not written");
    // through Steam after all: kept, and kept when something writes the record again (an install of Kiln, Add again)
    let steam = StartSettings { mode: Some(StartMode::Steam), ..Default::default() };
    game::set_start(&dirs, &g.id, &steam, &roots).unwrap();
    assert_eq!(game::record(&dirs, &g.id).unwrap().unwrap().start, Some(steam.clone()));
    game::write_record(&dirs, &g, None).unwrap();
    assert_eq!(game::record(&dirs, &g.id).unwrap().unwrap().start, Some(steam.clone()));
    game::add_with(&dirs, &folder, &roots).unwrap();
    assert_eq!(direct::mode_of(&dirs, &g), StartMode::Steam);
    // a relative runner or prefix is refused and changes nothing
    let bad = StartSettings { mode: Some(StartMode::Direct), prefix: Some("rel/pfx".into()), runner: None };
    if !cfg!(windows) {
        assert!(game::set_start(&dirs, &g.id, &bad, &roots).is_err());
        assert_eq!(direct::mode_of(&dirs, &g), StartMode::Steam);
    }
    // back to the defaults: the record holds no choice again
    game::set_start(&dirs, &g.id, &StartSettings::default(), &roots).unwrap();
    assert!(game::record(&dirs, &g.id).unwrap().unwrap().start.is_none());
    // servers are not started here
    let srv = t.0.join("srv");
    put(&srv.join("enshrouded_server.exe"), b"MZ srv");
    let s = game::add_with(&dirs, &srv, &roots).unwrap();
    assert!(game::set_start(&dirs, &s.id, &StartSettings { mode: Some(StartMode::Direct), ..Default::default() }, &roots).is_err());
}

#[cfg(unix)]
#[test]
fn a_direct_start_under_linux_runs_in_the_chosen_prefix() {
    let t = tmp("linux-pfx");
    let dirs = DataDirs::at(t.0.join("data"));
    let folder = t.0.join("old-build");
    put(&folder.join("enshrouded.exe"), b"MZ old");
    let ge = t.0.join("tools/GE-Proton10-4");
    exec(&ge.join("proton"));
    let compat = t.0.join("prefixes/mine");
    let roots: Vec<SteamRoot> = Vec::new();
    let g = game::add_with(&dirs, &folder, &roots).unwrap();
    let pick = StartSettings { mode: Some(StartMode::Direct), runner: Some(ge.clone()), prefix: Some(compat.clone()) };
    let g2 = game::set_start(&dirs, &g.id, &pick, &roots).unwrap();
    assert_eq!(g2.platform, Platform::Proton);
    assert_eq!(g2.prefix.as_deref(), Some(compat.join("pfx").as_path()), "saves and path mode follow the prefix the game runs in");
    assert_eq!(game::known(&dirs).unwrap()[0].prefix, g2.prefix);
    // no Steam root: the command cannot be built for Proton, and says why
    let e = direct::plan(&g2, &pick, &roots).unwrap_err();
    assert!(e.message.contains("Steam"), "{}", e.message);
    let steam = vec![SteamRoot { path: t.0.join("Steam"), flavor: SteamFlavor::Native }];
    let c = direct::plan(&g2, &pick, &steam).unwrap();
    assert_eq!(c.program, ge.join("proton"));
    assert_eq!(env(&c, "STEAM_COMPAT_DATA_PATH"), Some(compat.display().to_string().as_str()));
    assert_eq!(env(&c, "STEAM_COMPAT_CLIENT_INSTALL_PATH"), Some(t.0.join("Steam").display().to_string().as_str()));
    // with a plain Wine the same folder is the prefix itself
    let wine = t.0.join("wine/bin/wine");
    exec(&wine);
    let w = StartSettings { runner: Some(wine.clone()), ..pick.clone() };
    let g3 = game::set_start(&dirs, &g.id, &w, &roots).unwrap();
    assert_eq!((g3.platform, g3.prefix.as_deref()), (Platform::Wine, Some(compat.as_path())));
    let c = direct::plan(&g3, &w, &roots).unwrap();
    assert_eq!((c.program.as_path(), env(&c, "WINEPREFIX")), (wine.as_path(), Some(compat.display().to_string().as_str())));
}

#[test]
fn the_launch_flow_hands_a_direct_start_to_the_starter_not_to_steam() {
    let t = tmp("flow");
    let dirs = DataDirs::at(t.0.join("data"));
    let game_dir = t.0.join("game");
    put(&game_dir.join("enshrouded.exe"), b"MZ");
    let inst = client(&game_dir, "manual", None);
    let cmd = direct::command_for(true, &inst, None, None, None, false).unwrap();
    let ran: RefCell<Vec<PathBuf>> = RefCell::new(Vec::new());
    let go = |c: &DirectCommand| {
        ran.borrow_mut().push(c.program.clone());
        Ok(())
    };
    let req = LaunchRequest { mode: LaunchMode::NoMods, profile_id: None, skip_backup: true };
    let r = launch::play_via(&dirs, &inst, &req, Some(&[]), Starter::Direct(&cmd, &go)).unwrap();
    assert_eq!(r.via, StartMode::Direct);
    assert!(r.uri.is_empty());
    assert_eq!(ran.borrow().as_slice(), [game_dir.join("enshrouded.exe")]);
    assert!(kiln_manage::playonce::pending(&game_dir).unwrap().is_some(), "the play-once flag is written as for Steam");
    // a start that fails takes its flag back
    let fail = |_: &DirectCommand| Err(kiln_manage::Error::invalid("no runner"));
    assert!(launch::play_via(&dirs, &inst, &req, Some(&[]), Starter::Direct(&cmd, &fail)).is_err());
    assert!(kiln_manage::playonce::pending(&game_dir).unwrap().is_none());
    // through Steam the URI is opened as before
    let r = launch::play_with(&dirs, &inst, &req, Some(&[]), &|_| Ok(())).unwrap();
    assert_eq!((r.via, r.uri.as_str()), (StartMode::Steam, "steam://rungameid/1203620"));
    // an older LaunchResult without `via` reads as a Steam start
    let old: launch::LaunchResult = serde_json::from_str(r#"{"gameId":"g","mode":"profile","nonce":null,"backup":null,"uri":"steam://rungameid/1203620","started":"2026-11-17T00:00:00Z"}"#).unwrap();
    assert_eq!(old.via, StartMode::Steam);
}

/// A build record as sigdb/builds/client/<sha256>.json has it (format 2's top fields; only `verified` is read).
fn record(sha: &str, verified: bool) -> String {
    format!(r#"{{"sha256":"{sha}","side":"client","record":2,"pdb":"enshrouded.pdb","steam_build":26430625,"verified":{verified},"note":"fixture"}}"#)
}

#[test]
fn the_build_lookup_reads_the_games_records_then_the_bundles_index() {
    let t = tmp("build");
    let game_dir = t.0.join("game");
    put(&game_dir.join("enshrouded.exe"), b"MZ a build kept for mods");
    let sha = kiln_manage::hash::sha256_file(&game_dir.join("enshrouded.exe")).unwrap().0;
    let inst = client(&game_dir, "manual", None);
    // neither Kiln in the game nor a runtime: nothing to say
    assert_eq!(direct::build_check(&inst, || None), None);
    // Kiln installed with records for other builds only: unknown
    let builds = game_dir.join("kiln/sigdb/builds/client");
    put(&builds.join(format!("{}.json", "c".repeat(64))), record(&"c".repeat(64), true).as_bytes());
    let b = direct::build_check(&inst, || None).unwrap();
    assert!(!b.known);
    assert_eq!((b.short.as_str(), b.from.as_str()), (&sha[..12], "game"));
    assert_eq!(direct::unknown_build_sentence(&b.short),
        format!("Kiln doesn't know this game build ({}). Kiln starts with the game but stays idle: no mods load.", &sha[..12]));
    // a record that is not verified: still idle under verified-only
    put(&builds.join(format!("{sha}.json")), record(&sha, false).as_bytes());
    assert!(!direct::build_check(&inst, || None).unwrap().known);
    put(&builds.join(format!("{sha}.json")), record(&sha, true).as_bytes());
    assert!(direct::build_check(&inst, || None).unwrap().known);
    // the runtime zip's index while Kiln is not in the game yet
    std::fs::remove_dir_all(game_dir.join("kiln")).unwrap();
    let m: kiln_manage::bundle::RuntimeManifest = serde_json::from_str(&std::fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../schema/examples/kiln-runtime.json")).unwrap()).unwrap();
    let known = "ce6fdba63e27744943b695921383a29133285d359e07671f6ca33a47e2147618";
    assert!(direct::known_in_manifest(&m, kiln_manage::manifest::RunSide::Client, known));
    assert!(!direct::known_in_manifest(&m, kiln_manage::manifest::RunSide::Server, known));
    assert!(!direct::known_in_manifest(&m, kiln_manage::manifest::RunSide::Client, &sha));
    #[cfg(feature = "zip")]
    {
        let stage = t.0.join("stage");
        put(&stage.join("kiln-runtime.json"), format!(r#"{{"format":1,"kiln":"0.2.0-dev","abi":2,"api":"0.2.0",
            "components":{{"core":{{"sides":["client","server"],"default":true,"requires":[]}}}},
            "builds":{{"client":[{{"sha256":"{sha}","verified":true}}]}}}}"#).as_bytes());
        put(&stage.join("core/kiln/kiln-core.dll"), b"core");
        put(&stage.join("core/dinput8.dll"), &kiln_manage::pe::build_test_dll(&["DirectInput8Create", "GetdfDIJoystick", "KilnProxyInfo"], b"KilnProxy/2 test\0"));
        let zip = kiln_manage::bundle::create(&stage, &t.0.join("out"), &kiln_manage::bundle::CreateOptions { source_date_epoch: 1_793_620_800, git: "abcdef0".into() }).unwrap();
        let b = direct::build_check(&inst, || Some(zip.clone())).unwrap();
        assert!(b.known && b.from == "bundle");
        put(&game_dir.join("enshrouded.exe"), b"MZ another build");
        assert!(!direct::build_check(&inst, || Some(zip.clone())).unwrap().known, "the exe changed: hashed again");
    }
}
