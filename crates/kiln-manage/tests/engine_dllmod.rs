//! Packages for AndyPimms' Enshrouded ModLoader (the Library kind `dllmod`, run by kiln-dllmod): CarryOn and Belt
//! Lantern as their release zips ship them (`Mods/EnshroudedModLoader/<package>/<mod>.dll` + `.dll.mod`, inert stand-ins
//! here), what the Library imports from each shape, what it leaves out and why, and where the profile has the copy
//! kiln-dllmod (or that loader itself) reads: `<game>/Mods/EnshroudedModLoader/<id>/`.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::engine_mods;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, DetectKind, ImportReport, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::io::Write;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let n = N.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let p = std::env::temp_dir().join(format!("km-dllmod-{tag}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) -> PathBuf {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(&p, data).unwrap();
    p
}

const CARRYON_MOD: &str = "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=0.1.50-dev\nabi=1\nsetting=move_chests|bool|Chests|true\n\
    setting=move_objects|bool|Decor|true\nsetting=move_interactive|bool|Usable objects|true\nsetting=inspect_objects|bool|Debug (object IDs)|false\n";
const LANTERN_MOD: &str = "ENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=0.1.29-dev\r\nabi=1\r\n";

fn carryon_dll(v: &str) -> Vec<u8> {
    build_test_dll(&["EnshroudedModConfigure", "EnshroudedModInitialize"], v.as_bytes())
}

/// CarryOn's release zip (0.1.50-dev): the package under Mods/EnshroudedModLoader/, its texts beside it.
fn carryon_zip(t: &Path, version: &str, wrapper: &str) -> PathBuf {
    let z = t.join(format!("CarryOn-{version}-win64.zip"));
    let mut w = zip::ZipWriter::new(std::fs::File::create(&z).unwrap());
    let manifest = CARRYON_MOD.replace("0.1.50-dev", version);
    for (name, data) in [
        (format!("{wrapper}INSTALLATION.txt"), b"CARRYON - WINDOWS X64".to_vec()),
        (format!("{wrapper}SHA256SUMS.txt"), b"0000 carryon.dll".to_vec()),
        (format!("{wrapper}Mods/EnshroudedModLoader/carryon/carryon.dll"), carryon_dll(version)),
        (format!("{wrapper}Mods/EnshroudedModLoader/carryon/carryon.dll.mod"), manifest.into_bytes()),
    ] {
        w.start_file(name, zip::write::SimpleFileOptions::default()).unwrap();
        w.write_all(&data).unwrap();
    }
    w.finish().unwrap();
    z
}

fn summary(r: &ImportReport) -> String {
    let mut out = Vec::new();
    for x in r.imported.iter().chain(&r.existing) {
        out.push(format!("+ {}/{} {}", x.kind.as_str(), x.id, x.version));
    }
    for s in &r.skipped {
        out.push(format!("- {}: {}", s.path, s.reason));
    }
    out.join("\n")
}

fn import(dirs: &DataDirs, paths: &[PathBuf]) -> ImportReport {
    library::import(dirs, paths, &mut NoProgress).unwrap()
}

#[test]
fn every_shape_of_a_release_is_one_dllmod_entry_with_its_manifest() {
    let t = tmp("shapes");
    let unpacked = t.0.join("unpacked");
    put(&unpacked, "Mods/EnshroudedModLoader/carryon/carryon.dll", &carryon_dll("0.1.50-dev"));
    put(&unpacked, "Mods/EnshroudedModLoader/carryon/carryon.dll.mod", CARRYON_MOD.as_bytes());
    put(&unpacked, "INSTALLATION.txt", b"text");
    let shapes = [
        ("zip", carryon_zip(&t.0, "0.1.50-dev", "")),
        ("wrapped zip", { std::fs::create_dir_all(t.0.join("w")).unwrap(); carryon_zip(&t.0.join("w"), "0.1.50-dev", "CarryOn-0.1.50-dev/") }),
        ("unpacked folder", unpacked.clone()),
        ("package folder", unpacked.join("Mods/EnshroudedModLoader/carryon")),
        ("the DLL beside its manifest", unpacked.join("Mods/EnshroudedModLoader/carryon/carryon.dll")),
    ];
    for (how, path) in shapes {
        let d = t.0.join(format!("data-{}", how.replace(' ', "-")));
        let dirs = DataDirs::at(d);
        let r = import(&dirs, &[path.clone()]);
        let got: Vec<_> = r.imported.iter().map(|x| (x.kind, x.id.as_str(), x.version.as_str())).collect();
        assert_eq!(got, [(ModKind::Dllmod, "carryon", "0.1.50-dev")], "{how}: {}", summary(&r));
        let folder = library::path_of(&dirs, &r.imported[0]).unwrap();
        assert!(folder.join("carryon.dll").is_file() && folder.join("carryon.dll.mod").is_file(), "{how}: {}", folder.display());
        let rec = library::record(&dirs, &r.imported[0]).unwrap();
        assert_eq!((rec.side, rec.clients), (Some(kiln_manage::manifest::Side::Client), Some(kiln_manage::manifest::Clients::None)), "{how}");
    }
}

#[test]
fn what_the_library_leaves_out_and_why() {
    let t = tmp("left-out");
    let r = &t.0;
    // the mod DLL without its manifest: nothing to run it by (no version, no settings)
    let lone = put(r, "loose/lantern.dll", &build_test_dll(&["EnshroudedModInitialize"], b""));
    let d = library::detect(&lone).unwrap();
    assert_eq!(d.kind, DetectKind::Unknown, "{d:?}");
    assert!(d.reason.contains("without its manifest") && d.reason.contains("lantern.dll.mod"), "{}", d.reason);
    // a manifest the format refuses, and a mod package without its export: left out with the reason
    put(r, "bad-abi/x.dll", &build_test_dll(&["EnshroudedModInitialize"], b""));
    put(r, "bad-abi/x.dll.mod", b"ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=2\n");
    put(r, "no-export/y.dll", &build_test_dll(&["Something"], b""));
    put(r, "no-export/y.dll.mod", LANTERN_MOD.as_bytes());
    put(r, "no-configure/z.dll", &build_test_dll(&["EnshroudedModInitialize"], b""));
    put(r, "no-configure/z.dll.mod", CARRYON_MOD.as_bytes());
    for (dir, why) in [("bad-abi", "abi"), ("no-export", "EnshroudedModInitialize"), ("no-configure", "EnshroudedModConfigure")] {
        let d = library::detect(&r.join(dir)).unwrap();
        assert_eq!(d.kind, DetectKind::Unknown, "{dir}: {d:?}");
        assert!(d.reason.contains("Enshrouded ModLoader") && d.reason.contains(why), "{dir}: {}", d.reason);
        assert!(!d.reason.ends_with('.'), "the launcher adds the period: {}", d.reason);
    }
    let dirs = DataDirs::at(r.join("data"));
    let rep = import(&dirs, &[r.join("bad-abi"), r.join("no-export"), lone]);
    assert!(rep.imported.is_empty() && rep.skipped.len() == 3, "{}", summary(&rep));
    // another file named <dll>.mod beside a plain DLL changes nothing
    let fps = put(r, "other/fps.dll", &build_test_dll(&["x"], b""));
    put(r, "other/fps.dll.mod", b"[settings]\n");
    assert_eq!(library::detect(&fps).unwrap().kind, DetectKind::BareDll);
    // Enshrouded ModLoader's own dinput8.dll (as its release ships it in dist/): a DLL mod Kiln chain-loads, as before
    // (kiln-dllmod steps aside while it runs)
    let loader = put(r, "Enshrouded-ModLoader/dist/dinput8.dll",
        &build_test_dll(&["DirectInput8Create", "GetdfDIJoystick"], b"ENSHROUDED_MODS_V1\0ENSHROUDED_PACKAGE_V1\0"));
    let d = library::detect(&loader).unwrap();
    assert_eq!(d.kind, DetectKind::BareDll, "{d:?}");
    assert_eq!(kiln_manage::pe::classify_proxy(&loader).unwrap().kind, kiln_manage::pe::ProxyKind::Dllmod);
}

#[test]
fn a_library_package_and_two_mods_in_one_folder_of_packages() {
    let t = tmp("folder-of-packages");
    let r = &t.0;
    put(r, "Mods/EnshroudedModLoader/carryon/carryon.dll", &carryon_dll("1"));
    put(r, "Mods/EnshroudedModLoader/carryon/carryon.dll.mod", CARRYON_MOD.as_bytes());
    put(r, "Mods/EnshroudedModLoader/lantern/lantern.dll", &build_test_dll(&["EnshroudedModInitialize"], b"l"));
    put(r, "Mods/EnshroudedModLoader/lantern/lantern.dll.mod", LANTERN_MOD.as_bytes());
    put(r, "Mods/EnshroudedModLoader/shared/base.dll", &build_test_dll(&["Base"], b""));
    put(r, "Mods/EnshroudedModLoader/shared/base.dll.mod", b"ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1.0\nabi=1\n");
    let dirs = DataDirs::at(r.join("data"));
    let rep = import(&dirs, &[r.join("Mods")]);
    let mut got: Vec<_> = rep.imported.iter().map(|x| (x.kind, x.id.clone())).collect();
    got.sort();
    assert_eq!(got, [(ModKind::Dllmod, "carryon".to_string()), (ModKind::Dllmod, "lantern".into()), (ModKind::Dllmod, "shared".into())],
        "{}", summary(&rep));
}

// ---- the profile: Kiln's copy in <game>/Mods/EnshroudedModLoader/<id>/ ----

fn game(t: &Path, mode: PathMode) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    if mode == PathMode::Z {
        std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    }
    GameInstall {
        id: "g-test".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: mode, source: "manual".into(),
    }
}

fn runtime_in_game(dirs: &DataDirs, g: &GameInstall, reader: Option<u32>, git: &str) {
    let s = g.path.parent().unwrap().join(format!("stage-{git}"));
    let mut m = serde_json::json!({ "format": 1, "kiln": "0.2.0-dev", "abi": 1, "api": "0.2.0",
        "components": { "core": { "sides": ["server", "client"], "default": true, "description": "core" } } });
    if let Some(r) = reader { m["profileReader"] = r.into(); }
    put(&s, "kiln-runtime.json", serde_json::to_string_pretty(&m).unwrap().as_bytes());
    put(&s, "core/kiln/kiln-core.dll", format!("core {git}").as_bytes());
    let opts = kiln_manage::bundle::CreateOptions { source_date_epoch: 1_794_000_000, git: git.into() };
    let zip = kiln_manage::bundle::create(&s, &g.path.parent().unwrap().join(format!("out-{git}")), &opts).unwrap();
    let b = kiln_manage::bundle::import(dirs, &zip).unwrap();
    let core = format!("core {git}");
    put(&g.path, "kiln/kiln-core.dll", core.as_bytes());
    put(&g.path, "kiln/.install.json", serde_json::to_string_pretty(&serde_json::json!({
        "format": 1, "installId": "c7a3e1f0-2b4d-4e6f-8a9b-0c1d2e3f4a5b", "installed": "2026-11-11T00:00:00Z",
        "runtime": { "version": b.version, "components": ["core"] }, "side": "client",
        "files": [{ "path": "kiln/kiln-core.dll", "sha256": kiln_manage::hash::sha256_hex(core.as_bytes()), "component": "core" }],
    })).unwrap().as_bytes());
}

fn z(p: &Path) -> String {
    format!("Z:{}", p.display()).replace('/', "\\")
}

fn import_carryon(dirs: &DataDirs, t: &Path, version: &str) -> LibraryRef {
    let r = import(dirs, &[carryon_zip(&t.join(version), version, "")]);
    r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Dllmod).cloned().unwrap_or_else(|| panic!("{}", summary(&r)))
}

fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

fn activate_with(dirs: &DataDirs, g: &GameInstall, pid: &str, mods: Vec<ProfileMod>) -> profile::ProfileFile {
    let mut p = profile::get(dirs, pid).unwrap();
    p.mods.dllmod = mods;
    profile::save(dirs, &p).unwrap();
    profile::activate(dirs, pid, g).unwrap();
    profile::read_game_profile(&g.path).unwrap().unwrap()
}

#[test]
fn the_package_runs_from_mods_enshroudedmodloader_and_keeps_what_it_wrote_across_updates() {
    let t = tmp("placed");
    let t = &t.0;
    std::fs::create_dir_all(t.join("0.1.50-dev")).unwrap();
    std::fs::create_dir_all(t.join("0.1.51-dev")).unwrap();
    let dirs = DataDirs::at(t.join("data"));
    let g = game(t, PathMode::Z);
    let v50 = import_carryon(&dirs, t, "0.1.50-dev");
    let pid = profile::create(&dirs, "Carry", None).unwrap().id;
    let file = activate_with(&dirs, &g, &pid, vec![pm(&v50, true)]);
    // the folder Enshrouded ModLoader 0.6.12 reads (its package id carryon/carryon.dll, its per-world settings keyed by it)
    let copy = g.path.join("Mods/EnshroudedModLoader/carryon");
    assert_eq!(file.mods.dllmod[0].path, z(&copy), "the profile names the game-folder copy");
    assert!(copy.join("carryon.dll").is_file() && copy.join("carryon.dll.mod").is_file());
    assert_eq!(engine_mods::marker(&copy).map(|m| m.version), Some("0.1.50-dev".into()));
    // a mod writes its data beside its DLL (Belt Lantern: characters/); it survives an update
    put(&copy, "characters/marker.bin", b"saved");
    let v51 = import_carryon(&dirs, t, "0.1.51-dev");
    let file = activate_with(&dirs, &g, &pid, vec![pm(&v51, true)]);
    assert_eq!(file.mods.dllmod[0].path, z(&copy));
    assert!(std::fs::read_to_string(copy.join("carryon.dll.mod")).unwrap().contains("0.1.51-dev"));
    assert_eq!(std::fs::read(copy.join("characters/marker.bin")).unwrap(), b"saved");
    // switched off: the copy goes (the loader would offer it in its MODS page otherwise), its data waits for it
    let file = activate_with(&dirs, &g, &pid, vec![pm(&v51, false)]);
    assert!(!copy.exists(), "a switched-off package is not left where a loader runs it");
    assert!(!file.mods.dllmod[0].enabled);
    activate_with(&dirs, &g, &pid, vec![pm(&v51, true)]);
    assert_eq!(std::fs::read(copy.join("characters/marker.bin")).unwrap(), b"saved", "and comes back with it");
    // the core and the plugin read mods.dllmod; the status file of kiln-dllmod is the engine's row
    assert_eq!(engine_mods::state_file(ModKind::Dllmod), Some("dllmod.json"));
    assert_eq!(engine_mods::plugin_of(ModKind::Dllmod), Some("kiln-dllmod"));
}

#[test]
fn copy_mode_names_the_copy_only_for_a_core_that_reads_mods_dllmod() {
    let t = tmp("copy-mode");
    let t = &t.0;
    std::fs::create_dir_all(t.join("0.1.50-dev")).unwrap();
    let dirs = DataDirs::at(t.join("data"));
    let g = game(t, PathMode::Copy);
    let v50 = import_carryon(&dirs, t, "0.1.50-dev");
    let pid = profile::create(&dirs, "Carry", None).unwrap().id;
    runtime_in_game(&dirs, &g, Some(3), "e3d6185");
    let file = activate_with(&dirs, &g, &pid, vec![pm(&v50, true)]);
    assert_eq!(file.mods.dllmod[0].path, format!("profiles/{pid}/dllmod/carryon"), "a core of reader 3 does not know mods.dllmod");
    runtime_in_game(&dirs, &g, Some(profile::PROFILE_READER_DLLMOD), "a633659");
    let file = activate_with(&dirs, &g, &pid, vec![pm(&v50, true)]);
    assert_eq!(file.mods.dllmod[0].path, "../Mods/EnshroudedModLoader/carryon");
    assert!(g.path.join("Mods/EnshroudedModLoader/carryon/carryon.dll").is_file());
    // the core's own parser (a host build of profile.cpp, native/core/tests/test-profile.sh) reads what the launcher wrote
    let written = t.join("dllmod-copy-mode.json");
    std::fs::write(&written, serde_json::to_vec_pretty(&file).unwrap()).unwrap();
    let script = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../native/core/tests/test-profile.sh");
    let out = std::process::Command::new("bash").arg(&script).arg(&written).env("TMPDIR", t).output()
        .expect("bash runs native/core/tests/test-profile.sh (a C++20 compiler, c++ or $CXX, builds the parser)");
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    assert!(out.status.success() && text.contains("launcher profile ok"), "the native parser refused a profile the launcher wrote:\n{text}");
}
