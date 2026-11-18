//! EMF's Lua mods (inbox 61): a player dropped EMF's release zip into the launcher, and every Lua mod in it was left out
//! ("mod.json does not describe a recognised mod package"), while EMF's lua mod runs them from <game>/emf/mods. The
//! fixture is EMF's own release layout (EMF scripts/package.ps1: `EMF-<version>/game/emf/mods/<id>/` with the shipped
//! mods, the Lua examples and the Lua starter beside `game/`, the source copies in `mods/`, EML content in `content/`).
//! The import takes the Lua mods as EMF mods, activation copies them to emf/mods/<id>/ (EMF's lua mod finds a Lua mod
//! only by its folder there), and the profile names that copy.
#![cfg(unix)]

use kiln_manage::engine_mods;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, DetectKind, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::quick;
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-emf-lua-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// EMF's `lua` mod (mods/lua/mod.json of EMF 0.6.8) with its DLL.
fn lua_host(dir: &Path) {
    put(dir, "mod.json", br#"{"id": "lua", "name": "Lua Scripts", "version": "1.33.0", "authors": ["EMF"], "emf": 20}"#);
    put(dir, "lua.dll", &build_test_dll(&["emf_mod_load"], b""));
}

/// An EMF Lua mod as EMF ships it (Beastmaster 1.17.1's mod.json keys, shortened): mod.json, main.lua, more Lua files.
fn lua_mod(dir: &Path, id: &str, name: &str, version: &str) {
    put(dir, "mod.json", format!(r#"{{
  "id": "{id}",
  "name": "{name}",
  "version": "{version}",
  "description": "An EMF Lua mod. Installed switched off.",
  "authors": ["EMF contributors"],
  "enabled": false,
  "emf": 14,
  "side": "server",
  "main": "main.lua",
  "depends": {{"lua": ">=1.25"}},
  "settings": [{{"key": "wheel_key", "type": "keybind", "default": "F3", "label": "Wheel", "live": true}}]
}}"#).as_bytes());
    put(dir, "main.lua", b"local tame = require('tame')\n");
    put(dir, "tame.lua", b"return {}\n");
    put(dir, "README.md", b"# A Lua mod\n");
}

/// EMF-0.6.8/ as EMF's scripts/package.ps1 lays it out, with the files that matter here.
fn emf_release(root: &Path) -> PathBuf {
    let r = root.join("EMF-0.6.8");
    put(&r, "game/dinput8.dll", &build_test_dll(&["DirectInput8Create", "emf_loader_identity"], b""));
    put(&r, "game/emf/known_tools.txt", b"dinput8.dll\n");
    lua_host(&r.join("game/emf/mods/lua"));
    put(&r, "game/emf/mods/big_stacks/mod.json", br#"{"id":"big_stacks","name":"Big Stacks","version":"1.0.0","emf":14,"enabled":false}"#);
    put(&r, "game/emf/mods/big_stacks/big_stacks.dll", &build_test_dll(&["emf_mod_load"], b""));
    lua_mod(&r.join("game/emf/mods/beastmaster"), "beastmaster", "Beastmaster", "1.17.1");
    lua_mod(&r.join("game/emf/mods/kill_counter"), "kill_counter", "Kill Counter", "1.0.0");
    // the player install scripts and readmes
    for f in ["install.cmd", "install.ps1", "uninstall.cmd", "uninstall.ps1", "emf_common.ps1", "README.txt", "README_EMF.md"] {
        put(&r, f, b"rem\n");
    }
    // the sources of the shipped DLL mods (no DLL), the Lua examples and the Lua starter EMF does not install
    put(&r, "mods/companion/mod.json", br#"{"id":"companion","name":"Companion","version":"1.2.0","emf":20}"#);
    put(&r, "mods/companion/companion.cpp", b"// source\n");
    lua_mod(&r.join("examples/lua/chat_command"), "chat_command", "Chat Commands", "1.0.0");
    lua_mod(&r.join("template_lua"), "my_first_mod", "My First Mod", "1.0.0");
    // EML content (EMF's Emberbrand example): an EML mod, imported as before
    put(&r, "content/emberbrand/mod.json", br#"{"id":"emf_emberbrand","name":"Emberbrand (EMF content example)","version":"1.1.0","capabilities":["patch"]}"#);
    put(&r, "content/emberbrand/src/mod.lua", b"return {}\n");
    r
}

/// A client game folder with Kiln in it, profile paths through Proton's Z: drive (as on the Deck).
fn game(t: &Path) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    GameInstall {
        id: "g-emf-lua".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z, source: "manual".into(),
    }
}

fn ids(refs: &[LibraryRef], kind: ModKind) -> Vec<String> {
    let mut v: Vec<String> = refs.iter().filter(|r| r.kind == kind).map(|r| r.id.clone()).collect();
    v.sort();
    v
}

fn skipped<'a>(r: &'a library::ImportReport, tail: &str) -> &'a str {
    &r.skipped.iter().find(|s| s.path.replace('\\', "/").ends_with(tail))
        .unwrap_or_else(|| panic!("{tail} not among the skipped: {:#?}", r.skipped)).reason
}

fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

fn import_one(dirs: &DataDirs, src: &Path, id: &str) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Emf && x.id == id).cloned()
        .unwrap_or_else(|| panic!("emf {id} not imported: {r:?}"))
}

/// What the drop of EMF's download brings, a zip or its unpacked folder: the shipped mods (the Lua ones too), nothing
/// of what EMF does not install; the Lua mods are placed in emf/mods and the profile names those copies.
fn drop_brings_the_lua_mods(src: impl Fn(&Path) -> PathBuf, tag: &str) {
    let t = tmp(tag);
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0);
    let dropped = src(&t.0);
    let rep = quick::add_with(&dirs, &g, &[dropped], false, &mut NoProgress).unwrap();
    let imp = &rep.import;
    assert_eq!(ids(&imp.imported, ModKind::Emf), ["beastmaster", "big_stacks", "kill_counter", "lua"], "{imp:#?}");
    assert_eq!(ids(&imp.imported, ModKind::Eml), ["emf_emberbrand"], "EML content stays EML: {imp:#?}");
    assert!(!imp.skipped.iter().any(|s| s.reason.contains("does not describe a recognised mod package")), "{:#?}", imp.skipped);
    let beast = imp.imported.iter().find(|r| r.id == "beastmaster").unwrap();
    assert_eq!(beast.version, "1.17.1");
    assert_eq!(imp.names.get("emf/beastmaster").map(String::as_str), Some("Beastmaster"));
    // what EMF does not install is not added, and the reason says why
    for tail in ["examples/lua/chat_command", "template_lua"] {
        assert!(skipped(imp, tail).contains("example in EMF's download, which EMF does not install"), "{tail}: {:#?}", imp.skipped);
    }
    // the sources of EMF's DLL mods (no DLL): left out as one folder, as before
    assert!(!skipped(imp, "EMF-0.6.8/mods").is_empty());
    assert!(rep.applied, "{rep:#?}");
    // the Lua mods run where EMF's lua mod reads them: Kiln's copies in emf/mods/<id>/, with its marker
    for id in ["beastmaster", "kill_counter"] {
        let copy = g.path.join("emf/mods").join(id);
        assert!(copy.join("main.lua").is_file() && copy.join("tame.lua").is_file(), "{id}: {rep:#?}");
        let m = engine_mods::marker(&copy).unwrap_or_else(|| panic!("{id}: no marker"));
        assert_eq!((m.kind, m.id.as_str()), (ModKind::Emf, id));
    }
    // EMF's DLL mods run from the Library, as before
    assert!(!g.path.join("emf/mods/lua").exists() && !g.path.join("emf/mods/big_stacks").exists());
    let file = profile::read_game_profile(&g.path).unwrap().unwrap();
    let path = |id: &str| file.mods.emf.iter().find(|m| m.id == id).unwrap_or_else(|| panic!("{id} not in profile.json")).path.clone();
    assert!(path("beastmaster").ends_with(r"\game\emf\mods\beastmaster"), "{}", path("beastmaster"));
    assert!(path("kill_counter").ends_with(r"\game\emf\mods\kill_counter"), "{}", path("kill_counter"));
    assert!(path("lua").contains(r"\library\emf\lua\"), "{}", path("lua"));
    assert!(file.mods.emf.iter().all(|m| m.enabled), "{:?}", file.mods.emf);
}

#[test]
fn emf_release_folder_brings_its_lua_mods() {
    drop_brings_the_lua_mods(emf_release, "folder");
}

#[cfg(feature = "zip")]
#[test]
fn emf_release_zip_brings_its_lua_mods() {
    use std::io::Write;
    drop_brings_the_lua_mods(|t| {
        let dir = emf_release(&t.join("unpacked"));
        let zip = t.join("EMF-0.6.8.zip");
        // entries as EMF's Write-Zip names them: EMF-0.6.8/<path>, '/' separated
        let mut w = zip::ZipWriter::new(std::fs::File::create(&zip).unwrap());
        let mut stack = vec![dir.clone()];
        let mut files = Vec::new();
        while let Some(d) = stack.pop() {
            for e in std::fs::read_dir(&d).unwrap() {
                let p = e.unwrap().path();
                if p.is_dir() { stack.push(p) } else { files.push(p) }
            }
        }
        files.sort();
        for f in files {
            let name = format!("EMF-0.6.8/{}", f.strip_prefix(&dir).unwrap().to_string_lossy());
            w.start_file(name, zip::write::SimpleFileOptions::default()).unwrap();
            w.write_all(&std::fs::read(&f).unwrap()).unwrap();
        }
        w.finish().unwrap();
        std::fs::remove_dir_all(t.join("unpacked")).unwrap();
        zip
    }, "zip");
}

#[test]
fn a_lua_mod_folder_on_its_own_is_an_emf_mod() {
    let t = tmp("one");
    // the player's emf/mods/beastmaster, an example, and the starter: each dropped on its own is an EMF mod
    for (rel, id) in [("emf/mods/beastmaster", "beastmaster"), ("examples/lua/chat_command", "chat_command"), ("template_lua", "my_first_mod")] {
        let d = t.0.join(rel);
        lua_mod(&d, id, "N", "1.0.0");
        let found = library::detect(&d).unwrap();
        assert_eq!((found.kind, found.id.as_deref()), (DetectKind::Emf, Some(id)), "{found:?}");
        assert!(found.reason.contains("an EMF Lua mod (mod.json, main.lua, EMF API 14)"), "{}", found.reason);
    }
    // what EMF itself refuses or does not run stays out, with a reason that names EMF
    for (tag, manifest, why) in [
        ("source", r#"{"id":"companion","version":"1.2.0","emf":20}"#, "an EMF mod without its DLL (companion.dll)"),
        ("no-version", r#"{"id":"x","emf":14,"main":"main.lua"}"#, "no id or no version"),
        ("both", r#"{"id":"x","version":"1.0.0","emf":14,"main":"main.lua","dll":"x.dll"}"#, "both a DLL and a main file"),
        ("not-lua", r#"{"id":"x","version":"1.0.0","emf":14,"main":"main.js"}"#, "is not Lua"),
        ("no-main", r#"{"id":"x","version":"1.0.0","emf":14,"main":"gone.lua"}"#, "without its main file (gone.lua)"),
        ("api", r#"{"id":"x","version":"1.0.0","emf":"14","main":"main.lua"}"#, "\"emf\" as something other than a whole number"),
    ] {
        let d = t.0.join("odd").join(tag);
        put(&d, "mod.json", manifest.as_bytes());
        put(&d, "main.lua", b"");
        put(&d, "main.js", b"");
        let found = library::detect(&d).unwrap();
        assert_eq!(found.kind, DetectKind::Unknown, "{tag}: {found:?}");
        assert!(found.reason.contains(why), "{tag}: {}", found.reason);
    }
}

#[test]
fn the_lua_mods_copy_follows_the_profile() {
    let t = tmp("place");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0);
    let beast_src = t.0.join("src/beastmaster");
    lua_mod(&beast_src, "beastmaster", "Beastmaster", "1.17.1");
    let host_src = t.0.join("src/lua");
    lua_host(&host_src);
    let beast = import_one(&dirs, &beast_src, "beastmaster");
    let host = import_one(&dirs, &host_src, "lua");
    let mut p = profile::create(&dirs, "lua", None).unwrap();
    p.mods.emf = vec![pm(&beast, true), pm(&host, true)];
    let copy = g.path.join("emf/mods/beastmaster");

    // on: copied, without EMF's own loader; the DLL mod is not
    let rep = engine_mods::place(&dirs, &p, &g.path).unwrap();
    assert_eq!(rep.placed, ["emf:beastmaster"], "{rep:#?}");
    assert!(copy.join("main.lua").is_file() && !g.path.join("emf/mods/lua").exists());
    assert_eq!(engine_mods::game_path_of(&dirs, ModKind::Emf, &p.mods.emf[0], &g.path), Some(copy.clone()));
    assert_eq!(engine_mods::game_path_of(&dirs, ModKind::Emf, &p.mods.emf[1], &g.path), None, "a DLL mod runs from the Library");
    // again: kept as it is
    let rep = engine_mods::place(&dirs, &p, &g.path).unwrap();
    assert_eq!((rep.placed.len(), rep.kept.clone()), (0, vec!["emf:beastmaster".to_string()]), "{rep:#?}");
    // a file the mod wrote into its copy is the player's: kept when the copy goes
    put(&copy, "notes.txt", b"mine");

    // off: the copy goes, the profile names the Library entry again
    p.mods.emf[0].enabled = false;
    let rep = engine_mods::place(&dirs, &p, &g.path).unwrap();
    assert_eq!(rep.removed, ["emf:beastmaster"], "{rep:#?}");
    assert!(!copy.exists());
    assert_eq!(engine_mods::game_path_of(&dirs, ModKind::Emf, &p.mods.emf[0], &g.path), None);
    assert!(!rep.kept_files.is_empty(), "{rep:#?}");

    // the player's own folder of the mod: left as it is, nothing placed beside it, and the profile runs it from there
    p.mods.emf[0].enabled = true;
    lua_mod(&copy, "beastmaster", "Beastmaster", "1.16.0");
    let rep = engine_mods::place(&dirs, &p, &g.path).unwrap();
    assert_eq!((rep.placed.len(), rep.own.clone()), (0, vec!["emf:beastmaster".to_string()]), "{rep:#?}");
    assert!(engine_mods::marker(&copy).is_none());
    assert_eq!(engine_mods::game_path_of(&dirs, ModKind::Emf, &p.mods.emf[0], &g.path), Some(copy.clone()));
    // ... under another folder name too
    std::fs::rename(&copy, g.path.join("emf/mods/Beastmaster-1.16")).unwrap();
    let rep = engine_mods::place(&dirs, &p, &g.path).unwrap();
    assert_eq!((rep.placed.len(), rep.own.clone()), (0, vec!["emf:beastmaster".to_string()]), "{rep:#?}");
    assert_eq!(engine_mods::game_path_of(&dirs, ModKind::Emf, &p.mods.emf[0], &g.path), Some(g.path.join("emf/mods/Beastmaster-1.16")));
    assert!(!copy.exists());
}

#[test]
fn copy_mode_names_the_copy_under_kiln_profiles() {
    // Flatpak/Snap Steam: profile.json paths stay in kiln/ (no core reads ../emf/mods), and EMF's lua mod reads the Lua
    // files from Kiln's copy in emf/mods, the folder of the same name
    let t = tmp("copy");
    let dirs = DataDirs::at(t.0.join("data"));
    let mut g = game(&t.0);
    g.path_mode = PathMode::Copy;
    g.prefix = None;
    let src = t.0.join("src/beastmaster");
    lua_mod(&src, "beastmaster", "Beastmaster", "1.17.1");
    let beast = import_one(&dirs, &src, "beastmaster");
    let mut p = profile::create(&dirs, "copy", None).unwrap();
    p.mods.emf = vec![pm(&beast, true)];
    profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &g).unwrap();
    let file = profile::read_game_profile(&g.path).unwrap().unwrap();
    assert_eq!(file.mods.emf[0].path, format!("profiles/{}/emf/beastmaster", p.id));
    assert!(g.path.join("kiln").join(&file.mods.emf[0].path).join("main.lua").is_file());
    assert!(g.path.join("emf/mods/beastmaster/main.lua").is_file());
}

#[test]
fn play_once_keeps_the_lua_mods_copies() {
    let t = tmp("once");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0);
    let a_src = t.0.join("src/beastmaster");
    lua_mod(&a_src, "beastmaster", "Beastmaster", "1.17.1");
    let b_src = t.0.join("src/kill_counter");
    lua_mod(&b_src, "kill_counter", "Kill Counter", "1.0.0");
    let a = import_one(&dirs, &a_src, "beastmaster");
    let b = import_one(&dirs, &b_src, "kill_counter");
    let mut p = profile::create(&dirs, "active", None).unwrap();
    p.mods.emf = vec![pm(&a, true)];
    profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &g).unwrap();
    // another Lua selection for one start: refused, the copies in emf/mods are the active profile's
    let mut q = profile::create(&dirs, "once", None).unwrap();
    q.mods.emf = vec![pm(&b, true)];
    profile::save(&dirs, &q).unwrap();
    let e = profile::write_once(&dirs, &q.id, &g).unwrap_err();
    assert!(e.message.contains("EMF's Lua mods run from Kiln's copies in emf/mods"), "{}", e.message);
    // the same selection: fine
    let mut same = profile::create(&dirs, "same", None).unwrap();
    same.mods.emf = vec![pm(&a, true)];
    profile::save(&dirs, &same).unwrap();
    profile::write_once(&dirs, &same.id, &g).unwrap();
}
