//! The profile's EML mods while ShroudForge runs them (Kiln hosts ShroudForge for the profile's ShroudForge mods).
//! ShroudForge runs a package of its mods folder only when its `extended.mod.json` says `enabled: true`, and a legacy
//! EML package has no such file until ShroudForge writes one at the first change of its switch. A real client
//! (2026-11-11) ran one of four EML mods: the three without that file were applied by nobody. So Kiln's copy of a
//! profile EML mod in ShroudForge's folder carries ShroudForge's switch, set from the profile, and the file is part of
//! what Kiln placed; what the player (or ShroudForge) changes in it later is the player's.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::engine_mods;
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::own_loaders;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::shroudforge;
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-eml-under-sf-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A legacy EML package (More Gliders, Aldrich Bare Body): mod.json and Lua, no extended.mod.json.
fn legacy(root: &Path, id: &str, version: &str) -> PathBuf {
    let d = root.join("src").join(format!("{id}-{version}"));
    put(&d, "mod.json", format!(r#"{{"id":"{id}","name":"{id}","version":"{version}","capabilities":["patch"]}}"#).as_bytes());
    put(&d, "src/mod.lua", format!("-- {version}\nreturn {{}}").as_bytes());
    d
}

/// An EML package whose author ships ShroudForge's file (Wardrobe - Transmog: `launcher: "EML"`), with a setting.
const SHIPPED: &str = "{\n    \"$schema\" : \"https://bonsaibauer.github.io/shroudforge/schemas/extended.mod.schema.json\",\n    \"schemaVersion\" : 1,\n    \"enabled\" :   false,\n    \"targets\": [ \"client\" ],\n    \"launcher\": \"EML\",\n    \"settings\": { \"radius\": { \"label\": \"Radius\", \"control\": \"slider\", \"min\": 1, \"max\": 50, \"value\": 5 } }\n}\n";

fn shipped(root: &Path, id: &str, version: &str, ext: &str) -> PathBuf {
    let d = legacy(root, id, version);
    put(&d, "extended.mod.json", ext.as_bytes());
    d
}

/// A ShroudForge package of its own (`launcher: "SF"`): with one of them switched on, Kiln hosts ShroudForge.
fn sf_package(root: &Path, id: &str) -> PathBuf {
    let d = root.join("src").join(id);
    put(&d, "mod.json", format!(r#"{{"id":"{id}","name":"{id}","version":"1.0.0","capabilities":["runtime"]}}"#).as_bytes());
    put(&d, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client"]}"#);
    put(&d, "src/mod.lua", b"return {}");
    d
}

fn import(dirs: &DataDirs, src: &Path, kind: ModKind, id: &str) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.kind == kind && x.id == id).cloned()
        .unwrap_or_else(|| panic!("{kind:?} {id} not imported: {r:?}"))
}

fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

/// A game folder with Kiln's ShroudForge plugin and the bundled runtime (the files own_loaders::find looks for).
fn game(t: &Tmp) -> PathBuf {
    let g = t.0.join("game");
    put(&g, "enshrouded.exe", b"MZ");
    put(&g, "kiln/plugins/kiln-shroudforge.dll", b"x");
    put(&g, "kiln/plugins/shroudforge/shroudforge-runtime.dll", b"x");
    g
}

struct Setup {
    t: Tmp,
    dirs: DataDirs,
    game: PathBuf,
    sf: LibraryRef,
}

fn setup(tag: &str) -> Setup {
    let t = tmp(tag);
    let dirs = DataDirs::at(t.0.join("data"));
    let game = game(&t);
    let sf = import(&dirs, &sf_package(&t.0, "sf-crafting-essentials"), ModKind::ShroudForge, "sf-crafting-essentials");
    Setup { t, dirs, game, sf }
}

impl Setup {
    /// A profile with the ShroudForge mod on (Kiln hosts ShroudForge) and these EML mods.
    fn profile(&self, eml: Vec<ProfileMod>) -> profile::Profile {
        let mut p = profile::create(&self.dirs, "mixed", None).unwrap();
        p.mods.shroudforge = vec![pm(&self.sf, true)];
        p.mods.eml = eml;
        p
    }
}

fn ext_of(dir: &Path) -> Option<String> {
    std::fs::read_to_string(dir.join("extended.mod.json")).ok()
}

#[test]
fn a_legacy_eml_mod_placed_while_kiln_hosts_shroudforge_gets_shroudforges_switch_on() {
    let s = setup("legacy");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let p = s.profile(vec![pm(&gliders, true)]);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.placed.contains(&"eml:more-gliders".to_string()), "{rep:?}");
    let copy = s.game.join("mods/more-gliders");
    // the file ShroudForge's ensure_eml_extension writes for a legacy EML package, with the profile's switch
    let ext: serde_json::Value = serde_json::from_str(&ext_of(&copy).expect("Kiln's copy carries ShroudForge's switch")).unwrap();
    assert_eq!(ext, serde_json::json!({"$schema": "https://bonsaibauer.github.io/shroudforge/schemas/extended.mod.schema.json",
        "schemaVersion": 1, "enabled": true, "targets": ["client", "server"], "launcher": "EML", "settings": {}}));
    // read as ShroudForge 1.7.9 reads packages: switched on, an EML package, both sides
    let m = shroudforge::read_mod(&copy).unwrap();
    assert_eq!((m.enabled, m.extended, m.launcher.as_deref(), m.targets.clone()), (true, true, Some("EML"), vec!["client".to_string(), "server".into()]));
    let (listed, _) = shroudforge::list_mods(&s.game);
    assert!(listed.iter().any(|x| x.id == "more-gliders" && x.enabled), "{listed:?}");
    // the Library entry is immutable: nothing is written there
    assert!(ext_of(&library::path_of(&s.dirs, &gliders).unwrap()).is_none());
    // the file is part of what Kiln placed: the next activation keeps the copy, and uninstalling removes it
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.kept.contains(&"eml:more-gliders".to_string()) && rep.changed.is_empty() && rep.retained.is_empty(), "{rep:?}");
    assert!(rep.own_loaders.iter().any(|n| n.loader.kind == ModKind::Eml && n.copied == ["more-gliders"] && n.off.is_empty()), "{:?}", rep.own_loaders);
    let cleanup = engine_mods::unplace(&s.dirs, &s.game).unwrap();
    assert!(cleanup.retained.is_empty() && cleanup.removed.contains(&copy), "{cleanup:?}");
}

#[test]
fn a_shipped_extension_only_has_its_switch_changed() {
    let s = setup("shipped");
    let wardrobe = import(&s.dirs, &shipped(&s.t.0, "fcu-render-observer", "0.8.4", SHIPPED), ModKind::Eml, "fcu-render-observer");
    let p = s.profile(vec![pm(&wardrobe, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/fcu-render-observer");
    // byte for byte the author's file, with only the value of `enabled` replaced
    assert_eq!(ext_of(&copy).unwrap(), SHIPPED.replace("\"enabled\" :   false", "\"enabled\" :   true"));
    let m = shroudforge::read_mod(&copy).unwrap();
    assert_eq!((m.enabled, m.targets.clone()), (true, vec!["client".to_string()]));
    assert_eq!(ext_of(&library::path_of(&s.dirs, &wardrobe).unwrap()).unwrap(), SHIPPED, "the Library entry is not written");
}

#[test]
fn what_the_player_changes_in_shroudforges_file_survives_activation_and_an_update() {
    let s = setup("player");
    let v1 = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
    let mut p = s.profile(vec![pm(&v1, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/radius-mod");
    // in the game: a setting changed and the switch turned off on Kiln's ShroudForge page (or ShroudForge's Modloader)
    shroudforge::set_setting(&copy, "radius", &serde_json::json!(9)).unwrap();
    shroudforge::set_enabled(&copy, false).unwrap();
    // the next activation: the copy is still Kiln's, the profile is the switch, the setting is the player's
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.kept.contains(&"eml:radius-mod".to_string()) && rep.changed.is_empty() && rep.retained.is_empty(), "{rep:?}");
    let m = shroudforge::read_mod(&copy).unwrap();
    assert_eq!((m.enabled, &m.settings[0].value), (true, &serde_json::json!(9)));
    // a new version (new Lua, a new setting): placed, the player's value carried, the switch from the profile
    let next = SHIPPED.replace("\"value\": 5 } }", "\"value\": 5 }, \"extra\": { \"label\": \"Extra\", \"value\": true } }");
    let v2 = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.1.0", &next), ModKind::Eml, "radius-mod");
    p.mods.eml = vec![pm(&v2, true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.placed.contains(&"eml:radius-mod".to_string()) && rep.retained.is_empty(), "{rep:?}");
    assert_eq!(std::fs::read(copy.join("src/mod.lua")).unwrap(), b"-- 1.1.0\nreturn {}");
    let m = shroudforge::read_mod(&copy).unwrap();
    let value = |k: &str| m.settings.iter().find(|x| x.key == k).map(|x| x.value.clone());
    assert_eq!((m.enabled, value("radius"), value("extra")), (true, Some(serde_json::json!(9)), Some(serde_json::json!(true))));
    // the player's value is not Kiln's to throw away: uninstalling keeps the copy and says why
    let cleanup = engine_mods::unplace(&s.dirs, &s.game).unwrap();
    assert!(cleanup.retained.iter().any(|r| r.path == copy), "{cleanup:?}");
    assert_eq!(shroudforge::read_mod(&copy).unwrap().settings[0].value, serde_json::json!(9));
}

#[test]
fn the_profile_switching_an_eml_mod_off_takes_it_from_shroudforge_without_losing_the_players_values() {
    let s = setup("off");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let radius = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
    let mut p = s.profile(vec![pm(&gliders, true), pm(&radius, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let (gcopy, rcopy) = (s.game.join("mods/more-gliders"), s.game.join("mods/radius-mod"));
    shroudforge::set_setting(&rcopy, "radius", &serde_json::json!(12)).unwrap();
    p.mods.eml.iter_mut().for_each(|m| m.enabled = false);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    // a copy with nothing of the player's goes, as before: ShroudForge no longer sees it
    assert!(rep.removed.contains(&"eml:more-gliders".to_string()) && !gcopy.exists(), "{rep:?}");
    // a copy holding the player's ShroudForge setting stays, switched off in ShroudForge's file, and the result says so
    let m = shroudforge::read_mod(&rcopy).unwrap();
    assert_eq!((m.enabled, &m.settings[0].value), (false, &serde_json::json!(12)));
    assert!(rep.retained.iter().any(|r| r.path == rcopy && r.reason.contains("switched it off")), "{rep:?}");
    let (listed, _) = shroudforge::list_mods(&s.game);
    assert!(!listed.iter().any(|x| x.enabled && x.id != "sf-crafting-essentials"), "{listed:?}");
    // on again: the same copy, the switch on, the value as the player left it
    p.mods.eml.iter_mut().for_each(|m| m.enabled = true);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.kept.contains(&"eml:radius-mod".to_string()) && rep.placed.contains(&"eml:more-gliders".to_string()), "{rep:?}");
    let m = shroudforge::read_mod(&rcopy).unwrap();
    assert_eq!((m.enabled, &m.settings[0].value), (true, &serde_json::json!(12)));
    assert!(shroudforge::read_mod(&gcopy).unwrap().enabled);
}

/// The DLL-carrying EML mods (Auto Loot Plus) live in `<game>/mods/<folder>`, which is ShroudForge's mods folder too.
fn alp_package(root: &Path) -> PathBuf {
    let d = root.join("pkg/AutoLootPlus 1.1.8");
    put(&d, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"auto-loot-plus.log\0dinput8-chain.ini\0[Chain]\0mods\\auto-loot-plus\\config.txt"));
    put(&d, "mods/auto-loot-plus/mod.json", br#"{"id":"auto-loot-plus","name":"Auto Loot Plus","version":"1.1.8","capabilities":["patch"]}"#);
    put(&d, "mods/auto-loot-plus/src/mod.lua", b"return {}");
    put(&d, "mods/auto-loot-plus/config.txt", b"radius=15");
    d
}

#[test]
fn without_shroudforge_mods_no_switch_is_written_and_kiln_eml_runs_the_copy() {
    let s = setup("no-sf");
    let r = library::import(&s.dirs, &[alp_package(&s.t.0)], &mut NoProgress).unwrap();
    let alp = r.imported.iter().find(|x| x.kind == ModKind::Eml).cloned().expect("the EML part");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let mut p = s.profile(vec![pm(&alp, true), pm(&gliders, true)]);
    p.mods.shroudforge.clear();
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/auto-loot-plus");
    assert!(rep.placed.contains(&"eml:auto-loot-plus".to_string()), "{rep:?}");
    assert!(ext_of(&copy).is_none(), "no ShroudForge this start: kiln-eml runs the copy, and nothing needs the switch");
    assert!(!s.game.join("mods/more-gliders").exists(), "kiln-eml runs it from the Library");
    // the DLL writes its settings; then the profile gains a ShroudForge mod: the same copy gets the switch, settings stay
    std::fs::write(copy.join("config.txt"), b"radius=42").unwrap();
    p.mods.shroudforge = vec![pm(&s.sf, true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.changed.is_empty() && rep.retained.is_empty(), "{rep:?}");
    assert!(shroudforge::read_mod(&copy).unwrap().enabled, "{rep:?}");
    assert!(shroudforge::read_mod(&s.game.join("mods/more-gliders")).unwrap().enabled);
    assert_eq!(std::fs::read(copy.join("config.txt")).unwrap(), b"radius=42");
    // and without them again: the game-folder copy stays as it is (its switch is harmless: kiln-eml reads only
    // `launcher`), the other copy goes
    p.mods.shroudforge.clear();
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.kept.contains(&"eml:auto-loot-plus".to_string()) && rep.removed.contains(&"eml:more-gliders".to_string()), "{rep:?}");
    assert!(rep.retained.is_empty(), "{rep:?}");
}

/// EML's own loader (dbghelp.dll) next to the game: it runs every EML mod in mods/, whatever ShroudForge's switch says.
fn eml_loader(game: &Path) {
    put(game, "dbghelp.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
}

/// [`alp_package`] whose EML part ships ShroudForge's file too.
fn alp_shipping(root: &Path, ext: &str) -> PathBuf {
    let d = alp_package(root);
    put(&d, "mods/auto-loot-plus/extended.mod.json", ext.as_bytes());
    d
}

/// The ways a profile's EML mod stops: switched off in the profile, removed from it, and "No profile" or uninstall
/// (profile::deactivate_report and install::uninstall_report both call engine_mods::unplace).
#[derive(Clone, Copy, Debug)]
enum Stop {
    Off,
    Gone,
    Unplace,
}

/// Stops every EML mod of `p` the way `how` says; the copies Kiln kept, as the result names them.
fn stop(s: &Setup, p: &mut profile::Profile, how: Stop) -> (Vec<PathBuf>, Vec<PathBuf>) {
    let retained = |r: &[engine_mods::RetainedPlacement]| r.iter().map(|r| r.path.clone()).collect::<Vec<_>>();
    match how {
        Stop::Off | Stop::Gone => {
            if let Stop::Off = how { p.mods.eml.iter_mut().for_each(|m| m.enabled = false) } else { p.mods.eml.clear() }
            let rep = engine_mods::place(&s.dirs, p, &s.game).unwrap();
            (retained(&rep.retained), rep.removed.iter().map(|t| s.game.join("mods").join(t.trim_start_matches("eml:"))).collect())
        }
        Stop::Unplace => {
            let c = engine_mods::unplace(&s.dirs, &s.game).unwrap();
            (retained(&c.retained), c.removed)
        }
    }
}

/// pr-manage P1: an EML copy that ships `extended.mod.json`, placed while ShroudForge was not the EML runner (EML's own
/// loader ran them, or a game-folder copy before the profile had ShroudForge mods), got no record of Kiln's bytes. When
/// ShroudForge later ran it and the player changed a value there, every way of stopping the mod removed the copy and the
/// value with it, without a word.
#[test]
fn values_changed_in_a_copy_placed_before_shroudforge_ran_it_are_never_removed() {
    for how in [Stop::Off, Stop::Gone, Stop::Unplace] {
        let s = setup(&format!("before-sf-{how:?}"));
        let radius = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
        let steady = import(&s.dirs, &shipped(&s.t.0, "steady-mod", "1.0.0", SHIPPED), ModKind::Eml, "steady-mod");
        let r = library::import(&s.dirs, &[alp_shipping(&s.t.0, SHIPPED)], &mut NoProgress).unwrap();
        let alp = r.imported.iter().find(|x| x.kind == ModKind::Eml).cloned().expect("the EML part");
        eml_loader(&s.game);
        let mut p = s.profile(vec![pm(&radius, true), pm(&steady, true), pm(&alp, true)]);
        p.mods.shroudforge.clear();
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert_eq!(rep.placed.len(), 3, "{rep:?}");
        let (rcopy, scopy, acopy) = (s.game.join("mods/radius-mod"), s.game.join("mods/steady-mod"), s.game.join("mods/auto-loot-plus"));
        for c in [&rcopy, &scopy, &acopy] { assert_eq!(ext_of(c).unwrap(), SHIPPED, "no ShroudForge yet: the shipped file as it is"); }
        // later ShroudForge runs them, at the same version, and the player changes a value in ShroudForge's Modloader
        std::fs::remove_file(s.game.join("dbghelp.dll")).unwrap();
        p.mods.shroudforge = vec![pm(&s.sf, true)];
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert_eq!(rep.switched.len(), 3, "{rep:?}");
        for c in [&rcopy, &acopy] { shroudforge::set_setting(c, "radius", &serde_json::json!(9)).unwrap(); }
        let (retained, removed) = stop(&s, &mut p, how);
        for c in [&rcopy, &acopy] {
            assert!(retained.contains(c), "{how:?}: {} holds the player's value and must stay: retained {retained:?}, removed {removed:?}", c.display());
            assert_eq!(shroudforge::read_mod(c).unwrap().settings[0].value, serde_json::json!(9), "{how:?}");
        }
        // the switch Kiln wrote is Kiln's: a copy holding nothing of the player's still goes
        assert!(removed.contains(&scopy) && !scopy.exists(), "{how:?}: retained {retained:?}, removed {removed:?}");
    }
}

/// pr-manage P1, the receipts an earlier 0.2.0-dev.24 build wrote for such a copy: `extended.mod.json` listed without
/// its switch and settings, and no record of Kiln's bytes. A file whose values differ from the Library's is the
/// player's; one that holds the Library's values (Kiln's switch aside) is not.
#[test]
fn an_older_receipt_without_kilns_bytes_takes_values_that_differ_from_the_librarys_as_the_players() {
    for how in [Stop::Off, Stop::Gone, Stop::Unplace] {
        let s = setup(&format!("older-receipt-{how:?}"));
        let radius = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
        let steady = import(&s.dirs, &shipped(&s.t.0, "steady-mod", "1.0.0", SHIPPED), ModKind::Eml, "steady-mod");
        let r = library::import(&s.dirs, &[alp_shipping(&s.t.0, SHIPPED)], &mut NoProgress).unwrap();
        let alp = r.imported.iter().find(|x| x.kind == ModKind::Eml).cloned().expect("the EML part");
        eml_loader(&s.game);
        let mut p = s.profile(vec![pm(&radius, true), pm(&steady, true), pm(&alp, true)]);
        p.mods.shroudforge.clear();
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let file = s.game.join("kiln/state/library-placements.json");
        let mut receipts: serde_json::Value = serde_json::from_slice(&std::fs::read(&file).unwrap()).unwrap();
        for r in receipts.as_array_mut().unwrap() {
            assert!(r["files"].get("extended.mod.json").is_some(), "{r}");
            r.as_object_mut().unwrap().remove("extension");
        }
        std::fs::write(&file, serde_json::to_vec(&receipts).unwrap()).unwrap();
        std::fs::remove_file(s.game.join("dbghelp.dll")).unwrap();
        p.mods.shroudforge = vec![pm(&s.sf, true)];
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let (rcopy, scopy, acopy) = (s.game.join("mods/radius-mod"), s.game.join("mods/steady-mod"), s.game.join("mods/auto-loot-plus"));
        assert!(shroudforge::read_mod(&scopy).unwrap().enabled);
        for c in [&rcopy, &acopy] { shroudforge::set_setting(c, "radius", &serde_json::json!(9)).unwrap(); }
        let (retained, removed) = stop(&s, &mut p, how);
        for c in [&rcopy, &acopy] {
            assert!(retained.contains(c), "{how:?}: {} holds the player's value and must stay: retained {retained:?}, removed {removed:?}", c.display());
            assert_eq!(shroudforge::read_mod(c).unwrap().settings[0].value, serde_json::json!(9), "{how:?}");
        }
        assert!(removed.contains(&scopy) && !scopy.exists(), "{how:?}: retained {retained:?}, removed {removed:?}");
    }
}

/// The copy and receipt an older Kiln (0.2.0-dev.23, and dev.24 before ShroudForge's switch was part of Kiln's EML
/// copies) left while it hosted ShroudForge: the copy without extended.mod.json, the receipt listing no such file and
/// recording none of Kiln's bytes.
fn as_an_older_kiln_placed_it(game: &Path, copy: &Path) {
    std::fs::remove_file(copy.join("extended.mod.json")).unwrap();
    let file = game.join("kiln/state/library-placements.json");
    let mut receipts: serde_json::Value = serde_json::from_slice(&std::fs::read(&file).unwrap()).unwrap();
    let r = receipts.as_array_mut().unwrap().iter_mut().find(|r| r["path"].as_str() == copy.to_str()).expect("the copy's receipt");
    r["files"].as_object_mut().unwrap().remove("extended.mod.json").expect("listed now");
    r.as_object_mut().unwrap().remove("extension");
    std::fs::write(&file, serde_json::to_vec(&receipts).unwrap()).unwrap();
}

#[test]
fn a_copy_an_older_kiln_placed_without_the_switch_gets_it_at_the_next_play() {
    let s = setup("older");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let p = s.profile(vec![pm(&gliders, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/more-gliders");
    as_an_older_kiln_placed_it(&s.game, &copy);
    assert!(copy.is_dir() && ext_of(&copy).is_none());
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.kept.contains(&"eml:more-gliders".to_string()) && rep.switched.contains(&"eml:more-gliders".to_string()), "{rep:?}");
    assert!(shroudforge::read_mod(&copy).unwrap().enabled);
    // the file Kiln added is Kiln's: not a file the player wrote, so uninstalling removes the copy
    let cleanup = engine_mods::unplace(&s.dirs, &s.game).unwrap();
    assert!(cleanup.retained.is_empty() && cleanup.removed.contains(&copy), "{cleanup:?}");
    assert!(!s.game.join("kiln/kept-mod-files/eml/more-gliders").exists());
}

/// pr-manage P2: EML's own loader (dbghelp.dll) in the game folder, which under Wine or Proton runs only with a DLL
/// override the launcher cannot see. Without one it is not loaded, kiln-eml does not stand aside for it, and the
/// ShroudForge adapter hosts ShroudForge for the profile's ShroudForge mods (its `wanted()` looks at no proxy file):
/// ShroudForge runs the EML mods of its mods folder whose switch is on, and kiln-eml stands aside for it. Kiln's copies
/// there must carry that switch, and the launcher's note must say who runs them in that case.
#[test]
fn with_emls_proxy_file_in_the_folder_kilns_copies_still_carry_shroudforges_switch() {
    let s = setup("eml-proxy");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    eml_loader(&s.game);
    let p = s.profile(vec![pm(&gliders, true)]);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/more-gliders");
    assert!(rep.placed.contains(&"eml:more-gliders".to_string()), "{rep:?}");
    assert!(ext_of(&copy).is_some(), "the copy must carry ShroudForge's switch: {rep:?}");
    assert!(shroudforge::read_mod(&copy).unwrap().enabled);
    // EML's own loader, when it runs, reads only `launcher` from that file: it runs the copy all the same
    let eml = rep.own_loaders.iter().find(|n| n.loader.kind == ModKind::Eml).expect("a note for the EML mods");
    assert_eq!((eml.loader.file.as_str(), eml.copied.clone()), ("dbghelp.dll", vec!["more-gliders".to_string()]), "{eml:?}");
    assert!(!eml.text.contains("without one Kiln runs these mods itself"), "{}", eml.text);
    assert!(eml.text.contains("without one, ShroudForge (hosted by Kiln for this profile's ShroudForge mods) runs them"), "{}", eml.text);
    // switched off in the game: in that case ShroudForge does not run it, and the note says so
    shroudforge::set_enabled(&copy, false).unwrap();
    let view = own_loaders::ProfileView::of_profile(&p);
    let notes = own_loaders::notes(&s.game, &own_loaders::find(&s.game, Some(&view)), Some(&view));
    let eml = notes.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert_eq!(eml.off, ["more-gliders"], "{eml:?}");
    // the next Play switches it on again
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.switched.contains(&"eml:more-gliders".to_string()) && shroudforge::read_mod(&copy).unwrap().enabled, "{rep:?}");
    // without ShroudForge mods in the profile nothing is hosted: the note is EML's alone, and Kiln runs them itself
    let mut q = p.clone();
    q.mods.shroudforge.clear();
    let rep = engine_mods::place(&s.dirs, &q, &s.game).unwrap();
    let eml = rep.own_loaders.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert!(eml.text.contains("without one Kiln runs these mods itself"), "{}", eml.text);
}

/// EML's own loader chained by Kiln's proxy (`kiln/chain.ini`) always runs, as a proxy file on Windows does: kiln-eml
/// stands aside for it and ShroudForge cannot take the startup writer, so ShroudForge runs no EML mod and the note names
/// no ShroudForge switch (the switch Kiln writes is harmless: EML's loader reads only `launcher` from that file).
#[test]
fn with_emls_loader_chained_the_note_names_no_shroudforge_switch() {
    let s = setup("eml-chain");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    put(&s.game, "kiln/chain/eml-dbghelp.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
    put(&s.game, "kiln/chain.ini", b"[chain]\nchain\\eml-dbghelp.dll\n");
    let p = s.profile(vec![pm(&gliders, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/more-gliders");
    shroudforge::set_enabled(&copy, false).unwrap();
    let view = own_loaders::ProfileView::of_profile(&p);
    let notes = own_loaders::notes(&s.game, &own_loaders::find(&s.game, Some(&view)), Some(&view));
    let eml = notes.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert_eq!(eml.loader.via, own_loaders::Via::Chain, "{eml:?}");
    assert!(eml.off.is_empty() && !eml.text.contains("ShroudForge has"), "{eml:?}");
}

/// pr-manage P5: "activate the profile again to switch them on" helps a copy whose switch is off, not one for the other
/// side or one ShroudForge cannot read. Each gets its own list and a sentence that is true, Kiln's copies and the
/// player's own folders alike.
#[test]
fn the_launcher_tells_switched_off_other_side_and_unreadable_eml_mods_apart() {
    let s = setup("why-off");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let server = SHIPPED.replace("\"targets\": [ \"client\" ]", "\"targets\": [ \"server\" ]");
    let srv = import(&s.dirs, &shipped(&s.t.0, "server-tweaks", "1.0.0", &server), ModKind::Eml, "server-tweaks");
    let refused = SHIPPED.replace("\"schemaVersion\" : 1", "\"schemaVersion\" : 2");
    let bad = import(&s.dirs, &shipped(&s.t.0, "schema-two", "1.0.0", &refused), ModKind::Eml, "schema-two");
    // the player's own folders of two more profile mods: one for the other side, one ShroudForge cannot read
    let hand_srv = import(&s.dirs, &legacy(&s.t.0, "hand-server", "1.0.0"), ModKind::Eml, "hand-server");
    let hand_bad = import(&s.dirs, &legacy(&s.t.0, "hand-bad", "1.0.0"), ModKind::Eml, "hand-bad");
    for (dir, id, ext) in [("Hand Server", "hand-server", &server), ("Hand Bad", "hand-bad", &refused)] {
        let d = s.game.join("mods").join(dir);
        put(&d, "mod.json", format!(r#"{{"id":"{id}","name":"{id}","version":"1.0.0","capabilities":["patch"]}}"#).as_bytes());
        put(&d, "src/mod.lua", b"return {}");
        put(&d, "extended.mod.json", ext.replace("\"enabled\" :   false", "\"enabled\" :   true").as_bytes());
    }
    let p = s.profile(vec![pm(&gliders, true), pm(&srv, true), pm(&bad, true), pm(&hand_srv, true), pm(&hand_bad, true)]);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    shroudforge::set_enabled(&s.game.join("mods/more-gliders"), false).unwrap();
    let view = own_loaders::ProfileView::of_profile(&p);
    let notes = own_loaders::notes(&s.game, &own_loaders::find(&s.game, Some(&view)), Some(&view));
    let eml = notes.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert_eq!((eml.off.clone(), eml.other_side.clone(), eml.unreadable.clone()),
        (vec!["more-gliders".to_string()], vec!["server-tweaks".to_string(), "hand-server".into()], vec!["schema-two".to_string(), "hand-bad".into()]), "{eml:?}");
    // the player's own folders keep their reasons there too (review of 13 Nov 2026): one ShroudForge takes, one it refuses
    assert_eq!((eml.own.clone(), eml.own_off.clone()), (vec!["hand-server".to_string()], Vec::<String>::new()), "{eml:?}");
    let sentence = |id: &str| eml.text.split(". ").find(|s| s.contains(&format!("({id}"))).unwrap_or_else(|| panic!("{id}: {}", eml.text)).to_string();
    assert!(sentence("more-gliders").contains("activate the profile again to switch them on"), "{}", eml.text);
    assert!(sentence("server-tweaks").contains("server only") && !sentence("server-tweaks").contains("activate"), "{}", eml.text);
    assert!(sentence("schema-two").contains("cannot read") && !sentence("schema-two").contains("activate the profile again"), "{}", eml.text);
    let own = |id: &str| rep.own_notes.iter().find(|n| n.starts_with(&format!("eml:{id}"))).unwrap_or_else(|| panic!("{id}: {:?}", rep.own_notes)).clone();
    assert!(own("hand-server").contains("server only") && !own("hand-server").contains("switch it on"), "{}", own("hand-server"));
    assert!(own("hand-bad").contains("cannot read") && !own("hand-bad").contains("switch it on"), "{}", own("hand-bad"));
}

#[test]
fn the_launcher_says_when_shroudforge_has_an_eml_mod_off_and_never_writes_a_players_folder() {
    let s = setup("notes");
    let gliders = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    let bare = import(&s.dirs, &legacy(&s.t.0, "aldrich-bare-body", "0.5.3"), ModKind::Eml, "aldrich-bare-body");
    // the player installed this one by hand into the game's mods folder: no extended.mod.json, so ShroudForge has it off
    let hand = s.game.join("mods/Aldrich Body");
    put(&hand, "mod.json", br#"{"id":"aldrich-bare-body","name":"Aldrich","version":"0.5.3","capabilities":["patch"]}"#);
    put(&hand, "src/mod.lua", b"return {}");
    let p = s.profile(vec![pm(&gliders, true), pm(&bare, true)]);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.own.contains(&"eml:aldrich-bare-body".to_string()), "{rep:?}");
    assert!(ext_of(&hand).is_none() && engine_mods::marker(&hand).is_none(), "a player's folder is never written to");
    let note = rep.own_notes.iter().find(|n| n.starts_with("eml:aldrich-bare-body")).expect("a note for the player's folder");
    assert!(note.contains("ShroudForge has it switched off") && !note.contains("the game uses your own folder"), "{note}");
    // the words promised to a player on Nexus (14 Nov 2026): what happened and the two ways out, nothing about
    // ShroudForge's files (it read as "Kiln wants a ShroudForge file from an EML mod")
    assert!(note.starts_with("eml:aldrich-bare-body: You put this mod into ShroudForge's folder yourself, and ShroudForge has it switched \
        off. Switch it on in Kiln's in-game menu (Mods > ShroudForge), or remove the folder and press Play: the mod is in your profile, so \
        Kiln then places its own copy, switched on."), "{note}");
    assert!(note.contains(&hand.display().to_string()) && !note.contains("extended.mod.json"), "{note}");
    // a copy whose switch is off (turned off in the game, or a write that failed) is not said to run
    shroudforge::set_enabled(&s.game.join("mods/more-gliders"), false).unwrap();
    let view = own_loaders::ProfileView::of_profile(&p);
    let notes = own_loaders::notes(&s.game, &own_loaders::find(&s.game, Some(&view)), Some(&view));
    let eml = notes.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert_eq!(eml.off, ["more-gliders"], "{eml:?}");
    assert!(eml.text.contains("switched off in ShroudForge's settings"), "{}", eml.text);
}

/// A player's report (0.2.0-dev.31.9): the profile had an EML mod off, his own folder of it was in ShroudForge's mods folder
/// with ShroudForge's own switch on, and ShroudForge ran it. Kiln never writes into that folder; activation says what
/// happens to it (Kiln's bundled runtime skips it at each start) and how to stop it where Kiln cannot. One whose switch is
/// off does not run, and gets no note.
#[test]
fn a_players_own_eml_folder_of_a_mod_that_is_off_says_what_hosted_shroudforge_does() {
    let s = setup("own-off");
    let quiver = import(&s.dirs, &legacy(&s.t.0, "quiver-slots", "0.8.2"), ModKind::Eml, "quiver-slots");
    let quiet = import(&s.dirs, &legacy(&s.t.0, "quiet-own", "1.0.0"), ModKind::Eml, "quiet-own");
    let hand = s.game.join("mods/Quiver Slots");
    put(&hand, "mod.json", br#"{"id":"quiver-slots","name":"Quiver","version":"0.8.2","capabilities":["patch"]}"#);
    put(&hand, "src/mod.lua", b"return {}");
    put(&hand, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"EML","enabled":true,"targets":["client","server"]}"#);
    let shut = s.game.join("mods/quiet-own");
    put(&shut, "mod.json", br#"{"id":"quiet-own","name":"Quiet","version":"1.0.0","capabilities":["patch"]}"#);
    put(&shut, "src/mod.lua", b"return {}");
    put(&shut, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"EML","enabled":false}"#);
    let before = ext_of(&hand);
    let p = s.profile(vec![pm(&quiver, false), pm(&quiet, false)]);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let note = rep.safety_notes.iter().find(|n| n.starts_with("eml:quiver-slots")).unwrap_or_else(|| panic!("a safety note: {rep:?}"));
    assert!(note.starts_with(&format!("eml:quiver-slots: it is off in the profile, and your own folder {} ", hand.display())), "{note}");
    assert!(note.contains("when that runtime takes Kiln's skip list") && note.contains("kiln.log says at each start whether it was skipped")
        && !note.contains("Kiln's bundled runtime skips this folder at each start") && note.contains("Mods > ShroudForge")
        && note.contains("move the folder out"), "{note}");
    assert!(!note.contains("Let Kiln manage this folder"), "Kiln takes over ShroudForge folders only: {note}");
    assert!(rep.own_notes.contains(note), "{rep:?}");
    assert!(!rep.safety_notes.iter().any(|n| n.starts_with("eml:quiet-own")), "its switch is off: it does not run {rep:?}");
    assert_eq!(ext_of(&hand), before, "a player's folder is never written to");
    assert!(engine_mods::marker(&hand).is_none());
}

/// sol-playtest-manage S9: the receipt lists `extended.mod.json` without its switch and its whole `settings` object, and
/// the check for the player's ShroudForge settings compared only their values. An edit of a setting's metadata alone
/// (the player widened a slider's range by hand) must survive an update and every way of stopping the mod: kept in the
/// copy or set aside where the result says, never overwritten or deleted.
#[test]
fn a_settings_metadata_edit_in_kilns_eml_copy_survives_an_update_and_stopping_the_mod() {
    let wider = |copy: &Path| {
        let text = ext_of(copy).unwrap();
        assert!(text.contains("\"max\": 50"), "{text}");
        std::fs::write(copy.join("extended.mod.json"), text.replace("\"max\": 50", "\"max\": 500")).unwrap();
    };
    let edited = |copy: &Path| ext_of(copy).is_some_and(|t| t.contains("\"max\": 500"));
    // an update to a new version
    let s = setup("meta-update");
    let v1 = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
    let mut p = s.profile(vec![pm(&v1, true)]);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/radius-mod");
    wider(&copy);
    let next = SHIPPED.replace("\"value\": 5 } }", "\"value\": 5 }, \"extra\": { \"label\": \"Extra\", \"value\": true } }");
    let v2 = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.1.0", &next), ModKind::Eml, "radius-mod");
    p.mods.eml = vec![pm(&v2, true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let kept = s.game.join("kiln/kept-mod-files/eml/radius-mod");
    let set_aside = edited(&kept) && rep.kept_files.iter().any(|k| k.contains("extended.mod.json"));
    let stayed = edited(&copy) && rep.retained.iter().any(|r| r.path == copy);
    assert!(set_aside || stayed, "update: the player's edit is gone: copy {:?}, {rep:?}", ext_of(&copy));
    // the same for a copy in <game>/mods/<folder> for an EML mod's DLL (Auto Loot Plus), at its next version
    let alp = |version: &str, ext: &str| {
        let d = s.t.0.join(format!("pkg/AutoLootPlus {version}"));
        put(&d, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"auto-loot-plus.log\0dinput8-chain.ini\0[Chain]\0mods\\auto-loot-plus\\config.txt"));
        put(&d, "mods/auto-loot-plus/mod.json", format!(r#"{{"id":"auto-loot-plus","name":"Auto Loot Plus","version":"{version}","capabilities":["patch"]}}"#).as_bytes());
        put(&d, "mods/auto-loot-plus/src/mod.lua", format!("-- {version}").as_bytes());
        put(&d, "mods/auto-loot-plus/extended.mod.json", ext.as_bytes());
        let r = library::import(&s.dirs, &[d], &mut NoProgress).unwrap();
        r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Eml).cloned().expect("the EML part")
    };
    p.mods.eml = vec![pm(&alp("1.1.8", SHIPPED), true)];
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/auto-loot-plus");
    wider(&copy);
    p.mods.eml = vec![pm(&alp("1.1.9", &next), true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.placed.contains(&"eml:auto-loot-plus".to_string()), "{rep:?}");
    assert_eq!(std::fs::read(copy.join("src/mod.lua")).unwrap(), b"-- 1.1.9");
    let kept = s.game.join("kiln/kept-mod-files/eml/auto-loot-plus");
    assert!(edited(&kept) && rep.kept_files.iter().any(|k| k.starts_with("eml:auto-loot-plus") && k.contains("extended.mod.json")),
        "game-folder update: the player's edit is gone: copy {:?}, {rep:?}", ext_of(&copy));
    // stopping the mod
    for how in [Stop::Off, Stop::Gone, Stop::Unplace] {
        let s = setup(&format!("meta-{how:?}"));
        let v1 = import(&s.dirs, &shipped(&s.t.0, "radius-mod", "1.0.0", SHIPPED), ModKind::Eml, "radius-mod");
        let mut p = s.profile(vec![pm(&v1, true)]);
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/radius-mod");
        wider(&copy);
        let (retained, removed) = stop(&s, &mut p, how);
        assert!(retained.contains(&copy) && edited(&copy), "{how:?}: the player's edit is gone: retained {retained:?}, removed {removed:?}");
    }
}

/// Auto Loot Plus `version` as its author ships it (a dinput8.dll beside `mods/auto-loot-plus`, no extended.mod.json),
/// in the Library: its EML part.
fn alp_at(s: &Setup, version: &str) -> LibraryRef {
    let d = s.t.0.join(format!("pkg/AutoLootPlus {version}"));
    put(&d, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"auto-loot-plus.log\0dinput8-chain.ini\0[Chain]\0mods\\auto-loot-plus\\config.txt"));
    put(&d, "mods/auto-loot-plus/mod.json", format!(r#"{{"id":"auto-loot-plus","name":"Auto Loot Plus","version":"{version}","capabilities":["patch"]}}"#).as_bytes());
    put(&d, "mods/auto-loot-plus/src/mod.lua", format!("-- {version}").as_bytes());
    put(&d, "mods/auto-loot-plus/config.txt", b"radius=15");
    let r = library::import(&s.dirs, &[d], &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Eml).cloned().expect("the EML part")
}

/// ShroudForge's config as ShroudForge leaves it after a start: `paths.mods` as it writes it, from a canonical path.
fn sf_config(game: &Path, mods: &str) {
    put(game, "shroudforge/config/modloader-config.json", serde_json::json!({ "paths": { "mods": mods } }).to_string().as_bytes());
}

/// The file Kiln writes into its EML copy when the copy has none (ShroudForge's own `ensure_eml_extension`), switched on.
fn kilns_switch() -> serde_json::Value {
    serde_json::json!({"$schema": "https://bonsaibauer.github.io/shroudforge/schemas/extended.mod.schema.json",
        "schemaVersion": 1, "enabled": true, "targets": ["client", "server"], "launcher": "EML", "settings": {}})
}

/// Nexus report (2026-11-14, Windows, dev.28): Auto Loot Plus and Endless Chests carry a dinput8.dll, so Kiln copies their
/// EML part to `<game>\mods\<folder>`; ShroudForge, which Kiln hosts for the profile's ShroudForge mods, writes its
/// `paths.mods` as `\\?\C:\…\mods` at every start, Kiln compared that with `C:\…\mods`, and the copies never got
/// ShroudForge's switch. The comparison itself is Windows' (winpath::windows_below, tested on every system). Here the
/// same spellings as a Linux launcher meets them under Proton (the Z: drive, the verbatim prefix, another case): the
/// copy gets the switch with each, and the folder ShroudForge reads is `mods` in the launcher's note.
#[test]
fn a_game_folder_copy_gets_shroudforges_switch_however_shroudforge_spells_its_folder() {
    let z = |dir: &Path| format!("Z:{}", dir.display().to_string().replace('/', "\\"));
    let spellings: [(&str, fn(&str) -> String); 4] = [
        ("relative", |_| "mods".into()),
        ("z", |game_z| format!(r"{game_z}\mods")),
        ("verbatim", |game_z| format!(r"\\?\{game_z}\mods")),
        ("case", |game_z| format!(r"\\?\{}\MODS", game_z.to_uppercase())),
    ];
    for (tag, spell) in spellings {
        let s = setup(&format!("spell-{tag}"));
        // ShroudForge names its folder from a canonical path, so the folder is there
        std::fs::create_dir_all(s.game.join("mods")).unwrap();
        sf_config(&s.game, &spell(&z(&s.game)));
        assert_eq!(shroudforge::mods_dir(&s.game), s.game.join("mods"), "{tag}");
        let alp = alp_at(&s, "1.1.8");
        let p = s.profile(vec![pm(&alp, true)]);
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/auto-loot-plus");
        assert!(rep.placed.contains(&"eml:auto-loot-plus".to_string()) && rep.warnings.is_empty() && rep.retained.is_empty(), "{tag}: {rep:?}");
        let ext: serde_json::Value = serde_json::from_str(&ext_of(&copy).unwrap_or_else(|| panic!("{tag}: no switch in Kiln's copy"))).unwrap();
        assert_eq!(ext, kilns_switch(), "{tag}");
        let note = rep.own_loaders.iter().find(|n| n.loader.kind == ModKind::Eml).expect("ShroudForge runs the EML mods");
        assert!(note.loader.folder == "mods" && note.copied == ["auto-loot-plus"] && note.off.is_empty() && note.own.is_empty(), "{tag}: {note:?}");
    }
}

/// The workaround posted on Nexus for 0.2.0-dev.28 (the game closed, this file put by hand into each copy).
const HAND_SWITCH: &str = r#"{"schemaVersion":1,"enabled":true,"launcher":"EML"}"#;

/// The EML entries of a report list (the profile's ShroudForge mod is placed beside them).
fn eml(list: &[String]) -> Vec<&str> {
    list.iter().map(String::as_str).filter(|t| t.starts_with("eml:")).collect()
}

/// Every file named extended.mod.json under `dir`.
fn ext_files_under(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if p.is_dir() { out.extend(ext_files_under(&p)); } else if p.file_name().is_some_and(|n| n == "extended.mod.json") { out.push(p); }
    }
    out
}

/// A copy dev.28 left without ShroudForge's switch, with the switch the player then made by hand: the fixed build takes it
/// for the switch Kiln writes itself (it says the same). No error and nothing doubled; Kiln keeps managing the switch;
/// the file is never the player's setting: an update replaces the copy as usual and puts Kiln's own switch in, and
/// stopping the mod keeps nothing of it.
#[test]
fn a_switch_made_by_hand_in_kilns_copy_is_taken_as_kilns_own() {
    let s = setup("hand-switch");
    let alp = alp_at(&s, "1.1.8");
    // dev.28 on Windows: Kiln's copy without the switch (here: placed while the profile had no ShroudForge mod)
    let mut p = s.profile(vec![pm(&alp, true)]);
    p.mods.shroudforge.clear();
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/auto-loot-plus");
    assert!(ext_of(&copy).is_none());
    put(&copy, "extended.mod.json", HAND_SWITCH.as_bytes());
    std::fs::write(copy.join("config.txt"), b"radius=42").unwrap(); // the DLL's own setting
    // the fixed build, ShroudForge hosted again: kept, on, nothing to say
    p.mods.shroudforge = vec![pm(&s.sf, true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(eml(&rep.kept) == ["eml:auto-loot-plus"] && eml(&rep.placed).is_empty() && rep.switched.is_empty(), "{rep:?}");
    assert!(rep.changed.is_empty() && rep.retained.is_empty() && rep.warnings.is_empty() && rep.own.is_empty() && rep.kept_files.is_empty(), "{rep:?}");
    assert_eq!(ext_of(&copy).as_deref(), Some(HAND_SWITCH), "it already says what Kiln's file says");
    let note = rep.own_loaders.iter().find(|n| n.loader.kind == ModKind::Eml).unwrap();
    assert!(note.copied == ["auto-loot-plus"] && note.off.is_empty() && note.own.is_empty() && note.own_off.is_empty(), "{note:?}");
    // Kiln manages that switch: off in the game, on again at the next Play (the profile is the switch)
    shroudforge::set_enabled(&copy, false).unwrap();
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(eml(&rep.switched) == ["eml:auto-loot-plus"] && rep.retained.is_empty() && rep.kept_files.is_empty(), "{rep:?}");
    assert!(shroudforge::read_mod(&copy).unwrap().enabled);
    // the next version replaces the copy as usual: the DLL's setting goes over, the switch is Kiln's own file
    p.mods.eml = vec![pm(&alp_at(&s, "1.1.9"), true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(eml(&rep.placed) == ["eml:auto-loot-plus"] && rep.retained.is_empty() && rep.changed.is_empty(), "{rep:?}");
    assert_eq!(std::fs::read(copy.join("src/mod.lua")).unwrap(), b"-- 1.1.9");
    assert_eq!(std::fs::read(copy.join("config.txt")).unwrap(), b"radius=42");
    assert_eq!(serde_json::from_str::<serde_json::Value>(&ext_of(&copy).unwrap()).unwrap(), kilns_switch());
    assert!(!rep.kept_files.iter().any(|k| k.contains("extended.mod.json")), "{rep:?}");
    // stopping the mod: the copy goes, the DLL's setting waits for it, nothing of the switch is kept
    p.mods.eml[0].enabled = false;
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(eml(&rep.removed) == ["eml:auto-loot-plus"] && !copy.exists() && rep.retained.is_empty(), "{rep:?}");
    assert!(!rep.kept_files.iter().any(|k| k.contains("extended.mod.json")), "{rep:?}");
    assert_eq!(ext_files_under(&s.game.join("kiln")), Vec::<PathBuf>::new());
}

/// The same file when the next Play is also the mod's update: not carried to the new version as a setting of the
/// player's (it would stand in for Kiln's switch, and for a file the new version ships).
#[test]
fn a_switch_made_by_hand_is_not_carried_into_the_next_version() {
    let s = setup("hand-switch-update");
    let mut p = s.profile(vec![pm(&alp_at(&s, "1.1.8"), true)]);
    p.mods.shroudforge.clear();
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/auto-loot-plus");
    put(&copy, "extended.mod.json", HAND_SWITCH.as_bytes());
    p.mods.shroudforge = vec![pm(&s.sf, true)];
    p.mods.eml = vec![pm(&alp_at(&s, "1.1.9"), true)];
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(eml(&rep.placed) == ["eml:auto-loot-plus"] && rep.retained.is_empty() && rep.warnings.is_empty(), "{rep:?}");
    assert_eq!(serde_json::from_str::<serde_json::Value>(&ext_of(&copy).unwrap()).unwrap(), kilns_switch());
    assert!(!rep.kept_files.iter().any(|k| k.contains("extended.mod.json")), "{rep:?}");
    // and a hand-made file with something of the player's in it (a target they chose) stays the player's, as before
    let s = setup("hand-switch-targets");
    let mut p = s.profile(vec![pm(&alp_at(&s, "1.1.8"), true)]);
    p.mods.shroudforge.clear();
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/auto-loot-plus");
    let theirs = r#"{"schemaVersion":1,"enabled":true,"launcher":"EML","targets":["client"]}"#;
    put(&copy, "extended.mod.json", theirs.as_bytes());
    p.mods.shroudforge = vec![pm(&s.sf, true)];
    p.mods.eml = vec![pm(&alp_at(&s, "1.1.9"), true)];
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert_eq!(ext_of(&copy).as_deref(), Some(theirs));
}

#[test]
fn runtime_eml_alone_hosts_and_gets_a_switch() {
    let s = setup("runtime-alone");
    let src = legacy(&s.t.0, "quiver", "0.8.2");
    put(&src, "mod.json", br#"{"id":"quiver","name":"Quiver","version":"0.8.2","capabilities":["patch","runtime-register-dll"]}"#);
    let r = import(&s.dirs, &src, ModKind::Eml, "quiver");
    let mut p = s.profile(vec![pm(&r, true)]);
    p.mods.shroudforge.clear();
    let view = own_loaders::ProfileView::of_profile(&p).with_library(&s.dirs, &p);
    assert!(own_loaders::shroudforge_runs_eml(&own_loaders::find(&s.game, Some(&view))));
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(shroudforge::read_mod(&s.game.join("mods/quiver")).unwrap().enabled);
    let install = runtime_install(&s.game);
    let rendered = profile::render(&s.dirs, &p, &install, kiln_manage::game::PathMode::Native).unwrap();
    assert_eq!(rendered.eml_runtime, ["quiver"]);
    assert!(rendered.eml_runtime_blocked.is_empty());
    assert!(!rendered.mods.eml[0].enabled, "older kiln-eml must never fall back to data-only");
    assert!(rendered.mods.shroudforge.iter().any(|m| m.id == "quiver" && m.enabled), "older adapter hosting signal");
    let file_view = own_loaders::ProfileView::of_file(&rendered);
    assert!(own_loaders::shroudforge_runs_eml(&own_loaders::find(&s.game, Some(&file_view))));
}

fn runtime_install(game: &Path) -> kiln_manage::game::GameInstall {
    use kiln_manage::game::*;
    GameInstall { id: "test".into(), kind: GameKind::Client, path: game.into(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None,
        build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Native, source: "manual".into() }
}

#[test]
fn runtime_eml_missing_component_is_disabled_and_not_placed() {
    let s = setup("runtime-missing");
    std::fs::remove_file(s.game.join("kiln/plugins/shroudforge/shroudforge-runtime.dll")).unwrap();
    let src = legacy(&s.t.0, "quiver", "0.8.2");
    put(&src, "mod.json", br#"{"id":"quiver","name":"Quiver","version":"0.8.2","capabilities":["patch","runtime"]}"#);
    put(&src, "native-plugin.ini", b"test");
    let r = import(&s.dirs, &src, ModKind::Eml, "quiver");
    let mut p = s.profile(vec![pm(&r, true)]);
    p.mods.shroudforge.clear();
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(!s.game.join("mods/quiver").exists());
    assert!(rep.warnings.iter().any(|w| w.contains("needs ShroudForge's runtime")), "{rep:?}");
    let rendered = profile::render(&s.dirs, &p, &runtime_install(&s.game), kiln_manage::game::PathMode::Native).unwrap();
    assert!(!rendered.mods.eml[0].enabled);
    assert!(rendered.eml_runtime_blocked["quiver"].contains("component is missing"));
}

fn runtime_eml(s: &Setup, capability: &str) -> profile::Profile {
    let src = legacy(&s.t.0, "quiver", "0.8.2");
    put(&src, "mod.json", format!(r#"{{"id":"quiver","name":"Quiver","version":"0.8.2","capabilities":["patch","{capability}"]}}"#).as_bytes());
    let r = import(&s.dirs, &src, ModKind::Eml, "quiver");
    let mut p = s.profile(vec![pm(&r, true)]);
    p.mods.shroudforge.clear();
    p
}

fn assert_runtime_eml_refused<T: std::fmt::Debug>(result: kiln_manage::Result<T>, loader: &str) {
    let err = result.expect_err("a standalone EML loader prevents ShroudForge from starting the runtime");
    assert_eq!(err.code, kiln_manage::ErrorCode::Refused, "{err:?}");
    for text in ["quiver", "ShroudForge", "EML", loader, "Switch those mods off", "remove that loader", "activate"] {
        assert!(err.message.contains(text), "missing {text:?}: {}", err.message);
    }
}

#[test]
fn runtime_eml_refuses_render_and_placement_with_each_standalone_loader_route() {
    for capability in ["runtime", "runtime-register-dll"] {
        for route in ["proxy", "chain", "emf-chain"] {
            let s = setup(&format!("runtime-refuse-{capability}-{route}"));
            let p = runtime_eml(&s, capability);
            let loader = match route {
                "proxy" => { eml_loader(&s.game); "dbghelp.dll" },
                "chain" => {
                    put(&s.game, "kiln/chain/eml.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
                    put(&s.game, "kiln/chain.ini", b"[chain]\nchain\\eml.dll\n");
                    "kiln/chain/eml.dll"
                },
                _ => {
                    put(&s.game, "loaders/eml.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
                    put(&s.game, "dinput8-chain.ini", b"[EMF]\nloaders\\eml.dll\n");
                    "loaders/eml.dll"
                },
            };
            assert_runtime_eml_refused(engine_mods::place(&s.dirs, &p, &s.game), loader);
            assert_runtime_eml_refused(profile::render(&s.dirs, &p, &runtime_install(&s.game), kiln_manage::game::PathMode::Native), loader);
            assert!(!s.game.join("mods").exists(), "refuse before placing any engine package");
            assert!(!s.game.join("kiln/state/library-placements.json").exists());
        }
    }
}

#[test]
fn runtime_eml_refuses_activation_before_placing_packages() {
    let s = setup("runtime-refuse-activate");
    let p = runtime_eml(&s, "runtime-register-dll");
    let p = profile::save(&s.dirs, &p).unwrap();
    eml_loader(&s.game);
    assert_runtime_eml_refused(profile::activate(&s.dirs, &p.id, &runtime_install(&s.game)), "dbghelp.dll");
    assert!(!s.game.join("mods").exists());
    assert!(!s.game.join("kiln/profile.json").exists());
    assert!(!s.game.join("kiln/chain.ini").exists());
}

#[test]
fn runtime_eml_refuses_play_if_a_standalone_loader_appears_after_activation() {
    use kiln_manage::launch::{self, LaunchMode, LaunchRequest};
    let s = setup("runtime-refuse-play");
    let p = runtime_eml(&s, "runtime");
    let p = profile::save(&s.dirs, &p).unwrap();
    let install = runtime_install(&s.game);
    profile::activate(&s.dirs, &p.id, &install).unwrap();
    let before = std::fs::read(s.game.join("kiln/profile.json")).unwrap();
    std::fs::remove_dir_all(s.game.join("mods")).unwrap();
    eml_loader(&s.game);
    let request = LaunchRequest { mode: LaunchMode::Profile, profile_id: None, skip_backup: true };
    assert_runtime_eml_refused(launch::play_with(&s.dirs, &install, &request, Some(&[]), &|_| panic!("Steam must not be asked")), "dbghelp.dll");
    assert!(!s.game.join("mods").exists(), "Play must not republish to the standalone loader");
    assert_eq!(std::fs::read(s.game.join("kiln/profile.json")).unwrap(), before);
}

#[test]
fn runtime_eml_refuses_play_once_with_the_loader_conflict_message() {
    use kiln_manage::launch::{self, LaunchMode, LaunchRequest};
    let s = setup("runtime-refuse-once");
    let p = runtime_eml(&s, "runtime-register-dll");
    let p = profile::save(&s.dirs, &p).unwrap();
    eml_loader(&s.game);
    let request = LaunchRequest { mode: LaunchMode::Once, profile_id: Some(p.id), skip_backup: true };
    assert_runtime_eml_refused(launch::play_with(&s.dirs, &runtime_install(&s.game), &request, Some(&[]), &|_| panic!("Steam must not be asked")), "dbghelp.dll");
    assert!(!s.game.join("mods").exists());
    assert!(!s.game.join("kiln/profile.once.json").exists());
    assert!(!s.game.join("kiln/play-once.ini").exists());
}

/// sfi V8.9: the compatibility entry render adds to mods.shroudforge for a runtime EML package (and kiln-eml's route
/// switched off for it) are the launcher's own, not a change of ShroudForge's fixed folder: play-once of the active
/// selection runs, a different selection is still refused.
#[test]
fn play_once_of_a_runtime_eml_profile_is_not_refused_by_its_own_compatibility_entry() {
    for with_sf_mod in [false, true] {
        let s = setup(&format!("runtime-once-{with_sf_mod}"));
        let mut p = runtime_eml(&s, "runtime");
        if with_sf_mod { p.mods.shroudforge = vec![pm(&s.sf, true)]; }
        let p = profile::save(&s.dirs, &p).unwrap();
        let install = runtime_install(&s.game);
        profile::activate(&s.dirs, &p.id, &install).unwrap();
        let active: profile::ProfileFile = serde_json::from_slice(&std::fs::read(s.game.join("kiln/profile.json")).unwrap()).unwrap();
        assert!(active.mods.shroudforge.iter().any(|m| m.id == "quiver" && m.enabled), "the compatibility entry is there");
        let once = profile::write_once(&s.dirs, &p.id, &install).unwrap();
        assert_eq!(once.eml_runtime, ["quiver"]);
        assert!(s.game.join("kiln/profile.once.json").is_file());
        // A selection that differs in ShroudForge's folder still needs an activation: with a ShroudForge mod on, Kiln
        // hosts ShroudForge for the single start too, and it would run the switched-on copy of the package that is off.
        let mut off = profile::create(&s.dirs, "off", None).unwrap();
        off.mods = p.mods.clone();
        off.mods.eml[0].enabled = false;
        let off = profile::save(&s.dirs, &off).unwrap();
        if with_sf_mod {
            let e = profile::write_once(&s.dirs, &off.id, &install).unwrap_err();
            assert_eq!(e.code, kiln_manage::ErrorCode::Unsupported, "{e:?}");
        } else {
            // nothing hosts ShroudForge for that start (as before the runtime packages: an EML switch is the profile's)
            assert!(profile::write_once(&s.dirs, &off.id, &install).unwrap().eml_runtime.is_empty());
        }
        let mut sf_changed = profile::create(&s.dirs, "sf-changed", None).unwrap();
        sf_changed.mods = p.mods.clone();
        sf_changed.mods.shroudforge = if with_sf_mod { Vec::new() } else { vec![pm(&s.sf, true)] };
        let sf_changed = profile::save(&s.dirs, &sf_changed).unwrap();
        let e = profile::write_once(&s.dirs, &sf_changed.id, &install).unwrap_err();
        assert_eq!(e.code, kiln_manage::ErrorCode::Unsupported, "{e:?}");
        // and the other way round: the active profile has the package off, so ShroudForge's folder has no switched-on
        // copy of it; a single start that needs it there is refused, not started without it
        profile::activate(&s.dirs, &off.id, &install).unwrap();
        let e = profile::write_once(&s.dirs, &p.id, &install).unwrap_err();
        assert_eq!(e.code, kiln_manage::ErrorCode::Unsupported, "{e:?}");
    }
}

#[test]
fn patch_only_eml_and_disabled_runtime_eml_keep_standalone_loader_behavior() {
    let s = setup("runtime-refuse-controls");
    let mut p = runtime_eml(&s, "runtime");
    p.mods.eml[0].enabled = false;
    let patch = import(&s.dirs, &legacy(&s.t.0, "more-gliders", "0.2.2"), ModKind::Eml, "more-gliders");
    p.mods.eml.push(pm(&patch, true));
    let p = profile::save(&s.dirs, &p).unwrap();
    eml_loader(&s.game);
    profile::activate(&s.dirs, &p.id, &runtime_install(&s.game)).unwrap();
    assert!(s.game.join("mods/more-gliders/src/mod.lua").is_file());
    assert!(!s.game.join("mods/quiver").exists());
    let file = profile::read_game_profile(&s.game).unwrap().unwrap();
    assert!(file.eml_runtime.is_empty());
    assert!(file.mods.eml.iter().any(|m| m.id == "more-gliders" && m.enabled));
    assert!(file.mods.shroudforge.is_empty());
    assert!(ext_of(&s.game.join("mods/more-gliders")).is_none());
}

/// An EML package that is an XHL-Core plugin too (the XHL family, here XHL-Area-Harvest 2.1.0 as its author ships it:
/// mod.json and Lua beside the plugin DLL, XHL-Core's native-plugin.ini and config.txt), with ShroudForge's file `ext`
/// shipped. Its Library record names the game folder XHL-Core reads it from: `<game>/mods/XHL-Area-Harvest`.
fn xhl_hybrid(s: &Setup, ext: &str) -> LibraryRef {
    let d = s.t.0.join("pkg/XHL-Area-Harvest");
    put(&d, "mod.json", br#"{"id":"xhl-area-harvest","name":"XHL-Planting Assistant","version":"2.1.0","capabilities":["export"]}"#);
    put(&d, "src/mod.lua", b"return {}");
    put(&d, "XHL-Area-Harvest.dll", &build_test_dll(&["XhlPluginEntry"], b"XHL plugin"));
    put(&d, "native-plugin.ini", b"[Plugin]\r\nEnabled=1\r\nDll=XHL-Area-Harvest.dll\r\n\r\n[Planting]\r\nMode=Row\r\nSquareSize=5\r\n");
    put(&d, "config.txt", b"# XHL Planting Assistant 2.1.0\nenabled=true\n");
    put(&d, "extended.mod.json", ext.as_bytes());
    import(&s.dirs, &d, ModKind::Eml, "xhl-area-harvest")
}

/// inbox 21 (b): an EML package that is an XHL-Core plugin too, holding ShroudForge values the player set (ShroudForge's
/// menu), was kept when the profile stopped it and only switched off in ShroudForge's file. XHL-Core does not read that
/// switch: it loads every plugin folder in mods that has a native-plugin.ini, so the switched-off mod kept running. The
/// folder now goes as any XHL plugin's does, the values wait with its other settings and come back with the mod.
#[test]
fn an_xhl_plugin_eml_package_holding_shroudforge_values_goes_when_it_stops_and_its_values_come_back() {
    for gone in [false, true] {
        let s = setup(&format!("xhl-sf-values-{gone}"));
        let harvest = xhl_hybrid(&s, SHIPPED);
        let mut p = s.profile(vec![pm(&harvest, true)]);
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/XHL-Area-Harvest");
        assert!(eml(&rep.placed) == ["eml:xhl-area-harvest"] && shroudforge::read_mod(&copy).unwrap().enabled, "{rep:?}");
        // in the game: a value set in ShroudForge's menu, and the plugin's own setting
        shroudforge::set_setting(&copy, "radius", &serde_json::json!(9)).unwrap();
        put(&copy, "config.txt", b"enabled=false\n");
        if gone { p.mods.eml.clear() } else { p.mods.eml[0].enabled = false }
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(!copy.exists(), "gone {gone}: XHL-Core still loads the plugin from {}: {rep:?}", copy.display());
        assert!(eml(&rep.removed) == ["eml:xhl-area-harvest"] && rep.retained.is_empty() && rep.changed.is_empty(), "gone {gone}: {rep:?}");
        assert!(rep.kept_files.iter().any(|k| k.starts_with("eml:xhl-area-harvest") && k.contains("extended.mod.json")), "gone {gone}: {rep:?}");
        // back in the profile: the player's values come back with it, switched on in ShroudForge's file
        p.mods.eml = vec![pm(&harvest, true)];
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(eml(&rep.placed) == ["eml:xhl-area-harvest"] && rep.retained.is_empty() && rep.warnings.is_empty(), "gone {gone}: {rep:?}");
        let m = shroudforge::read_mod(&copy).unwrap();
        assert_eq!((m.enabled, &m.settings[0].value), (true, &serde_json::json!(9)), "gone {gone}");
        assert_eq!(std::fs::read(copy.join("config.txt")).unwrap(), b"enabled=false\n", "gone {gone}");
        assert!(!s.game.join("kiln/state/placed-settings/eml/xhl-area-harvest").exists(), "gone {gone}: what went back leaves the stash");
        // they are still the player's: stopping it again sets them aside again, nothing is lost
        p.mods.eml[0].enabled = false;
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(!copy.exists() && rep.retained.is_empty(), "gone {gone}: {rep:?}");
        p.mods.eml[0].enabled = true;
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert_eq!(shroudforge::read_mod(&copy).unwrap().settings[0].value, serde_json::json!(9), "gone {gone}");
    }
}

/// A ShroudForge package that is an XHL-Core plugin too (Building Tools for ShroudForge: BuildingTools.dll with XHL-Core's
/// native-plugin.ini). Switched off in the profile it is switched off in ShroudForge's file and its folder stays, as for
/// every ShroudForge mod; XHL-Core reads no such switch, so its ini is set aside too (inbox 86), and nothing is left
/// that may still run. Where the game has XHL-Core the result says what Kiln did, once; without it there is nothing to
/// say. On again (or back in the profile), the ini is back.
#[test]
fn a_switched_off_shroudforge_copy_xhl_core_loads_is_named() {
    for xhl in [true, false] {
        let s = setup(&format!("sf-xhl-{xhl}"));
        let tools = building_tools(&s, "native-plugin.ini");
        if xhl {
            xhl_core(&s, XhlCore::ByHand);
            assert!(engine_mods::xhl_host(&s.game).available);
        }
        let mut p = s.profile(vec![]);
        p.mods.shroudforge.push(pm(&tools, true));
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/building-tools");
        assert!(shroudforge::read_mod(&copy).unwrap().enabled && xhl_core_reads(&copy).is_some());
        for gone in [false, true] {
            if gone { p.mods.shroudforge.retain(|m| m.id != "building-tools") } else { p.mods.shroudforge[1].enabled = false }
            let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
            assert!(!shroudforge::read_mod(&copy).unwrap().enabled, "xhl {xhl}, gone {gone}: {rep:?}");
            assert!(xhl_core_reads(&copy).is_none() && rep.retained.is_empty() && rep.changed.is_empty(), "xhl {xhl}, gone {gone}: {rep:?}");
            let said: Vec<&String> = rep.warnings.iter().filter(|w| w.starts_with("shroudforge:building-tools:")).collect();
            assert_eq!(said.len(), usize::from(xhl), "xhl {xhl}, gone {gone}: {rep:?}");
            if xhl {
                let off = if gone { "it is not in the profile" } else { "it is off in the profile" };
                assert!(said[0].contains(off) && said[0].contains("native-plugin.ini.kiln-off"), "gone {gone}: {said:?}");
            }
            p.mods.shroudforge = vec![pm(&s.sf, true), pm(&tools, true)];
            let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
            assert!(shroudforge::read_mod(&copy).unwrap().enabled && rep.warnings.is_empty() && rep.retained.is_empty(), "xhl {xhl}, gone {gone}: {rep:?}");
            assert_eq!(xhl_core_reads(&copy).as_deref(), Some(BT_INI), "xhl {xhl}, gone {gone}");
        }
    }
}

/// XHL-Core 2.1.0's DLL: the markers xhl_host looks for (its log's name as Windows' wide text, as 2.x has it).
fn xhl_core_dll() -> Vec<u8> {
    let mut text = b"dinput8-chain.ini\0native-plugin.ini\0".to_vec();
    for c in "XHL-Native-Plugin-Host.log".encode_utf16() { text.extend_from_slice(&c.to_le_bytes()); }
    build_test_dll(&["DirectInput8Create"], &text)
}

/// How a test game has XHL-Core: none, the game's dinput8.dll (its author's instructions), or a Native mod of the
/// profile (Kiln chain-loads its copy, kiln/chain/xhl-core/xhl-core.dll, as in the player's report of inbox 86).
#[derive(Clone, Copy, Debug, PartialEq)]
enum XhlCore { Absent, ByHand, InProfile }

/// XHL-Core installed `how` in `s.game`: the profile entry to add when it is a Native mod of the profile.
fn xhl_core(s: &Setup, how: XhlCore) -> Option<ProfileMod> {
    match how {
        XhlCore::Absent => None,
        XhlCore::ByHand => { put(&s.game, "dinput8.dll", &xhl_core_dll()); None }
        XhlCore::InProfile => {
            put(&s.t.0, "XHL-Core/dinput8.dll", &xhl_core_dll());
            let r = library::import(&s.dirs, &[s.t.0.join("XHL-Core/dinput8.dll")], &mut NoProgress).unwrap();
            let core = r.imported.iter().find(|x| x.kind == ModKind::Native).cloned().unwrap_or_else(|| panic!("XHL-Core not imported: {r:?}"));
            Some(pm(&core, true))
        }
    }
}

/// Building Tools for ShroudForge as a Library ShroudForge mod: BuildingTools.dll with XHL-Core's ini, spelled `ini`, as
/// its package has them (no Lua: ShroudForge's DLL manager loads the DLL that ini names).
fn building_tools(s: &Setup, ini: &str) -> LibraryRef {
    building_tools_version(s, ini, "0.33.2")
}

fn building_tools_version(s: &Setup, ini: &str, version: &str) -> LibraryRef {
    let d = s.t.0.join(format!("src/building-tools-{version}"));
    put(&d, "mod.json", format!(r#"{{"id":"building-tools","name":"Building Tools","version":"{version}","capabilities":[]}}"#).as_bytes());
    put(&d, "extended.mod.json", br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"],"links":{"website":"https://www.nexusmods.com/enshrouded/mods/221"}}"#);
    put(&d, "BuildingTools.dll", &build_test_dll(&["XhlPluginEntry"], format!("plugin {version}").as_bytes()));
    put(&d, ini, BT_INI);
    import(&s.dirs, &d, ModKind::ShroudForge, "building-tools")
}
const BT_INI: &[u8] = b"[Plugin]\r\nEnabled=1\r\nDll=BuildingTools.dll\r\n";

/// The file XHL-Core 2.1.0 opens in a folder of mods: `<folder>\native-plugin.ini`, by that name (Windows and Wine
/// match it in any case). None: XHL-Core finds no plugin there and skips the folder.
fn xhl_core_reads(folder: &Path) -> Option<Vec<u8>> {
    std::fs::read_dir(folder).ok()?.flatten()
        .find(|e| e.file_name().to_str().is_some_and(|n| n.eq_ignore_ascii_case("native-plugin.ini")))
        .map(|e| std::fs::read(e.path()).unwrap())
}

/// inbox 86: two profiles, "Building" with Building Tools for ShroudForge (a ShroudForge mod whose folder is an
/// XHL-Core plugin too), "Default" with it off or without it, and XHL-Core installed. After a session in "Building",
/// "Default" left Kiln's copy in mods/ switched off only in ShroudForge's file: XHL-Core reads no profile and no
/// ShroudForge switch, found the copy's native-plugin.ini and loaded BuildingTools.dll without its ShroudForge side
/// (every start of "Default" crashed while the mods loaded, until XHL-Core was switched off). While the mod is off
/// Kiln now sets that ini aside in Kiln's own copy, so XHL-Core skips the folder, and puts it back as it was (the
/// values the mod or the player wrote into it too) when the mod is on again.
#[test]
fn inbox86_a_switched_off_shroudforge_copy_is_no_xhl_plugin_while_the_mod_is_off() {
    use XhlCore::*;
    for (xhl, gone, ini, edited) in [(InProfile, true, "native-plugin.ini", false), (ByHand, false, "native-plugin.ini", false),
                                     (InProfile, false, "Native-Plugin.INI", false), (InProfile, true, "native-plugin.ini", true),
                                     (ByHand, false, "native-plugin.ini", true), (Absent, false, "native-plugin.ini", false),
                                     (Absent, true, "native-plugin.ini", true)] {
        let case = format!("XHL-Core {xhl:?}, gone {gone}, {ini}, edited {edited}");
        let s = setup(&format!("inbox86-{xhl:?}-{gone}-{ini}-{edited}"));
        let core = xhl_core(&s, xhl);
        let tools = building_tools(&s, ini);
        let install = runtime_install(&s.game);
        let mut building = profile::create(&s.dirs, "Building", None).unwrap();
        building.mods.shroudforge = vec![pm(&s.sf, true), pm(&tools, true)];
        building.mods.native = core.clone().into_iter().collect();
        let building = profile::save(&s.dirs, &building).unwrap();
        // "Default": the player's usual mods, without Building Tools (or with it switched off)
        let mut default = profile::create(&s.dirs, "Default", None).unwrap();
        default.mods.shroudforge = if gone { vec![pm(&s.sf, true)] } else { vec![pm(&s.sf, true), pm(&tools, false)] };
        default.mods.native = core.into_iter().collect();
        let default = profile::save(&s.dirs, &default).unwrap();

        let a = profile::activate(&s.dirs, &building.id, &install).unwrap();
        assert_eq!(engine_mods::xhl_host(&s.game).available, xhl != Absent, "{case}");
        let copy = s.game.join("mods/building-tools");
        assert!(shroudforge::read_mod(&copy).unwrap().enabled && a.retained.is_empty(), "{case}: {a:?}");
        assert_eq!(xhl_core_reads(&copy).as_deref(), Some(BT_INI), "{case}");
        // a session: the plugin keeps its settings in its ini (as the XHL plugins do), still naming its own DLL
        let mut ini_now = BT_INI.to_vec();
        if edited {
            ini_now.extend_from_slice(b"\r\n[BuildingTools]\r\nSnapKey=71\r\n");
            std::fs::write(copy.join(ini), &ini_now).unwrap();
        }

        let a = profile::activate(&s.dirs, &default.id, &install).unwrap();
        assert!(!shroudforge::read_mod(&copy).unwrap().enabled, "{case}: {a:?}");
        assert_eq!(xhl_core_reads(&copy), None, "{case}: XHL-Core still loads BuildingTools.dll from {}: {a:?}", copy.display());
        assert!(a.retained.is_empty() && a.safety_notes.is_empty(), "{case}: nothing may still run, nothing to warn about: {a:?}");
        assert!(copy.join("BuildingTools.dll").is_file(), "{case}: the copy stays, as every ShroudForge copy does");
        assert_eq!(std::fs::read(copy.join(format!("{ini}.kiln-off"))).unwrap(), ini_now, "{case}: set aside as it was");
        // said once, in plain words, where the game has XHL-Core; nothing to say without it
        let said: Vec<&String> = a.warnings.iter().filter(|w| w.starts_with("shroudforge:building-tools:")).collect();
        if xhl == Absent {
            assert!(said.is_empty(), "{case}: {said:?}");
        } else {
            assert!(said.len() == 1 && said[0].contains(&format!("renamed its {ini}")) && said[0].contains(&format!("to {ini}.kiln-off"))
                && said[0].contains("XHL-Core does not load it"), "{case}: {said:?}");
        }
        // again (Play's reconcile): nothing to do, nothing to say
        let a = profile::activate(&s.dirs, &default.id, &install).unwrap();
        assert!(a.retained.is_empty() && a.kept_files.is_empty() && xhl_core_reads(&copy).is_none(), "{case}: {a:?}");
        assert!(!a.warnings.iter().any(|w| w.contains("building-tools")), "{case}: {a:?}");

        // on again: the ini is back where XHL-Core reads it, as it was
        let a = profile::activate(&s.dirs, &building.id, &install).unwrap();
        assert!(shroudforge::read_mod(&copy).unwrap().enabled && a.retained.is_empty(), "{case}: {a:?}");
        assert_eq!(xhl_core_reads(&copy), Some(ini_now.clone()), "{case}");
        assert!(copy.join(ini).is_file(), "{case}: under its own name");
        let names: Vec<String> = std::fs::read_dir(&copy).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        assert!(!names.iter().any(|n| n.to_ascii_lowercase().contains("kiln-off")), "{case}: {names:?}");
    }
}

/// inbox 86, a new version while the mod is off: the copy it replaces has its ini back for a moment, so the values the
/// plugin wrote there go over to the new version (as for a game-folder copy), and the new copy has it set aside again.
/// On, the new version's copy has the values in its ini. An ini the plugin never changed is the new version's own.
#[test]
fn inbox86_a_new_version_of_a_switched_off_xhl_plugin_keeps_its_ini_aside_and_its_values() {
    for edited in [false, true] {
        let s = setup(&format!("inbox86-update-{edited}"));
        xhl_core(&s, XhlCore::ByHand);
        let old = building_tools_version(&s, "native-plugin.ini", "0.33.2");
        let new = building_tools_version(&s, "native-plugin.ini", "0.34.0");
        let mut p = s.profile(vec![]);
        p.mods.shroudforge.push(pm(&old, true));
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/building-tools");
        let mut ini_now = BT_INI.to_vec();
        if edited {
            ini_now.extend_from_slice(b"\r\n[BuildingTools]\r\nSnapKey=71\r\n");
            std::fs::write(copy.join("native-plugin.ini"), &ini_now).unwrap();
        }
        p.mods.shroudforge[1].enabled = false;
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(xhl_core_reads(&copy).is_none() && rep.retained.is_empty(), "edited {edited}: {rep:?}");
        // the update, still off
        p.mods.shroudforge[1] = pm(&new, false);
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(rep.placed.contains(&"shroudforge:building-tools".to_string()) && rep.retained.is_empty(), "edited {edited}: {rep:?}");
        assert_eq!(engine_mods::marker(&copy).unwrap().version, "0.34.0");
        assert!(!shroudforge::read_mod(&copy).unwrap().enabled && xhl_core_reads(&copy).is_none(), "edited {edited}: {rep:?}");
        assert_eq!(std::fs::read(copy.join("native-plugin.ini.kiln-off")).unwrap(), ini_now, "edited {edited}");
        assert!(!s.game.join("kiln/kept-mod-files/shroudforge/building-tools").exists(), "edited {edited}: nothing set aside elsewhere");
        // on: the new version runs, with the plugin's values
        p.mods.shroudforge[1].enabled = true;
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(rep.kept.contains(&"shroudforge:building-tools".to_string()) && rep.retained.is_empty(), "edited {edited}: {rep:?}");
        assert!(shroudforge::read_mod(&copy).unwrap().enabled, "edited {edited}");
        assert_eq!(xhl_core_reads(&copy), Some(ini_now.clone()), "edited {edited}");
        assert_eq!(std::fs::read(copy.join("BuildingTools.dll")).unwrap(), std::fs::read(library::path_of(&s.dirs, &new).unwrap().join("BuildingTools.dll")).unwrap());
        // and a new version while it is on: the values go over too (an ini that names a DLL the new version has)
        let newer = building_tools_version(&s, "native-plugin.ini", "0.35.0");
        p.mods.shroudforge[1] = pm(&newer, true);
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(rep.placed.contains(&"shroudforge:building-tools".to_string()) && rep.retained.is_empty(), "edited {edited}: {rep:?}");
        assert_eq!(xhl_core_reads(&copy), Some(ini_now.clone()), "edited {edited}");
    }
}

/// inbox 86, Kiln's copy when Kiln stops managing the game (Deactivate, uninstall): one with nothing of the player's goes
/// whole, its set-aside ini with it; one with the plugin's values in its ini stays, its ini still set aside (the mod was
/// off), and the result says how to get it back.
#[test]
fn inbox86_deactivating_with_a_switched_off_xhl_plugin_leaves_nothing_that_loads() {
    for edited in [false, true] {
        let s = setup(&format!("inbox86-unplace-{edited}"));
        xhl_core(&s, XhlCore::ByHand);
        let tools = building_tools(&s, "native-plugin.ini");
        let mut p = s.profile(vec![]);
        p.mods.shroudforge.push(pm(&tools, true));
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/building-tools");
        if edited { std::fs::write(copy.join("native-plugin.ini"), [BT_INI, b"[BuildingTools]\r\nSnapKey=71\r\n"].concat()).unwrap(); }
        p.mods.shroudforge[1].enabled = false;
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let cleanup = engine_mods::unplace(&s.dirs, &s.game).unwrap();
        if edited {
            let kept = cleanup.retained.iter().find(|r| r.path == copy).unwrap_or_else(|| panic!("{cleanup:?}"));
            assert!(kept.reason.contains("native-plugin.ini is renamed native-plugin.ini.kiln-off") && kept.reason.contains("rename it back"), "{kept:?}");
            assert!(xhl_core_reads(&copy).is_none() && copy.join("native-plugin.ini.kiln-off").is_file());
        } else {
            assert!(!copy.exists() && cleanup.removed.contains(&copy) && cleanup.retained.is_empty(), "{cleanup:?}");
        }
    }
}

/// inbox 86 (b): the profile names a version whose Library entry is gone (the player's "Building" named Building Tools
/// 0.33.2 after its Library folder was deleted), while Kiln's copy of another version stays in the game. That copy is
/// still Kiln's by its marker and receipt: it follows the profile's switch all the same. Off: switched off in
/// ShroudForge's file and its ini set aside; on: it runs as it is, so its ini is back only while ShroudForge runs it
/// (a copy that was switched off before stays so, and XHL-Core must not load its DLL alone). The result says so.
#[test]
fn inbox86_a_copy_whose_profile_version_is_gone_from_the_library_follows_the_profile() {
    let s = setup("inbox86-gone");
    xhl_core(&s, XhlCore::ByHand);
    let gone = building_tools_version(&s, "native-plugin.ini", "0.33.2");
    let here = building_tools_version(&s, "native-plugin.ini", "0.34.0");
    let mut p = s.profile(vec![]);
    p.mods.shroudforge.push(pm(&here, true));
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/building-tools");
    assert!(shroudforge::read_mod(&copy).unwrap().enabled && xhl_core_reads(&copy).is_some());
    std::fs::remove_dir_all(library::path_of(&s.dirs, &gone).unwrap()).unwrap();
    for on in [false, true] {
        p.mods.shroudforge[1] = pm(&gone, on);
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let said = rep.warnings.iter().find(|w| w.contains("version 0.33.2 is not placed")).unwrap_or_else(|| panic!("on {on}: {rep:?}"));
        assert!(said.contains("Kiln's copy of version 0.34.0 stays in") && said.contains(if on { "as it is" } else { "switched off" }), "on {on}: {said}");
        assert_eq!(engine_mods::marker(&copy).unwrap().version, "0.34.0", "on {on}: the copy stays");
        // on: the earlier copy was switched off above and stays so, ShroudForge does not run it, and XHL-Core does
        // not load BuildingTools.dll without it
        assert!(!shroudforge::read_mod(&copy).unwrap().enabled && xhl_core_reads(&copy).is_none() && rep.retained.is_empty(), "on {on}: {rep:?}");
        assert_eq!(rep.switched.contains(&"shroudforge:building-tools".to_string()), !on, "on {on}: {rep:?}");
    }
    // a copy switched on that stays while the profile's version is gone runs as it is, its ini where both read it
    p.mods.shroudforge[1] = pm(&here, true);
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    p.mods.shroudforge[1] = pm(&gone, true);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(shroudforge::read_mod(&copy).unwrap().enabled && rep.retained.is_empty(), "{rep:?}");
    assert_eq!(xhl_core_reads(&copy).as_deref(), Some(BT_INI), "{rep:?}");
    // the copy's own version gone from the Library too: the marker and receipt are enough to switch it off
    p.mods.shroudforge[1] = pm(&here, true);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(xhl_core_reads(&copy).is_some(), "{rep:?}");
    std::fs::remove_dir_all(library::path_of(&s.dirs, &here).unwrap()).unwrap();
    p.mods.shroudforge[1] = pm(&here, false);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(!shroudforge::read_mod(&copy).unwrap().enabled && xhl_core_reads(&copy).is_none() && rep.retained.is_empty(), "{rep:?}");
    assert!(rep.kept.contains(&"shroudforge:building-tools".to_string()), "{rep:?}");
}

/// inbox 86, a new copy whose placement failed after its receipt was written (the files it took back from the kept
/// folder could not be tidied there): it is Kiln's copy, in place, and follows the profile all the same. Off, its ini is
/// set aside, or XHL-Core would load its DLL at the next start (a Steam start does not reconcile first) while ShroudForge
/// does not run it.
#[test]
fn inbox86_a_copy_whose_placement_failed_late_has_its_ini_aside_while_off() {
    use std::os::unix::fs::PermissionsExt;
    let s = setup("inbox86-late");
    xhl_core(&s, XhlCore::ByHand);
    let tools = building_tools(&s, "native-plugin.ini");
    // what the mod wrote into an earlier copy, kept when that copy went, in a folder Kiln cannot tidy now
    let kept = s.game.join("kiln/kept-mod-files/shroudforge/building-tools");
    put(&kept, "notes.txt", b"the mod's notes");
    let mode = |m: u32| std::fs::set_permissions(&kept, std::fs::Permissions::from_mode(m)).unwrap();
    mode(0o555);
    let mut p = s.profile(vec![]);
    p.mods.shroudforge.push(pm(&tools, false));
    let rep = engine_mods::place(&s.dirs, &p, &s.game);
    mode(0o755);
    let rep = rep.unwrap();
    let copy = s.game.join("mods/building-tools");
    assert!(rep.retained.iter().any(|r| r.path == copy), "the placement failed late: {rep:?}");
    assert_eq!(engine_mods::marker(&copy).map(|m| m.version).as_deref(), Some("0.33.2"), "{rep:?}");
    assert!(!shroudforge::read_mod(&copy).unwrap().enabled && copy.join("notes.txt").is_file(), "{rep:?}");
    assert!(xhl_core_reads(&copy).is_none(), "XHL-Core would load BuildingTools.dll while ShroudForge does not run it: {rep:?}");
    assert_eq!(std::fs::read(copy.join("native-plugin.ini.kiln-off")).unwrap(), BT_INI);
}

/// inbox 86, a rename that cannot be done (here a folder of the player's under the set-aside name, which a rename never
/// replaces): where Kiln finds XHL-Core, the copy is one that may still run; where it finds none, XHL-Core may be there
/// all the same (XHL's Kiln build, a Wine drive Kiln cannot map), so a plain note says so. Nothing is renamed over.
#[test]
fn inbox86_a_rename_that_fails_is_said_with_or_without_xhl_core() {
    for xhl in [XhlCore::ByHand, XhlCore::Absent] {
        let s = setup(&format!("inbox86-fails-{xhl:?}"));
        xhl_core(&s, xhl);
        let tools = building_tools(&s, "native-plugin.ini");
        let mut p = s.profile(vec![]);
        p.mods.shroudforge.push(pm(&tools, true));
        engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        let copy = s.game.join("mods/building-tools");
        std::fs::create_dir(copy.join("native-plugin.ini.kiln-off")).unwrap();
        p.mods.shroudforge[1].enabled = false;
        let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
        assert!(!shroudforge::read_mod(&copy).unwrap().enabled, "{xhl:?}: {rep:?}");
        assert_eq!(xhl_core_reads(&copy).as_deref(), Some(BT_INI), "{xhl:?}: nothing is renamed over the folder");
        assert!(copy.join("native-plugin.ini.kiln-off").is_dir());
        if xhl == XhlCore::Absent {
            let said = rep.warnings.iter().find(|w| w.starts_with("shroudforge:building-tools:")).unwrap_or_else(|| panic!("{rep:?}"));
            assert!(said.contains("could not rename its native-plugin.ini") && said.contains("If the game has XHL-Core"), "{said}");
            assert!(rep.retained.is_empty(), "{rep:?}");
        } else {
            let kept = rep.retained.iter().find(|r| r.path == copy).unwrap_or_else(|| panic!("{rep:?}"));
            assert!(kept.may_run && kept.off && kept.reason.contains("could not rename"), "{kept:?}");
        }
    }
}

/// inbox 86, an update while the mod is on whose new version spells its ini otherwise (Native-Plugin.ini): the copy never
/// gets a second ini beside it (a Linux folder holds both, and Wine's XHL-Core would read one of them by chance). The new
/// version has its own; the values the plugin wrote wait in the kept folder, and the result says so. Off, the one ini
/// is set aside.
#[test]
fn inbox86_an_update_that_spells_its_ini_otherwise_never_leaves_two() {
    let s = setup("inbox86-respelt");
    xhl_core(&s, XhlCore::ByHand);
    let old = building_tools_version(&s, "native-plugin.ini", "0.33.2");
    let new = building_tools_version(&s, "Native-Plugin.ini", "0.34.0");
    let mut p = s.profile(vec![]);
    p.mods.shroudforge.push(pm(&old, true));
    engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    let copy = s.game.join("mods/building-tools");
    let edited = [BT_INI, b"\r\n[BuildingTools]\r\nSnapKey=71\r\n"].concat();
    std::fs::write(copy.join("native-plugin.ini"), &edited).unwrap();
    p.mods.shroudforge[1] = pm(&new, true);
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(rep.placed.contains(&"shroudforge:building-tools".to_string()) && rep.retained.is_empty(), "{rep:?}");
    let inis: Vec<String> = std::fs::read_dir(&copy).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.to_ascii_lowercase().starts_with("native-plugin.ini")).collect();
    assert_eq!(inis, ["Native-Plugin.ini"], "{rep:?}");
    assert_eq!(std::fs::read(copy.join("Native-Plugin.ini")).unwrap(), BT_INI);
    assert_eq!(std::fs::read(s.game.join("kiln/kept-mod-files/shroudforge/building-tools/native-plugin.ini")).unwrap(), edited);
    assert!(rep.kept_files.iter().any(|k| k.contains("native-plugin.ini") && k.contains("ships its own file")), "{rep:?}");
    p.mods.shroudforge[1].enabled = false;
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    assert!(xhl_core_reads(&copy).is_none() && copy.join("Native-Plugin.ini.kiln-off").is_file() && rep.retained.is_empty(), "{rep:?}");
}

/// Every file under `root` whose name has the set-aside suffix (any case), relative to `root` with '/'.
fn set_aside_under(root: &Path) -> Vec<String> {
    fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            let path = e.path();
            if e.file_type().unwrap().is_dir() { walk(root, &path, out); }
            if e.file_name().to_string_lossy().to_ascii_lowercase().ends_with(".kiln-off") {
                out.push(path.strip_prefix(root).unwrap().to_string_lossy().into_owned());
            }
        }
    }
    let mut out = Vec::new();
    walk(root, root, &mut out);
    out.sort();
    out
}

/// inbox 86: what the rename may touch. Only Kiln's own copy of a ShroudForge mod in ShroudForge's mods folder, while
/// ShroudForge does not run it. Never an XHL companion of the profile while it is on (Kiln's copy in mods/; off, it goes
/// whole, as before), the player's own XHL-Core plugin folder (XHL's XHL-Area-Harvest, whose ini the Kiln port of Area
/// Harvest reads its first values from), or a Kiln mod (kiln.mod.json, as XHL's Kiln builds are) with a
/// native-plugin.ini, in the Library or in the game's kiln/mods.
#[test]
fn inbox86_only_kilns_shroudforge_copy_ever_has_its_ini_renamed() {
    let s = setup("inbox86-only-sf");
    let core = xhl_core(&s, XhlCore::InProfile);
    let tools = building_tools(&s, "native-plugin.ini");
    let climb_ini: &[u8] = b"[Plugin]\r\nEnabled=1\r\nDll=XHL-Free-Climb.dll\r\n\r\n[Climbing]\r\nHoldKeyVk=72\r\n";
    let d = s.t.0.join("src/XHL-Free-Climb");
    put(&d, "mod.json", br#"{"id":"xhl-free-climb","name":"XHL Free Climbing","version":"2.1.0","capabilities":[]}"#);
    put(&d, "XHL-Free-Climb.dll", &build_test_dll(&["XhlPluginEntry"], b"XHL plugin"));
    put(&d, "native-plugin.ini", climb_ini);
    let climb = import(&s.dirs, &d, ModKind::Xhl, "xhl-free-climb");
    let kiln_ini: &[u8] = b"[Planting]\r\nMode=Square\r\nSquareSize=7\r\n";
    let k = s.t.0.join("src/xhl-kiln-area-harvest");
    put(&k, "kiln.mod.json", br#"{ "id": "xhl-kiln-area-harvest", "version": "0.1.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }"#);
    put(&k, "M.dll", b"a Kiln mod");
    put(&k, "native-plugin.ini", kiln_ini);
    let harvest = import(&s.dirs, &k, ModKind::Kiln, "xhl-kiln-area-harvest");
    // the player's own: XHL's Area Harvest in mods/, a Kiln mod unzipped into kiln/mods/
    let own_ini: &[u8] = b"[Plugin]\r\nEnabled=1\r\nDll=XHL-Area-Harvest.dll\r\n[Planting]\r\nMode=Square\r\n";
    put(&s.game, "mods/XHL-Area-Harvest/native-plugin.ini", own_ini);
    put(&s.game, "mods/XHL-Area-Harvest/XHL-Area-Harvest.dll", &build_test_dll(&["XhlPluginEntry"], b"harvest"));
    put(&s.game, "kiln/mods/xhl-kiln-vein-mining/kiln.mod.json",
        br#"{ "id": "xhl-kiln-vein-mining", "version": "0.1.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }"#);
    put(&s.game, "kiln/mods/xhl-kiln-vein-mining/native-plugin.ini", kiln_ini);
    let install = runtime_install(&s.game);
    let mut building = profile::create(&s.dirs, "Building", None).unwrap();
    building.mods.shroudforge = vec![pm(&s.sf, true), pm(&tools, true)];
    building.mods.native = core.into_iter().collect();
    building.mods.xhl = vec![pm(&climb, true)];
    building.mods.kiln = vec![pm(&harvest, true)];
    let building = profile::save(&s.dirs, &building).unwrap();
    let mut default = profile::create(&s.dirs, "Default", None).unwrap();
    default.mods = building.mods.clone();
    default.mods.shroudforge[1].enabled = false;
    let default = profile::save(&s.dirs, &default).unwrap();
    let untouched = |case: &str| {
        assert_eq!(std::fs::read(s.game.join("mods/XHL-Area-Harvest/native-plugin.ini")).unwrap(), own_ini, "{case}");
        assert_eq!(std::fs::read(s.game.join("kiln/mods/xhl-kiln-vein-mining/native-plugin.ini")).unwrap(), kiln_ini, "{case}");
        assert_eq!(std::fs::read(library::path_of(&s.dirs, &harvest).unwrap().join("native-plugin.ini")).unwrap(), kiln_ini, "{case}");
        assert_eq!(std::fs::read(library::path_of(&s.dirs, &climb).unwrap().join("native-plugin.ini")).unwrap(), climb_ini, "{case}");
    };
    let copy = s.game.join("mods/building-tools");
    let a = profile::activate(&s.dirs, &building.id, &install).unwrap();
    assert!(xhl_core_reads(&copy).is_some() && set_aside_under(&s.t.0).is_empty(), "{a:?}");
    untouched("Building");
    let a = profile::activate(&s.dirs, &default.id, &install).unwrap();
    assert_eq!(set_aside_under(&s.t.0), ["game/mods/building-tools/native-plugin.ini.kiln-off"], "{a:?}");
    assert_eq!(std::fs::read(s.game.join("mods/XHL-Free-Climb/native-plugin.ini")).unwrap(), climb_ini, "the companion is on: {a:?}");
    untouched("Default");
    // the companion off too: its copy goes whole, as before (nothing of it renamed), and the Kiln mod off changes nothing
    let mut default = profile::get(&s.dirs, &default.id).unwrap();
    default.mods.xhl[0].enabled = false;
    default.mods.kiln[0].enabled = false;
    let default = profile::save(&s.dirs, &default).unwrap();
    let a = profile::activate(&s.dirs, &default.id, &install).unwrap();
    assert!(!s.game.join("mods/XHL-Free-Climb").exists(), "{a:?}");
    assert_eq!(set_aside_under(&s.t.0), ["game/mods/building-tools/native-plugin.ini.kiln-off"], "{a:?}");
    untouched("Default, the companion and the Kiln mod off");
    // and back: every ini where its reader looks, nothing set aside anywhere
    let a = profile::activate(&s.dirs, &building.id, &install).unwrap();
    assert!(set_aside_under(&s.t.0).is_empty() && xhl_core_reads(&copy).is_some(), "{a:?}");
    assert_eq!(std::fs::read(s.game.join("mods/XHL-Free-Climb/native-plugin.ini")).unwrap(), climb_ini, "{a:?}");
    untouched("Building again");
}

#[test]
fn runtime_eml_environment_note() {
    const CHILD: &str = "KILN_TEST_SFOFF_NOTE_CHILD";
    if std::env::var_os(CHILD).is_none() {
        for value in ["0", "1", "other", ""] {
            let output = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "runtime_eml_environment_note", "--nocapture"])
                .env(CHILD, "1").env("KILN_SHROUDFORGE", value).output().unwrap();
            assert!(output.status.success(), "value={value:?}: {}{}",
                String::from_utf8_lossy(&output.stdout), String::from_utf8_lossy(&output.stderr));
        }
        return;
    }
    let s = setup("runtime-env-note");
    let p = runtime_eml(&s, "runtime");
    let rendered = profile::render(&s.dirs, &p, &runtime_install(&s.game), kiln_manage::game::PathMode::Native).unwrap();
    let rep = engine_mods::place(&s.dirs, &p, &s.game).unwrap();
    if std::env::var("KILN_SHROUDFORGE").unwrap() == "0" {
        let note = &rendered.eml_runtime_blocked["quiver"];
        for text in ["KILN_SHROUDFORGE=0 environment variable switches off", "Remove that variable (do not set it to 0)",
                     "restart Steam and the Kiln launcher", "start the game again"] {
            assert!(note.contains(text), "{note}");
        }
        assert!(!note.contains("Install"), "{note}");
        assert!(rep.warnings.iter().any(|w| w == &format!("eml:quiver: {note}")), "{rep:?}");
        assert!(!rendered.mods.eml[0].enabled);
        assert!(!s.game.join("mods/quiver").exists());
    } else {
        assert!(rendered.eml_runtime_blocked.is_empty());
        assert!(rep.warnings.is_empty(), "{rep:?}");
    }
}
