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
use kiln_manage::manifest::{ModKind, RunSide};
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

/// The launcher's native plan (profile::resolve_preview; its mod list and load order, dev28 inbox x-01 task 37, GitHub
/// #41): the package runs, from the game folder, so its entry is on and its reason only says how. One that Kiln cannot
/// place (two of its files would be one file on Windows: activation places none of them) does not run, and says why.
#[test]
fn the_preview_shows_a_game_folder_package_as_running_and_one_kiln_cannot_place_as_not() {
    let t = tmp("preview");
    let dirs = DataDirs::at(t.0.join("data"));
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    let pre = profile::resolve_preview(&dirs, &p, RunSide::Client).unwrap();
    let n = pre.native.iter().find(|n| n.id == r.id).unwrap_or_else(|| panic!("{pre:#?}"));
    assert!(n.enabled, "it runs: {n:#?}");
    let how = n.reason.as_deref().unwrap_or_default();
    assert!(how.contains("game folder") && how.contains("powrprof.dll"), "{how}");

    let dirs = DataDirs::at(t.0.join("data-clash"));
    let (pkg, _) = autoloot_package(&t.0.join("clash"));
    put(&pkg, "plugins/autolootcoop.dll", &build_test_dll(&[], b"AutolootCoop again"));
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    let pre = profile::resolve_preview(&dirs, &p, RunSide::Client).unwrap();
    let n = pre.native.iter().find(|n| n.id == r.id).unwrap_or_else(|| panic!("{pre:#?}"));
    assert!(!n.enabled, "Kiln places none of its files, so it does not run: {n:#?}");
    let why = n.reason.as_deref().unwrap_or_default();
    assert!(why.contains("not placed in the game folder") && why.contains("would both be"), "{why}");
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

#[test]
fn ordinary_mod_kinds_do_not_enter_game_folder_package_preflight() {
    use std::os::unix::fs::PermissionsExt;
    for kind in [ModKind::Native, ModKind::Kiln, ModKind::Eml, ModKind::ShroudForge] {
        let t = tmp(&format!("ordinary-{kind:?}"));
        let dirs = DataDirs::at(t.0.join("data"));
        let install = game(&t.0);
        // Preflight would refuse these two spellings. Neither belongs to the ordinary mod being activated.
        put(&install.path, "xinput9_1_0.dll", b"player's first file");
        put(&install.path, "XINPUT9_1_0.DLL", b"player's second file");
        let plugins = install.path.join("plugins");
        std::fs::create_dir(&plugins).unwrap();
        std::fs::set_permissions(&plugins, std::fs::Permissions::from_mode(0o555)).unwrap();
        let pkg = t.0.join("ordinary");
        match kind {
            ModKind::Native => put(&pkg, "winmm.dll", &build_test_dll(&["timeGetTime"], b"ordinary")),
            ModKind::Kiln => {
                put(&pkg, "kiln.mod.json", br#"{"id":"ordinary","version":"1.0.0","apiVersion":"0.2","entryAssembly":"M.dll","side":"both"}"#);
                put(&pkg, "M.dll", b"managed fixture");
            }
            ModKind::Eml | ModKind::ShroudForge => {
                put(&pkg, "mod.json", br#"{"id":"ordinary","name":"Ordinary","version":"1.0.0","capabilities":["patch"]}"#);
                put(&pkg, "src/mod.lua", b"return {}");
                if kind == ModKind::ShroudForge {
                    put(&pkg, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client","server"],"settings":{}}"#);
                }
            }
            _ => unreachable!(),
        }
        let r = import_one(&dirs, &pkg, kind);
        let mut p = profile_with(&dirs, "ordinary", Vec::new());
        match kind {
            ModKind::Native => p.mods.native.push(pm(&r, true)),
            ModKind::Kiln => p.mods.kiln.push(pm(&r, true)),
            ModKind::Eml => p.mods.eml.push(pm(&r, true)),
            ModKind::ShroudForge => p.mods.shroudforge.push(pm(&r, true)),
            _ => unreachable!(),
        }
        let p = profile::save(&dirs, &p).unwrap();
        let activated = profile::activate(&dirs, &p.id, &install);
        std::fs::set_permissions(&plugins, std::fs::Permissions::from_mode(0o755)).unwrap();
        assert!(activated.is_ok(), "{kind:?}: {activated:?}");
        assert!(!install.path.join(kiln_manage::game_files::RECEIPTS).exists());
        assert!(!install.path.join("kiln/state/game-files.lock").exists());
        assert_eq!(read(&install.path.join("xinput9_1_0.dll")), b"player's first file");
        assert_eq!(read(&install.path.join("XINPUT9_1_0.DLL")), b"player's second file");
        assert_eq!(std::fs::read_dir(&plugins).unwrap().count(), 0);
    }
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

// Review regressions exercise import, activation and launch without starting a game.
#[test]
fn receipt_staging_links_never_change_an_unrelated_file() {
    for hard in [false, true] {
        let t = tmp(if hard { "receipt-hard" } else { "receipt-sym" });
        let dirs = DataDirs::at(t.0.join("data"));
        let install = game(&t.0);
        let (pkg, _) = autoloot_package(&t.0);
        let r = import_one(&dirs, &pkg, ModKind::Native);
        let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
        put(&t.0, "unrelated", b"keep this file");
        let link = install.path.join("kiln/state/game-files.json.tmp");
        std::fs::create_dir_all(link.parent().unwrap()).unwrap();
        if hard { std::fs::hard_link(t.0.join("unrelated"), &link).unwrap(); }
        else { std::os::unix::fs::symlink(t.0.join("unrelated"), &link).unwrap(); }
        profile::activate(&dirs, &p.id, &install).unwrap();
        assert_eq!(read(&t.0.join("unrelated")), b"keep this file");
    }
}

#[test]
fn activation_refuses_legacy_staging_before_placing_a_loader() {
    let t = tmp("activation-stage");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let old = profile_with(&dirs, "old", vec![]);
    profile::activate(&dirs, &old.id, &install).unwrap();
    let before = read(&install.path.join("kiln/profile.json"));
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "new", vec![pm(&r, true)]);
    std::fs::create_dir(install.path.join("kiln/profile.json.tmp")).unwrap();
    assert!(profile::activate(&dirs, &p.id, &install).is_err());
    assert_eq!(read(&install.path.join("kiln/profile.json")), before);
    assert!(!install.path.join("powrprof.dll").exists());
    assert!(!install.path.join("plugins").exists());
}

#[test]
fn a_later_activation_failure_restores_the_placed_packages() {
    let t = tmp("activation-rollback");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let old = profile_with(&dirs, "old", vec![pm(&r, true)]);
    profile::activate(&dirs, &old.id, &install).unwrap();
    let before = read(&install.path.join("kiln/profile.json"));
    let new = profile_with(&dirs, "new", vec![]);
    // chain.ini is written after game-folder placement. Make that write fail.
    std::fs::remove_file(install.path.join("kiln/chain.ini")).unwrap();
    std::fs::create_dir(install.path.join("kiln/chain.ini")).unwrap();
    assert!(profile::activate(&dirs, &new.id, &install).is_err());
    assert_eq!(read(&install.path.join("kiln/profile.json")), before);
    assert_eq!(read(&install.path.join("powrprof.dll")), loader);
    assert!(install.path.join("plugins/AutolootCoop.dll").is_file());
}

#[test]
fn activation_failure_after_chaining_ual_restores_the_chain_and_removes_new_files() {
    let t = tmp("activation-ual-rollback");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let old = profile_with(&dirs, "old", vec![]);
    profile::activate(&dirs, &old.id, &install).unwrap();
    let before_profile = read(&install.path.join("kiln/profile.json"));
    let before_chain = read(&install.path.join("kiln/chain.ini"));
    let pkg = t.0.join("SmartWorkshop");
    put(&pkg, "dinput8.dll", &ual());
    put(&pkg, "Scripts/SmartWorkshop.asi", &asi(b"plugin"));
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "new", vec![pm(&r, true)]);
    // Engine placement reads this after game-files placement and the chain write.
    put(&install.path, "kiln/state/library-placements.json", b"broken receipt");
    assert!(profile::activate(&dirs, &p.id, &install).is_err());
    assert_eq!(read(&install.path.join("kiln/profile.json")), before_profile);
    assert_eq!(read(&install.path.join("kiln/chain.ini")), before_chain);
    assert!(!install.path.join("xinput9_1_0.dll").exists());
    assert!(!install.path.join("Scripts").exists());
    assert!(!install.path.join("kiln/state/game-files.json").exists());
}

#[test]
fn vanilla_and_no_mods_refuse_a_placed_loader_before_opening_steam() {
    use kiln_manage::launch::{self, LaunchMode, LaunchRequest};
    let t = tmp("launch-refusal");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let opened = std::cell::Cell::new(false);
    let opener = |_: &str| { opened.set(true); Ok(()) };
    for mode in [LaunchMode::Vanilla, LaunchMode::NoMods] {
        let request = LaunchRequest { mode, profile_id: None, skip_backup: true };
        let e = launch::play_with(&dirs, &install, &request, Some(&[]), &opener).unwrap_err();
        assert!(e.message.contains("powrprof.dll") && e.message.contains("activate"), "{e:?}");
        assert!(!opened.get());
        assert!(!install.path.join("kiln/play-once.ini").exists());
    }
    // An edited loader is still a loader. Turning it off cannot silently enable vanilla.
    put(&install.path, "powrprof.dll", b"changed loader");
    set_native(&dirs, &mut p, false);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let request = LaunchRequest { mode: LaunchMode::Vanilla, profile_id: None, skip_backup: true };
    assert!(launch::play_with(&dirs, &install, &request, Some(&[]), &opener).is_err());
    assert!(!opened.get());
    // Receipts that cannot be read must not silently allow a start without mods.
    put(&install.path, "kiln/state/game-files.json", b"broken receipt");
    assert!(launch::play_with(&dirs, &install, &request, Some(&[]), &opener).is_err());
    assert!(!opened.get());
}

#[test]
fn play_once_compares_placements_to_an_unapplied_profile_edit() {
    let t = tmp("once-placement");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    profile::write_once(&dirs, &p.id, &install).unwrap();
    set_native(&dirs, &mut p, false);
    let e = profile::write_once(&dirs, &p.id, &install).unwrap_err();
    assert!(e.message.contains("activate"), "{e:?}");
    assert!(install.path.join("powrprof.dll").is_file());
}

#[test]
fn native_phase_and_dependency_folders_do_not_turn_a_mod_into_a_loader() {
    for (name, phase, description, folder) in [
        ("world", "world", "Generated by Kiln from package", "plugins"),
        ("author", "early", "An author's native mod", "plugins"),
        ("deps", "early", "Generated by Kiln from package", "deps"),
    ] {
        let t = tmp(name);
        put(&t.0, "native.json", serde_json::to_vec(&serde_json::json!({
            "id":"m", "version":"1.0.0", "dlls":["powrprof.dll"], "phase":phase, "description":description
        })).unwrap().as_slice());
        put(&t.0, "powrprof.dll", &build_test_dll(&[], b"ordinary mod"));
        put(&t.0, &format!("{folder}/helper.dll"), &build_test_dll(&[], b"helper"));
        assert!(kiln_manage::game_files::plan(&t.0).is_none(), "{name}");
    }
}

#[test]
fn ultimate_asi_loader_requires_the_export_not_a_text_marker() {
    let t = tmp("ual-marker");
    put(&t.0, "native.json", br#"{"id":"m","version":"1.0.0","dlls":["dinput8.dll"]}"#);
    put(&t.0, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"mentions IsUltimateASILoader only"));
    assert!(kiln_manage::game_files::plan(&t.0).is_none());
}

#[test]
fn a_manifestless_dual_build_package_keeps_its_native_dll() {
    let t = tmp("dual-build");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let pkg = t.0.join("DualBuild");
    put(&pkg, "m.dll", &build_test_dll(&[], b"native build"));
    put(&pkg, "m.asi", &asi(b"optional ASI build"));
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let folder = library::path_of(&dirs, &r).unwrap();
    let n = kiln_manage::manifest::read_native(&folder).unwrap();
    assert_eq!(n.dlls, ["m.dll"]);
    assert!(n.asi.is_empty());
    let p = profile_with(&dirs, "dual", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(native_listed(&install.path, &r.id));
    assert!(!install.path.join("scripts").exists());
}

#[test]
fn synthetic_ual_obeys_the_shipped_auto_loot_plus_ordering_rule() {
    let t = tmp("ual-order");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let pkg = t.0.join("AutoLootPlus");
    put(&pkg, "native.json", br#"{"id":"auto-loot-plus","version":"1.0.0","dlls":["alp.dll"],"phase":"dllmain"}"#);
    put(&pkg, "alp.dll", &build_test_dll(&[], b"ALP"));
    let alp = import_one(&dirs, &pkg, ModKind::Native);
    let pkg = t.0.join("ual");
    put(&pkg, "dinput8.dll", &ual());
    let ual = import_one(&dirs, &pkg, ModKind::Native);
    put(&install.path, "kiln/compat-rules.json", &read(&Path::new(env!("CARGO_MANIFEST_DIR")).join("../../native/core/compat-rules.json")));
    let p = profile_with(&dirs, "order", vec![pm(&alp, true), pm(&ual, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let ini = kiln_manage::chain::read(&install.path).unwrap();
    assert!(ini.late.iter().any(|e| e.path == kiln_manage::game_files::UAL_CHAIN_LINE), "{ini:?}");
    let view = kiln_manage::compat::view(&dirs, &p, &install).unwrap();
    assert!(view.rules.iter().any(|r| r.id == "auto-loot-plus-before-ultimate-asi-loader" && r.applied), "{view:?}");
    let info = kiln_manage::game::launch_options_info(&install, &[]).unwrap();
    assert!(info.chained_override_missing, "a late UAL still needs its Proton override: {info:?}");
    assert!(info.advice.unwrap().contains("xinput9_1_0"));
}

#[test]
fn case_renamed_placements_still_block_vanilla_and_are_removed() {
    use kiln_manage::launch::{self, LaunchMode, LaunchRequest};
    for deactivate in [false, true] {
        let t = tmp(if deactivate { "case-deactivate" } else { "case-disable" });
        let dirs = DataDirs::at(t.0.join("data"));
        let install = game(&t.0);
        let (pkg, _) = autoloot_package(&t.0);
        let r = import_one(&dirs, &pkg, ModKind::Native);
        let mut p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
        profile::activate(&dirs, &p.id, &install).unwrap();
        std::fs::rename(install.path.join("powrprof.dll"), install.path.join("POWRPROF.DLL")).unwrap();
        std::fs::rename(install.path.join("plugins"), install.path.join("PLUGINS")).unwrap();
        let opened = std::cell::Cell::new(false);
        let request = LaunchRequest { mode: LaunchMode::Vanilla, profile_id: None, skip_backup: true };
        assert!(launch::play_with(&dirs, &install, &request, Some(&[]), &|_| { opened.set(true); Ok(()) }).is_err());
        assert!(!opened.get(), "Proton can still load the case-renamed loader");
        if deactivate { profile::deactivate_report(&dirs, &install).unwrap(); }
        else {
            set_native(&dirs, &mut p, false);
            profile::activate(&dirs, &p.id, &install).unwrap();
        }
        assert!(!install.path.join("POWRPROF.DLL").exists());
        assert!(!install.path.join("PLUGINS").exists());
    }
}

#[test]
fn conflicting_packages_are_never_partly_placed() {
    let t = tmp("package-conflict");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let first = import_one(&dirs, &pkg, ModKind::Native);
    let other = t.0.join("OtherLoot");
    put(&other, "powrprof.dll", &build_test_dll(&[], b"other loader plugins\\"));
    put(&other, "plugins/Other.dll", &build_test_dll(&[], b"other plugin"));
    let second = import_one(&dirs, &other, ModKind::Native);
    let p = profile_with(&dirs, "both", vec![pm(&first, true), pm(&second, true)]);
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(install.path.join("plugins/AutolootCoop.dll").is_file());
    assert!(!install.path.join("plugins/Other.dll").exists(), "a blocked loader must not leave its plugins behind: {a:?}");
    assert!(a.warnings.iter().any(|w| w.contains("powrprof.dll") && w.contains("not placed")), "{a:?}");
}

#[test]
fn cleanup_finds_case_renamed_files_and_folders() {
    let t = tmp("case-cleanup");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    std::fs::rename(install.path.join("powrprof.dll"), install.path.join("POWRPROF.DLL")).unwrap();
    std::fs::rename(install.path.join("plugins"), install.path.join("PLUGINS")).unwrap();
    kiln_manage::game_files::unplace(&install.path).unwrap();
    assert!(!install.path.join("POWRPROF.DLL").exists());
    assert!(!install.path.join("PLUGINS").exists());
}

#[test]
fn a_reused_hand_installed_loader_cannot_bypass_the_vanilla_guard() {
    use kiln_manage::launch::{self, LaunchMode, LaunchRequest};
    let t = tmp("borrowed-loader");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    put(&install.path, "powrprof.dll", &loader);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    for mode in [LaunchMode::Vanilla, LaunchMode::NoMods] {
        let opened = std::cell::Cell::new(false);
        let request = LaunchRequest { mode, profile_id: None, skip_backup: true };
        assert!(launch::play_with(&dirs, &install, &request, Some(&[]), &|_| { opened.set(true); Ok(()) }).is_err());
        assert!(!opened.get());
    }
    profile::deactivate_report(&dirs, &install).unwrap();
    assert_eq!(read(&install.path.join("powrprof.dll")), loader, "the borrowed loader stays the player's");
}

#[test]
fn asi_packages_share_one_folder_spelling_before_it_exists() {
    let t = tmp("asi-folders");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    put(&install.path, "xinput9_1_0.dll", &ual());
    let one = t.0.join("One");
    let two = t.0.join("Two");
    put(&one, "Scripts/a.asi", &asi(b"a"));
    put(&two, "scripts/b.asi", &asi(b"b"));
    let first = import_one(&dirs, &one, ModKind::Native);
    let second = import_one(&dirs, &two, ModKind::Native);
    let p = profile_with(&dirs, "both", vec![pm(&first, true), pm(&second, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(install.path.join("Scripts/a.asi").is_file());
    assert!(install.path.join("Scripts/b.asi").is_file());
    assert!(!install.path.join("scripts").exists());
}

#[test]
fn an_identical_loader_transfers_between_packages_without_disappearing() {
    let t = tmp("loader-transfer");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    let first = import_one(&dirs, &pkg, ModKind::Native);
    let other = t.0.join("OtherLoot");
    put(&other, "powrprof.dll", &loader);
    put(&other, "plugins/Other.dll", &build_test_dll(&[], b"other plugin"));
    let second = import_one(&dirs, &other, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&first, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    p.mods.native = vec![pm(&second, true)];
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&install.path.join("powrprof.dll")), loader);
    assert!(install.path.join("plugins/Other.dll").is_file());
    assert!(!install.path.join("plugins/AutolootCoop.dll").exists());
    profile::deactivate_report(&dirs, &install).unwrap();
    assert!(!install.path.join("powrprof.dll").exists());
}

#[test]
fn a_new_package_cannot_borrow_an_enabled_packages_owned_loader() {
    let t = tmp("enabled-owner");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    let first = import_one(&dirs, &pkg, ModKind::Native);
    let other = t.0.join("OtherLoot");
    put(&other, "powrprof.dll", &loader);
    put(&other, "plugins/Other.dll", &build_test_dll(&[], b"other plugin"));
    let second = import_one(&dirs, &other, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&first, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    // Put the new package first, so planning order cannot turn an owned file into a borrowed one.
    p.mods.native = vec![pm(&second, true), pm(&first, true)];
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!install.path.join("plugins/Other.dll").exists());
    assert!(install.path.join("plugins/AutolootCoop.dll").is_file());
    assert_eq!(read(&install.path.join("powrprof.dll")), loader);
}

#[test]
fn a_borrowed_loader_keeps_its_active_package_when_profile_order_changes() {
    let t = tmp("borrowed-order");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    put(&install.path, "powrprof.dll", &loader);
    let first = import_one(&dirs, &pkg, ModKind::Native);
    let other = t.0.join("OtherLoot");
    put(&other, "powrprof.dll", &loader);
    put(&other, "plugins/Other.dll", &build_test_dll(&[], b"other plugin"));
    let second = import_one(&dirs, &other, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&first, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    p.mods.native = vec![pm(&second, true), pm(&first, true)];
    let p = profile::save(&dirs, &p).unwrap();
    let result = profile::activate(&dirs, &p.id, &install).unwrap();
    assert!(!install.path.join("plugins/Other.dll").exists(), "the active owner must win even when it borrows the loader: {result:?}");
    assert!(install.path.join("plugins/AutolootCoop.dll").is_file());
    assert_eq!(read(&install.path.join("powrprof.dll")), loader);
}

#[test]
fn ambiguous_folder_spellings_refuse_before_removing_either_copy() {
    let t = tmp("ambiguous-folders");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    put(&install.path, "PLUGINS/AutolootCoop.dll", b"player's other copy");
    assert!(kiln_manage::game_files::unplace(&install.path).is_err(), "two existing spellings cannot be resolved safely");
    assert!(install.path.join("powrprof.dll").is_file());
    assert!(install.path.join("plugins/AutolootCoop.dll").is_file());
    assert_eq!(read(&install.path.join("PLUGINS/AutolootCoop.dll")), b"player's other copy");
}

#[test]
fn a_case_spelling_update_never_claims_the_players_parallel_folder() {
    let t = tmp("case-update-ownership");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let plugin = read(&install.path.join("plugins/AutolootCoop.dll"));
    put(&install.path, "PLUGINS/AutolootCoop.dll", &plugin);
    let update = t.0.join("update");
    let native = serde_json::json!({ "format": 1, "id": r.id, "version": "2.0.0", "description": "Generated by Kiln from package",
        "phase": "early", "dlls": ["powrprof.dll"] });
    put(&update, "native.json", &serde_json::to_vec(&native).unwrap());
    put(&update, "powrprof.dll", &loader);
    put(&update, "PLUGINS/AutolootCoop.dll", &plugin);
    let r = import_one(&dirs, &update, ModKind::Native);
    p.mods.native = vec![pm(&r, true)];
    let p = profile::save(&dirs, &p).unwrap();
    match profile::activate(&dirs, &p.id, &install) {
        Err(e) => assert!(e.message.contains("more than one name"), "{e:?}"),
        Ok(_) => { kiln_manage::game_files::unplace(&install.path).unwrap(); }
    }
    assert_eq!(read(&install.path.join("PLUGINS/AutolootCoop.dll")), plugin, "the player's identical copy must never become Kiln's");
    assert_eq!(read(&install.path.join("plugins/AutolootCoop.dll")), plugin, "the existing placement must not be forgotten");
}

#[test]
fn unix_readonly_files_can_be_removed_without_changing_their_permissions() {
    let t = tmp("readonly-loader");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, _) = autoloot_package(&t.0);
    let r = import_one(&dirs, &pkg, ModKind::Native);
    let p = profile_with(&dirs, "loot", vec![pm(&r, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let loader = install.path.join("powrprof.dll");
    let mut permissions = std::fs::metadata(&loader).unwrap().permissions();
    permissions.set_readonly(true);
    std::fs::set_permissions(&loader, permissions).unwrap();
    let preserved = t.0.join("players-link.dll");
    std::fs::hard_link(&loader, &preserved).unwrap();
    let result = kiln_manage::game_files::unplace(&install.path);
    assert!(result.is_ok(), "Unix deletes through the writable parent folder: {result:?}");
    assert!(!loader.exists());
    assert!(!install.path.join("plugins").exists());
    assert!(!install.path.join("AutolootCoop.ini").exists());
    assert!(std::fs::metadata(&preserved).unwrap().permissions().readonly(), "Kiln must not clear the player's permissions");
}

#[test]
fn a_conflicting_update_keeps_all_files_of_the_earlier_version() {
    let t = tmp("update-conflict");
    let dirs = DataDirs::at(t.0.join("data"));
    let install = game(&t.0);
    let (pkg, loader) = autoloot_package(&t.0);
    let old = import_one(&dirs, &pkg, ModKind::Native);
    let mut p = profile_with(&dirs, "loot", vec![pm(&old, true)]);
    profile::activate(&dirs, &p.id, &install).unwrap();
    let old_plugin = read(&install.path.join("plugins/AutolootCoop.dll"));
    std::fs::remove_file(pkg.join("plugins/AutolootCoop.dll")).unwrap();
    put(&pkg, "plugins/NewLoot.dll", &build_test_dll(&[], b"new plugin"));
    put(&pkg, "powrprof.dll", &build_test_dll(&[], b"new loader plugins\\"));
    let new = import_one(&dirs, &pkg, ModKind::Native);
    p.mods.native = vec![pm(&new, true)];
    let p = profile::save(&dirs, &p).unwrap();
    put(&install.path, "plugins/NewLoot.dll", b"player's file");
    let a = profile::activate(&dirs, &p.id, &install).unwrap();
    assert_eq!(read(&install.path.join("powrprof.dll")), loader);
    assert_eq!(read(&install.path.join("plugins/AutolootCoop.dll")), old_plugin);
    assert_eq!(read(&install.path.join("plugins/NewLoot.dll")), b"player's file");
    assert!(a.warnings.iter().any(|w| w.contains("NewLoot.dll")), "{a:?}");
    assert!(profile::write_once(&dirs, &p.id, &install).is_err(), "the blocked replacement is not the placed version");
}

#[test]
fn aliased_asi_destinations_are_refused_before_placement() {
    for nested in ["scripts/a.asi", "Scripts/A.ASI"] {
        let t = tmp(if nested.starts_with('S') { "alias-case" } else { "alias-map" });
        let dirs = DataDirs::at(t.0.join("data"));
        let install = game(&t.0);
        put(&install.path, "xinput9_1_0.dll", &ual());
        let pkg = t.0.join("Aliased");
        put(&pkg, "a.asi", &asi(b"root"));
        put(&pkg, nested, &asi(b"nested"));
        let r = import_one(&dirs, &pkg, ModKind::Native);
        let p = profile_with(&dirs, "alias", vec![pm(&r, true)]);
        let a = profile::activate(&dirs, &p.id, &install).unwrap();
        assert!(!install.path.join("scripts").exists() && !install.path.join("Scripts").exists(), "{a:?}");
        assert!(a.warnings.iter().any(|w| w.contains("both")), "{a:?}");
    }
}
