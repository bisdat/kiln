//! Packages whose files must be in the game folder (docs/kiln/engines.md "Files in the game folder"), from three player
//! reports (dev28 inbox x-01, tasks 22, 25 and 26):
//!
//! - a plugin loader under a proxy name the game loads itself (`powrprof.dll` + `plugins\AutolootCoop.dll` + its ini,
//!   Nexus 266): Kiln places the package in the game folder at activation and removes what it placed when the mod is
//!   off, and leaves a game-folder `powrprof.dll` it did not place alone;
//! - Ultimate ASI Loader with `.asi` plugins (Smart Workshop, Nexus 361): UAL goes next to the game as
//!   `xinput9_1_0.dll` (or Kiln's chained one is reused), chained from `kiln/chain.ini`, and the plugins where UAL looks;
//! - shroudlink's singleplayer package (Nexus 278): its client addon loads `shroudlink_core.dll` from the game folder,
//!   so a core left there makes kiln-shroudlink step aside; the launcher names that file.
//!
//! The mods' own files are not available: the packages here are made of test DLLs with the file layout the reports show.
#![cfg(unix)]

use kiln_manage::engine_mods;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
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
    let p = std::env::temp_dir().join(format!("km-gamefiles-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn read(p: &Path) -> Vec<u8> {
    std::fs::read(p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

fn game(t: &Path) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    GameInstall {
        id: "g-files".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z, source: "manual".into(),
    }
}

fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

fn import_one(dirs: &DataDirs, src: &Path, kind: ModKind) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    assert!(r.skipped.is_empty(), "nothing of the package is left out: {r:#?}");
    let of_kind: Vec<&LibraryRef> = r.imported.iter().chain(&r.existing).filter(|x| x.kind == kind).collect();
    assert_eq!(of_kind.len(), 1, "one {kind:?} entry: {r:#?}");
    of_kind[0].clone()
}

fn profile_with(dirs: &DataDirs, name: &str, native: Vec<ProfileMod>) -> profile::Profile {
    let mut p = profile::create(dirs, name, None).unwrap();
    p.mods.native = native;
    profile::save(dirs, &p).unwrap()
}

fn set_native(dirs: &DataDirs, p: &mut profile::Profile, enabled: bool) {
    for m in &mut p.mods.native { m.enabled = enabled; }
    *p = profile::save(dirs, p).unwrap();
}

/// chain.ini `[profile]` lines as the proxy reads them.
fn profile_lines(game: &Path) -> Vec<String> {
    kiln_manage::chain::read(game).map(|ini| ini.profile.into_iter().map(|e| e.path).collect()).unwrap_or_default()
}

fn native_listed(game: &Path, id: &str) -> bool {
    profile::read_game_profile(game).unwrap().unwrap().mods.native.iter().any(|m| m.id == id && m.enabled)
}

// ---- 22: a plugin loader under a proxy name the game loads itself ----

/// "Autoloot and coop loot sharing" 1.6.0 as the report lists it: `powrprof.dll` (the plugin loader), its plugin in
/// `plugins\`, the plugin's ini and loot list. Where the ini sits in the real package is not known (assumed: the
/// package root, beside the loader, as the player's working hand installation mirrored the package into the game
/// folder).
fn autoloot_package(root: &Path) -> (PathBuf, Vec<u8>) {
    let p = root.join("AutolootCoop 1.6.0 266 1.6.0 2026-11-12T08-39Z V06lte3xj");
    let loader = build_test_dll(&["PowerGetActiveScheme", "PowerReadACValue"], b"PluginLoader.log plugins\\");
    put(&p, "powrprof.dll", &loader);
    put(&p, "plugins/AutolootCoop.dll", &build_test_dll(&[], b"AutolootCoop"));
    put(&p, "AutolootCoop.ini", b"[Autoloot]\nEnabled=1\n");
    put(&p, "plugins/AutolootCoop_lootlist.txt", b"Iron Ore\nShroud Spores\n");
    put(&p, "README.txt", b"copy everything into the game folder");
    (p, loader)
}

#[test]
fn a_powrprof_plugin_loader_package_runs_from_the_game_folder() {
    let t = tmp("loader");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let g = &install.path;
    let (pkg, loader) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    assert_eq!(r.id, "autolootcoop");
    let mut p = profile_with(&dirs, "loot", vec![pm(&r, true)]);

    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&g.join("powrprof.dll")), loader, "the game imports POWRPROF.dll: the loader is next to the game");
    assert!(g.join("plugins/AutolootCoop.dll").is_file() && g.join("plugins/AutolootCoop_lootlist.txt").is_file());
    assert!(g.join("AutolootCoop.ini").is_file());
    assert!(!g.join("README.txt").exists(), "documents stay in the Library");
    assert!(!native_listed(g, &r.id), "Kiln does not load the Library copy of the loader as well");
    assert!(a.warnings.iter().any(|w| w.contains("powrprof.dll") && w.contains("game folder")), "{:#?}", a.warnings);

    // switched off: what Kiln placed goes, also the folder it made
    set_native(&dirs, &mut p, false);
    profile::activate(&dirs, &p.id, &install).unwrap();
    for f in ["powrprof.dll", "plugins", "AutolootCoop.ini"] {
        assert!(!g.join(f).exists(), "{f} is removed with the mod switched off");
    }

    // on again; the plugin writes its settings; deactivating takes the code away and leaves the player's settings
    set_native(&dirs, &mut p, true);
    profile::activate(&dirs, &p.id, &install).unwrap();
    std::fs::write(g.join("AutolootCoop.ini"), b"[Autoloot]\nEnabled=1\nRange=30\n").unwrap();
    let cleanup = profile::deactivate_report(&dirs, &install).unwrap();
    assert!(!g.join("powrprof.dll").exists() && !g.join("plugins/AutolootCoop.dll").exists(), "{cleanup:#?}");
    assert_eq!(read(&g.join("AutolootCoop.ini")), b"[Autoloot]\nEnabled=1\nRange=30\n", "the player's settings stay");
}

#[test]
fn a_game_folder_powrprof_kiln_did_not_place_is_left_alone() {
    let t = tmp("set");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let g = &install.path;
    // Shrouded Engine Tweaks' own loader, installed by hand
    let set = build_test_dll(&[], b"shrouded_engine_tweaks");
    put(g, "powrprof.dll", &set);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&g.join("powrprof.dll")), set, "SET's loader is not replaced");
    assert!(!g.join("plugins").exists(), "nothing of the package is placed beside another powrprof.dll");
    assert!(a.warnings.iter().any(|w| w.contains("powrprof.dll") && w.contains("did not place")), "{:#?}", a.warnings);
    profile::deactivate_report(&dirs, &install).unwrap();
    assert_eq!(read(&g.join("powrprof.dll")), set);
}

#[test]
fn a_plain_dll_mod_under_a_proxy_name_still_runs_from_the_library() {
    // InventoryLock (winmm.dll + its ini, no plugins folder) works from the Library today: unchanged
    let t = tmp("plain");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let pkg = t.0.join("InventoryLock 1.3.2 309");
    put(&pkg, "winmm.dll", &build_test_dll(&["timeGetTime"], b"InventoryLock"));
    put(&pkg, "InventoryLock.ini", b"[Lock]\n");
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "lock", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!install.path.join("winmm.dll").exists() && !install.path.join("InventoryLock.ini").exists());
    assert!(native_listed(&install.path, &r.id));
}

// ---- 25: Ultimate ASI Loader and .asi plugins ----

fn ual() -> Vec<u8> {
    build_test_dll(&["DirectInput8Create", "IsUltimateASILoader"], b"Ultimate ASI Loader")
}
fn asi(tag: &[u8]) -> Vec<u8> {
    build_test_dll(&["InitializeASI"], tag)
}

#[test]
fn ultimate_asi_loader_with_its_plugins_is_imported_and_placed() {
    let t = tmp("ual");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let g = &install.path;
    let pkg = t.0.join("Smart Workshop with ASI Loader");
    put(&pkg, "dinput8.dll", &ual());
    put(&pkg, "Scripts/SmartWorkshop.asi", &asi(b"smart workshop"));
    put(&pkg, "Scripts/SmartWorkshop.ini", b"[Workshop]\n");
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "asi", vec![pm(&r, true)]);

    profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&g.join("xinput9_1_0.dll")), ual(), "UAL runs under a name it knows, next to the game");
    assert!(!g.join("dinput8.dll").exists(), "Kiln's own proxy slot is never written by a mod");
    assert_eq!(read(&g.join("Scripts/SmartWorkshop.asi")), asi(b"smart workshop"));
    assert!(g.join("Scripts/SmartWorkshop.ini").is_file());
    let lines = profile_lines(g);
    assert!(lines.iter().any(|l| l.eq_ignore_ascii_case("..\\xinput9_1_0.dll")), "Kiln's proxy chains it: {lines:?}");
    assert!(!native_listed(g, &r.id), "nothing of it is a DLL kiln-core loads");
    // Proton: the Steam launch option the launcher asks for names xinput9_1_0 too
    let lo = kiln_manage::game::launch_options_info(&install, &[]).unwrap();
    assert!(lo.chained_override_missing, "{lo:?}");
    assert!(lo.advice.as_deref().is_some_and(|a| a.contains("xinput9_1_0")), "{lo:?}");

    set_native(&dirs, &mut p, false);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!g.join("xinput9_1_0.dll").exists() && !g.join("Scripts").exists());
    assert!(!profile_lines(g).iter().any(|l| l.to_ascii_lowercase().contains("xinput9_1_0")));
}

#[test]
fn asi_plugins_alone_use_the_ultimate_asi_loader_kiln_chains_already() {
    let t = tmp("asi-only");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let g = &install.path;
    // Kiln's installer found UAL as dinput8.dll and chained it as xinput9_1_0.dll (engines.md "Installer")
    put(g, "xinput9_1_0.dll", &ual());
    put(g, "kiln/chain.ini", b"[chain]\n..\\xinput9_1_0.dll\n[profile]\n");
    // Smart Workshop's client file: a Scripts folder with the .asi, no loader
    let pkg = t.0.join("Smart Workshop 361 1.0.0 2026-11-15T10-00Z AbCdEf123");
    put(&pkg, "Scripts/SmartWorkshop.asi", &asi(b"smart workshop"));
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "asi", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&g.join("Scripts/SmartWorkshop.asi")), asi(b"smart workshop"));
    assert_eq!(read(&g.join("xinput9_1_0.dll")), ual(), "the installer's UAL stays as it is");
    let ini = kiln_manage::chain::read(g).unwrap();
    assert!(ini.chain.iter().any(|e| e.path.eq_ignore_ascii_case("..\\xinput9_1_0.dll")));
    assert!(!ini.profile.iter().any(|e| e.path.to_ascii_lowercase().contains("xinput9_1_0")), "chained once: {ini:?}");
    // uninstalling the profile leaves the installer's UAL; the plugin Kiln placed goes
    set_native(&dirs, &mut p, false);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!g.join("Scripts").exists());
    assert_eq!(read(&g.join("xinput9_1_0.dll")), ual());
}

#[test]
fn asi_plugins_without_any_ultimate_asi_loader_say_what_is_missing() {
    let t = tmp("asi-none");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let pkg = t.0.join("SmartWorkshop");
    put(&pkg, "Scripts/SmartWorkshop.asi", &asi(b"smart workshop"));
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "asi", vec![pm(&r, true)]);
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!install.path.join("Scripts").exists(), "{:#?}", a.warnings);
    assert!(a.warnings.iter().any(|w| w.contains("Ultimate ASI Loader")), "{:#?}", a.warnings);
}

#[test]
fn a_lone_asi_file_and_a_lone_ultimate_asi_loader_are_library_mods() {
    let t = tmp("lone");
    let dirs = DataDirs::at(t.0.join("data"));
    put(&t.0, "drop/SmartWorkshop.asi", &asi(b"x"));
    put(&t.0, "ual/dinput8.dll", &ual());
    import_one(&dirs, &t.0.join("drop/SmartWorkshop.asi"), ModKind::Native);
    // UAL's own download is a dinput8.dll: never "copy it into the Enshrouded folder" (it would replace Kiln's proxy)
    import_one(&dirs, &t.0.join("ual/dinput8.dll"), ModKind::Native);
}

// ---- 26: shroudlink's singleplayer package ----

fn shroudlink_core() -> Vec<u8> {
    build_test_dll(&["CrashHandlerFactory", "DirectInput8Create"], b"shroudlink server 0.8.0 chestsort_server_")
}

#[test]
fn a_shroudlink_core_in_the_game_folder_is_named_when_the_addon_comes_from_the_library() {
    let t = tmp("sl");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let g = &install.path;
    // an older hand installation of the singleplayer package left its core next to the game (Spirimtin's report)
    put(g, "shroudlink_core.dll", &shroudlink_core());
    let pkg = t.0.join("Singleplayer - addon core and mods 278 0.8.1 2026-11-15T20-27Z QVky0pgrn");
    put(&pkg, "dinput8.dll", &build_test_dll(&["DirectInput8Create", "shroudlink_shutdown"], b"shroudlink client addon"));
    put(&pkg, "shroudlink_core.dll", &shroudlink_core());
    put(&pkg, "shroudlink_mods/assistants.dll", &build_test_dll(&["shroudlink_mod_v2"], b""));
    let r = library::import(&dirs, &[pkg], &mut NoProgress).unwrap();
    let addon = r.imported.iter().find(|m| m.kind == ModKind::Native).cloned().expect("the addon is a native mod");
    let assistants = r.imported.iter().find(|m| m.kind == ModKind::Shroudlink).cloned().expect("assistants");
    let mut p = profile::create(&dirs, "sp", None).unwrap();
    p.mods.native = vec![pm(&addon, true)];
    p.mods.shroudlink = vec![pm(&assistants, true)];
    let p = profile::save(&dirs, &p).unwrap();
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    let note = a.own_loaders.iter().find(|n| n.loader.kind == ModKind::Shroudlink)
        .unwrap_or_else(|| panic!("the addon starts the core in the game folder: {:#?}", a.own_loaders));
    assert_eq!(note.loader.file, "shroudlink_core.dll");
    assert_eq!(note.not_copied, ["assistants"]);
    assert!(note.text.contains("Remove shroudlink_core.dll") && note.text.contains("MODS tab"), "{}", note.text);
    assert!(!note.text.contains("Wine"), "the addon loads it by path, no DLL override is involved: {}", note.text);
    let st = engine_mods::status(g);
    let row = st.iter().find(|s| s.kind == ModKind::Shroudlink).unwrap();
    assert_eq!(row.own_loader.as_ref().map(|l| l.loader.file.as_str()), Some("shroudlink_core.dll"));

    // without that file the addon only draws the MODS tab: kiln-shroudlink runs the mods, nothing to say
    std::fs::remove_file(g.join("shroudlink_core.dll")).unwrap();
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(a.own_loaders.iter().all(|n| n.loader.kind != ModKind::Shroudlink), "{:#?}", a.own_loaders);
}
