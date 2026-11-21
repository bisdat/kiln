// SPDX-License-Identifier: MIT
//! A kiln-mode profile beside the other loaders the player uses: Kiln mods, native mods (a plugin
//! loader in the game folder, XHL-Core chained with a ShroudForge package that is an XHL-Core plugin too), EML, EMF,
//! Greeneye, Flame and SET mods in one profile whose kiln.json selects Kiln's interpreter.
//!
//! - **One backend for the whole profile**: one plan, Kiln's interpreter selected with kiln-eml as its data writer, the
//!   EML mods carried by the interpreter (no hosted ShroudForge), every other loader's copies where main puts them.
//! - **Other loaders' folders survive a skip**: a package the plan skips, and a start whose plan is refused, leave
//!   every other loader's copies as they were.
//! - **No DLL twice**: the DLL XHL-Core loads from a ShroudForge package is one file, in the package Kiln placed once;
//!   chained natives (XHL-Core itself, a plugin loader) are never planned for the interpreter.
//! - **ShroudForge's own loader in the game**: the interpreter stands aside (main's route, no plan, no journal), and
//!   the launcher's card and engine row say so.
//!
//! Every package and DLL here is synthetic.

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::engine_mods;
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::{ModKind, RunSide};
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile;
use kiln_manage::sfi_activation;
use kiln_manage::sfi_backend::{self, InterpreterRoute};
use kiln_manage::sfi_plan::Mode;
use serde_json::Value;
use sfi_profile_common::*;
use sfi_routing_common::*;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// XHL-Core 2.1.0's DLL: the markers Kiln recognizes it by.
fn xhl_core_dll() -> Vec<u8> {
    let mut text = b"dinput8-chain.ini\0native-plugin.ini\0".to_vec();
    for c in "XHL-Native-Plugin-Host.log".encode_utf16() {
        text.extend_from_slice(&c.to_le_bytes());
    }
    build_test_dll(&["DirectInput8Create"], &text)
}

const BT_INI: &[u8] = b"[Plugin]\r\nEnabled=1\r\nDll=BuildingTools.dll\r\n";

/// A small x64 DLL image the read-only import gate passes (sfi_plan's `pe()`): a test DLL built for the scanner would be
/// refused (no subsystem), and the package with it skipped.
fn gate_passing_dll() -> Vec<u8> {
    fn at(b: &mut [u8], off: usize, v: u64, n: usize) {
        b[off..off + n].copy_from_slice(&v.to_le_bytes()[..n]);
    }
    let mut b = vec![0u8; 0x1000];
    b[..2].copy_from_slice(b"MZ");
    at(&mut b, 0x3c, 0x80, 4);
    b[0x80..0x84].copy_from_slice(b"PE\0\0");
    at(&mut b, 0x84, 0x8664, 2);
    at(&mut b, 0x86, 1, 2);
    at(&mut b, 0x94, 240, 2);
    at(&mut b, 0x96, 0x2022, 2);
    at(&mut b, 0x98, 0x20b, 2);
    at(&mut b, 0x98 + 24, 0x180000000, 8);
    at(&mut b, 0x98 + 56, 0x2000, 4);
    at(&mut b, 0x98 + 60, 0x200, 4);
    at(&mut b, 0x98 + 68, 3, 2);
    at(&mut b, 0x98 + 108, 16, 4);
    b[0x188..0x18d].copy_from_slice(b".test");
    at(&mut b, 0x188 + 8, 0xe00, 4);
    at(&mut b, 0x188 + 12, 0x1000, 4);
    at(&mut b, 0x188 + 16, 0xe00, 4);
    at(&mut b, 0x188 + 20, 0x200, 4);
    b
}

/// Building Tools for ShroudForge: a ShroudForge package whose folder is an XHL-Core plugin too.
fn building_tools(root: &Path) -> PathBuf {
    let d = root.join("src/building-tools");
    put(
        &d,
        "mod.json",
        br#"{"id":"building-tools","name":"Building Tools","version":"0.33.2","capabilities":[]}"#,
    );
    put(
        &d,
        "extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
    );
    put(&d, "BuildingTools.dll", &gate_passing_dll());
    put(&d, "native-plugin.ini", BT_INI);
    d
}

/// A ShroudForge package that needs one the game does not have: the plan skips it.
fn needs_twin(root: &Path) -> PathBuf {
    let d = root.join("src/needs-twin");
    put(
        &d,
        "mod.json",
        br#"{"id":"needs-twin","name":"needs-twin","version":"1.0.0","capabilities":["runtime"],"dependencies":[{"id":"twin","version":"^1"}]}"#,
    );
    put(
        &d,
        "extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
    );
    put(&d, "src/mod.lua", b"return {}");
    d
}

/// A Kiln mod.
fn kiln_mod(root: &Path) -> PathBuf {
    let d = root.join("src/new-items");
    put(
        &d,
        "kiln.mod.json",
        br#"{ "id": "new-items", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }"#,
    );
    put(&d, "M.dll", b"new-items");
    d
}

fn import_all(dirs: &kiln_manage::paths::DataDirs, src: &Path) -> Vec<LibraryRef> {
    let r = library::import(
        dirs,
        &[src.to_path_buf()],
        &mut kiln_manage::task::NoProgress,
    )
    .unwrap();
    r.imported.iter().chain(&r.existing).cloned().collect()
}

/// The world's profile with every other loader's mods beside its ShroudForge mod: Building Tools (on or off), a mod the
/// plan skips, XHL-Core and the plugin loader (native), a plain EML mod and one that needs ShroudForge's runtime, an EMF
/// Lua mod, a Greeneye, a Flame and a SET mod, a Kiln mod. ShroudForge is installed with its config naming another
/// folder.
fn everything(w: &World, building_tools_on: bool) {
    sf_install(&w.game, Some("sfmods"));
    let mut p = profile::get(&w.dirs, &w.profile).unwrap();
    let t = &w.root;
    p.mods.shroudforge.push(pm(
        &import(
            &w.dirs,
            &building_tools(t),
            ModKind::ShroudForge,
            "building-tools",
        ),
        building_tools_on,
    ));
    p.mods.shroudforge.push(pm(
        &import(&w.dirs, &needs_twin(t), ModKind::ShroudForge, "needs-twin"),
        true,
    ));
    put(t, "XHL-Core/dinput8.dll", &xhl_core_dll());
    p.mods.native.push(pm(
        &import_kind(&w.dirs, &t.join("XHL-Core/dinput8.dll"), ModKind::Native),
        true,
    ));
    p.mods.eml.push(pm(
        &import(&w.dirs, &eml_package(t, "gliders"), ModKind::Eml, "gliders"),
        true,
    ));
    p.mods.emf.push(pm(
        &import(
            &w.dirs,
            &emf_lua_package(t, "beastmaster"),
            ModKind::Emf,
            "beastmaster",
        ),
        true,
    ));
    for src in other_packages(t) {
        for x in import_all(&w.dirs, &src) {
            match x.kind {
                ModKind::Greeneye => p.mods.greeneye.push(pm(&x, true)),
                ModKind::Flame => p.mods.flame.push(pm(&x, true)),
                ModKind::Set => p.mods.set.push(pm(&x, true)),
                _ => {}
            }
        }
    }
    p.mods.kiln.push(pm(
        &import(&w.dirs, &kiln_mod(t), ModKind::Kiln, "new-items"),
        true,
    ));
    profile::save(&w.dirs, &p).unwrap();
}

fn activate(w: &World, what: &str) -> profile::Activation {
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{what}: {e:?}"))
}

/// Every file of the game folder outside the folders Kiln's interpreter has (`<game>/mods` and its launch plans) and the
/// records that change with every activation: what the other loaders run.
fn others(w: &World) -> BTreeMap<String, String> {
    snapshot(&w.game)
        .into_iter()
        .filter(|(rel, _)| {
            !rel.starts_with("mods/")
                && !rel.starts_with("kiln/launch-plans")
                && !rel.starts_with("kiln/state/")
                && !rel.starts_with("kiln/profile.json")
                && !rel.starts_with("kiln/kiln.json")
        })
        .collect()
}

/// The plan's package records by id.
fn packages(plan: &Value) -> BTreeMap<String, Value> {
    plan["packages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| (p["id"].as_str().unwrap().to_owned(), p.clone()))
        .collect()
}

/// Every file named `name` in the game folder (any case), game-relative.
fn files_named(game: &Path, name: &str) -> Vec<String> {
    snapshot(game)
        .into_keys()
        .filter(|rel| {
            rel.rsplit('/')
                .next()
                .is_some_and(|n| n.eq_ignore_ascii_case(name))
        })
        .collect()
}

#[test]
fn a_kiln_profile_beside_every_other_loader_has_one_backend_and_their_folders_survive_a_skip() {
    let w = world("all", true);
    everything(&w, true);
    let a = activate(&w, "kiln");
    let doc = w.doc(Mode::Active);
    let plan = pinned_plan(&w.game, &doc, Mode::Active);
    assert_eq!(w.plans().len(), 1);
    assert_eq!(
        (
            plan["backend"]["selected"].as_str(),
            plan["backend"]["dataWriter"].as_str()
        ),
        (Some("kiln"), Some("kiln-eml"))
    );
    // the EML mods go to the interpreter, as the hosted ShroudForge would have them: no hosted ShroudForge in kiln mode
    let loaders = serde_json::to_value(&a).unwrap()["ownLoaders"].clone();
    let eml: Vec<&Value> = loaders
        .as_array()
        .into_iter()
        .flatten()
        .filter(|l| l["kind"] == "eml")
        .collect();
    assert!(
        eml.iter().all(|l| l["via"] == "interpreter") && !eml.is_empty(),
        "{loaders}"
    );
    // nothing in ShroudForge's own folder
    assert!(!w.file("sfmods/sfpack").exists() && !w.file("sfmods/building-tools").exists());
    let pk = packages(&plan);
    assert_eq!(pk["needs-twin"]["decision"], "skip", "{}", pk["needs-twin"]);
    // one backend: the ShroudForge mods and both EML mods are the interpreter's candidates
    for id in ["sfpack", "building-tools", "gliders", "wardrobe"] {
        assert_eq!(pk[id]["decision"], "candidate", "{id}: {}", pk[id]);
    }
    // the Flame mod shares <game>/mods with the interpreter, which never runs it (Flame's loader does)
    assert_ne!(
        pk["creative"]["decision"], "candidate",
        "{}",
        pk["creative"]
    );
    assert!(w.file("mods/creative/creative.dll").is_file());
    // every other loader's copies are where main puts them
    for rel in [
        "emf/mods/beastmaster/main.lua",
        "greeneye/worstmap/map.dll",
        "shrouded_engine_tweaks/mods/flymode/init.lua",
        "kiln/chain/xhl-core/xhl-core.dll",
        "powrprof.dll",
        "plugins/AutolootCoop.dll",
    ] {
        assert!(w.file(rel).is_file(), "{rel}");
    }
    // the Kiln mod runs from the Library through the profile file, as on every backend
    assert!(doc.to_string().contains("new-items"), "the Kiln mod: {doc}");
    let before = others(&w);
    // the start's plan refused (no archive answer): every other loader keeps what it had
    refuse_inspection(&w.game, true);
    activate(&w, "refused");
    refuse_inspection(&w.game, false);
    let st = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert!(
        st.refused.is_some() && w.doc(Mode::Active).get("sfLaunchPlan").is_none(),
        "{st:?}"
    );
    assert_eq!(
        others(&w),
        before,
        "a refused plan changed another loader's files"
    );
    // and the plan again
    activate(&w, "again");
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert_eq!(others(&w), before);
    assert!(sfi_activation::start_check(&w.game).is_none());
}

#[test]
fn a_dll_xhl_core_loads_is_one_file_and_chained_natives_are_never_planned() {
    for on in [true, false] {
        let w = world(&format!("xhl-{on}"), true);
        everything(&w, on);
        activate(&w, "kiln");
        let plan = pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
        let pk = packages(&plan);
        let bt = pk.get("building-tools").cloned().unwrap_or(Value::Null);
        let copies = files_named(&w.game, "BuildingTools.dll");
        assert_eq!(
            copies,
            ["mods/building-tools/BuildingTools.dll"],
            "on={on}: one file"
        );
        let ini = files_named(&w.game, "native-plugin.ini");
        if on {
            assert_eq!(
                ini,
                ["mods/building-tools/native-plugin.ini"],
                "XHL-Core reads the one copy"
            );
            assert_eq!(bt["decision"], "candidate", "{bt}");
            // the interpreter's record of the same file, once: the DLL XHL-Core loads is the one image kiln-sfi finds
            // already loaded (its first-loader rule), never a second copy it would load again
            let dlls = bt["dlls"].as_array().unwrap();
            assert_eq!(dlls.len(), 1, "{bt}");
            let sha = kiln_manage::hash::sha256_hex(&std::fs::read(w.file(&copies[0])).unwrap());
            assert_eq!(
                (dlls[0]["path"].as_str(), dlls[0]["sha256"].as_str()),
                (Some("BuildingTools.dll"), Some(sha.as_str())),
                "{bt}"
            );
        } else {
            assert!(
                ini.is_empty(),
                "a switched-off copy is no XHL-Core plugin: {ini:?}"
            );
            assert_ne!(bt["decision"], "candidate", "{bt}");
        }
        // what the plan has the interpreter load: DLLs of packages in <game>/mods only
        let core_sha = kiln_manage::hash::sha256_hex(&xhl_core_dll());
        let text = plan.to_string();
        assert!(
            !text.contains(&core_sha),
            "on={on}: XHL-Core's DLL in the plan"
        );
        for (id, p) in &pk {
            assert!(
                w.file(&format!("mods/{id}")).is_dir(),
                "{id}: a plan package outside <game>/mods"
            );
            for d in p["dlls"].as_array().into_iter().flatten() {
                let path = d["path"].as_str().unwrap_or_default();
                assert!(!path.contains("..") && !path.starts_with('/'), "{id}: {d}");
            }
        }
        assert!(
            !pk.contains_key("autolootcoop") && !pk.keys().any(|k| k.contains("xhl")),
            "{:?}",
            pk.keys()
        );
    }
}

#[test]
fn shroudforges_own_loader_in_the_game_makes_the_interpreter_stand_aside() {
    let w = world("own", true);
    sf_install(&w.game, Some("sfmods"));
    sf_own_loader(&w.game);
    let n = inspections(&w.game);
    activate(&w, "own loader");
    assert_eq!(
        inspections(&w.game),
        n,
        "no archive read for a start the interpreter does not have"
    );
    assert!(
        w.plans().is_empty() && w.doc(Mode::Active).get("sfLaunchPlan").is_none(),
        "{:?}",
        w.plans()
    );
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    // main's route: ShroudForge's own config names the folder its loader reads
    assert!(w.file("sfmods/sfpack").is_dir() && !w.file("mods/sfpack").exists());
    let c = sfi_backend::card(&w.game, RunSide::Client, &sfi_backend::Env::default()).unwrap();
    assert_eq!(
        c.interpreter_route,
        Some(InterpreterRoute::OwnLoader),
        "{c:?}"
    );
    let row = engine_mods::status(&w.game)
        .into_iter()
        .find(|s| s.kind == ModKind::ShroudForge)
        .unwrap();
    assert_ne!(row.plugin, "kiln-sfi", "{row:?}");
    let st = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert!(st.plan.is_none(), "{st:?}");
}
