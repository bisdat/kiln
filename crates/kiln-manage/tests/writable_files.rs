//! Files Kiln stores in its Library and places into the game are writable by the player (inbox 84): a folder import
//! once kept a read-only file read-only (`std::fs::copy` keeps permissions), and so did placement and taking over the
//! player's own folder, so a mod's or the player's settings could not be saved there. Only the owner's write bit is
//! added; no other permission changes.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::engine_mods;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        // read-only folders cannot be emptied: give them back their write bit first
        let _ = kiln_manage::util::make_tree_writable(&self.0);
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-writable-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn mode(p: &Path) -> u32 {
    std::fs::symlink_metadata(p).unwrap().permissions().mode() & 0o7777
}
fn chmod(p: &Path, m: u32) {
    std::fs::set_permissions(p, std::fs::Permissions::from_mode(m)).unwrap();
}

/// A ShroudForge package whose settings file and DLL are read-only, as a folder copied from a read-only place is.
fn read_only_package(root: &Path) -> PathBuf {
    let sf = root.join("src/unifiedemltweaks");
    put(&sf, "mod.json", br#"{"id":"unifiedemltweaks","name":"Unified EML Tweaks","version":"2.1.1"}"#);
    put(&sf, "extended.mod.json", br#"{"schemaVersion":1,"enabled":false,"settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    put(&sf, "bin/ut_native.dll", b"MZ not run");
    put(&sf, "src/mod.lua", b"return {}");
    chmod(&sf.join("extended.mod.json"), 0o444);
    chmod(&sf.join("bin/ut_native.dll"), 0o555);
    chmod(&sf.join("mod.json"), 0o440);
    sf
}

fn game(t: &Path, z: bool) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    if z { std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap(); }
    GameInstall {
        id: "g-test".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: if z { PathMode::Z } else { PathMode::Copy }, source: "manual".into(),
    }
}

fn import(dirs: &DataDirs, src: &Path) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::ShroudForge).cloned().unwrap_or_else(|| panic!("{r:?}"))
}

fn profile_with(dirs: &DataDirs, r: &LibraryRef) -> profile::Profile {
    let mut p = profile::create(dirs, "My mods", None).unwrap();
    p.mods.shroudforge.push(ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true });
    profile::save(dirs, &p).unwrap()
}

#[test]
fn a_folder_import_stores_writable_files_and_widens_nothing_else() {
    let t = tmp("import");
    let dirs = DataDirs::at(t.0.join("data"));
    let r = import(&dirs, &read_only_package(&t.0));
    let entry = library::path_of(&dirs, &r).unwrap();
    // the owner's write bit, nothing more: read for all stays read for all, execute stays execute, group/other gain nothing
    assert_eq!(mode(&entry.join("extended.mod.json")), 0o644);
    assert_eq!(mode(&entry.join("bin/ut_native.dll")), 0o755);
    assert_eq!(mode(&entry.join("mod.json")), 0o640);
    // the source folder is the player's: left as it was
    assert_eq!(mode(&t.0.join("src/unifiedemltweaks/extended.mod.json")), 0o444);
    // the content is the same: its kdh1 does not see permissions
    assert_eq!(kiln_manage::hash::dirhash(&entry).unwrap(), r.hash);
}

#[test]
fn placement_writes_writable_copies_and_fixes_a_read_only_placed_file_at_the_next_update() {
    let t = tmp("place");
    let dirs = DataDirs::at(t.0.join("data"));
    let r = import(&dirs, &read_only_package(&t.0));
    // a Library entry an older Kiln imported from a folder: its files still read-only
    let entry = library::path_of(&dirs, &r).unwrap();
    chmod(&entry.join("extended.mod.json"), 0o444);
    let g = game(&t.0, true);
    let p = profile_with(&dirs, &r);
    profile::activate(&dirs, &p.id, &g).unwrap();
    let placed = g.path.join("mods/unifiedemltweaks");
    assert_eq!(mode(&placed.join("extended.mod.json")) & 0o200, 0o200, "a new copy is writable");
    // the settings write the in-game page and the launcher make works there
    engine_mods::set_setting(ModKind::ShroudForge, "unifiedemltweaks", "speed", &serde_json::json!(7), Some(&g.path),
        &dirs.profile(&p.id).join("config"), None).unwrap();

    // a copy an older Kiln placed read-only, kept as it is by the next update: made writable then, its values kept
    chmod(&placed.join("extended.mod.json"), 0o444);
    chmod(&placed.join("bin/ut_native.dll"), 0o555);
    // a file the player added and made read-only on purpose: theirs, left as it is
    put(&placed, "my-notes.txt", b"mine");
    chmod(&placed.join("my-notes.txt"), 0o444);
    let a = profile::refresh_active(&dirs, &g).unwrap();
    assert!(a.as_ref().is_none_or(|a| a.retained.is_empty()), "{a:?}");
    assert_eq!(mode(&placed.join("extended.mod.json")), 0o644);
    assert_eq!(mode(&placed.join("bin/ut_native.dll")), 0o755);
    assert_eq!(mode(&placed.join("my-notes.txt")), 0o444);
    assert_eq!(kiln_manage::shroudforge::read_mod(&placed).unwrap().settings.iter().find(|s| s.key == "speed").unwrap().value, 7);
}

#[test]
fn copy_mode_profile_copies_are_writable() {
    let t = tmp("copy-mode");
    let dirs = DataDirs::at(t.0.join("data"));
    let r = import(&dirs, &read_only_package(&t.0));
    chmod(&library::path_of(&dirs, &r).unwrap().join("extended.mod.json"), 0o444);
    let g = game(&t.0, false);
    let p = profile_with(&dirs, &r);
    let a = profile::activate(&dirs, &p.id, &g).unwrap();
    assert_eq!(a.path_mode, PathMode::Copy);
    let second = g.path.join(format!("kiln/profiles/{}/shroudforge/unifiedemltweaks", p.id));
    assert_eq!(mode(&second.join("extended.mod.json")), 0o644);
    // kept by the next activation (unchanged), and made writable when an older Kiln left it read-only; a file the
    // player added there and keeps read-only is not Kiln's to change
    chmod(&second.join("extended.mod.json"), 0o444);
    put(&second, "notes/mine.txt", b"mine");
    chmod(&second.join("notes/mine.txt"), 0o444);
    profile::activate(&dirs, &p.id, &g).unwrap();
    assert_eq!(mode(&second.join("extended.mod.json")), 0o644);
    assert_eq!(mode(&second.join("notes/mine.txt")), 0o444);
    assert_eq!(mode(&g.path.join("mods/unifiedemltweaks/extended.mod.json")), 0o644);
}

#[test]
fn taking_over_the_players_own_folder_makes_its_files_writable() {
    let t = tmp("adopt");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = read_only_package(&t.0);
    let r = import(&dirs, &src);
    let g = game(&t.0, true);
    // the player's own folder of the same package in ShroudForge's mods folder, read-only as they copied it
    let own = g.path.join("mods/unifiedemltweaks");
    std::fs::create_dir_all(own.join("bin")).unwrap();
    std::fs::create_dir_all(own.join("src")).unwrap();
    for rel in ["mod.json", "extended.mod.json", "bin/ut_native.dll", "src/mod.lua"] {
        std::fs::copy(src.join(rel), own.join(rel)).unwrap();
    }
    assert_eq!(mode(&own.join("extended.mod.json")), 0o444);
    let p = profile_with(&dirs, &r);
    let done = engine_mods::adopt(&dirs, &p, &g.path, ModKind::ShroudForge, "unifiedemltweaks").unwrap();
    assert!(!done.already);
    assert_eq!(mode(&own.join("extended.mod.json")), 0o644);
    assert_eq!(mode(&own.join("bin/ut_native.dll")), 0o755);
}
