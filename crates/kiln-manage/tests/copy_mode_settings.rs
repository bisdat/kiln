//! Copy path mode (the launcher's data folder has no Windows path in the game's prefix: a Steam Deck with the game on an
//! SD card) and the settings of engine mods (inbox 84). ShroudForge reads its mods only from its own folder in the game,
//! where activation places Kiln's copy; the profile file names a second copy under `<game>/kiln/profiles/`, which
//! ShroudForge never reads. A setting changed in the copy ShroudForge runs survives the launcher's next profile update;
//! a profile copy that was changed in the game folder is never replaced without a word.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::engine_mods;
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
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-copy-mode-settings-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A ShroudForge package shaped like Unified EML Tweaks: switched off as shipped, one setting, a native-plugin.ini.
fn sf_package(root: &Path, version: &str) -> PathBuf {
    let sf = root.join(format!("src-{version}/unifiedemltweaks"));
    put(&sf, "mod.json", format!(r#"{{"id":"unifiedemltweaks","name":"Unified EML Tweaks","version":"{version}","capabilities":["patch","export"]}}"#).as_bytes());
    put(&sf, "extended.mod.json", br#"{"schemaVersion":1,"enabled":false,"targets":["client"],
        "settings":{"Enable_StackSize":{"label":"Bigger stacks","control":"toggle","value":false},"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    put(&sf, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=bin/ut_native.dll\n");
    put(&sf, "bin/ut_native.dll", b"MZ not run");
    put(&sf, "src/mod.lua", b"return {}");
    sf
}

/// A game whose prefix has no Z: drive: the launcher activates it in Copy mode.
fn copy_mode_game(t: &Path) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    GameInstall {
        id: "g-deck".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    }
}

fn import_one(dirs: &DataDirs, src: &Path, kind: ModKind) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.kind == kind).cloned().unwrap_or_else(|| panic!("not imported: {r:?}"))
}

fn pm(r: &LibraryRef) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true }
}

/// The value of one setting in a package's extended.mod.json.
fn value(folder: &Path, key: &str) -> serde_json::Value {
    kiln_manage::shroudforge::read_mod(folder).unwrap().settings.into_iter().find(|s| s.key == key).unwrap().value
}

/// What the game's Kiln menu (Kiln.Ui ShroudForgePackages.SetSetting) does with a setting: the file read, the value
/// set, the file written again as System.Text.Json writes it (indented, its own order).
fn set_in_game(folder: &Path, key: &str, v: serde_json::Value) {
    let file = folder.join("extended.mod.json");
    let mut doc: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&file).unwrap()).unwrap();
    doc["settings"][key]["value"] = v;
    std::fs::write(&file, serde_json::to_string_pretty(&doc).unwrap()).unwrap();
}

fn game_profile(g: &GameInstall) -> serde_json::Value {
    serde_json::from_str(&std::fs::read_to_string(g.path.join("kiln/profile.json")).unwrap()).unwrap()
}

#[test]
fn in_copy_mode_a_setting_changed_in_the_copy_shroudforge_runs_survives_profile_updates() {
    let t = tmp("survives");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = copy_mode_game(&t.0);
    let sf = import_one(&dirs, &sf_package(&t.0, "2.1.1"), ModKind::ShroudForge);
    let mut p = profile::create(&dirs, "My mods", None).unwrap();
    p.mods.shroudforge.push(pm(&sf));
    let p = profile::save(&dirs, &p).unwrap();
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    assert_eq!(a.path_mode, PathMode::Copy);
    // the player's kiln/profile.json: the entry names the second copy under kiln/profiles/, ShroudForge runs <game>/mods
    let file = game_profile(&g);
    assert_eq!(file["pathMode"], "copy");
    assert_eq!(file["mods"]["shroudforge"][0]["path"], format!("profiles/{}/shroudforge/unifiedemltweaks", p.id));
    let placed = g.path.join("mods/unifiedemltweaks");
    let second = g.path.join(format!("kiln/profiles/{}/shroudforge/unifiedemltweaks", p.id));
    assert!(second.join("extended.mod.json").is_file());
    assert_eq!(engine_mods::marker(&placed).unwrap().hash, sf.hash, "Kiln's copy, the one the in-game page now edits");
    assert!(kiln_manage::shroudforge::read_mod(&placed).unwrap().enabled, "the profile is the switch");

    // the player changes two settings in the game's Kiln menu (the row's folder is the placed copy)
    set_in_game(&placed, "Enable_StackSize", true.into());
    set_in_game(&placed, "speed", 9.into());
    // the launcher's next profile update: the same profile reconciled, then an edit (another activation)
    assert!(profile::refresh_active(&dirs, &g).unwrap().is_none_or(|a| a.retained.is_empty()));
    assert_eq!((value(&placed, "Enable_StackSize"), value(&placed, "speed")), (true.into(), 9.into()));
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    assert!(a.retained.is_empty() && !a.warnings.iter().any(|w| w.contains("unifiedemltweaks")), "{a:?}");
    assert_eq!((value(&placed, "Enable_StackSize"), value(&placed, "speed")), (true.into(), 9.into()));
    // what the launcher's own settings editor reads is the same file
    let cfg = dirs.profile(&p.id).join("config");
    let s = engine_mods::settings(ModKind::ShroudForge, "unifiedemltweaks", Some(&g.path), &cfg, None).unwrap();
    assert_eq!((s.file.as_deref().map(PathBuf::from), &s.values["speed"]), (Some(placed.join("extended.mod.json")), &serde_json::json!(9)));
    // a new version of the mod: the values go over to it
    let next = import_one(&dirs, &sf_package(&t.0, "2.1.2"), ModKind::ShroudForge);
    let mut p2 = profile::get(&dirs, &p.id).unwrap();
    p2.mods.shroudforge[0] = pm(&next);
    let p2 = profile::save(&dirs, &p2).unwrap();
    let a = profile::activate(&dirs, &p2.id, &g).unwrap();
    assert_eq!(engine_mods::marker(&placed).unwrap().version, "2.1.2", "{a:?}");
    assert_eq!((value(&placed, "Enable_StackSize"), value(&placed, "speed")), (true.into(), 9.into()));
    // the switch is the profile's: switched off there, it is off in the game, and the settings stay
    let mut p3 = profile::get(&dirs, &p.id).unwrap();
    p3.mods.shroudforge[0].enabled = false;
    let p3 = profile::save(&dirs, &p3).unwrap();
    profile::activate(&dirs, &p3.id, &g).unwrap();
    assert!(!kiln_manage::shroudforge::read_mod(&placed).unwrap().enabled);
    assert_eq!(value(&placed, "speed"), 9);
}

#[test]
fn a_profile_copy_changed_in_the_game_folder_is_kept_aside_and_named() {
    let t = tmp("changed-copy");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = copy_mode_game(&t.0);
    let sf = import_one(&dirs, &sf_package(&t.0, "2.1.1"), ModKind::ShroudForge);
    let mut p = profile::create(&dirs, "My mods", None).unwrap();
    p.mods.shroudforge.push(pm(&sf));
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &g).unwrap();
    let second = g.path.join(format!("kiln/profiles/{}/shroudforge/unifiedemltweaks", p.id));
    // what an older in-game page did: it wrote the second copy, which ShroudForge never reads
    set_in_game(&second, "speed", 8.into());
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    let note = a.warnings.iter().find(|w| w.starts_with("shroudforge:unifiedemltweaks:")).unwrap_or_else(|| panic!("no word of it: {a:?}"));
    assert!(note.contains("extended.mod.json") && note.contains("ShroudForge never reads that copy") && note.contains("kept the changed copy in"), "{note}");
    assert!(note.contains(&g.path.join("mods/unifiedemltweaks").display().to_string()), "where it runs: {note}");
    // the copy is the Library's again, the changed one is kept in the game's backups, and the note names it
    assert_eq!(value(&second, "speed"), 3);
    let prefix = format!("profile-copy-{}-shroudforge-unifiedemltweaks-", p.id);
    let backups: Vec<PathBuf> = std::fs::read_dir(g.path.join("kiln/backups")).unwrap().flatten().map(|e| e.path())
        .filter(|b| b.file_name().unwrap().to_string_lossy().starts_with(&prefix)).collect();
    assert_eq!(backups.len(), 1, "{backups:?}");
    assert_eq!(value(&backups[0], "speed"), 8);
    assert!(note.contains(&backups[0].display().to_string()), "{note}");
    // the placed copy ShroudForge runs was never touched by either
    assert_eq!(value(&g.path.join("mods/unifiedemltweaks"), "speed"), 3);
    // said once: the next update finds the Library's copy and says nothing
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    assert!(!a.warnings.iter().any(|w| w.contains("unifiedemltweaks")), "{a:?}");
}

/// A copy changed again before every activation (a mod that rewrites its own files there) leaves at most three backups
/// of it, the newest ones; another mod's backups whose id starts with this one's are not its own.
#[test]
fn changed_profile_copies_keep_the_three_newest_backups_of_a_mod() {
    let t = tmp("changed-copy-bound");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = copy_mode_game(&t.0);
    let sf = import_one(&dirs, &sf_package(&t.0, "2.1.1"), ModKind::ShroudForge);
    let mut p = profile::create(&dirs, "My mods", None).unwrap();
    p.mods.shroudforge.push(pm(&sf));
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &g).unwrap();
    let backups = g.path.join("kiln/backups");
    let other = backups.join(format!("profile-copy-{}-shroudforge-unifiedemltweaks-live-20200101T000000Z", p.id));
    std::fs::create_dir_all(&other).unwrap();
    let second = g.path.join(format!("kiln/profiles/{}/shroudforge/unifiedemltweaks", p.id));
    for speed in 4..=8 {
        set_in_game(&second, "speed", speed.into());
        let a = profile::activate(&dirs, &p.id, &g).unwrap();
        assert!(a.warnings.iter().any(|w| w.starts_with("shroudforge:unifiedemltweaks:")), "{a:?}");
    }
    let prefix = format!("profile-copy-{}-shroudforge-unifiedemltweaks-2", p.id);
    let mut kept: Vec<PathBuf> = std::fs::read_dir(&backups).unwrap().flatten().map(|e| e.path())
        .filter(|b| b.file_name().unwrap().to_string_lossy().starts_with(&prefix)).collect();
    kept.sort();
    let mut values: Vec<i64> = kept.iter().map(|b| value(b, "speed").as_i64().unwrap()).collect();
    values.sort();
    assert_eq!(values, [6, 7, 8], "{kept:?}");
    assert!(other.is_dir(), "another mod's backup stays");
}

#[test]
fn an_eml_copy_whose_settings_file_changed_keeps_the_players_settings() {
    let t = tmp("eml-settings");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = copy_mode_game(&t.0);
    let eml = t.0.join("src/stacks");
    put(&eml, "mod.json", br#"{"id":"stacks","name":"Bigger Stacks","version":"1.2.0","capabilities":["patch"]}"#);
    put(&eml, "src/mod.lua", b"return {}");
    put(&eml, "config.txt", b"stack=100\n");
    let r = import_one(&dirs, &eml, ModKind::Eml);
    let mut p = profile::create(&dirs, "My mods", None).unwrap();
    p.mods.eml.push(pm(&r));
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &g).unwrap();
    let copy = g.path.join(format!("kiln/profiles/{}/eml/stacks", p.id));
    assert_eq!(game_profile(&g)["mods"]["eml"][0]["path"], format!("profiles/{}/eml/stacks", p.id));
    // kiln-eml runs this copy in Copy mode; the mod (or the player) writes its settings file there
    put(&copy, "config.txt", b"stack=999\n");
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    assert_eq!(std::fs::read(copy.join("config.txt")).unwrap(), b"stack=999\n", "{a:?}");
    assert!(!a.warnings.iter().any(|w| w.contains("eml:stacks")), "{a:?}");
    assert!(!g.path.join("kiln/backups").exists() || std::fs::read_dir(g.path.join("kiln/backups")).unwrap().next().is_none());
    // its code changed there: replaced from the Library, and the changed copy kept and named
    put(&copy, "src/mod.lua", b"return { edited = true }");
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    let note = a.warnings.iter().find(|w| w.starts_with("eml:stacks:")).unwrap_or_else(|| panic!("{a:?}"));
    assert!(note.contains("src/mod.lua") && note.contains("kept the changed copy in"), "{note}");
    assert_eq!(std::fs::read(copy.join("src/mod.lua")).unwrap(), b"return {}");
}
