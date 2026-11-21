// SPDX-License-Identifier: MIT
//! The route of a game's ShroudForge-format mods as kiln-manage reads it: kiln.json read as the core
//! reads it, ShroudForge's own loader, Kiln's interpreter installed or not; and the two runner states the environment
//! decides, through explicit routes.

mod sfi_routing_common;

use kiln_manage::manifest::ModKind;
use kiln_manage::own_loaders::{self, ProfileView};
use kiln_manage::sfi_backend::Backend;
use kiln_manage::sfi_routing::{self, Legacy, Route, Runner};
use sfi_routing_common::*;
use std::path::Path;

/// (case, kiln.json, interpreter installed, extra files, route)
type Row = (
    &'static str,
    Option<String>,
    bool,
    Vec<(&'static str, Vec<u8>)>,
    Route,
);

fn route(game: &Path) -> Route {
    sfi_routing::route_of(game)
}

#[test]
fn route_of_reads_the_selection_as_the_core_reads_it() {
    let t = tmp("table");
    let sf_dll = kiln_manage::pe::build_test_dll(&[], b"EML_LOG_FILE_PATH shroudforge");
    let eml_dll = kiln_manage::pe::build_test_dll(&[], b"EML_LOG_FILE_PATH");
    let ready = Route::Interpreter(Runner::Ready);
    // kiln with no "preview": held only while this build's gate list holds the interpreter, else a normal choice
    let held = interpreter_is_a_preview();
    let held_or_ready = if held { Route::Legacy(Legacy::Refused) } else { ready.clone() };
    // (case, kiln.json, interpreter installed, extra files, route)
    let rows: Vec<Row> = vec![
        (
            "no kiln.json",
            None,
            true,
            vec![],
            Route::Legacy(Legacy::Selected(Backend::Hosted)),
        ),
        (
            "hosted",
            Some(HOSTED.into()),
            true,
            vec![],
            Route::Legacy(Legacy::Selected(Backend::Hosted)),
        ),
        (
            "installed",
            Some(INSTALLED.into()),
            true,
            vec![],
            Route::Legacy(Legacy::Selected(Backend::Installed)),
        ),
        (
            "useInstalled",
            Some(USE_INSTALLED.into()),
            true,
            vec![],
            Route::Legacy(Legacy::Selected(Backend::Installed)),
        ),
        (
            "invalid backend",
            Some(INVALID.into()),
            true,
            vec![],
            Route::Legacy(Legacy::Refused),
        ),
        (
            "kiln, held",
            Some(KILN_HELD.into()),
            true,
            vec![],
            held_or_ready.clone(),
        ),
        ("kiln", Some(KILN.into()), true, vec![], ready.clone()),
        (
            "kiln, no interpreter",
            Some(KILN.into()),
            false,
            vec![],
            Route::Interpreter(Runner::Missing),
        ),
        (
            "kiln, a BOM",
            Some(format!("\u{feff}{KILN}")),
            true,
            vec![],
            ready.clone(),
        ),
        (
            "kiln, comments",
            Some(format!("// mine\n{KILN}")),
            true,
            vec![],
            ready.clone(),
        ),
        // config.cpp reads no "preview" from a file with a trailing comma: held at boot while the list holds it
        (
            "kiln, a trailing comma",
            Some(
                r#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"},}"#
                    .into(),
            ),
            true,
            vec![],
            held_or_ready.clone(),
        ),
        (
            "kiln, ShroudForge's own loader",
            Some(KILN.into()),
            true,
            vec![("dbghelp.dll", sf_dll.clone())],
            Route::Legacy(Legacy::OwnLoader("dbghelp.dll".into())),
        ),
        (
            "kiln, ShroudForge's bootstrap under another case",
            Some(KILN.into()),
            true,
            vec![("WinMM.dll", sf_dll.clone())],
            Route::Legacy(Legacy::OwnLoader("WinMM.dll".into())),
        ),
        (
            "kiln, ShroudForge chained by Kiln's proxy",
            Some(KILN.into()),
            true,
            vec![
                ("kiln/chain/sf-winmm.dll", sf_dll.clone()),
                ("kiln/chain.ini", b"[chain]\nchain\\sf-winmm.dll\n".to_vec()),
            ],
            Route::Legacy(Legacy::OwnLoader("kiln/chain/sf-winmm.dll".into())),
        ),
        (
            "kiln, ShroudForge chained by EMF's list",
            Some(KILN.into()),
            true,
            vec![
                ("sf/sf-loader.dll", sf_dll.clone()),
                (
                    "dinput8-chain.ini",
                    b"[Chain]\nsf\\sf-loader.dll\n".to_vec(),
                ),
            ],
            Route::Legacy(Legacy::OwnLoader("sf/sf-loader.dll".into())),
        ),
        (
            "kiln, a switched-off chain line",
            Some(KILN.into()),
            true,
            vec![
                ("kiln/chain/sf-winmm.dll", sf_dll.clone()),
                (
                    "kiln/chain.ini",
                    b"[chain]\n;chain\\sf-winmm.dll\n".to_vec(),
                ),
            ],
            ready.clone(),
        ),
        (
            "kiln, EML's own loader",
            Some(KILN.into()),
            true,
            vec![("dbghelp.dll", eml_dll.clone())],
            ready.clone(),
        ),
        (
            "hosted, ShroudForge's own loader",
            Some(HOSTED.into()),
            true,
            vec![("dbghelp.dll", sf_dll.clone())],
            Route::Legacy(Legacy::Selected(Backend::Hosted)),
        ),
    ];
    for (i, (case, json, interpreter, files, want)) in rows.into_iter().enumerate() {
        let g = game(&t.0.join(format!("g{i}")), interpreter);
        select(&g, json.as_deref());
        for (rel, bytes) in &files {
            put(&g, rel, bytes);
        }
        assert_eq!(route(&g), want, "{case}");
        // what the route says for the ShroudForge mods folder: <game>/mods, or the folder ShroudForge's config names
        sf_install(&g, Some("SFMODS"));
        let sf = route(&g).sf_mods(&g);
        assert_eq!(
            sf,
            g.join(if want.interpreter() { "mods" } else { "SFMODS" }),
            "{case}"
        );
    }
}

/// A dedicated server reads the same setting; its side's hold is lifted by the same top-level "preview".
#[test]
fn a_server_folder_has_its_own_route() {
    let t = tmp("server");
    let g = t.0.join("server");
    put(&g, "enshrouded_server.exe", b"MZ");
    select(&g, Some(KILN));
    assert_eq!(route(&g), Route::Interpreter(Runner::Missing));
    put(&g, "kiln/plugins/kiln-sfi.dll", b"interpreter");
    assert_eq!(route(&g), Route::Interpreter(Runner::Ready));
    assert_eq!(
        kiln_manage::engine_mods::server_root(ModKind::ShroudForge, &g),
        Some(g.join("mods"))
    );
    select(&g, Some(HOSTED));
    sf_install(&g, Some("SFMODS"));
    assert_eq!(
        kiln_manage::engine_mods::server_root(ModKind::ShroudForge, &g),
        Some(g.join("SFMODS"))
    );
}

fn view(mods: &[(ModKind, &str, bool)], eml_runtime: &[&str], dropins: bool) -> ProfileView {
    ProfileView {
        mods: mods
            .iter()
            .map(|(k, id, on)| (*k, id.to_string(), *on))
            .collect(),
        dropins_shroudforge: dropins,
        eml_runtime: eml_runtime.iter().map(|s| s.to_string()).collect(),
        ..ProfileView::default()
    }
}

/// Kiln's interpreter carries the EML mods on the rule Kiln's hosted ShroudForge has: a ShroudForge mod on, an EML
/// package that needs ShroudForge's runtime, or ShroudForge drop-ins with a package switched on in its folder (here
/// `<game>/mods`); only while it runs; never on main's route.
#[test]
fn the_interpreter_carries_the_eml_mods_when_the_profile_makes_it_run_them() {
    let t = tmp("carry");
    let g = game(&t.0.join("game"), true);
    let carries = |v: &ProfileView, r: &Route| {
        own_loaders::find_routed(&g, Some(v), r)
            .iter()
            .any(|l| l.file == "kiln/plugins/kiln-sfi.dll")
    };
    let ready = Route::Interpreter(Runner::Ready);
    assert!(carries(
        &view(&[(ModKind::ShroudForge, "a", true)], &[], false),
        &ready
    ));
    assert!(!carries(
        &view(&[(ModKind::ShroudForge, "a", false)], &[], false),
        &ready
    ));
    assert!(carries(&view(&[], &["wardrobe"], false), &ready));
    assert!(
        !carries(
            &view(&[(ModKind::Eml, "gliders", true)], &[], false),
            &ready
        ),
        "EML mods alone: kiln-eml runs them"
    );
    for r in [
        Route::Interpreter(Runner::EnvOff),
        Route::Interpreter(Runner::Missing),
    ] {
        let v = view(&[(ModKind::ShroudForge, "a", true)], &["wardrobe"], true);
        assert!(
            own_loaders::find_routed(&g, Some(&v), &r).is_empty(),
            "{r:?}"
        );
    }
    assert!(own_loaders::find_routed(&g, None, &ready).is_empty());
    // drop-ins: a package switched on in <game>/mods (the adapter's rule: an unreadable switch counts, an EML one not)
    let drop = view(&[], &[], true);
    assert!(!carries(&drop, &ready));
    for (ext, on) in [
        (r#"{"enabled":true}"#, true),
        (r#"{"enabled":false}"#, false),
        (r#"{}"#, true),
        (r#"{"launcher":"EML"}"#, false),
        (r#"{"enabled":"yes"}"#, true),
        ("not json", true),
        (r#"[1]"#, true),
    ] {
        put(&g, "mods/drop/extended.mod.json", ext.as_bytes());
        assert_eq!(carries(&drop, &ready), on, "{ext}");
        assert!(
            !carries(&view(&[], &[], false), &ready),
            "{ext}: drop-ins off"
        );
    }
    // ShroudForge's own folder is not the interpreter's
    std::fs::remove_dir_all(g.join("mods/drop")).unwrap();
    sf_install(&g, Some("SFMODS"));
    put(&g, "SFMODS/drop/extended.mod.json", br#"{"enabled":true}"#);
    assert!(!carries(&drop, &ready));
}

/// `KILN_SHROUDFORGE=0` under Kiln's interpreter: the folders do not move, nothing runs a package that needs
/// ShroudForge's runtime (not placed, own_loaders' note), and no loader carries the EML mods.
#[cfg(unix)]
#[test]
fn env_off_places_no_half_of_a_runtime_package() {
    use kiln_manage::{engine_mods, library, paths::DataDirs, profile, shroudforge};
    let t = tmp("env-off");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0.join("game"), true);
    sf_install(&g, Some("SFMODS"));
    select(&g, Some(KILN));
    let sf = import(
        &dirs,
        &sf_package(&t.0, "sfpack", false),
        ModKind::ShroudForge,
        "sfpack",
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
    let mut p = profile::create(&dirs, "env", None).unwrap();
    p.mods.shroudforge = vec![pm(&sf, true)];
    p.mods.eml = vec![pm(&gliders, true), pm(&wardrobe, true)];
    let p = profile::save(&dirs, &p).unwrap();
    let mut wire = Tripwire::arm(&g.join("shroudforge"));
    let rep =
        engine_mods::place_routed(&dirs, &p, &g, &Route::Interpreter(Runner::EnvOff)).unwrap();
    assert!(
        g.join("mods/sfpack").is_dir() && !g.join("SFMODS").exists(),
        "{rep:#?}"
    );
    assert!(
        !g.join("mods/wardrobe").exists() && !g.join("mods/gliders").exists(),
        "{rep:#?}"
    );
    // own_loaders' note (own_loaders::RUNTIME_ENV_OFF_NOTE)
    let note = format!(
        "eml:wardrobe: {}",
        Route::Interpreter(Runner::EnvOff).runtime_note().unwrap()
    );
    assert!(note.contains("KILN_SHROUDFORGE=0"), "{note}");
    assert!(rep.warnings.contains(&note), "{rep:#?}");
    assert!(rep.own_loaders.is_empty(), "{:#?}", rep.own_loaders);
    assert!(
        shroudforge::read_mod(&g.join("mods/sfpack"))
            .unwrap()
            .enabled
    );
    assert_eq!(wire.events(), Vec::<String>::new());
    // the same game on the interpreter's route that runs: the whole package goes in
    let rep = engine_mods::place_routed(&dirs, &p, &g, &Route::Interpreter(Runner::Ready)).unwrap();
    assert!(
        g.join("mods/wardrobe").is_dir() && g.join("mods/gliders").is_dir(),
        "{rep:#?}"
    );
    let _ = library::path_of(&dirs, &sf).unwrap();
}

/// A publication a stop left in `<game>/mods` (its old copy parked beside it, the receipt already the new one's) is
/// finished under Kiln's interpreter: the journal is checked against the interpreter's folder, never ShroudForge's
/// config. On main's route right after the interpreter's (a route change) it is finished too, and the copy
/// then follows the route to ShroudForge's folder. On main's route alone (no interpreter's route before: the record
/// is gone) the same journal names a folder that is not ShroudForge's (its config says SFMODS), so it is left untouched
/// and said, as main does.
#[cfg(unix)]
#[test]
fn a_stopped_publication_in_game_mods_is_finished_on_the_interpreters_route_only() {
    use kiln_manage::{engine_mods, paths::DataDirs, profile};
    for (route, changed) in [
        (Route::Interpreter(Runner::Ready), false),
        (Route::Legacy(Legacy::Selected(Backend::Hosted)), true),
        (Route::Legacy(Legacy::Selected(Backend::Hosted)), false),
    ] {
        let t = tmp("recover");
        let dirs = DataDirs::at(t.0.join("data"));
        let g = game(&t.0.join("game"), true);
        sf_install(&g, Some("SFMODS"));
        select(&g, Some(KILN));
        let sf = import(
            &dirs,
            &sf_package(&t.0, "sfpack", false),
            ModKind::ShroudForge,
            "sfpack",
        );
        let mut p = profile::create(&dirs, "recover", None).unwrap();
        p.mods.shroudforge = vec![pm(&sf, true)];
        let p = profile::save(&dirs, &p).unwrap();
        engine_mods::place_routed(&dirs, &p, &g, &Route::Interpreter(Runner::Ready)).unwrap();
        // the stop: the receipt is the new copy's, the old copy is still parked beside it
        let receipts: serde_json::Value = serde_json::from_slice(
            &std::fs::read(g.join("kiln/state/library-placements.json")).unwrap(),
        )
        .unwrap();
        let receipt = receipts
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["path"] == g.join("mods/sfpack").to_string_lossy().as_ref())
            .unwrap()
            .clone();
        let parked = g.join("mods/.kiln-place-0123456789ab/old");
        std::fs::create_dir_all(&parked).unwrap();
        std::fs::copy(
            g.join("mods/sfpack/.kiln-library.json"),
            parked.join(".kiln-library.json"),
        )
        .unwrap();
        let journal = serde_json::json!([{"path": receipt["path"], "from": receipt["marker"], "to": receipt, "parked": parked}]);
        put(
            &g,
            "kiln/state/placement-journal.json",
            serde_json::to_string_pretty(&journal).unwrap().as_bytes(),
        );
        if !route.interpreter() && !changed {
            std::fs::remove_file(g.join("kiln/state/sfi-route.json")).unwrap();
        }
        let mut wire = Tripwire::arm(&g.join("shroudforge"));
        let rep = engine_mods::place_routed(&dirs, &p, &g, &route).unwrap();
        let untouched = rep
            .warnings
            .iter()
            .any(|w| w.contains("publication recovery left this folder untouched"));
        if route.interpreter() {
            assert!(!parked.parent().unwrap().exists() && !untouched, "{rep:#?}");
            assert_eq!(wire.events(), Vec::<String>::new());
        } else if changed {
            assert!(
                !parked.parent().unwrap().exists() && !untouched,
                "the route change settles it: {rep:#?}"
            );
            assert!(
                g.join("SFMODS/sfpack").is_dir() && !g.join("mods/sfpack").exists(),
                "{rep:#?}"
            );
        } else {
            assert!(
                parked.is_dir() && untouched,
                "main's route: not ShroudForge's folder: {rep:#?}"
            );
        }
    }
}

/// On Windows the receipts of a hosted start keep the older launcher's spelling of ShroudForge's mods folder (its
/// config's `\\?\\` path); under Kiln's interpreter that config is never asked, and the receipts name the copies as they
/// are.
#[cfg(windows)]
#[test]
fn windows_receipts_under_the_interpreter_never_ask_shroudforges_config() {
    use kiln_manage::{engine_mods, paths::DataDirs, profile};
    for (tag, json) in [("hosted", HOSTED), ("kiln", KILN)] {
        let t = tmp(&format!("win-receipts-{tag}"));
        let dirs = DataDirs::at(t.0.join("data"));
        let g = game(&t.0.join("game"), true);
        let verbatim = format!(r"\\?\{}", g.join("mods").display());
        sf_install(&g, Some(&verbatim));
        select(&g, Some(json));
        let sf = import(
            &dirs,
            &sf_package(&t.0, "sfpack", false),
            ModKind::ShroudForge,
            "sfpack",
        );
        let mut p = profile::create(&dirs, "win", None).unwrap();
        p.mods.shroudforge = vec![pm(&sf, true)];
        let p = profile::save(&dirs, &p).unwrap();
        engine_mods::place(&dirs, &p, &g).unwrap();
        let receipts =
            std::fs::read_to_string(g.join("kiln/state/library-placements.json")).unwrap();
        let doc: serde_json::Value = serde_json::from_str(&receipts).unwrap();
        let path = doc[0]["path"].as_str().unwrap().to_string();
        let plain = g.join("mods").join("sfpack").display().to_string();
        match tag {
            "hosted" => assert_eq!(path, format!(r"\\?\{plain}"), "{receipts}"),
            _ => assert_eq!(path, plain, "{receipts}"),
        }
        // either spelling names Kiln's copy: the next activation keeps it
        let rep = engine_mods::place(&dirs, &p, &g).unwrap();
        assert!(
            rep.kept.iter().any(|x| x == "shroudforge:sfpack") && rep.placed.is_empty(),
            "{tag}: {rep:#?}"
        );
        let cleanup = engine_mods::unplace(&dirs, &g).unwrap();
        assert_eq!(
            cleanup.removed,
            vec![g.join("mods").join("sfpack")],
            "{tag}"
        );
    }
}

/// On Windows the receipts an activation saves are spelled by the route it passes (the one it froze), not by kiln.json
/// read again at each save: an explicit interpreter's route while kiln.json says hosted names the copies as they are
/// (ShroudForge's config is not asked), main's route while kiln.json says kiln keeps the older launcher's spelling.
#[cfg(windows)]
#[test]
fn windows_receipts_follow_the_route_the_activation_passes() {
    use kiln_manage::{engine_mods, paths::DataDirs, profile};
    for (tag, json, route) in [
        ("frozen-kiln", HOSTED, Route::Interpreter(Runner::Ready)),
        (
            "frozen-hosted",
            KILN,
            Route::Legacy(Legacy::Selected(Backend::Hosted)),
        ),
    ] {
        let t = tmp(&format!("win-frozen-{tag}"));
        let dirs = DataDirs::at(t.0.join("data"));
        let g = game(&t.0.join("game"), true);
        let verbatim = format!(r"\\?\{}", g.join("mods").display());
        sf_install(&g, Some(&verbatim));
        select(&g, Some(json));
        let sf = import(
            &dirs,
            &sf_package(&t.0, "sfpack", false),
            ModKind::ShroudForge,
            "sfpack",
        );
        let mut p = profile::create(&dirs, "win", None).unwrap();
        p.mods.shroudforge = vec![pm(&sf, true)];
        let p = profile::save(&dirs, &p).unwrap();
        engine_mods::place_routed(&dirs, &p, &g, &route).unwrap();
        let receipts =
            std::fs::read_to_string(g.join("kiln/state/library-placements.json")).unwrap();
        let doc: serde_json::Value = serde_json::from_str(&receipts).unwrap();
        let path = doc[0]["path"].as_str().unwrap().to_string();
        let plain = g.join("mods").join("sfpack").display().to_string();
        if route.interpreter() {
            assert_eq!(path, plain, "{tag}: {receipts}");
        } else {
            assert_eq!(path, format!(r"\\?\{plain}"), "{tag}: {receipts}");
        }
    }
}
