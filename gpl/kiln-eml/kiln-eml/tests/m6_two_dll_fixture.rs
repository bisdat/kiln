// SPDX-License-Identifier: GPL-3.0-or-later
// test inputs of the interpreter's two-DLL startup integration.
//! The synthetic game folders of native/core/tests/sfi_startup (the two-DLL host that loads the real kiln-eml.dll
//! and kiln-sfi.dll, game-free). The archive is the synthetic m6 archive; the packages, the frozen launch plan and the
//! profile are written as the launcher's producer writes them (sfi-policy's strict v1, the inspector's frames, the
//! packages' content digests with sfi-policy's own function, SF's load order), so the plan is the one kiln-eml's and
//! kiln-sfi's readers accept. The run script calls the two ignored tests below with a spec (G14D_FIXTURE_SPEC) and to read
//! a game folder's archive back (G14D_VERIFY_GAME); the plain test checks that such a folder's plan is accepted by
//! kiln-eml's reader with the inspector's frames.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use camino::{Utf8Path, Utf8PathBuf};
use fixture::archive::{ArchiveFixture, STEM};
use kiln_eml::inspection;
use serde_json::{Value, json};
use sha2::{Digest as _, Sha256};
use std::fs;

pub const PLAN_ID: &str = "6714d000-0000-4000-8000-00000000d001";
/// The executable bytes when the spec names no host (never run).
const STAND_IN_EXE: &[u8] = b"m6 g14d stand-in executable bytes (never run)";

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn copy_tree(from: &Utf8Path, to: &Utf8Path) {
    fs::create_dir_all(to).unwrap();
    for entry in fs::read_dir(from).unwrap() {
        let entry = entry.unwrap();
        let name = entry.file_name().into_string().unwrap();
        let (src, dst) = (from.join(&name), to.join(&name));
        if entry.file_type().unwrap().is_dir() {
            copy_tree(&src, &dst);
        } else {
            fs::copy(&src, &dst).unwrap();
        }
    }
}

/// The Lua of a package that creates one content blob in the startup pass (SF's patch).
fn content_lua(id: &str) -> String {
    format!("game.assets.create_content(buffer.wrap(\"{id} blob\"))\nshroudforge.log.info(\"{id} blob ran\")")
}
/// The Lua of a runtime package: a line in the core's log, and whether game.types answers (the granted archive view).
fn runtime_lua(id: &str) -> String {
    format!(
        "local ok, t = pcall(function() return game.types.get(\"m6.Root\") end)\n\
         shroudforge.log.info(\"{id} ran types=\" .. tostring(ok and t ~= nil))"
    )
}

/// Writes one game folder below spec.out: game/ (archive, executable, packages, plan, profile, extras) and fixture.json
/// (the plan's SHA-256 and id, the executable's SHA-256, the archive's own containers).
fn build(spec: &Value) -> Value {
    let out = Utf8PathBuf::from(spec["out"].as_str().expect("spec.out"));
    let game = out.join("game");
    assert!(!game.exists(), "{game} exists");
    let f = ArchiveFixture::new("g14d");
    copy_tree(&f.game, &game);
    drop(f);
    let exe = match spec["host"].as_str() {
        Some(host) => fs::read(host).unwrap(),
        None => STAND_IN_EXE.to_vec(),
    };
    fs::write(game.join(format!("{STEM}.exe")), &exe).unwrap();
    let mut containers: Vec<String> = fs::read_dir(&game)
        .unwrap()
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| n.starts_with(&format!("{STEM}_")) && n.ends_with(".dat"))
        .collect();
    containers.sort();
    let packages = spec["packages"].as_array().cloned().unwrap_or_default();
    for p in &packages {
        let id = p["id"].as_str().unwrap();
        let dir = game.join("mods").join(id);
        fs::create_dir_all(dir.join("src")).unwrap();
        let caps = p["capabilities"].clone();
        let deps: Vec<Value> = p["requires"]
            .as_array()
            .map(|r| r.iter().map(|r| json!({"id": r, "version": "^1.0.0"})).collect())
            .unwrap_or_default();
        fs::write(
            dir.join("mod.json"),
            serde_json::to_vec(&json!({"id": id, "name": id, "version": "1.0.0", "capabilities": caps,
                "dependencies": deps}))
            .unwrap(),
        )
        .unwrap();
        if p["kind"].as_str() != Some("eml") {
            fs::write(dir.join("extended.mod.json"), br#"{"schemaVersion":1,"enabled":true}"#).unwrap();
        }
        let lua = match p["lua_src"].as_str() {
            Some(text) => text.to_owned(),
            None if p["data"].as_bool() == Some(true) => content_lua(id),
            None => runtime_lua(id),
        };
        fs::write(dir.join("src/mod.lua"), lua).unwrap();
        for dll in p["dlls"].as_array().cloned().unwrap_or_default() {
            fs::copy(dll["from"].as_str().unwrap(), dir.join(dll["path"].as_str().unwrap())).unwrap();
        }
    }
    if spec["xhl_receipt"].as_bool() == Some(true) {
        // a Library copy of an EML mod (an XHL edit) the plan does not list, with a placement receipt a Linux launcher
        // wrote (a Unix path): kiln-mode discovery asks Wine for that path's Windows name (discover.rs dos_name)
        let dir = game.join("mods/xhl-copy");
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(
            dir.join("mod.json"),
            br#"{"id":"xhl-copy","name":"xhl-copy","version":"1.0.0","capabilities":["patch"]}"#,
        )
        .unwrap();
        fs::write(dir.join("src/mod.lua"), "-- an EML edit the plan does not list").unwrap();
        let marker = json!({"format": 1, "kind": "eml", "id": "xhl-copy", "version": "1.0.0", "hash": "h"});
        fs::write(dir.join(".kiln-library.json"), serde_json::to_vec(&marker).unwrap()).unwrap();
        fs::create_dir_all(game.join("kiln/state")).unwrap();
        fs::write(
            game.join("kiln/state/library-placements.json"),
            serde_json::to_vec(
                &json!([{"path": "/g14d-launcher/steamapps/common/Enshrouded/mods/xhl-copy",
                "marker": marker}]),
            )
            .unwrap(),
        )
        .unwrap();
    }
    if spec["shroudforge_decoy"].as_bool() == Some(true) {
        // ShroudForge's own folder with an applied record: kiln mode never opens it
        let sf = game.join("shroudforge/config");
        fs::create_dir_all(&sf).unwrap();
        fs::write(
            sf.join("state.json"),
            serde_json::to_vec(&json!({"schemaVersion": 1, "windows": {"favorite": "cyan"}, "assets": {"schemaVersion": 1,
                "status": "applied", "phase": "startup", "target": STEM, "fingerprint": "a".repeat(64), "completedAt": 42,
                "mods": [{"id": "player-sf", "version": "1.0.0"}]}}))
            .unwrap(),
        )
        .unwrap();
        fs::write(
            sf.join("modloader-config.json"),
            br#"{"modloader":"shroudforge","paths":{"mods":"mods"}}"#,
        )
        .unwrap();
        fs::write(game.join("shroudforge/version.json"), br#"{"version":"1.7.9"}"#).unwrap();
    }
    let (bytes, exe_sha256) = plan(&game, &packages, spec.get("plan_edit"));
    let folder = game.join("kiln/launch-plans");
    fs::create_dir_all(&folder).unwrap();
    fs::write(folder.join(format!("{PLAN_ID}.json")), &bytes).unwrap();
    let hash: [u8; 32] = Sha256::digest(&bytes).into();
    let mut profile =
        json!({"mods": {}, "sfLaunchPlan": {"path": format!("launch-plans/{PLAN_ID}.json"), "sha256": hex(&hash)}});
    if spec["dropins"].as_bool() == Some(true) {
        // kiln-eml discovers the EML drop-in folders too (kiln/eml/mods, <game>/mods), so kiln mode's discovery runs in
        // full: the Library-copy check with its placement receipts
        profile["dropins"] = json!(["eml"]);
    }
    fs::write(game.join("kiln/profile.json"), profile.to_string()).unwrap();
    let meta =
        json!({"plan_sha256": hex(&hash), "plan_id": PLAN_ID, "exe_sha256": exe_sha256, "containers": containers});
    fs::write(out.join("fixture.json"), serde_json::to_vec_pretty(&meta).unwrap()).unwrap();
    meta
}

/// The frozen launch plan of these packages over the game folder as it stands, and the executable's SHA-256 it names.
fn plan(game: &Utf8Path, packages: &[Value], edit: Option<&Value>) -> (Vec<u8>, String) {
    let inspected = inspection::inspect(
        game.as_std_path(),
        &inspection::Options {
            side: inspection::Side::Client,
            hash_containers: false,
        },
    );
    assert_eq!(inspected.status, inspection::Status::Ok, "{:?}", inspected.reasons);
    let exe_sha256 = inspected.target.executable.as_ref().unwrap().sha256.clone();
    // dependencies first, then by id: the order SF's planner gives these packages
    let id = |p: &Value| p["id"].as_str().unwrap().to_owned();
    let requires = |p: &Value| -> Vec<String> {
        p["requires"]
            .as_array()
            .map(|r| r.iter().map(|v| v.as_str().unwrap().to_owned()).collect())
            .unwrap_or_default()
    };
    let mut left: Vec<&Value> = packages.iter().collect();
    left.sort_by_key(|p| id(p));
    let mut order: Vec<&Value> = Vec::new();
    while !left.is_empty() {
        let at = left
            .iter()
            .position(|p| requires(p).iter().all(|r| order.iter().any(|o| id(o) == *r)))
            .expect("no cycle");
        order.push(left.remove(at));
    }
    let caps = |p: &Value| -> Vec<String> {
        p["capabilities"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c.as_str().unwrap().to_owned())
            .collect()
    };
    let kind = |p: &Value| p["kind"].as_str().unwrap_or("shroudforge").to_owned();
    let needs_sf = packages.iter().any(|p| {
        (kind(p) == "shroudforge" && caps(p).iter().any(|c| c == "patch"))
            || (kind(p) == "eml" && caps(p).iter().any(|c| c == "runtime" || c == "runtime-register-dll"))
    });
    let has_eml = packages.iter().any(|p| kind(p) == "eml");
    let dialect = if needs_sf {
        "shroudforge"
    } else if has_eml {
        "eml"
    } else {
        "none"
    };
    let ff = "f".repeat(64);
    let entries: Vec<Value> = order
        .iter()
        .map(|p| {
            let pid = id(p);
            let digest = sfi_policy::digest::package_content_sha256(game.join("mods").join(&pid).as_std_path()).unwrap();
            let dlls: Vec<Value> = p["dlls"]
                .as_array()
                .cloned()
                .unwrap_or_default()
                .iter()
                .map(|d| {
                    let bytes = fs::read(d["from"].as_str().unwrap()).unwrap();
                    json!({"path": d["path"], "sha256": hex(&Sha256::digest(&bytes)), "mode": "library", "source": "ini",
                        "required": true, "gate": "pass", "gateProof_sha256": "b".repeat(64), "bridgeAbi": 0})
                })
                .collect();
            let (data, lua) = (p["data"].as_bool() == Some(true), p["lua"].as_bool() == Some(true));
            json!({
                "id": pid, "version": "1.0.0", "kind": kind(p), "root": format!("mods/{pid}"),
                "content_sha256": hex(&digest), "config_sha256": hex(&Sha256::digest(b"{}")),
                "enabled": true, "targets": ["client"], "capabilities": caps(p),
                "dependencies": requires(p).iter().map(|r| json!({"id": r, "range": "^1.0.0", "optional": false, "included": true})).collect::<Vec<_>>(),
                "requiredApis": [], "services": [], "requires180": false, "saveSensitive": false,
                "decision": "candidate", "reasons": [],
                "data": {"required": data, "admission": if data { "admit" } else { "not-needed" }},
                "lua": {"required": lua, "admission": if lua { "admit" } else { "not-needed" }},
                "dlls": dlls,
            })
        })
        .collect();
    let mut plan = json!({
        "format": 1, "minReader": 1, "plan_id": PLAN_ID, "launch_id": null, "startupDialect": dialect,
        "writer": {"component": "kiln-manage", "version": "0.2.0-test"},
        "profile": {"id": "g14d", "generation": 1, "mode": "active", "semantic_sha256": ff},
        "game": {"side": "client", "exe_sha256": exe_sha256,
            "archiveBase_sha256": inspected.base.as_ref().unwrap().sha256,
            "archiveInput_sha256": inspected.input.as_ref().unwrap().sha256},
        "backend": {"requested": "kiln", "selected": "kiln", "fallbackOrder": [], "dataWriter": "kiln-eml",
            "selectionReason": "kiln.json shroudforge.backend: \"kiln\""},
        "contract": {"apiVersion": "1.7.9", "sourceCommit": "d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8",
            "upstream180Final": false, "inventory_sha256": ff, "parity_sha256": ff, "serviceContracts_sha256": ff},
        "fingerprints": {"packages_sha256": ff, "settings_sha256": ff, "chain_sha256": ff, "loaderInventory_sha256": ff,
            "compat_sha256": ff, "releasePolicy_sha256": ff, "placementReceipts_sha256": ff},
        "packages": entries,
        "loadOrder": order.iter().map(|p| id(p)).collect::<Vec<_>>(),
        "warnings": [],
    });
    if let Some(Value::Object(edits)) = edit {
        for (pointer, value) in edits {
            *plan
                .pointer_mut(pointer)
                .unwrap_or_else(|| panic!("plan_edit {pointer}")) = value.clone();
        }
    }
    (serde_json::to_vec(&plan).unwrap(), exe_sha256)
}

/// The archive of a game folder read back: whether it validates and its content blobs (as text).
fn verify(game: &Utf8Path) -> Value {
    let types = fixture::types::registry();
    match fixture::archive::validate(game, STEM).and_then(|_| fixture::archive::semantics(game, STEM, &types)) {
        Ok(s) => json!({
            "valid": true,
            "contents": s.contents.values().map(|b| String::from_utf8_lossy(b).into_owned()).collect::<Vec<_>>(),
            "resources": s.resources.len(),
        }),
        Err(e) => json!({"valid": false, "error": format!("{e:#}")}),
    }
}

#[test]
#[ignore = "run by native/core/tests/sfi_startup/run.py with G14D_FIXTURE_SPEC"]
fn g14d_fixture_from_spec() {
    let spec: Value = serde_json::from_slice(&fs::read(std::env::var("G14D_FIXTURE_SPEC").unwrap()).unwrap()).unwrap();
    println!("{}", build(&spec));
}

#[test]
#[ignore = "run by native/core/tests/sfi_startup/run.py with G14D_VERIFY_GAME and G14D_VERIFY_OUT"]
fn g14d_verify_archive() {
    let game = Utf8PathBuf::from(std::env::var("G14D_VERIFY_GAME").unwrap());
    fs::write(
        std::env::var("G14D_VERIFY_OUT").unwrap(),
        serde_json::to_vec_pretty(&verify(&game)).unwrap(),
    )
    .unwrap();
}

// The folders the run script lays out are what kiln-eml's early phase reads: its strict plan reader (sfi-policy's,
// with build_info's executable digest and side, check_folders) accepts the plan the profile names, the inspector's
// frames match, and the archive reads back with no package blob in it.
#[test]
fn a_two_dll_fixture_is_a_plan_kiln_eml_reads() {
    let root = fixture::TestRoot::new("g14d-self");
    let dll = root.0.join("stand-in.dll");
    fs::write(&dll, b"MZ stand-in package DLL bytes (never loaded)").unwrap();
    let spec = json!({
        "out": root.0.join("case").as_str(),
        "packages": [
            {"id": "alpha", "capabilities": ["patch"], "data": true, "lua": false, "requires": []},
            {"id": "dllpack", "capabilities": ["patch"], "data": true, "lua": false, "requires": [],
             "dlls": [{"path": "dllpack.dll", "from": dll.as_str()}]},
            {"id": "rt-alpha", "capabilities": ["runtime"], "data": false, "lua": true, "requires": ["alpha"]},
        ],
        "xhl_receipt": true,
        "shroudforge_decoy": true,
        "dropins": true,
    });
    let meta = build(&spec);
    let game = root.0.join("case/game");
    let kiln = game.join("kiln");
    let profile = fs::read_to_string(kiln.join("profile.json")).unwrap();
    assert!(kiln_eml::discover::profile_dropins(&profile), "{profile}");
    let mut hash = [0u8; 32];
    let text = meta["plan_sha256"].as_str().unwrap();
    for (i, b) in hash.iter_mut().enumerate() {
        *b = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap();
    }
    let context = sfi_policy::launch_plan::ValidationContext {
        exe_sha256: meta["exe_sha256"].as_str().unwrap().to_owned(),
        side: "client".into(),
        upstream_180_final: false,
    };
    let read = kiln_eml::kiln_mode::read_plan(Some(&profile), &kiln, &game, &hash, &context);
    assert!(read.is_ok(), "{:?}", read.err());
    assert_eq!(meta["exe_sha256"].as_str().unwrap(), hex(&Sha256::digest(STAND_IN_EXE)));
    let plan: Value =
        serde_json::from_slice(&fs::read(kiln.join(format!("launch-plans/{PLAN_ID}.json"))).unwrap()).unwrap();
    assert_eq!(plan["loadOrder"], json!(["alpha", "dllpack", "rt-alpha"]));
    assert_eq!(plan["startupDialect"], "shroudforge");
    assert_eq!(
        plan["packages"][1]["dlls"][0]["sha256"].as_str().unwrap(),
        hex(&Sha256::digest(fs::read(&dll).unwrap()))
    );
    let back = verify(&game);
    assert_eq!(back["valid"], true, "{back}");
    assert!(
        back["contents"]
            .as_array()
            .unwrap()
            .iter()
            .all(|c| !c.as_str().unwrap().ends_with(" blob")),
        "{back}"
    );
    assert!(
        game.join("shroudforge/config/state.json").is_file() && game.join("mods/xhl-copy/.kiln-library.json").is_file()
    );
}
