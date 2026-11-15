//! Kept versions and roll back (kiln_manage::kept, 0.2.0-dev.29): an update keeps the mod's previous Library entry with
//! a copy of the settings its game-folder copy held then; "Roll back to <version>" puts it back into the profile as an
//! ordinary profile change (undone by quick::revert), nothing in the game folder changes until the profile is placed
//! again, and then the old settings are back; the version rolled away from becomes the kept one. The count in Settings
//! (keepVersions), the disk check, and that a kept entry is never picked, cleaned up, exported or offered as an update.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::kept;
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::task::NoProgress;
use kiln_manage::{engine_mods, hash, profile, quick, ErrorCode};
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-kept-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn read(root: &Path, rel: &str) -> String {
    std::fs::read_to_string(root.join(rel)).unwrap_or_else(|e| panic!("{rel}: {e}"))
}

/// A game with Kiln in it (Z: paths through a prefix), known to the launcher.
struct World {
    _t: Tmp,
    root: PathBuf,
    dirs: DataDirs,
    g: GameInstall,
}

fn world(tag: &str) -> World {
    let t = tmp(tag);
    let root = t.0.clone();
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = root.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    let g = GameInstall {
        id: "g-kept".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z, source: "manual".into(),
    };
    kiln_manage::game::write_record(&dirs, &g, None).unwrap();
    World { _t: t, root, dirs, g }
}

impl World {
    /// Drops a mod folder on the launcher (quick::add: import, into the active profile, profile.json written).
    fn drop_mod(&self, src: &Path) -> quick::AddReport {
        quick::add_with(&self.dirs, &self.g, &[src.to_path_buf()], false, &mut NoProgress).unwrap()
    }
    fn pid(&self) -> String {
        profile::read_game_profile(&self.g.path).unwrap().expect("profile.json").id
    }
    fn entry(&self, kind: ModKind, id: &str) -> profile::ProfileMod {
        profile::get(&self.dirs, &self.pid()).unwrap().mods.list(kind).iter().find(|m| m.id == id).cloned().expect("in the profile")
    }
    /// The Library's versions of a mod: (version, kept?), in the list's order.
    fn versions(&self, kind: ModKind, id: &str) -> Vec<(String, bool)> {
        library::list(&self.dirs, Some(kind)).unwrap().into_iter().find(|e| e.id == id).map(|e| e.versions.into_iter()
            .map(|v| (v.version, v.kept.is_some())).collect()).unwrap_or_default()
    }
    fn kept_settings(&self, kind: ModKind, id: &str, version: &str) -> Vec<String> {
        library::list(&self.dirs, Some(kind)).unwrap().into_iter().find(|e| e.id == id).and_then(|e| e.versions.into_iter()
            .find(|v| v.version == version && v.kept.is_some())).and_then(|v| v.kept).map(|k| k.settings).unwrap_or_default()
    }
    /// What the launcher does after a profile save: the game's profile.json written again (placement included).
    fn reapply(&self) {
        profile::refresh_active(&self.dirs, &self.g).unwrap();
    }
    fn settings(&self, json: &str) {
        put(&self.dirs.root, "settings.json", json.as_bytes());
    }
    /// The active profile's entry of the mod, as bytes: the Library folder it names hashes to its hash.
    fn entry_bytes_match(&self, kind: ModKind, id: &str) -> bool {
        let m = self.entry(kind, id);
        let folder = library::path_of(&self.dirs, &LibraryRef { kind, id: id.into(), version: m.version.clone(), hash: m.hash.clone() }).unwrap();
        hash::of_mod(kind, &folder).unwrap() == m.hash
    }
}

fn greeneye(root: &Path, version: &str) -> PathBuf {
    let d = root.join("src/worstmap");
    put(&d, "mod.json", format!(r#"{{"id":"worstmap","name":"The Worst Map Mod","version":"{version}","dll":"map.dll","side":"client"}}"#).as_bytes());
    put(&d, "map.dll", &build_test_dll(&["GreeneyeModStart"], version.as_bytes()));
    d
}

fn kiln_mod(root: &Path, version: &str) -> PathBuf {
    let d = root.join(format!("src/stacks-{version}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "stacks", "name": "Big Stacks", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#).as_bytes());
    put(&d, "M.dll", format!("stacks {version}").as_bytes());
    d
}

const MAP: &str = "greeneye/worstmap";
const INI: &str = "greeneye/worstmap/minimap_config.ini";

/// Greeneye (a mod that writes its settings into Kiln's copy in <game>/greeneye/<id>): update, roll back, roll back
/// again. Each step's profile entry is the Library's bytes, and the settings are the ones that version had.
#[test]
fn an_update_keeps_the_old_version_and_a_roll_back_brings_back_its_settings() {
    let w = world("greeneye");
    w.drop_mod(&greeneye(&w.root, "1.0.0"));
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "1.0.0");
    put(&w.g.path, "greeneye/worstmap/minimap_config.ini", b"zoom=1"); // the mod saves its settings in its copy
    let up = w.drop_mod(&greeneye(&w.root, "2.0.0"));
    assert_eq!(w.entry(ModKind::Greeneye, "worstmap").version, "2.0.0");
    // the update carried the settings over (as before dev.29), and kept 1.0.0 with them as they were
    assert_eq!(read(&w.g.path, INI), "zoom=1");
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("2.0.0".into(), false), ("1.0.0".into(), true)], "{:?}", up.notices);
    assert_eq!(w.kept_settings(ModKind::Greeneye, "worstmap", "1.0.0"), [INI]);
    put(&w.g.path, INI, b"zoom=9"); // the player changes a setting in 2.0.0

    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    assert_eq!((r.from.version.as_str(), r.to.version.as_str(), r.kept), ("2.0.0", "1.0.0", true), "{r:?}");
    assert_eq!(r.settings, [INI]);
    assert_eq!(w.entry(ModKind::Greeneye, "worstmap").version, "1.0.0");
    assert!(w.entry_bytes_match(ModKind::Greeneye, "worstmap"));
    // nothing in the game folder changed yet
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "2.0.0");
    assert_eq!(read(&w.g.path, INI), "zoom=9");
    // placed again (a profile save's re-apply, or Play): 1.0.0 with the settings it had before the update
    w.reapply();
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "1.0.0");
    assert_eq!(read(&w.g.path, INI), "zoom=1");
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("1.0.0".into(), false), ("2.0.0".into(), true)]);
    assert_eq!(w.kept_settings(ModKind::Greeneye, "worstmap", "2.0.0"), [INI]);

    // roll back twice: the newer version again, with its own settings
    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    assert_eq!(r.to.version, "2.0.0");
    w.reapply();
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "2.0.0");
    assert_eq!(read(&w.g.path, INI), "zoom=9");
    assert!(w.entry_bytes_match(ModKind::Greeneye, "worstmap"));
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("2.0.0".into(), false), ("1.0.0".into(), true)]);
    // the settings copies of versions that are not kept are gone
    let copies = w.dirs.library().join(kept::SETTINGS_DIR).join("greeneye/worstmap");
    assert_eq!(std::fs::read_dir(&copies).unwrap().count(), 1, "only 1.0.0's copy");
}

/// The Undo of a roll back is the launcher's Undo of a profile change (quick::revert): the newer version again, the
/// older one kept again with its settings, also when the game folder was never placed in between (the game ran).
#[test]
fn undo_of_a_roll_back_puts_the_newer_version_back_and_keeps_the_older_one() {
    let w = world("undo");
    w.drop_mod(&greeneye(&w.root, "1.0.0"));
    put(&w.g.path, INI, b"zoom=1");
    w.drop_mod(&greeneye(&w.root, "2.0.0"));
    put(&w.g.path, INI, b"zoom=9");
    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    // Undo before any placement
    let p = quick::revert(&w.dirs, &r.profile_id, &r.changes).unwrap();
    assert_eq!(p.mods.greeneye[0].version, "2.0.0");
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("2.0.0".into(), false), ("1.0.0".into(), true)]);
    assert_eq!(w.kept_settings(ModKind::Greeneye, "worstmap", "1.0.0"), [INI], "its settings stay kept");
    w.reapply();
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "2.0.0");
    assert_eq!(read(&w.g.path, INI), "zoom=9");
    // and the roll back still works afterwards, with the old settings
    kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    w.reapply();
    assert_eq!(read(&w.g.path, INI), "zoom=1");
    // Undo after the placement: 2.0.0 with its settings
    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    w.reapply();
    assert_eq!(read(&w.g.path, INI), "zoom=9");
    quick::revert(&w.dirs, &r.profile_id, &r.changes).unwrap();
    w.reapply();
    assert_eq!(engine_mods::marker(&w.g.path.join(MAP)).unwrap().version, "1.0.0");
    assert_eq!(read(&w.g.path, INI), "zoom=1");
}

/// SET (settings.json in Kiln's copy in shrouded_engine_tweaks/mods/<id>) and ShroudForge (setting values in its
/// extended.mod.json): the roll back gives each the settings it had.
#[test]
fn set_and_shroudforge_mods_roll_back_with_their_settings() {
    let w = world("set-sf");
    let set = w.root.join("src/flymode");
    put(&set, "init.lua", b"print('fly 1')");
    let sf = w.root.join("src/sfpack");
    put(&sf, "mod.json", br#"{"id":"sfpack","name":"SF Pack","version":"1.0.0"}"#);
    put(&sf, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client","server"],
        "settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    put(&sf, "src/mod.lua", b"return {}");
    w.drop_mod(&set);
    w.drop_mod(&sf);
    let set_json = "shrouded_engine_tweaks/mods/flymode/settings.json";
    put(&w.g.path, set_json, br#"{"height":1}"#);
    let cfg = w.root.join("config");
    engine_mods::set_setting(ModKind::ShroudForge, "sfpack", "speed", &serde_json::json!(7), Some(&w.g.path), &cfg, None).unwrap();
    // the updates
    put(&set, "init.lua", b"print('fly 2')");
    put(&sf, "mod.json", br#"{"id":"sfpack","name":"SF Pack","version":"2.0.0"}"#);
    w.drop_mod(&set);
    w.drop_mod(&sf);
    let speed = |w: &World| kiln_manage::shroudforge::read_mod(&w.g.path.join("mods/sfpack")).unwrap().settings[0].value.clone();
    assert_eq!(speed(&w), 7, "carried over by the update");
    assert_eq!(read(&w.g.path, set_json), r#"{"height":1}"#);
    put(&w.g.path, set_json, br#"{"height":5}"#);
    engine_mods::set_setting(ModKind::ShroudForge, "sfpack", "speed", &serde_json::json!(9), Some(&w.g.path), &cfg, None).unwrap();
    assert_eq!(w.kept_settings(ModKind::Set, "flymode", "0.0.0"), [set_json]);

    let pid = w.pid();
    let set_before = w.entry(ModKind::Set, "flymode");
    let r = kept::rollback(&w.dirs, Some(&w.g), &pid, ModKind::Set, "flymode", None).unwrap();
    assert_ne!(r.to.hash, set_before.hash);
    kept::rollback(&w.dirs, Some(&w.g), &pid, ModKind::ShroudForge, "sfpack", None).unwrap();
    assert_eq!(speed(&w), 9, "nothing changes before the placement");
    w.reapply();
    assert_eq!(read(&w.g.path, set_json), r#"{"height":1}"#);
    assert_eq!(read(&w.g.path, "shrouded_engine_tweaks/mods/flymode/init.lua"), "print('fly 1')");
    let m = kiln_manage::shroudforge::read_mod(&w.g.path.join("mods/sfpack")).unwrap();
    assert_eq!((m.version.as_str(), m.settings[0].value.clone()), ("1.0.0", serde_json::json!(7)));
    assert!(w.entry_bytes_match(ModKind::Set, "flymode") && w.entry_bytes_match(ModKind::ShroudForge, "sfpack"));
}

/// A Kiln mod runs from its Library entry (no game-folder copy, its configs are the profile's): the roll back swaps
/// the entry. A kept entry is listed after the others (what the launcher picks by default), Clean up leaves it, and
/// update checks look at the newest version, so the version rolled away from is not offered as an update again.
#[test]
fn a_kept_kiln_mod_is_never_picked_cleaned_up_or_offered_as_an_update() {
    let w = world("kiln");
    w.drop_mod(&kiln_mod(&w.root, "1.0.0"));
    w.drop_mod(&kiln_mod(&w.root, "2.0.0"));
    assert_eq!(w.versions(ModKind::Kiln, "stacks"), [("2.0.0".into(), false), ("1.0.0".into(), true)]);
    let unused: Vec<String> = library::unused(&w.dirs).unwrap().into_iter().map(|r| r.version).collect();
    assert!(unused.is_empty(), "Clean up leaves a kept version: {unused:?}");
    let target = |w: &World| kiln_manage::updates::coordinator::targets(&w.dirs).unwrap().into_iter()
        .find(|t| t.library_ref.id == "stacks").unwrap().library_ref.version;
    assert_eq!(target(&w), "2.0.0");

    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Kiln, "stacks", None).unwrap();
    assert_eq!(r.to.version, "1.0.0");
    assert!(r.settings.is_empty());
    assert!(w.entry_bytes_match(ModKind::Kiln, "stacks"));
    w.reapply();
    let file = profile::read_game_profile(&w.g.path).unwrap().unwrap();
    assert_eq!(file.mods.kiln[0].version.as_deref(), Some("1.0.0"));
    // 2.0.0 is the kept one now: listed after 1.0.0, so switching the mod on in another profile takes 1.0.0
    assert_eq!(w.versions(ModKind::Kiln, "stacks"), [("1.0.0".into(), false), ("2.0.0".into(), true)]);
    // update checks compare with 2.0.0, the newest the player has: no notice offers it again
    assert_eq!(target(&w), "2.0.0");
    // another profile made from scratch lists nothing kept; one copied from this one lists what it lists
    let other = profile::create(&w.dirs, "Other", Some(&w.pid())).unwrap();
    assert_eq!(other.mods.kiln.iter().map(|m| m.version.as_str()).collect::<Vec<_>>(), ["1.0.0"]);
}

/// keepVersions 0 keeps none (as before dev.29: the old version stays an ordinary unused Library version), 1 the last,
/// 2 the last two; lowering it trims the kept ones.
#[test]
fn the_count_in_settings_limits_the_kept_versions() {
    let w = world("count");
    w.settings(r#"{"keepVersions": 0}"#);
    w.drop_mod(&kiln_mod(&w.root, "1.0.0"));
    w.drop_mod(&kiln_mod(&w.root, "2.0.0"));
    assert_eq!(w.versions(ModKind::Kiln, "stacks"), [("2.0.0".into(), false), ("1.0.0".into(), false)]);
    assert_eq!(library::unused(&w.dirs).unwrap().len(), 1, "Clean up may remove it, as before");
    let e = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Kiln, "stacks", None).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound);

    w.settings(r#"{"keepVersions": 1}"#);
    w.drop_mod(&kiln_mod(&w.root, "3.0.0"));
    w.drop_mod(&kiln_mod(&w.root, "4.0.0"));
    let kept_of = |w: &World| w.versions(ModKind::Kiln, "stacks").into_iter().filter(|v| v.1).map(|v| v.0).collect::<Vec<_>>();
    assert_eq!(kept_of(&w), ["3.0.0"]);

    w.settings(r#"{"keepVersions": 2}"#);
    w.drop_mod(&kiln_mod(&w.root, "5.0.0"));
    w.drop_mod(&kiln_mod(&w.root, "6.0.0"));
    assert_eq!(kept_of(&w), ["5.0.0", "4.0.0"]);
    // the newest kept is the first roll back; a named one can be picked
    let r = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Kiln, "stacks",
        Some(&kept::VersionRef { version: "4.0.0".into(), hash: w.versions_hash("4.0.0") })).unwrap();
    assert_eq!(r.to.version, "4.0.0");
    assert_eq!(kept_of(&w), ["6.0.0", "5.0.0"]);
    w.settings(r#"{"keepVersions": 1}"#);
    kept::apply_count(&w.dirs).unwrap();
    assert_eq!(kept_of(&w), ["6.0.0"]);
}

impl World {
    fn versions_hash(&self, version: &str) -> String {
        library::list(&self.dirs, Some(ModKind::Kiln)).unwrap().into_iter().flat_map(|e| e.versions).find(|v| v.version == version).unwrap().hash
    }
}

/// A version is not kept when the Library's disk would then have less free than the data folder check keeps (the
/// update's result says so); with room, it is.
#[test]
fn the_disk_check_refuses_to_keep_and_the_update_says_so() {
    let w = world("disk");
    w.drop_mod(&kiln_mod(&w.root, "1.0.0"));
    let up = kept::with_free_space_for_tests(Some(64 * 1024 * 1024), || w.drop_mod(&kiln_mod(&w.root, "2.0.0")));
    assert_eq!(up.added[0].version, "2.0.0", "the update itself goes on");
    assert!(up.notices.iter().any(|n| n.contains("Big Stacks 1.0.0 was not kept for a roll back") && n.contains("free")), "{:?}", up.notices);
    assert_eq!(w.versions(ModKind::Kiln, "stacks"), [("2.0.0".into(), false), ("1.0.0".into(), false)]);
    let up = kept::with_free_space_for_tests(Some(64 * 1024 * 1024 * 1024), || w.drop_mod(&kiln_mod(&w.root, "3.0.0")));
    assert!(up.notices.is_empty(), "{:?}", up.notices);
    assert!(w.versions(ModKind::Kiln, "stacks").contains(&("2.0.0".into(), true)));
}

/// A kept entry removed by hand (or by an older launcher's Clean up) is forgotten: not listed as kept, no roll back
/// to it, its settings copy gone. One removed with the launcher's Remove too.
#[test]
fn a_kept_entry_removed_by_hand_is_forgotten() {
    let w = world("removed");
    w.drop_mod(&greeneye(&w.root, "1.0.0"));
    put(&w.g.path, INI, b"zoom=1");
    w.drop_mod(&greeneye(&w.root, "2.0.0"));
    let old = library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions[1].clone();
    assert!(old.kept.is_some());
    std::fs::remove_dir_all(&old.folder).unwrap();
    std::fs::remove_file(format!("{}.json", old.folder.display())).unwrap();
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("2.0.0".into(), false)]);
    let e = kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound, "{e:?}");
    // the next start (or change) tidies under the state's lock: the settings copy of the entry that is gone goes
    kept::recover(&w.dirs);
    assert!(!w.dirs.library().join(kept::SETTINGS_DIR).join("greeneye/worstmap").exists(), "its settings copy is gone");
    // the launcher's Remove of a kept version
    w.drop_mod(&greeneye(&w.root, "3.0.0"));
    let two = library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions.iter().find(|v| v.version == "2.0.0").cloned().unwrap();
    assert!(two.kept.is_some());
    library::remove(&w.dirs, &LibraryRef { kind: ModKind::Greeneye, id: "worstmap".into(), version: two.version, hash: two.hash }, false).unwrap();
    assert!(kept::kept_versions(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    assert!(!w.dirs.library().join(kept::SETTINGS_DIR).join("greeneye/worstmap").exists());
}

/// A modpack holds what its server was synced with (the server profile's entries): a kept entry no profile lists
/// never gets in.
#[test]
fn a_kept_entry_is_not_exported_into_a_modpack() {
    use kiln_manage::server::{control, instance::ServerInstance};
    let w = world("modpack");
    let eml = w.root.join("src/bigstacks");
    put(&eml, "mod.json", br#"{"id":"bigstacks","name":"Bigger Stacks","version":"1.2.0","capabilities":["patch"]}"#);
    put(&eml, "src/mod.lua", b"return 1");
    w.drop_mod(&eml);
    put(&eml, "mod.json", br#"{"id":"bigstacks","name":"Bigger Stacks","version":"1.3.0","capabilities":["patch"]}"#);
    w.drop_mod(&eml);
    assert_eq!(w.versions(ModKind::Eml, "bigstacks"), [("1.3.0".into(), false), ("1.2.0".into(), true)]);
    let server = w.root.join("server");
    std::fs::create_dir_all(server.join("kiln")).unwrap();
    let inst: ServerInstance = serde_json::from_value(serde_json::json!({
        "id": "t", "name": "t", "kind": "local", "dir": server, "port": 27140, "queryPort": 27140, "wine": null,
        "policy": "any", "source": "launcher", "remote": null })).unwrap();
    let m = w.entry(ModKind::Eml, "bigstacks");
    control::sync(&w.dirs, &inst, &[LibraryRef { kind: ModKind::Eml, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() }], &mut NoProgress).unwrap();
    let built = kiln_manage::modpack::build(&server, &w.root.join("out"), "Isles", "1", &mut NoProgress).unwrap();
    assert_eq!(built.manifest.mods.iter().map(|x| x.version.as_str()).collect::<Vec<_>>(), ["1.3.0"]);
}

/// "Use <version>" in a mod's menu (a profile saved as a whole: profile_save, kilnctl profile add) is an update too: the
/// version it replaced is kept, with its settings.
#[test]
fn a_version_picked_by_hand_keeps_the_one_it_replaced() {
    let w = world("picked");
    w.settings(r#"{"keepVersions": 1}"#);
    let v1 = greeneye(&w.root, "1.0.0");
    w.drop_mod(&v1);
    put(&w.g.path, INI, b"zoom=1");
    library::import(&w.dirs, &[greeneye(&w.root, "2.0.0")], &mut NoProgress).unwrap();
    let two = library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions[0].clone();
    assert_eq!(two.version, "2.0.0");
    let before = profile::get(&w.dirs, &w.pid()).unwrap();
    let mut after = before.clone();
    after.mods.greeneye[0].version = two.version.clone();
    after.mods.greeneye[0].hash = two.hash.clone();
    let saved = profile::save(&w.dirs, &after).unwrap();
    assert!(kept::follow_saved(&w.dirs, &before, &saved).is_empty());
    assert_eq!(w.versions(ModKind::Greeneye, "worstmap"), [("2.0.0".into(), false), ("1.0.0".into(), true)]);
    assert_eq!(w.kept_settings(ModKind::Greeneye, "worstmap", "1.0.0"), [INI]);
}

/// A DLL mod with its settings in the game's mods folder (Auto Loot Plus Lite: dinput8.dll + mods/auto-loot-plus/
/// config.txt, a native mod whose settings folder Kiln places there) and a Flame mod (Kiln's copy in mods/<id>):
/// the roll back brings back the config the DLL had written.
#[test]
fn a_dll_mods_config_and_a_flame_mods_settings_come_back_with_the_roll_back() {
    let w = world("dll-flame");
    let alp = w.root.join("src/AutoLootPlus Lite");
    let dll = |v: &str| build_test_dll(&["DirectInput8Create"], format!("auto-loot-plus.log\0dinput8-chain.ini\0mods\\auto-loot-plus\\config.txt\0{v}").as_bytes());
    put(&alp, "dinput8.dll", &dll("1"));
    put(&alp, "mods/auto-loot-plus/config.txt", b"radius=15");
    let flame = w.root.join("src/creative");
    put(&flame, "mod.json", br#"{"id":"creative","name":"Creative Mode","version":"1.4.6"}"#);
    put(&flame, "flame.json", br#"{"dll":"creative.dll"}"#);
    put(&flame, "creative.dll", &build_test_dll(&["FlameStartRuntime"], b"1.4.6"));
    w.drop_mod(&alp);
    w.drop_mod(&flame);
    let cfg = "mods/auto-loot-plus/config.txt";
    let fl = "mods/creative/settings.ini";
    assert_eq!(read(&w.g.path, cfg), "radius=15", "placed with the mod");
    put(&w.g.path, cfg, b"radius=20"); // the DLL saves the player's settings
    put(&w.g.path, fl, b"fly=1");
    // the updates
    put(&alp, "dinput8.dll", &dll("2"));
    put(&flame, "mod.json", br#"{"id":"creative","name":"Creative Mode","version":"1.5.0"}"#);
    put(&flame, "creative.dll", &build_test_dll(&["FlameStartRuntime"], b"1.5.0"));
    w.drop_mod(&alp);
    w.drop_mod(&flame);
    assert_eq!((read(&w.g.path, cfg), read(&w.g.path, fl)), ("radius=20".into(), "fly=1".into()), "carried over by the updates");
    assert_eq!(w.kept_settings(ModKind::Native, "auto-loot-plus", "0.0.0"), [cfg]);
    assert_eq!(w.kept_settings(ModKind::Flame, "creative", "1.4.6"), [fl]);
    put(&w.g.path, cfg, b"radius=30");
    put(&w.g.path, fl, b"fly=9");
    let pid = w.pid();
    let native_new = w.entry(ModKind::Native, "auto-loot-plus");
    kept::rollback(&w.dirs, Some(&w.g), &pid, ModKind::Native, "auto-loot-plus", None).unwrap();
    kept::rollback(&w.dirs, Some(&w.g), &pid, ModKind::Flame, "creative", None).unwrap();
    assert_ne!(w.entry(ModKind::Native, "auto-loot-plus").hash, native_new.hash);
    assert_eq!(read(&w.g.path, cfg), "radius=30", "nothing changes before the placement");
    w.reapply();
    assert_eq!(read(&w.g.path, cfg), "radius=20");
    assert_eq!(read(&w.g.path, fl), "fly=1");
    assert_eq!(engine_mods::marker(&w.g.path.join("mods/creative")).unwrap().version, "1.4.6");
    assert!(w.entry_bytes_match(ModKind::Native, "auto-loot-plus") && w.entry_bytes_match(ModKind::Flame, "creative"));
    // and forward again
    kept::rollback(&w.dirs, Some(&w.g), &pid, ModKind::Native, "auto-loot-plus", None).unwrap();
    w.reapply();
    assert_eq!(read(&w.g.path, cfg), "radius=30");
    assert_eq!(w.entry(ModKind::Native, "auto-loot-plus").hash, native_new.hash);
}

#[test]
fn removing_a_mod_cancels_its_pending_restore_before_it_is_added_again() {
    let w = world("remove-pending");
    w.drop_mod(&greeneye(&w.root, "1.0.0"));
    put(&w.g.path, INI, b"zoom=1");
    w.drop_mod(&greeneye(&w.root, "2.0.0"));
    put(&w.g.path, INI, b"zoom=9");
    kept::rollback(&w.dirs, Some(&w.g), &w.pid(), ModKind::Greeneye, "worstmap", None).unwrap();
    let before = profile::get(&w.dirs, &w.pid()).unwrap();
    let mut removed = before.clone();
    removed.mods.greeneye.clear();
    let removed = profile::save(&w.dirs, &removed).unwrap();
    kept::follow_saved(&w.dirs, &before, &removed);
    let mut readded = before.clone();
    readded.generation = removed.generation;
    let added = profile::save(&w.dirs, &readded).unwrap();
    kept::follow_saved(&w.dirs, &removed, &added);
    w.reapply();
    assert_eq!(read(&w.g.path, INI), "zoom=9", "the canceled restore must not overwrite settings on a later add");
}
