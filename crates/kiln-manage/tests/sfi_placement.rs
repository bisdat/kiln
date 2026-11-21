// SPDX-License-Identifier: MIT
//! Kiln's interpreter route for ShroudForge-format mods in kiln-manage, through main's own operations:
//! activation (engine_mods::place, profile::activate), settings, taking a folder over, removal, own_loaders::find.
//!
//! - With kiln.json selecting the interpreter (its hold lifted), ShroudForge's mods folder is `<game>/mods`: a
//!   ShroudForge install whose config names another folder is never read, never written (an inotify tripwire over
//!   `<game>/shroudforge` sees nothing, a snapshot sees nothing changed), and nothing lands in its folder.
//! - ShroudForge's own loader in the game keeps main's route; so do hosted, installed, an invalid setting and a held
//!   `kiln` (the golden below was written by main's code, before the route existed).
//! - Every other loader's route (EMF and its Lua mods, Greeneye, Flame, SET, XHL-Core's ini, the game-folder natives,
//!   Copy mode) is the same in kiln mode as in hosted mode.
//!
//! Every test here but the twin of the other loaders' routes (and the check of the test helper it uses) needs a Wine
//! prefix's Z: drive or the Linux tripwire: they run on Unix; the twin runs on Windows too (its real paths and Copy mode).
#![cfg_attr(not(unix), allow(dead_code, unused_imports))]

mod sfi_routing_common;

use kiln_manage::engine_mods;
use kiln_manage::game::PathMode;
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::own_loaders::{self, ProfileView};
use kiln_manage::paths::DataDirs;
use kiln_manage::profile;
use kiln_manage::shroudforge;
use sfi_routing_common::*;
use std::path::{Path, PathBuf};

struct World {
    t: Tmp,
    dirs: DataDirs,
    game: PathBuf,
    sf: LibraryRef,
    off: LibraryRef,
    tools: LibraryRef,
    gliders: LibraryRef,
    wardrobe: LibraryRef,
}

/// Building Tools for ShroudForge (inbox 86): a ShroudForge package that is an XHL-Core plugin too.
fn building_tools(root: &Path) -> PathBuf {
    let d = root.join("src/building-tools");
    put(
        &d,
        "mod.json",
        br#"{"id":"building-tools","name":"Building Tools","version":"0.33.2"}"#,
    );
    put(
        &d,
        "extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"settings":{}}"#,
    );
    put(
        &d,
        "BuildingTools.dll",
        &kiln_manage::pe::build_test_dll(&["XhlPluginEntry"], b"plugin"),
    );
    put(
        &d,
        "native-plugin.ini",
        b"[Plugin]\r\nDll=BuildingTools.dll\r\n",
    );
    d
}

/// A game (its ShroudForge install's config naming `sf_mods`), a kiln.json, and a Library with two ShroudForge mods,
/// Building Tools, a legacy EML mod and one that needs ShroudForge's runtime.
fn world(tag: &str, kiln_json: Option<&str>, interpreter: bool, sf_mods: Option<&str>) -> World {
    let t = tmp(tag);
    let dirs = DataDirs::at(t.0.join("data"));
    let game = game(&t.0.join("game"), interpreter);
    sf_install(&game, sf_mods);
    select(&game, kiln_json);
    let sf = import(
        &dirs,
        &sf_package(&t.0, "sfpack", false),
        ModKind::ShroudForge,
        "sfpack",
    );
    let off = import(
        &dirs,
        &sf_package(&t.0, "sfoff", false),
        ModKind::ShroudForge,
        "sfoff",
    );
    let tools = import(
        &dirs,
        &building_tools(&t.0),
        ModKind::ShroudForge,
        "building-tools",
    );
    let gliders = import(
        &dirs,
        &eml_package(&t.0, "gliders"),
        ModKind::Eml,
        "gliders",
    );
    let wardrobe = import(
        &dirs,
        &eml_runtime_package(&t.0, "wardrobe"),
        ModKind::Eml,
        "wardrobe",
    );
    World {
        t,
        dirs,
        game,
        sf,
        off,
        tools,
        gliders,
        wardrobe,
    }
}

impl World {
    fn profile(&self) -> profile::Profile {
        let mut p = profile::create(&self.dirs, "routes", None).unwrap();
        p.mods.shroudforge = vec![
            pm(&self.sf, true),
            pm(&self.off, false),
            pm(&self.tools, false),
        ];
        p.mods.eml = vec![pm(&self.gliders, true), pm(&self.wardrobe, true)];
        profile::save(&self.dirs, &p).unwrap()
    }
    fn view(&self, p: &profile::Profile) -> ProfileView {
        ProfileView::of_profile(p).with_library(&self.dirs, p)
    }
    fn lib(&self, r: &LibraryRef) -> PathBuf {
        library::path_of(&self.dirs, r).unwrap()
    }
}

/// shroudlink's singleplayer package (Nexus 278 layout, test DLLs): the client addon (a native mod), its core, a mod;
/// an older hand installation left the core next to the game. -> (the addon, the mod)
fn shroudlink_package(dirs: &DataDirs, root: &Path, game: &Path) -> (LibraryRef, LibraryRef) {
    let core = kiln_manage::pe::build_test_dll(
        &["CrashHandlerFactory", "DirectInput8Create"],
        b"shroudlink server 0.8.0 chestsort_server_",
    );
    put(game, "shroudlink_core.dll", &core);
    let pkg = root.join("src/Singleplayer - addon core and mods 278 0.8.1");
    put(
        &pkg,
        "dinput8.dll",
        &kiln_manage::pe::build_test_dll(
            &["DirectInput8Create", "shroudlink_shutdown"],
            b"shroudlink client addon",
        ),
    );
    put(&pkg, "shroudlink_core.dll", &core);
    put(
        &pkg,
        "shroudlink_mods/assistants.dll",
        &kiln_manage::pe::build_test_dll(&["shroudlink_mod_v2"], b""),
    );
    let r = library::import(dirs, &[pkg], &mut kiln_manage::task::NoProgress).unwrap();
    let addon = r
        .imported
        .iter()
        .find(|m| m.kind == ModKind::Native)
        .cloned()
        .expect("the addon");
    let mods = r
        .imported
        .iter()
        .find(|m| m.kind == ModKind::Shroudlink)
        .cloned()
        .expect("assistants");
    (addon, mods)
}

fn tags(v: &[String]) -> Vec<&str> {
    v.iter().map(String::as_str).collect()
}

fn copy_dir(from: &Path, to: &Path) {
    for e in std::fs::read_dir(from).unwrap().flatten() {
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if e.file_type().unwrap().is_dir() {
            std::fs::create_dir_all(&dst).unwrap();
            copy_dir(&src, &dst);
        } else {
            std::fs::create_dir_all(to).unwrap();
            std::fs::copy(&src, &dst).unwrap();
        }
    }
}

const SHROUDFORGE_DIR: &str = "shroudforge";

#[cfg(unix)]
#[test]
fn kiln_mode_places_switches_sets_takes_over_and_removes_in_game_mods_without_reading_shroudforges_folder()
 {
    // ShroudForge's config names SFMODS; a hosted start would place there (the next test)
    let w = world("kiln", Some(KILN), true, Some("SFMODS"));
    let p = w.profile();
    let sf_root = w.game.join(SHROUDFORGE_DIR);
    let before = snapshot(&sf_root);
    let mut wire = Tripwire::arm(&sf_root);
    let mods = w.game.join("mods");

    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    for t in [
        "shroudforge:sfpack",
        "shroudforge:sfoff",
        "shroudforge:building-tools",
        "eml:gliders",
        "eml:wardrobe",
    ] {
        assert!(rep.placed.iter().any(|x| x == t), "{t}: {rep:#?}");
    }
    assert!(
        !w.game.join("SFMODS").exists(),
        "nothing goes to the folder ShroudForge's own config names"
    );
    let on = |id: &str| shroudforge::read_mod(&mods.join(id)).unwrap().enabled;
    assert!(
        on("sfpack") && !on("sfoff") && !on("building-tools"),
        "the profile is the switch"
    );
    // the interpreter carries the EML mods where Kiln's hosted ShroudForge did: with ShroudForge's switch on
    assert!(on("gliders") && on("wardrobe"));
    // XHL-Core's ini of a ShroudForge copy that is off is set aside, as on main's route
    assert!(
        mods.join("building-tools/native-plugin.ini.kiln-off")
            .is_file(),
        "{rep:#?}"
    );
    let eml = rep
        .own_loaders
        .iter()
        .find(|n| n.loader.kind == ModKind::Eml)
        .expect("the EML note");
    assert_eq!(
        (
            eml.loader.file.as_str(),
            eml.loader.folder.as_str(),
            eml.loader.engine
        ),
        (
            "kiln/plugins/kiln-sfi.dll",
            "mods",
            kiln_manage::engines::Engine::ShroudForge
        ),
        "{eml:#?}"
    );
    assert_eq!(serde_json::to_value(eml.loader.via).unwrap(), "interpreter");
    assert_eq!(tags(&eml.copied), ["gliders", "wardrobe"], "{eml:#?}");
    assert!(
        eml.text.contains("Kiln's interpreter"),
        "{}",
        eml.text
    );
    assert!(
        rep.own_loaders
            .iter()
            .all(|n| serde_json::to_value(n.loader.via).unwrap() != "hosted-shroud-forge"),
        "{:#?}",
        rep.own_loaders
    );
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "activation read below <game>/shroudforge"
    );

    // the same answers outside an activation
    assert_eq!(
        engine_mods::placed_folder(ModKind::ShroudForge, &w.game, "sfpack"),
        Some(mods.join("sfpack"))
    );
    assert_eq!(
        engine_mods::server_root(ModKind::ShroudForge, &w.game),
        Some(mods.clone())
    );
    let loaders = own_loaders::find(&w.game, Some(&w.view(&p)));
    assert!(
        loaders
            .iter()
            .any(|l| l.file == "kiln/plugins/kiln-sfi.dll")
            && !loaders
                .iter()
                .any(|l| l.file == "kiln/plugins/kiln-shroudforge.dll"),
        "{loaders:#?}"
    );
    assert_eq!(
        own_loaders::sf_eml_folder(&w.game, &loaders),
        Some(mods.clone())
    );
    let notes = own_loaders::notes(&w.game, &loaders, Some(&w.view(&p)));
    assert!(
        notes
            .iter()
            .any(|n| n.loader.kind == ModKind::Eml && n.copied.len() == 2),
        "{notes:#?}"
    );

    // settings live in Kiln's copy in <game>/mods
    let cfg = w.t.0.join("cfg");
    let s = engine_mods::settings(
        ModKind::ShroudForge,
        "sfpack",
        Some(&w.game),
        &cfg,
        Some(&w.lib(&w.sf)),
    )
    .unwrap();
    assert_eq!(
        s.file.as_deref(),
        Some(
            mods.join("sfpack/extended.mod.json")
                .to_string_lossy()
                .as_ref()
        ),
        "{s:#?}"
    );
    engine_mods::set_setting(
        ModKind::ShroudForge,
        "sfpack",
        "speed",
        &serde_json::json!(7),
        Some(&w.game),
        &cfg,
        Some(&w.lib(&w.sf)),
    )
    .unwrap();
    assert_eq!(
        shroudforge::read_mod(&mods.join("sfpack"))
            .unwrap()
            .settings[0]
            .value,
        serde_json::json!(7)
    );

    // a second activation keeps every copy; nothing of the player's to take over; the copy is Kiln's already
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        rep.placed.is_empty() && rep.changed.is_empty() && rep.retained.is_empty(),
        "{rep:#?}"
    );
    assert_eq!(
        engine_mods::own_folders(&w.dirs, &p, &w.game).unwrap(),
        Vec::new()
    );
    let a = engine_mods::adopt(&w.dirs, &p, &w.game, ModKind::ShroudForge, "sfpack").unwrap();
    assert!(a.already && a.path == mods.join("sfpack"), "{a:#?}");
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "settings, a second activation or a take-over read below <game>/shroudforge"
    );

    // the player edits Kiln's copy, then switches the mod off: the copy stays, named, and is not switched
    put(&mods, "sfpack/src/mod.lua", b"-- the player's edit");
    let mut off = p.clone();
    off.mods.shroudforge[0].enabled = false;
    let rep = engine_mods::place(&w.dirs, &off, &w.game).unwrap();
    assert!(
        rep.retained
            .iter()
            .any(|r| r.path == mods.join("sfpack") && r.may_run),
        "{rep:#?}"
    );
    assert!(on("sfpack"), "a copy the player changed is never switched");

    // removal: Kiln's unchanged copies go, a copy with the player's values or edits stays
    let cleanup = engine_mods::unplace(&w.dirs, &w.game).unwrap();
    for id in ["sfoff", "building-tools", "gliders", "wardrobe"] {
        assert!(
            cleanup.removed.contains(&mods.join(id)),
            "{id}: {cleanup:#?}"
        );
    }
    assert!(
        cleanup
            .retained
            .iter()
            .any(|r| r.path == mods.join("sfpack")),
        "{cleanup:#?}"
    );
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "removal read below <game>/shroudforge"
    );
    assert_eq!(
        snapshot(&sf_root),
        before,
        "nothing below <game>/shroudforge changed"
    );
}

/// The whole activation (profile::activate: game files, chain plan, placement, profile.json), its refresh, a play-once
/// profile, the active profile's reading and deactivation, in kiln mode: profile.json names the copies in
/// `<game>/mods`, and nothing below `<game>/shroudforge` is read or written.
#[cfg(unix)]
#[test]
fn kiln_mode_activation_refresh_once_and_deactivation_never_read_shroudforges_folder() {
    for mode in [PathMode::Z, PathMode::Copy] {
        let w = world("activate", Some(KILN), true, Some("SFMODS"));
        let p = w.profile();
        let inst = install(&w.t.0, &w.game, mode);
        let sf_root = w.game.join(SHROUDFORGE_DIR);
        let before = snapshot(&sf_root);
        let mut wire = Tripwire::arm(&sf_root);
        let a = profile::activate(&w.dirs, &p.id, &inst).unwrap();
        assert!(
            a.own_loaders
                .iter()
                .any(|n| serde_json::to_value(n.loader.via).unwrap() == "interpreter"),
            "{a:#?}"
        );
        let file = profile::read_game_profile(&w.game).unwrap().unwrap();
        let text = serde_json::to_string(&file).unwrap();
        assert!(!text.contains("SFMODS"), "{mode:?}: {text}");
        assert!(w.game.join("mods/sfpack").is_dir() && !w.game.join("SFMODS").exists());
        profile::refresh_active(&w.dirs, &inst).unwrap();
        profile::active(&w.dirs, &inst).unwrap();
        // (main refuses a play-once profile that lists ShroudForge mods: their folder is fixed; its rule, either route)
        let _ = profile::write_once(&w.dirs, &p.id, &inst);
        let cleanup = profile::deactivate_report(&w.dirs, &inst).unwrap();
        assert!(
            cleanup.removed.contains(&w.game.join("mods/sfpack")),
            "{cleanup:#?}"
        );
        assert_eq!(wire.events(), Vec::<String>::new(), "{mode:?}");
        assert_eq!(snapshot(&sf_root), before, "{mode:?}");
    }
}

/// The control: the same game with main's route (no kiln.json: hosted) places in the folder ShroudForge's config names,
/// and the tripwire sees it read that config.
#[cfg(unix)]
#[test]
fn main_route_follows_shroudforges_own_config_and_the_tripwire_sees_it() {
    let w = world("hosted", None, true, Some("SFMODS"));
    let p = w.profile();
    let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        w.game.join("SFMODS/sfpack").is_dir() && !w.game.join("mods/sfpack").exists(),
        "{rep:#?}"
    );
    assert!(
        rep.own_loaders
            .iter()
            .any(|n| serde_json::to_value(n.loader.via).unwrap() == "hosted-shroud-forge"),
        "{:#?}",
        rep.own_loaders
    );
    #[cfg(target_os = "linux")]
    assert!(
        wire.events()
            .iter()
            .any(|e| e.contains("modloader-config.json")),
        "the tripwire sees ShroudForge's config read"
    );
    let _ = &mut wire;
}

/// Nondefault and Windows-spelled folders in ShroudForge's config (relative, verbatim, another drive) change nothing in
/// kiln mode.
#[cfg(unix)]
#[test]
fn kiln_mode_ignores_every_folder_shroudforges_config_names() {
    for (i, cfg) in [
        "SFMODS",
        "..\\outside\\mods",
        r"\\?\C:\Games\Enshrouded\mods2",
        r"D:\sf\mods",
        r"\\nas\mods",
    ]
    .into_iter()
    .enumerate()
    {
        let w = world(&format!("cfg{i}"), Some(KILN), true, Some(cfg));
        let p = w.profile();
        let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
        let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
        assert!(
            w.game.join("mods/sfpack").is_dir()
                && rep.placed.iter().any(|x| x == "shroudforge:sfpack"),
            "{cfg}: {rep:#?}"
        );
        assert_eq!(wire.events(), Vec::<String>::new(), "{cfg}");
        assert_eq!(
            engine_mods::placed_folder(ModKind::ShroudForge, &w.game, "sfpack"),
            Some(w.game.join("mods/sfpack")),
            "{cfg}"
        );
    }
}

/// A player's own folder in <game>/mods: listed, refused when two of them hold the package, taken over on request, its
/// settings read where it is. ShroudForge's own folder (SFMODS, holding the same package) is never looked at.
#[cfg(unix)]
#[test]
fn kiln_mode_takes_over_and_refuses_the_players_folders_in_game_mods() {
    let w = world("own", Some(KILN), true, Some("SFMODS"));
    let p = w.profile();
    let mods = w.game.join("mods");
    copy_dir(&w.lib(&w.sf), &mods.join("my-sfpack"));
    copy_dir(&w.lib(&w.sf), &mods.join("again-sfpack"));
    copy_dir(&w.lib(&w.sf), &w.game.join("SFMODS/sfpack"));
    let before = snapshot(&w.game.join(SHROUDFORGE_DIR));
    let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
    let cfg = w.t.0.join("cfg");
    // two folders hold the package (duplicate package ids): Kiln picks none
    let own = engine_mods::own_folders(&w.dirs, &p, &w.game).unwrap();
    assert!(
        own.len() == 1
            && own[0]
                .refusal
                .as_deref()
                .is_some_and(|r| r.contains("more than one of your folders")),
        "{own:#?}"
    );
    let s = engine_mods::settings(
        ModKind::ShroudForge,
        "sfpack",
        Some(&w.game),
        &cfg,
        Some(&w.lib(&w.sf)),
    )
    .unwrap();
    assert!(
        s.note
            .as_deref()
            .is_some_and(|n| n.contains("more than one")),
        "{s:#?}"
    );
    assert!(engine_mods::adopt(&w.dirs, &p, &w.game, ModKind::ShroudForge, "sfpack").is_err());
    // activation places nothing beside the player's folders and never switches them
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        rep.own.iter().any(|x| x == "shroudforge:sfpack") && !mods.join("sfpack").exists(),
        "{rep:#?}"
    );
    // one left: listed with Kiln's name for it, its settings read there, taken over on request
    std::fs::remove_dir_all(mods.join("again-sfpack")).unwrap();
    let own = engine_mods::own_folders(&w.dirs, &p, &w.game).unwrap();
    assert_eq!(
        (
            own.len(),
            own[0].path.clone(),
            own[0].rename_to.clone(),
            own[0].refusal.clone()
        ),
        (1, mods.join("my-sfpack"), Some(mods.join("sfpack")), None),
        "{own:#?}"
    );
    let s = engine_mods::settings(
        ModKind::ShroudForge,
        "sfpack",
        Some(&w.game),
        &cfg,
        Some(&w.lib(&w.sf)),
    )
    .unwrap();
    assert!(
        s.own_folder
            && s.file.as_deref()
                == Some(
                    mods.join("my-sfpack/extended.mod.json")
                        .to_string_lossy()
                        .as_ref()
                ),
        "{s:#?}"
    );
    let a = engine_mods::adopt(&w.dirs, &p, &w.game, ModKind::ShroudForge, "sfpack").unwrap();
    assert_eq!(
        (a.path.clone(), a.renamed_from.clone(), a.already),
        (mods.join("sfpack"), Some(mods.join("my-sfpack")), false),
        "{a:#?}"
    );
    assert!(
        w.game.join("SFMODS/sfpack").is_dir(),
        "ShroudForge's own folder is not Kiln's to touch"
    );
    assert_eq!(wire.events(), Vec::<String>::new());
    assert_eq!(snapshot(&w.game.join(SHROUDFORGE_DIR)), before);
}

/// Without Kiln's interpreter installed nothing runs a package that needs ShroudForge's runtime: it is not placed at all
/// (no startup data without its runtime), and no loader carries the EML mods (kiln-eml runs them from the Library).
#[cfg(unix)]
#[test]
fn kiln_mode_without_the_interpreter_places_no_half_of_a_runtime_package() {
    let w = world("missing", Some(KILN), false, Some("SFMODS"));
    let p = w.profile();
    let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    let mods = w.game.join("mods");
    assert!(
        !mods.join("wardrobe").exists() && !mods.join("gliders").exists(),
        "{rep:#?}"
    );
    assert!(
        rep.warnings
            .iter()
            .any(|x| x.starts_with("eml:wardrobe: ") && x.contains("kiln/plugins/kiln-sfi.dll")),
        "{rep:#?}"
    );
    assert!(
        mods.join("sfpack").is_dir(),
        "ShroudForge mods are always placed, where the interpreter reads them"
    );
    assert!(
        rep.own_loaders
            .iter()
            .all(|n| n.loader.kind != ModKind::Eml),
        "{:#?}",
        rep.own_loaders
    );
    assert_eq!(wire.events(), Vec::<String>::new());
    // installed later: the next activation places the whole package
    put(&w.game, "kiln/plugins/kiln-sfi.dll", b"interpreter");
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        mods.join("wardrobe").is_dir() && rep.placed.iter().any(|x| x == "eml:wardrobe"),
        "{rep:#?}"
    );
}

/// ShroudForge's own loader in the game wins in kiln mode (both Kiln backends stand aside): main's route, into the
/// folder ShroudForge's config names.
#[cfg(unix)]
#[test]
fn shroudforges_own_loader_keeps_main_route_in_kiln_mode() {
    let w = world("sf-own", Some(KILN), true, Some("SFMODS"));
    sf_own_loader(&w.game);
    let p = w.profile();
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        w.game.join("SFMODS/sfpack").is_dir() && !w.game.join("mods/sfpack").exists(),
        "{rep:#?}"
    );
    let loaders = own_loaders::find(&w.game, Some(&w.view(&p)));
    assert!(
        loaders
            .iter()
            .any(|l| l.file == "dbghelp.dll" && l.kind == ModKind::ShroudForge),
        "{loaders:#?}"
    );
    assert!(
        !loaders
            .iter()
            .any(|l| l.file == "kiln/plugins/kiln-sfi.dll"),
        "main's list, the interpreter not in it: {loaders:#?}"
    );
}

/// EML's own loader listed first in kiln mode: it decides the folder (`mods`, the interpreter's too) and the copies
/// carry ShroudForge's switch; a package that needs ShroudForge's runtime is refused as on main's route.
#[cfg(unix)]
#[test]
fn emls_own_loader_first_in_kiln_mode() {
    let w = world("eml-own", Some(KILN), true, Some("SFMODS"));
    put(
        &w.game,
        "dbghelp.dll",
        &kiln_manage::pe::build_test_dll(&[], b"EML_LOG_FILE_PATH"),
    );
    let mut p = w.profile();
    let refused = engine_mods::place(&w.dirs, &p, &w.game).unwrap_err();
    assert_eq!(refused.code, kiln_manage::ErrorCode::Refused, "{refused:?}");
    p.mods.eml.retain(|m| m.id != "wardrobe");
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    let loaders = own_loaders::find(&w.game, Some(&w.view(&p)));
    assert_eq!(
        loaders
            .iter()
            .filter(|l| l.kind == ModKind::Eml)
            .map(|l| l.file.as_str())
            .collect::<Vec<_>>(),
        ["dbghelp.dll", "kiln/plugins/kiln-sfi.dll"]
    );
    assert!(
        shroudforge::read_mod(&w.game.join("mods/gliders"))
            .unwrap()
            .enabled,
        "{rep:#?}"
    );
    let eml = rep
        .own_loaders
        .iter()
        .find(|n| n.loader.kind == ModKind::Eml)
        .unwrap();
    assert!(
        eml.text
            .contains("without one, Kiln's interpreter runs them"),
        "{}",
        eml.text
    );
}

/// Changing the selection between hosted and kiln with ShroudForge's config at its default (`<game>/mods` on both
/// routes, the common case): every copy is kept where it is, none is placed twice, none removed. (With a config that
/// names another folder the copies of one route stay in that route's folder: a route change.)
#[cfg(unix)]
#[test]
fn hosted_and_kiln_share_game_mods_when_shroudforges_config_is_at_its_default() {
    let w = world("transition", None, true, None);
    let p = w.profile();
    let first = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert_eq!(first.placed.len(), 5, "{first:#?}");
    let receipts = || std::fs::read(w.game.join("kiln/state/library-placements.json")).unwrap();
    let placed = receipts();
    for json in [Some(KILN), Some(HOSTED), Some(KILN)] {
        select(&w.game, json);
        let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
        assert!(
            rep.placed.is_empty()
                && rep.removed.is_empty()
                && rep.retained.is_empty()
                && rep.kept.len() == 5,
            "{json:?}: {rep:#?}"
        );
        assert_eq!(receipts(), placed, "{json:?}");
    }
}

// ---- every other route is the same in kiln mode ----

/// Twin games, one hosted and one in kiln mode, ShroudForge's config left at its default (`<game>/mods` for both
/// routes), with mods of every other kind: activation (Z: on Unix, real paths on Windows, and Copy mode), a second
/// activation and removal leave the same files, the same profile.json and the same reports, except the note on who runs
/// the EML mods.
#[test]
fn every_other_loaders_route_is_the_same_in_kiln_mode() {
    #[cfg(unix)]
    let modes = [PathMode::Z, PathMode::Copy];
    #[cfg(windows)]
    let modes = [PathMode::Native, PathMode::Copy];
    for mode in modes {
        let run = |tag: &str, kiln_json: Option<&str>| {
            let w = world(tag, kiln_json, true, None);
            let mut p = w.profile();
            for src in other_packages(&w.t.0) {
                let r =
                    library::import(&w.dirs, &[src], &mut kiln_manage::task::NoProgress).unwrap();
                for x in r.imported.iter().chain(&r.existing) {
                    match x.kind {
                        ModKind::Greeneye => p.mods.greeneye.push(pm(x, true)),
                        ModKind::Flame => p.mods.flame.push(pm(x, true)),
                        ModKind::Set => p.mods.set.push(pm(x, true)),
                        _ => {}
                    }
                }
            }
            p.mods.emf.push(pm(
                &import(
                    &w.dirs,
                    &emf_lua_package(&w.t.0, "beastmaster"),
                    ModKind::Emf,
                    "beastmaster",
                ),
                true,
            ));
            p.mods.native.push(pm(
                &import_kind(&w.dirs, &game_folder_package(&w.t.0), ModKind::Native),
                true,
            ));
            // shroudlink's singleplayer package: its addon from the Library starts the core left next to the game
            let (addon, assistants) = shroudlink_package(&w.dirs, &w.t.0, &w.game);
            p.mods.native.push(pm(&addon, true));
            p.mods.shroudlink.push(pm(&assistants, true));
            let p = profile::save(&w.dirs, &p).unwrap();
            let inst = install(&w.t.0, &w.game, mode);
            let mut out = Vec::new();
            let mut eml_notes = Vec::new();
            for step in ["first", "again"] {
                let a = profile::activate(&w.dirs, &p.id, &inst)
                    .unwrap_or_else(|e| panic!("{tag} {mode:?}: {e:?}"));
                let mut v = serde_json::to_value(&a).unwrap();
                let notes = v.as_object_mut().unwrap().remove("ownLoaders");
                eml_notes.push(notes);
                out.push(format!("{step}: {}", stable_json(&v, &w.t.0)));
                // (kiln mode records that the interpreter's route placed in <game>/mods, for a later route change;
                // its activations hold the activation journal's lock)
                out.extend(dump_tree(&w.game, &w.t.0).into_iter().filter(|l| {
                    !l.starts_with("kiln/kiln.json")
                        && !l.starts_with("kiln/state/sfi-route.json")
                        && !l.starts_with("kiln/state/sfi-activation.lock")
                }));
            }
            let cleanup = engine_mods::unplace(&w.dirs, &w.game).unwrap();
            out.push(format!(
                "unplace: {}",
                stable_json(&serde_json::to_value(&cleanup).unwrap(), &w.t.0)
            ));
            out.extend(dump_tree(&w.game, &w.t.0).into_iter().filter(|l| {
                !l.starts_with("kiln/kiln.json") && !l.starts_with("kiln/state/sfi-activation.lock")
            }));
            (out, eml_notes, w)
        };
        let (hosted, hosted_notes, _h) = run("twin-hosted", None);
        let (kiln, kiln_notes, _k) = run("twin-kiln", Some(KILN));
        assert_eq!(hosted.len(), kiln.len(), "{mode:?}");
        for (a, b) in hosted.iter().zip(&kiln) {
            assert_eq!(a, b, "{mode:?}: hosted and kiln mode differ");
        }
        // the one difference: who runs the EML mods (Kiln's hosted ShroudForge, Kiln's interpreter), same folder, same copies;
        // every other loader's note (shroudlink's core in the game folder among them) is the same
        for (h, k) in hosted_notes.iter().zip(&kiln_notes) {
            let others = |n: &Option<serde_json::Value>| {
                n.as_ref()
                    .and_then(|v| v.as_array().cloned())
                    .unwrap_or_default()
                    .into_iter()
                    .filter(|x| x["kind"] != "eml")
                    .map(|x| stable_json(&x, Path::new("/none")))
                    .collect::<Vec<_>>()
            };
            assert_eq!(others(h), others(k), "{mode:?}");
            assert!(
                others(k).iter().any(|n| n.contains("shroudlink_core.dll")),
                "{mode:?}: {k:?}"
            );
            let eml = |n: &Option<serde_json::Value>| {
                n.as_ref()
                    .and_then(|v| v.as_array().cloned())
                    .unwrap_or_default()
                    .into_iter()
                    .find(|x| x["kind"] == "eml")
                    .unwrap_or_else(|| panic!("{n:?}"))
            };
            let (h, k) = (eml(h), eml(k));
            assert_eq!(
                (h["via"].as_str(), k["via"].as_str()),
                (Some("hosted-shroud-forge"), Some("interpreter"))
            );
            assert_eq!(
                (
                    h["folder"].clone(),
                    h["copied"].clone(),
                    h["notCopied"].clone()
                ),
                (
                    k["folder"].clone(),
                    k["copied"].clone(),
                    k["notCopied"].clone()
                )
            );
        }
    }
}

/// The twins' comparison takes each twin's temporary folder out however a file spells it: as it is, as the game's Z:
/// path, and as JSON writes it (a Windows path doubles its backslashes there).
#[test]
fn a_twins_folder_is_taken_out_however_a_file_spells_it() {
    let t = Path::new(r"C:\kiln-runner\sfi\tmp\km-sfi-routing-twin-kiln-1-2");
    let v = serde_json::json!({ "path": format!(r"{}\game\kiln", t.display()) });
    assert_eq!(stable_json(&v, t), "{\n  \"path\": \"<T>\\\\game\\\\kiln\"\n}");
    assert_eq!(normal(&format!(r"{}\game", t.display()), t), r"<T>\game");
    let unix = Path::new("/tmp/km-sfi-routing-twin-kiln-1-2");
    assert_eq!(normal("Z:\\tmp\\km-sfi-routing-twin-kiln-1-2\\game /tmp/km-sfi-routing-twin-kiln-1-2", unix), "<T>\\game <T>");
}

// ---- main's routes, byte for byte: written by main's code before the route existed ----

/// The steps of the golden: two activations, the player's folders, settings, removal, each with what the game folder
/// holds after it.
fn legacy_run(tag: &str, kiln_json: Option<&str>, own_loader: bool, mode: PathMode) -> Vec<String> {
    let w = world(tag, kiln_json, true, Some("SFMODS"));
    if own_loader {
        sf_own_loader(&w.game);
    }
    let mut p = w.profile();
    p.mods.emf.push(pm(
        &import(
            &w.dirs,
            &emf_lua_package(&w.t.0, "beastmaster"),
            ModKind::Emf,
            "beastmaster",
        ),
        true,
    ));
    let p = profile::save(&w.dirs, &p).unwrap();
    let inst = install(&w.t.0, &w.game, mode);
    let mut out = vec![format!("== {tag} {mode:?}")];
    for step in ["first", "again"] {
        match profile::activate(&w.dirs, &p.id, &inst) {
            Ok(a) => out.push(format!(
                "{step}: {}",
                stable_json(&serde_json::to_value(&a).unwrap(), &w.t.0)
            )),
            Err(e) => out.push(format!(
                "{step}: error {:?} {}",
                e.code,
                normal(&e.message, &w.t.0)
            )),
        }
        out.extend(dump_tree(&w.game, &w.t.0));
    }
    let own = engine_mods::own_folders(&w.dirs, &p, &w.game).unwrap();
    out.push(format!(
        "own: {}",
        stable_json(&serde_json::to_value(&own).unwrap(), &w.t.0)
    ));
    let cfg = w.t.0.join("cfg");
    match engine_mods::settings(
        ModKind::ShroudForge,
        "sfpack",
        Some(&w.game),
        &cfg,
        Some(&w.lib(&w.sf)),
    ) {
        Ok(s) => out.push(format!(
            "settings: {}",
            stable_json(&serde_json::to_value(&s).unwrap(), &w.t.0)
        )),
        Err(e) => out.push(format!(
            "settings: error {:?} {}",
            e.code,
            normal(&e.message, &w.t.0)
        )),
    }
    out.push(format!(
        "placed: {:?}",
        engine_mods::placed_folder(ModKind::ShroudForge, &w.game, "sfpack")
            .map(|p| normal(&p.to_string_lossy(), &w.t.0))
    ));
    let loaders = own_loaders::find(&w.game, Some(&w.view(&p)));
    out.push(format!(
        "loaders: {}",
        stable_json(&serde_json::to_value(&loaders).unwrap(), &w.t.0)
    ));
    let cleanup = engine_mods::unplace(&w.dirs, &w.game).unwrap();
    out.push(format!(
        "unplace: {}",
        stable_json(&serde_json::to_value(&cleanup).unwrap(), &w.t.0)
    ));
    out.extend(dump_tree(&w.game, &w.t.0));
    out
}

/// Hosted, installed, the older useInstalled, an invalid backend, a held `kiln`, and `kiln` with ShroudForge's own
/// loader in the game: main's route, the same bytes as main's code gave (tests/fixtures/sfi-routing/legacy.golden,
/// written before the interpreter's route was added, with KILN_SFI_ROUTING_BLESS=1). The held `kiln` is a case only
/// while this build's gate list holds the interpreter; without its row `kiln` is a normal choice and its golden blocks
/// are left out of the comparison.
#[cfg(unix)]
#[test]
fn main_routes_are_byte_for_byte_mains() {
    let golden =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/sfi-routing/legacy.golden");
    let mut out = Vec::new();
    let held = interpreter_is_a_preview();
    let cases: [(&str, Option<&str>, bool); 8] = [
        ("none", None, false),
        ("hosted", Some(HOSTED), false),
        ("installed", Some(INSTALLED), false),
        ("use-installed", Some(USE_INSTALLED), false),
        ("invalid", Some(INVALID), false),
        ("held", Some(KILN_HELD), false),
        ("sf-own-loader", Some(KILN), true),
        ("hosted-sf-own-loader", Some(HOSTED), true),
    ];
    for (tag, json, own) in cases {
        if tag == "held" && !held {
            continue;
        }
        for mode in [PathMode::Z, PathMode::Copy] {
            out.extend(legacy_run(&format!("golden-{tag}"), json, own, mode));
        }
    }
    let text = out.join("\n") + "\n";
    if std::env::var("KILN_SFI_ROUTING_BLESS").as_deref() == Ok("1") {
        assert!(held, "bless the golden on a build whose gate list holds the interpreter: it has the held case");
        std::fs::create_dir_all(golden.parent().unwrap()).unwrap();
        std::fs::write(&golden, &text).unwrap();
        return;
    }
    let want = std::fs::read_to_string(&golden).expect("the golden main's code wrote");
    let mut in_held = false;
    let want: Vec<&str> = want
        .lines()
        .filter(|l| {
            if l.starts_with("== ") {
                in_held = !held && l.starts_with("== golden-held ");
            }
            !in_held
        })
        .collect();
    let got: Vec<&str> = text.lines().collect();
    for (i, (g, w)) in got.iter().zip(&want).enumerate() {
        assert_eq!(g, w, "line {} of the golden", i + 1);
    }
    assert_eq!(got.len(), want.len());
}

// ---- the game-folder scan ----

/// The game-folder scan (found::scan) in kiln mode: ShroudForge's mods folder is `<game>/mods` there too, so a player's
/// ShroudForge package in it is the interpreter's to run, and an EML package there runs while its ShroudForge switch is on,
/// by Kiln (its interpreter carries the EML mods as Kiln's hosted ShroudForge does), not by a loader of its own. The
/// folder ShroudForge's config names is not listed, and nothing below `<game>/shroudforge` is read. The control: on
/// main's route the same game lists that folder.
#[cfg(unix)]
#[test]
fn found_mods_in_kiln_mode_are_in_game_mods_and_run_by_kiln() {
    use kiln_manage::found::{self, FoundState, FoundVia};
    let w = world("found", Some(KILN), true, Some("SFMODS"));
    // a ShroudForge mod switched on: the interpreter carries the EML mods
    let p = w.profile();
    put(
        &w.game,
        "dinput8.dll",
        &kiln_manage::pe::build_test_dll(&["KilnProxyInfo", "DirectInput8Create"], b""),
    );
    // the player's own folders: a ShroudForge package and an EML package switched on in ShroudForge's file in
    // <game>/mods, another ShroudForge package in the folder ShroudForge's config names
    put(
        &w.game,
        "mods/hand-sf/mod.json",
        br#"{"id":"hand-sf","name":"Hand SF","version":"1.0.0"}"#,
    );
    put(
        &w.game,
        "mods/hand-sf/extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"settings":{}}"#,
    );
    put(&w.game, "mods/hand-sf/src/mod.lua", b"return {}");
    put(
        &w.game,
        "mods/HandEml/mod.json",
        br#"{"id":"HandEml","name":"Hand EML","version":"1.0.1"}"#,
    );
    put(&w.game, "mods/HandEml/src/mod.lua", b"return {}");
    put(
        &w.game,
        "mods/HandEml/extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"],"launcher":"EML","settings":{}}"#,
    );
    put(
        &w.game,
        "SFMODS/sf-there/mod.json",
        br#"{"id":"sf-there","name":"SF there","version":"1.0.0"}"#,
    );
    put(
        &w.game,
        "SFMODS/sf-there/extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"settings":{}}"#,
    );
    put(&w.game, "SFMODS/sf-there/src/mod.lua", b"return {}");
    let inst = install(&w.t.0, &w.game, PathMode::Z);
    let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
    let r = found::scan(&w.dirs, &inst, &p);
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "the scan read below <game>/shroudforge"
    );
    let one = |r: &found::FoundReport, path: &str| {
        r.mods
            .iter()
            .find(|m| m.path == path)
            .cloned()
            .unwrap_or_else(|| panic!("{path}: {r:#?}"))
    };
    let sf = one(&r, "mods/hand-sf");
    assert_eq!(
        (sf.kind, sf.state, sf.via),
        (
            Some(ModKind::ShroudForge),
            FoundState::Loads,
            Some(FoundVia::Kiln)
        ),
        "{sf:#?}"
    );
    let eml = one(&r, "mods/HandEml");
    assert_eq!(
        (eml.kind, eml.state, eml.via),
        (Some(ModKind::Eml), FoundState::Loads, Some(FoundVia::Kiln)),
        "{eml:#?}"
    );
    assert!(
        eml.reason.contains("its ShroudForge switch is on"),
        "{}",
        eml.reason
    );
    assert!(
        !r.mods.iter().any(|m| m.path.starts_with("SFMODS")),
        "{r:#?}"
    );
    // the control: main's route lists the folder ShroudForge's config names
    select(&w.game, Some(HOSTED));
    let r = found::scan(&w.dirs, &inst, &p);
    assert_eq!(
        one(&r, "SFMODS/sf-there").state,
        FoundState::Loads,
        "{r:#?}"
    );
}

// ---- a route change ----

/// A route change with ShroudForge's config naming another folder: Kiln's copies of the route left behind follow the
/// route (engine_mods' sweep, by their receipts). Going from kiln to hosted, a copy of a ShroudForge package that is an
/// XHL-Core plugin too would otherwise keep its native-plugin.ini in `<game>/mods`, so XHL-Core could load its DLL with
/// nothing running the rest of the package (the inbox 86 shape). With ShroudForge's config at its default both routes
/// share `<game>/mods` and nothing moves (hosted_and_kiln_share_game_mods_when_shroudforges_config_is_at_its_default).
#[cfg(unix)]
#[test]
fn a_route_change_leaves_no_copy_of_kilns_in_the_other_routes_folder() {
    let w = world("sweep", Some(HOSTED), true, Some("SFMODS"));
    let mut p = w.profile();
    // Building Tools on: its XHL-Core ini is active in its copy
    p.mods.shroudforge[2].enabled = true;
    let p = profile::save(&w.dirs, &p).unwrap();
    engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        w.game
            .join("SFMODS/building-tools/native-plugin.ini")
            .is_file()
    );
    // hosted -> kiln: every copy in <game>/mods, none left in the hosted route's folder
    select(&w.game, Some(KILN));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    for id in ["sfpack", "sfoff", "building-tools", "gliders", "wardrobe"] {
        assert!(w.game.join("mods").join(id).is_dir(), "{id}: {rep:#?}");
        assert!(
            !w.game.join("SFMODS").join(id).exists(),
            "{id}: left in the hosted route's folder: {rep:#?}"
        );
    }
    // kiln -> hosted: none left in <game>/mods, where XHL-Core reads native-plugin.ini
    select(&w.game, Some(HOSTED));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        !w.game.join("mods/building-tools").exists(),
        "XHL-Core may load its DLL alone: {rep:#?}"
    );
}

/// A route change moves Kiln's copies with the player's settings in them, settles the old route's publication records
/// once (no "publication recovery left this folder untouched" at every activation), and leaves what is not Kiln's: a
/// player's own folder in ShroudForge's folder never moves, and a copy of Kiln's whose files the player changed stays
/// where it is (said). Back on main's route the copies go back, and the record of the interpreter's route goes.
#[cfg(unix)]
#[test]
fn a_route_change_moves_kilns_copies_with_their_settings_and_leaves_what_is_not_kilns() {
    let w = world("moves", Some(HOSTED), true, Some("SFMODS"));
    let p = w.profile();
    engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    shroudforge::set_setting(
        &w.game.join("SFMODS/sfpack"),
        "speed",
        &serde_json::json!(7),
    )
    .unwrap();
    put(
        &w.game,
        "SFMODS/players-own/mod.json",
        br#"{"id":"players-own","name":"Mine","version":"1.0.0"}"#,
    );
    put(
        &w.game,
        "SFMODS/sfoff/src/mod.lua",
        b"return { edited = true }",
    );
    let players = snapshot(&w.game.join("SFMODS/players-own"));
    let edited = snapshot(&w.game.join("SFMODS/sfoff"));
    let untouched = |rep: &engine_mods::PlaceReport| {
        rep.warnings
            .iter()
            .any(|n| n.contains("publication recovery left this folder untouched"))
    };
    let speed = |dir: &str| {
        shroudforge::read_mod(&w.game.join(dir))
            .unwrap()
            .settings
            .into_iter()
            .find(|s| s.key == "speed")
            .unwrap()
            .value
    };
    // hosted -> kiln
    select(&w.game, Some(KILN));
    let mut wire = Tripwire::arm(&w.game.join(SHROUDFORGE_DIR));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "the sweep read below <game>/shroudforge"
    );
    assert!(!untouched(&rep), "{rep:#?}");
    assert_eq!(
        speed("mods/sfpack"),
        serde_json::json!(7),
        "the setting moved with the copy: {rep:#?}"
    );
    assert!(
        !w.game.join("SFMODS/sfpack").exists() && !w.game.join("SFMODS/gliders").exists(),
        "{rep:#?}"
    );
    assert!(
        rep.kept_files
            .iter()
            .any(|n| n.starts_with("shroudforge:sfpack: Kiln moved its copy")),
        "{rep:#?}"
    );
    assert_eq!(
        snapshot(&w.game.join("SFMODS/players-own")),
        players,
        "the player's folder"
    );
    assert!(!w.game.join("mods/players-own").exists());
    assert_eq!(
        snapshot(&w.game.join("SFMODS/sfoff")),
        edited,
        "Kiln's copy the player changed stays as it is"
    );
    assert!(
        rep.retained
            .iter()
            .any(|r| r.path == w.game.join("SFMODS/sfoff") && r.reason.contains("used before")),
        "{rep:#?}"
    );
    assert!(w.game.join("kiln/state/sfi-route.json").is_file());
    // the next activation says nothing more about the old route's folder
    let again = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        !untouched(&again) && !again.kept_files.iter().any(|n| n.contains("Kiln moved")),
        "{again:#?}"
    );
    assert_eq!(wire.events(), Vec::<String>::new());
    // kiln -> hosted: the copies go back with the setting; Kiln's fresh copy of sfoff finds its place taken by the
    // changed one and goes (nothing of the player's in it)
    select(&w.game, Some(HOSTED));
    let back = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(!untouched(&back), "{back:#?}");
    assert_eq!(speed("SFMODS/sfpack"), serde_json::json!(7), "{back:#?}");
    for id in ["sfpack", "sfoff", "building-tools"] {
        assert!(!w.game.join("mods").join(id).exists(), "{id}: {back:#?}");
    }
    assert_eq!(snapshot(&w.game.join("SFMODS/sfoff")), edited);
    assert_eq!(snapshot(&w.game.join("SFMODS/players-own")), players);
    assert!(
        !w.game.join("kiln/state/sfi-route.json").exists(),
        "main's route has no copy of Kiln's left in <game>/mods"
    );
    let again = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        !untouched(&again)
            && !again
                .kept_files
                .iter()
                .any(|n| n.contains("Kiln moved") || n.contains("Kiln removed")),
        "{again:#?}"
    );
}

/// Main's own route never sweeps: a copy of Kiln's in `<game>/mods` while ShroudForge's config names another folder,
/// with no interpreter's route before (the player changed ShroudForge's config by hand), stays as main leaves it.
#[cfg(unix)]
#[test]
fn main_route_alone_never_sweeps() {
    let w = world("mainonly", Some(HOSTED), true, None);
    let p = w.profile();
    engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(w.game.join("mods/sfpack").is_dir());
    sf_install(&w.game, Some("SFMODS"));
    let rep = engine_mods::place(&w.dirs, &p, &w.game).unwrap();
    assert!(
        w.game.join("mods/sfpack").is_dir() && w.game.join("SFMODS/sfpack").is_dir(),
        "{rep:#?}"
    );
    assert!(
        !rep.kept_files
            .iter()
            .any(|n| n.contains("Kiln moved") || n.contains("Kiln removed")),
        "{rep:#?}"
    );
    assert!(!w.game.join("kiln/state/sfi-route.json").exists());
    // and its removal leaves that copy as main's does (not ShroudForge's folder: retained)
    let cleanup = engine_mods::unplace(&w.dirs, &w.game).unwrap();
    assert!(w.game.join("mods/sfpack").is_dir(), "{cleanup:#?}");
    assert!(
        cleanup
            .retained
            .iter()
            .any(|r| r.path == w.game.join("mods/sfpack")),
        "{cleanup:#?}"
    );
}
