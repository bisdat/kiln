//! What a server advertises for another engine's mod stays the same while the mod runs (inbox 84): the launcher's copy
//! is advertised by its Library entry's kdh1, whatever the mod writes into its folder and whatever the owner sets; a
//! folder of the owner's own by its files without logs and settings. The modpack built from the server carries what it
//! advertises (Kiln.Network AdvertiseIdentity.cs computes the same hash in the game).
#![cfg(all(unix, feature = "zip", feature = "server"))]

use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::modpack;
use kiln_manage::paths::DataDirs;
use kiln_manage::server::{control, instance::ServerInstance};
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-advertised-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A ShroudForge package shaped like Unified EML Tweaks (shipped switched off), for both sides.
fn package(root: &Path) -> PathBuf {
    let sf = root.join("src/unifiedemltweaks");
    put(&sf, "mod.json", br#"{"id":"unifiedemltweaks","name":"Unified EML Tweaks","version":"2.1.1","target":"client-server"}"#);
    put(&sf, "extended.mod.json", br#"{"schemaVersion":1,"enabled":false,"targets":["client","server"],
        "settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    put(&sf, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=bin/ut_native.dll\n");
    put(&sf, "bin/ut_native.dll", b"MZ not run");
    put(&sf, "src/mod.lua", b"return {}");
    sf
}

fn server(t: &Path) -> (PathBuf, ServerInstance) {
    let server = t.join("server");
    std::fs::create_dir_all(server.join("kiln")).unwrap();
    let inst: ServerInstance = serde_json::from_value(serde_json::json!({
        "id": "isles", "name": "Isles", "kind": "local", "dir": server, "port": 27140, "queryPort": 27140, "wine": null,
        "policy": "any", "source": "launcher", "remote": null })).unwrap();
    (server, inst)
}

/// What the mod and the owner do while the server runs.
fn run(copy: &Path) {
    put(copy, "bin/ut_native.log", b"started\n");
    put(copy, "bin/native-status.txt", b"tick 1\n");
    kiln_manage::shroudforge::set_setting(copy, "speed", &serde_json::json!(8)).unwrap();
    put(copy, "bin/native-status.txt", b"tick 2\n");
}

fn sf_hash(server: &Path) -> String {
    modpack::required(server).unwrap().into_iter().find(|m| m.id == "unifiedemltweaks").unwrap_or_else(|| panic!("not advertised")).hash
}

#[test]
fn a_synced_copy_is_advertised_by_its_library_entry_and_its_modpack_carries_that() {
    let t = tmp("synced");
    let dirs = DataDirs::at(t.0.join("data"));
    let r = library::import(&dirs, &[package(&t.0)], &mut NoProgress).unwrap();
    let lib: LibraryRef = r.imported.iter().find(|x| x.kind == ModKind::ShroudForge).cloned().unwrap();
    let (server, inst) = server(&t.0);
    control::sync(&dirs, &inst, std::slice::from_ref(&lib), &mut NoProgress).unwrap();
    let copy = server.join("mods/unifiedemltweaks");
    assert!(kiln_manage::shroudforge::read_mod(&copy).unwrap().enabled, "synced switched on");
    assert_ne!(kiln_manage::hash::dirhash(&copy).unwrap(), lib.hash, "the copy's own kdh1 is not the Library's (its switch)");

    // advertised: the Library entry, the same before and after a run that writes logs and changes a setting
    assert_eq!(sf_hash(&server), lib.hash);
    run(&copy);
    assert_eq!(sf_hash(&server), lib.hash);

    // the launcher's modpack packs the Library entry: its hash is what the server advertises, for any player's launcher
    let built = modpack::build_with(Some(&dirs), &server, &t.0.join("out"), "Isles", "1", &mut NoProgress).unwrap();
    let m = built.manifest.mods.iter().find(|m| m.id == "unifiedemltweaks").unwrap();
    assert_eq!((&m.hash, &m.identity), (&lib.hash, &None));
    let verified = modpack::verify(&built.file, Some(&built.sha256)).unwrap();
    modpack::check_against(&verified, &modpack::required(&server).unwrap()).unwrap();
    // without the Library (an older build call): the placed files, and what the server advertises beside their hash
    let built = modpack::build(&server, &t.0.join("out2"), "Isles", "1", &mut NoProgress).unwrap();
    let m = built.manifest.mods.iter().find(|m| m.id == "unifiedemltweaks").unwrap();
    assert_ne!(m.hash, lib.hash);
    assert_eq!(m.identity.as_deref(), Some(lib.hash.as_str()));
    let verified = modpack::verify(&built.file, Some(&built.sha256)).unwrap();
    modpack::check_against(&verified, &modpack::required(&server).unwrap()).unwrap();
    // the files the mod wrote are not packed
    let names = zip_names(&built.file);
    assert!(names.iter().any(|n| n == "shroudforge/unifiedemltweaks/bin/ut_native.dll"), "{names:?}");
    assert!(!names.iter().any(|n| n.ends_with("ut_native.log") || n.ends_with("native-status.txt")), "{names:?}");

    // its code changed on the server: no longer the Library's files, so not advertised as them
    put(&copy, "src/mod.lua", b"return { edited = true }");
    assert_ne!(sf_hash(&server), lib.hash);
}

#[test]
fn an_owners_own_folder_is_advertised_without_its_logs_and_settings() {
    let t = tmp("own");
    let (server, _) = server(&t.0);
    let own = server.join("mods/unifiedemltweaks");
    for rel in ["mod.json", "extended.mod.json", "native-plugin.ini", "bin/ut_native.dll", "src/mod.lua"] {
        put(&own, rel, &std::fs::read(package(&t.0).join(rel)).unwrap());
    }
    kiln_manage::shroudforge::set_enabled(&own, true).unwrap();
    let before = sf_hash(&server);
    put(&own, "bin/ut_native.log", b"started\n");
    kiln_manage::shroudforge::set_setting(&own, "speed", &serde_json::json!(8)).unwrap();
    assert_eq!(sf_hash(&server), before);
    // a forged marker without the server's receipt is not the launcher's copy
    put(&own, ".kiln-source.json", serde_json::to_string(&serde_json::json!({ "format": 1, "synced": "now", "by": "me",
        "library": { "kind": "shroudforge", "id": "unifiedemltweaks", "version": "2.1.1", "hash": format!("kdh1:{}", "a".repeat(64)) } })).unwrap().as_bytes());
    assert_eq!(sf_hash(&server), before);
    // its modpack carries the files without the logs, and the advertised hash
    let built = modpack::build(&server, &t.0.join("out"), "Isles", "1", &mut NoProgress).unwrap();
    let verified = modpack::verify(&built.file, Some(&built.sha256)).unwrap();
    modpack::check_against(&verified, &modpack::required(&server).unwrap()).unwrap();
    assert!(!zip_names(&built.file).iter().any(|n| n.ends_with(".log")));
}

fn zip_names(file: &Path) -> Vec<String> {
    let mut z = zip::ZipArchive::new(std::fs::File::open(file).unwrap()).unwrap();
    (0..z.len()).map(|i| z.by_index(i).unwrap().name().unwrap().to_string()).collect()
}
