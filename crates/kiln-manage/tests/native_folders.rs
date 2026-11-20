//! Mods made for CharosDX's dinput8.dll loader (Sparkworks Enhanced, Nexus 360; Cross-Base Storage, 245; Craft From
//! Stations, 215): a top-level `dinput8.dll` that LoadLibrary's every `mods\<mod>\native\*.dll` from its DllMain
//! (github.com/CharosDX/sparkworks-enhanced src/dinput8_proxy.c, MIT), and per mod a mod.json that only names it plus
//! `native/<mod>.dll`. Inbox 41: 0.2.0-dev.29.9 imported the loader as a native mod and skipped the mod itself.
#![cfg(all(unix, feature = "zip"))]

use kiln_manage::engines::Engine;
use kiln_manage::library::{self, DetectKind, ImportReport};
use kiln_manage::manifest::{ModKind, NativePhase, Side};
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::{build_test_dll, ProxyKind};
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
    let p = std::env::temp_dir().join(format!("km-native-folders-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// The loader as built from dinput8_proxy.c: exports DirectInput8Create, carries its log name in a format string.
fn charos_loader() -> Vec<u8> {
    build_test_dll(&["DirectInput8Create"], b"%snative-loader.log\0%s%s\\native\\\0mods\\\0FAILED to load \0")
}
/// A mod's own DLL: no exports, starts itself in DllMain.
fn charos_mod_dll(name: &str) -> Vec<u8> {
    build_test_dll(&[], format!("{name} config.ini debug.log").as_bytes())
}
fn mod_json(id: &str, name: &str, version: &str) -> Vec<u8> {
    format!(r#"{{"id":"{id}","name":"{name}","version":"{version}","description":"Native DLL, loaded by the dinput8.dll loader","authors":["CharosDX"],"capabilities":[]}}"#).into_bytes()
}

/// Sparkworks Enhanced 1.0.0 (Nexus 360, file 2208) as the player reported its zip.
fn sparkworks(root: &Path) {
    put(root, "dinput8.dll", &charos_loader());
    put(root, "mods/Sparkworks enhanced/mod.json", &mod_json("sparkworks-enhanced", "Sparkworks Enhanced", "1.0.0"));
    put(root, "mods/Sparkworks enhanced/native/sparkworks-enhanced.dll", &charos_mod_dll("sparkworks"));
}

fn zip_of(dir: &Path, out: &Path) {
    let mut w = zip::ZipWriter::new(std::fs::File::create(out).unwrap());
    let mut stack = vec![dir.to_path_buf()];
    let mut files = Vec::new();
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() { stack.push(p) } else { files.push(p) }
        }
    }
    files.sort();
    for f in files {
        let name = f.strip_prefix(dir).unwrap().to_string_lossy().replace('\\', "/");
        w.start_file(name, zip::write::SimpleFileOptions::default()).unwrap();
        w.write_all(&std::fs::read(&f).unwrap()).unwrap();
    }
    w.finish().unwrap();
}

fn import(t: &Tmp, paths: &[PathBuf]) -> (DataDirs, ImportReport) {
    let dirs = DataDirs::at(t.0.join("data"));
    let r = library::import(&dirs, paths, &mut NoProgress).unwrap();
    (dirs, r)
}

fn found(r: &ImportReport) -> Vec<String> {
    let mut v: Vec<String> = r.imported.iter().chain(&r.existing).map(|x| format!("{}/{}", x.kind.as_str(), x.id)).collect();
    v.sort();
    v
}

/// The generated native.json of an imported native mod, and its Library folder.
fn native_of(dirs: &DataDirs, r: &ImportReport, id: &str) -> (kiln_manage::manifest::NativeManifest, PathBuf) {
    let rf = r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Native && x.id == id).unwrap_or_else(|| panic!("{id}: {r:?}"));
    let folder = library::path_of(dirs, rf).unwrap();
    (kiln_manage::manifest::read_native(&folder).unwrap(), folder)
}

#[test]
fn the_loader_is_recognised_under_its_proxy_name_only() {
    let t = tmp("classify");
    put(&t.0, "dinput8.dll", &charos_loader());
    assert_eq!(kiln_manage::pe::classify_proxy(&t.0.join("dinput8.dll")).unwrap().kind, ProxyKind::NativeFolders);
    let d = library::detect(&t.0.join("dinput8.dll")).unwrap();
    assert_eq!((d.kind, d.engine), (DetectKind::Proxy, Some(Engine::NativeFolders)), "{d:?}");
    assert!(d.reason.contains("not needed"), "{}", d.reason);
    // a mod DLL that merely mentions the file name, not under a proxy name, keeps Kiln's own rules
    put(&t.0, "other/helper.dll", &build_test_dll(&[], b"reads native-loader.log"));
    assert_eq!(library::detect(&t.0.join("other/helper.dll")).unwrap().kind, DetectKind::BareDll);
}

#[test]
fn sparkworks_enhanced_imports_as_a_native_mod_without_its_loader() {
    let t = tmp("zip");
    let pkg = t.0.join("Sparkworks Enhanced 1.0.0");
    sparkworks(&pkg);
    let z = t.0.join("Sparkworks Enhanced 1.0.0-360-1-0-0-1794851986.zip");
    zip_of(&pkg, &z);
    let (dirs, r) = import(&t, &[z]);
    assert_eq!(found(&r), ["native/sparkworks-enhanced"], "{r:?}");
    assert!(r.skipped.is_empty(), "nothing is left out: {:?}", r.skipped);
    assert!(r.notes.iter().any(|n| n.text.contains("dinput8.dll is CharosDX's DLL mod loader: not needed")), "{:?}", r.notes);
    let (m, folder) = native_of(&dirs, &r, "sparkworks-enhanced");
    assert_eq!(m.dlls, ["native/sparkworks-enhanced.dll"]);
    assert_eq!(m.phase, NativePhase::Early, "loaded before Kiln hooks anything, as its own loader did");
    assert_eq!(m.side, Side::Both, "the logic runs on the host or the dedicated server");
    assert_eq!((m.name.as_deref(), m.version.as_str()), (Some("Sparkworks Enhanced"), "1.0.0"));
    // the author's files stay as shipped, and the loader is not in the mod
    assert!(folder.join("mod.json").is_file() && folder.join("native/sparkworks-enhanced.dll").is_file());
    assert!(!folder.join("dinput8.dll").exists());
    let entry = library::list(&dirs, Some(ModKind::Native)).unwrap();
    assert_eq!(entry.len(), 1, "one Library entry, not the loader: {entry:?}");
}

#[test]
fn its_config_and_log_written_in_the_library_are_no_change() {
    let t = tmp("output");
    let pkg = t.0.join("pkg");
    sparkworks(&pkg);
    let (dirs, r) = import(&t, &[pkg]);
    assert!(r.notes.iter().any(|n| n.text.contains("Sparkworks Enhanced: Kiln loads native/sparkworks-enhanced.dll from the mod's folder in Kiln's Library")
        && n.text.contains("config.ini")), "{:?}", r.notes);
    let (_, folder) = native_of(&dirs, &r, "sparkworks-enhanced");
    let rf = r.imported[0].clone();
    let rec = library::record(&dirs, &rf).unwrap();
    // the game ran: the mod made its config.ini and wrote debug.log beside its DLL (an older record, a newer folder)
    let record = PathBuf::from(format!("{}.json", folder.display()));
    std::fs::File::options().write(true).open(&record).unwrap()
        .set_modified(std::time::SystemTime::now() - std::time::Duration::from_secs(120)).unwrap();
    // the folder newer than its record (a file came and went), so the entry is hashed again
    put(&folder, "gone.tmp", b"");
    std::fs::remove_file(folder.join("gone.tmp")).unwrap();
    put(&folder, "native/config.ini", b"colour=#A87D12\narea_size=16\ndebug=0\n");
    put(&folder, "native/debug.log", b"Sparkworks Enhanced 1.0.0, game build 0\nready: trigger areas, chests\n");
    assert!(!library::changed_on_disk(&folder, &rec), "its own output is no edit");
    // an edit of the mod's DLL is
    put(&folder, "native/sparkworks-enhanced.dll", &charos_mod_dll("patched"));
    assert!(library::changed_on_disk(&folder, &rec), "a changed DLL is a change");
}

#[test]
fn the_mod_folder_alone_and_the_same_folder_twice_are_one_entry() {
    let t = tmp("folder");
    let pkg = t.0.join("pkg");
    sparkworks(&pkg);
    let (dirs, r) = import(&t, &[pkg.join("mods/Sparkworks enhanced")]);
    assert_eq!(found(&r), ["native/sparkworks-enhanced"], "{r:?}");
    let again = library::import(&dirs, &[pkg.clone()], &mut NoProgress).unwrap();
    assert!(again.imported.is_empty(), "the same files are already there: {again:?}");
    assert_eq!(found(&again), ["native/sparkworks-enhanced"]);
}

#[test]
fn every_dll_of_the_native_folder_loads_in_the_loaders_order() {
    let t = tmp("order");
    let pkg = t.0.join("Cross-Base Storage");
    put(&pkg, "dinput8.dll", &charos_loader());
    put(&pkg, "mods/cross-base-storage/mod.json", &mod_json("cross-base-storage", "Cross-Base Storage", "1.1.0"));
    put(&pkg, "mods/cross-base-storage/native/b-helper.dll", &charos_mod_dll("b"));
    put(&pkg, "mods/cross-base-storage/native/Cross-Base-Storage.dll", &charos_mod_dll("cbs"));
    put(&pkg, "mods/cross-base-storage/native/config.ini", b"x=1");
    let (dirs, r) = import(&t, &[pkg]);
    assert_eq!(found(&r), ["native/cross-base-storage"], "{r:?}");
    let (m, _) = native_of(&dirs, &r, "cross-base-storage");
    assert_eq!(m.dlls, ["native/b-helper.dll", "native/Cross-Base-Storage.dll"], "by name, case ignored");
}

#[test]
fn eml_mods_and_other_engines_dlls_are_not_taken_for_one() {
    let t = tmp("negative");
    // an EML mod (it has Lua) keeps being an EML mod, even with a native folder
    let eml = t.0.join("eml");
    put(&eml, "mods/m/mod.json", br#"{"id":"m","name":"M","version":"1.0.0","capabilities":["runtime"]}"#);
    put(&eml, "mods/m/src/mod.lua", b"return {}");
    put(&eml, "mods/m/native/x.dll", &charos_mod_dll("x"));
    assert_ne!(library::detect(&eml.join("mods/m")).unwrap().kind, DetectKind::BareDll);
    // an empty native folder, or one with another engine's mod DLL, is no such mod
    let empty = t.0.join("empty");
    put(&empty, "mod.json", &mod_json("e", "E", "1.0.0"));
    std::fs::create_dir_all(empty.join("native")).unwrap();
    assert_ne!(library::detect(&empty).unwrap().kind, DetectKind::BareDll);
    let other = t.0.join("other");
    put(&other, "mod.json", &mod_json("o", "O", "1.0.0"));
    put(&other, "native/o.dll", &build_test_dll(&["GreeneyeModStart"], b""));
    assert_ne!(library::detect(&other).unwrap().kind, DetectKind::BareDll);
}

#[test]
fn an_id_that_is_no_kiln_id_becomes_its_slug() {
    let t = tmp("slug");
    let pkg = t.0.join("pkg");
    put(&pkg, "mods/My Mod/mod.json", &mod_json("My Mod", "My Mod", "not-a-version"));
    put(&pkg, "mods/My Mod/native/my.dll", &charos_mod_dll("my"));
    let (dirs, r) = import(&t, &[pkg]);
    assert_eq!(found(&r), ["native/my-mod"], "{r:?}");
    let (m, _) = native_of(&dirs, &r, "my-mod");
    assert_eq!(m.version, "0.0.0");
}
