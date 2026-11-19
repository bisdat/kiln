//! Library detection of mods made for other engines (engines.rs): every kind, the reason a player reads, and the
//! rule that an unsupported kind is never imported as a plain DLL. Temp folders and fake DLLs only.

use kiln_manage::engines::Engine;
use kiln_manage::library::{self, DetectKind};
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-engines-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}
fn put(root: &Path, rel: &str, data: &[u8]) -> PathBuf {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(&p, data).unwrap();
    p
}

fn detect(p: &Path) -> library::Detected {
    library::detect(p).unwrap()
}

#[test]
fn eml_capabilities_and_legacy_extensions_do_not_imply_shroudforge() {
    let t = tmp("eml-real");
    for (tag, ext, kind) in [
        ("plain", None, DetectKind::Eml),
        ("legacy", Some(r#"{"launcher":"EML"}"#), DetectKind::Eml),
        ("sf", Some(r#"{"launcher":"SF"}"#), DetectKind::ShroudForge),
    ] {
        let d = t.0.join(tag);
        put(&d, "mod.json", br#"{"id":"stacks","name":"Stacks","version":"1.0.0","capabilities":["patch","runtime"]}"#);
        put(&d, "src/mod.lua", b"return {}");
        if let Some(ext) = ext { put(&d, "extended.mod.json", ext.as_bytes()); }
        let found = detect(&d);
        assert_eq!(found.kind, kind, "{found:?}");
        assert!(found.reason.contains(if kind == DetectKind::Eml { "EML" } else { "ShroudForge" }));
        assert!(found.reason.contains("Library importer"), "engine_mods imports it: {found:?}");
        assert!(found.kind.importable());
    }
}

#[test]
fn odd_manifests_never_fall_through_to_helper_dll_imports() {
    let t = tmp("odd-manifest");
    for (tag, manifest) in [("broken", b"{".as_slice()), ("unknown", br#"{"id":"odd"}"#.as_slice()),
                            ("caps-only", br#"{"id":"odd","name":"Odd","version":"1.0.0","capabilities":["patch"]}"#.as_slice()),
                            ("lua-emf", br#"{"id":"lua","emf":26,"main":"main.lua"}"#.as_slice())] {
        let d = t.0.join(tag);
        put(&d, "mod.json", manifest);
        put(&d, "helper.dll", &build_test_dll(&["helper"], b""));
        put(&d, "main.lua", b"return {}");
        let found = detect(&d);
        assert_eq!(found.kind, DetectKind::Unknown, "{found:?}");
        assert!(found.items.is_empty(), "unknown package must stay intact: {found:?}");
        assert!(!found.reason.is_empty());
        let report = library::import(&DataDirs::at(t.0.join("data")), &[d], &mut NoProgress).unwrap();
        assert!(report.imported.is_empty() && report.existing.is_empty(), "{report:?}");
        assert_eq!(report.skipped.len(), 1);
    }
    let d = t.0.join("no-manifest");
    put(&d, "src/mod.lua", b"return {}");
    put(&d, "helper.dll", &build_test_dll(&["helper"], b""));
    let found = detect(&d);
    assert_eq!(found.kind, DetectKind::Unknown);
    assert!(found.items.is_empty());
}

#[test]
fn sf_install_only_reports_its_own_helper_proxies() {
    let t = tmp("sf-proxy-owners");
    put(&t.0, kiln_manage::shroudforge::RUNTIME_DLL, b"runtime");
    put(&t.0, "dbghelp.dll", &build_test_dll(&[], b"EML_LOG_FILE_PATH"));
    put(&t.0, "winmm.dll", &build_test_dll(&[], b"shroudtopia"));
    let sf = kiln_manage::shroudforge::detect(&t.0).unwrap();
    assert!(!sf.dbghelp && !sf.bootstrap);
    for name in ["dbghelp.dll", "winmm.dll"] { put(&t.0, name, &build_test_dll(&["ShroudforgeSetDinput8Provider"], b"")); }
    let sf = kiln_manage::shroudforge::detect(&t.0).unwrap();
    assert!(sf.dbghelp && sf.bootstrap);
}

#[test]
fn own_proxy_without_dinput_export_and_x86_engine_dll_are_not_mods() {
    let t = tmp("dll-identity");
    let own = put(&t.0, "own.dll", &build_test_dll(&["KilnProxyInfo"], b""));
    assert_eq!(detect(&own).kind, DetectKind::Unknown);
    let mut x86 = build_test_dll(&["GreeneyeModStart"], b"");
    x86[0x84..0x86].copy_from_slice(&0x14cu16.to_le_bytes());
    put(&t.0.join("x86"), "mod.dll", &x86);
    assert_eq!(detect(&t.0.join("x86")).kind, DetectKind::Unknown);
}

#[test]
fn malformed_pe_exports_are_bounded_by_file_backed_sections() {
    use kiln_manage::pe::{self, ProxyKind};
    let good = build_test_dll(&["emf_loader_identity"], b"");
    let section = 0x188;
    let mut cases = Vec::new();
    let mut b = good.clone();
    b[section + 20..section + 24].copy_from_slice(&0xfffffff0u32.to_le_bytes());
    cases.push(b);
    let mut b = good.clone();
    b[section + 16..section + 20].copy_from_slice(&40u32.to_le_bytes()); // names live outside the raw section
    cases.push(b);
    let mut b = good.clone();
    b[0x94..0x96].copy_from_slice(&2u16.to_le_bytes()); // optional header no longer covers directories
    cases.push(b);
    let mut b = good.clone();
    b.truncate(0x190); // truncated section table
    cases.push(b);
    let mut b = good;
    b[0x218..0x21c].copy_from_slice(&0xffffffffu32.to_le_bytes()); // unbounded name count
    cases.push(b);
    for b in cases {
        assert!(pe::parse(&b).is_err());
        assert_eq!(pe::classify_bytes(&b).kind, ProxyKind::NotDll);
    }
    let wide: Vec<u8> = "emf-loader-7D3C9F".encode_utf16().flat_map(u16::to_le_bytes).collect();
    assert_eq!(pe::classify_bytes(&build_test_dll(&["ShroudforgeSetDinput8Provider"], &wide)).kind, ProxyKind::Emf);
}

#[cfg(unix)]
#[test]
fn odd_folder_links_and_deep_trees_do_not_recurse_or_import_dll_targets() {
    let t = tmp("links-depth");
    let outside = put(&t.0, "outside.dll", &build_test_dll(&["x"], b""));
    let folder = t.0.join("folder");
    std::fs::create_dir_all(&folder).unwrap();
    std::os::unix::fs::symlink(&folder, folder.join("loop")).unwrap();
    std::os::unix::fs::symlink(&outside, folder.join("helper.dll")).unwrap();
    // Unsafe folder trees fail before recursive detection; do not hide links as ordinary unknown assets.
    assert_eq!(library::detect(&folder).unwrap_err().code, kiln_manage::ErrorCode::Refused);
    assert_eq!(detect(&folder.join("helper.dll")).kind, DetectKind::Unknown);
    let dirs = DataDirs::at(t.0.join("data"));
    assert_eq!(library::import(&dirs, &[folder], &mut NoProgress).unwrap_err().code, kiln_manage::ErrorCode::Refused);
    assert!(library::list(&dirs, None).unwrap().is_empty());
    let root = t.0.join("deep");
    let mut d = root.clone();
    for _ in 0..40 { d.push("level"); }
    put(&d, "helper.dll", &build_test_dll(&[], b""));
    assert_eq!(library::detect(&root).unwrap_err().code, kiln_manage::ErrorCode::Refused);
    assert_eq!(library::import(&dirs, &[root], &mut NoProgress).unwrap_err().code, kiln_manage::ErrorCode::Refused);
    assert!(library::list(&dirs, None).unwrap().is_empty());
}

#[test]
fn every_engine_kind_is_detected_with_its_reason() {
    let t = tmp("kinds");
    let r = &t.0;
    let cases: Vec<(PathBuf, DetectKind, Option<Engine>, &str)> = vec![
        // Flame (Creative Mode): flame.json in the mod folder
        ({
            let d = r.join("creative");
            put(&d, "mod.json", br#"{"id":"creative","name":"Creative Mode","version":"1.4.6"}"#);
            put(&d, "flame.json", br#"{"dll":"creative.dll"}"#);
            put(&d, "creative.dll", &build_test_dll(&["FlameStartRuntime"], b""));
            put(&d, "sigs/26430625.json", b"{}");
            d
        }, DetectKind::Flame, Some(Engine::Flame), "Creative Mode: a Flame mod (Library importer: kiln-flame)"),
        // ShroudForge package: EML layout + extended.mod.json
        ({
            let d = r.join("sfpkg");
            put(&d, "mod.json", br#"{"id":"sfpkg","name":"SF Pack","version":"1.0.0"}"#);
            put(&d, "extended.mod.json", br#"{"enabled":true}"#);
            put(&d, "src/mod.lua", b"-- lua");
            d
        }, DetectKind::ShroudForge, Some(Engine::ShroudForge), "ShroudForge"),
        // EML mod: mod.json + src/mod.lua, nothing of ShroudForge's
        ({
            let d = r.join("emlmod");
            put(&d, "mod.json", br#"{"id":"emlmod","name":"Bigger Stacks","version":"0.1.0"}"#);
            put(&d, "src/mod.lua", b"-- lua");
            d
        }, DetectKind::Eml, Some(Engine::Eml), "Bigger Stacks"),
        // Enshwick data pack
        ({
            let d = r.join("author.pack");
            put(&d, "pack.toml", b"id = \"author.pack\"\n");
            d
        }, DetectKind::Enshwick, Some(Engine::Enshwick), "Enshwick"),
        // Enshwick DLL mod (with a helper DLL of its own: the folder is the mod)
        ({
            let d = r.join("author.lockers");
            put(&d, "lockers.dll", &build_test_dll(&["enshwick_mod_abi", "enshwick_mod_query", "enshwick_mod_init"], b""));
            put(&d, "helper.dll", &build_test_dll(&["helper"], b""));
            d
        }, DetectKind::Enshwick, Some(Engine::Enshwick), "author.lockers: an Enshwick mod (Library importer: kiln-enshwick)"),
        // Enshwick Luau mod: pack.toml names it
        ({
            let d = r.join("luau-folder");
            put(&d, "pack.toml", b"id = \"author.greeter\"\nversion = \"0.2.0\"\nname = \"Greeter\"\n");
            put(&d, "main.luau", b"print('hi')");
            d
        }, DetectKind::Enshwick, Some(Engine::Enshwick), "Greeter: an Enshwick mod"),
        // shroudlink mod DLL
        (put(r, "sl/chestsort.dll", &build_test_dll(&["shroudlink_mod_v2"], b"")), DetectKind::Shroudlink, Some(Engine::Shroudlink),
         "chestsort.dll: a shroudlink mod (Library importer: kiln-shroudlink)"),
        // Ember mod: one library in ember/mods/<mod>/ (no ABI: the place names it), with the note that it doesn't run yet
        ({
            let d = r.join("server/ember/mods/private-chests");
            put(&d, "private_chests.dll", &build_test_dll(&[], b""));
            put(&d, "config.json", b"{}");
            d
        }, DetectKind::Ember, Some(Engine::Ember), "It doesn't run yet: Ember has not published its mod ABI"),
        // SET mod
        ({
            let d = r.join("set-mod");
            put(&d, "init.lua", b"print('hi')");
            put(&d, "mod.json", br#"{"name":"Fly Mode"}"#);
            d
        }, DetectKind::Set, Some(Engine::Set), "Fly Mode: a Shrouded Engine Tweaks mod"),
        // Greeneye mod
        ({
            let d = r.join("worstmap");
            put(&d, "mod.json", br#"{"id":"worstmap","name":"The Worst Map Mod","version":"1.0.0","dll":"map.dll","side":"client","api":2}"#);
            put(&d, "map.dll", &build_test_dll(&["GreeneyeModStart"], b""));
            d
        }, DetectKind::Greeneye, Some(Engine::Greeneye), "The Worst Map Mod: a Greeneye Loader mod"),
        // Shroudtopia mod DLL
        (put(r, "loose/minimap.dll", &build_test_dll(&["CreateModInstance"], b"")), DetectKind::Shroudtopia, Some(Engine::Shroudtopia),
         "minimap.dll: a Shroudtopia mod"),
        // loaders under proxy names
        (put(r, "eml/dbghelp.dll", &build_test_dll(&["MiniDumpWriteDump"], b"EML_LOG_FILE_PATH")), DetectKind::Proxy, Some(Engine::Eml),
         "EML's own loader is in this package: not needed, Kiln runs EML mods"),
        (put(r, "gl/dbgcore.dll", &build_test_dll(&["MiniDumpWriteDump"], b"Greeneye Loader")), DetectKind::Proxy, Some(Engine::Greeneye),
         "Greeneye's own loader is in this package: not needed, Kiln runs Greeneye mods"),
        (put(r, "emf/dinput8.dll", &build_test_dll(&["DirectInput8Create", "ShroudforgeSetDinput8Provider", "emf_loader_identity"], b"")),
         DetectKind::Proxy, Some(Engine::Emf), "EMF's own loader"),
        (put(r, "sf/dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"shroudforge/shroudforge-runtime.dll")), DetectKind::Proxy,
         Some(Engine::ShroudForge), "ShroudForge's own loader"),
        (put(r, "lm/iphlpapi.dll", &build_test_dll(&["GetAdaptersInfo"], b"livemap.ini")), DetectKind::Proxy, Some(Engine::LiveMap),
         "copy it into the server folder"),
        // files that are no mods
        (put(r, "f/3ad85aea_info", b"x"), DetectKind::Save, None, "a save file"),
        (put(r, "f/world.ksd", b"x"), DetectKind::Save, None, "a save file"),
        (put(r, "f/Enshrouded.CT", b"x"), DetectKind::CheatTable, None, "Cheat Engine table"),
        (put(r, "f/MyReShade.ini", b"x"), DetectKind::Reshade, Some(Engine::ReShade), "ReShade preset"),
        // OptiScaler: its DLL under its own names, its settings and its .asi build; a tool that runs on its own
        (put(r, "os/dxgi.dll", &build_test_dll(&["CreateDXGIFactory"], b"OptiScaler.ini ReShade64.dll")), DetectKind::Proxy,
         Some(Engine::OptiScaler), "is OptiScaler, which runs on its own next to Kiln"),
        (put(r, "os2/OptiScaler.dll", &build_test_dll(&["CreateDXGIFactory"], &utf16("OptiScaler.ini"))), DetectKind::Proxy,
         Some(Engine::OptiScaler), "is OptiScaler"),
        (put(r, "os3/d3d12.dll", &build_test_dll(&["D3D12CreateDevice"], b"OptiScaler.log")), DetectKind::Proxy,
         Some(Engine::OptiScaler), "is OptiScaler"),
        (put(r, "f/OptiScaler.ini", b"[Menu]\n"), DetectKind::Proxy, Some(Engine::OptiScaler), "is OptiScaler"),
        (put(r, "f/OptiScaler.asi", b"x"), DetectKind::Proxy, Some(Engine::OptiScaler), "is OptiScaler"),
        (put(r, "f/data_0001.kfc", b"x"), DetectKind::Data, None, "patched game data file"),
        (put(r, "f/plugin.asi", b"x"), DetectKind::Asi, Some(Engine::UltimateAsiLoader), "Ultimate ASI Loader"),
    ];
    for (path, kind, engine, words) in cases {
        let d = detect(&path);
        assert_eq!((d.kind, d.engine), (kind, engine), "{}: {}", path.display(), d.reason);
        assert!(d.reason.contains(words), "{}: {:?} lacks {words:?}", path.display(), d.reason);
        assert!(!d.reason.ends_with('.'), "{}: the launcher adds the period: {:?}", path.display(), d.reason);
        // importable: every engine mod kind (engine_mods); never loaders or files, except (dev.30) .asi plugins, which the
        // Library takes as a native mod that activation places in the game folder (game_files, tests/game_files.rs)
        let runs = kiln_manage::manifest::ModKind::of_detect(d.kind).is_some_and(|k| kiln_manage::engine_mods::IMPORTED.contains(&k))
            || d.kind == DetectKind::Asi;
        assert_eq!(d.kind.importable(), runs, "{}: {:?}", path.display(), d.kind);
    }
    // Kiln's own kinds are untouched: a stand-alone dinput8 mod and a plain DLL stay bare DLLs
    assert_eq!(detect(&put(r, "s/dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"sprint toggle"))).kind, DetectKind::BareDll);
    assert_eq!(detect(&put(r, "s/fps.dll", &build_test_dll(&["x"], b"mentions reshade"))).kind, DetectKind::BareDll);
    // a DLL mod that merely mentions OptiScaler.ini under its own name stays a DLL mod
    assert_eq!(detect(&put(r, "s/upscale-helper.dll", &build_test_dll(&["x"], b"reads OptiScaler.ini"))).kind, DetectKind::BareDll);
}

/// Creative Mode's zip as it is laid out: the Flame loader as dinput8.dll and mods/creative/. Before, both were imported
/// as plain DLLs that never started; now the mod goes into the library as a Flame mod (kiln-flame runs it) and the
/// loader is skipped with its reason.
#[cfg(feature = "zip")]
#[test]
fn creative_mode_is_never_imported_as_plain_dlls() {
    let t = tmp("creative");
    let src = t.0.join("CreativeMode");
    put(&src, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"FlameStartRuntime\0mods\\\0flame.json"));
    put(&src, "mods/creative/mod.json", br#"{"id":"creative","name":"Creative Mode","version":"1.4.6"}"#);
    put(&src, "mods/creative/flame.json", br#"{"dll":"creative.dll"}"#);
    put(&src, "mods/creative/creative.dll", &build_test_dll(&["FlameStartRuntime"], b""));
    put(&src, "mods/creative/symbols/a.json", b"{}");
    let d = detect(&src);
    assert_eq!(d.kind, DetectKind::Unknown);
    let kinds: Vec<(DetectKind, Option<Engine>)> = d.items.iter().map(|i| (i.kind, i.engine)).collect();
    assert_eq!(kinds, [(DetectKind::Proxy, Some(Engine::Flame)), (DetectKind::Flame, Some(Engine::Flame))]);
    // the loader in it is "not needed", never something "Kiln can't load yet" (a player's report: Find's zip with
    // Greeneye Loader's dbgcore.dll read as if a part of the mod could not be loaded)
    assert_eq!(d.reason, "a folder with 1 importable item(s); 1 loader(s) in it are not needed");
    let dirs = DataDirs::at(t.0.join("data"));
    let rep = library::import(&dirs, std::slice::from_ref(&src), &mut NoProgress).unwrap();
    assert_eq!(rep.imported.iter().map(|r| (r.kind, r.id.as_str())).collect::<Vec<_>>(), [(kiln_manage::manifest::ModKind::Flame, "creative")], "{rep:?}");
    // Flame's own loader is information, not a failure: Kiln runs Flame mods itself
    assert!(rep.skipped.is_empty(), "{:?}", rep.skipped);
    assert_eq!(rep.notes.iter().map(|n| n.text.as_str()).collect::<Vec<_>>(), ["Flame's own loader is in this package: not needed, Kiln runs Flame mods"]);
    let l = library::list(&dirs, None).unwrap();
    assert_eq!(l.len(), 1);
    assert!(l[0].versions[0].folder.join("creative.dll").is_file() && l[0].versions[0].folder.join("flame.json").is_file());
}


fn utf16(s: &str) -> Vec<u8> {
    s.encode_utf16().flat_map(|c| c.to_le_bytes()).collect()
}

/// OptiScaler in the game folder (engines::optiscaler_in, game::graphics_tools): found by its contents under any of its
/// names, never by a name alone; the proxy-name rules other loaders share stay as they were.
#[test]
fn optiscaler_in_the_game_folder_is_found_by_its_contents() {
    use kiln_manage::engines::{optiscaler_in, scan_proxies};
    use kiln_manage::pe::ProxyKind;
    let t = tmp("optiscaler-game");
    let g = &t.0;
    assert_eq!(optiscaler_in(g), None);
    assert!(kiln_manage::game::graphics_tools(g).is_empty());
    // its settings left behind by an uninstall, ReShade's dxgi.dll, Shroudtopia's winmm.dll and a plain version.dll
    put(g, "OptiScaler.ini", b"[Menu]\n");
    put(g, "dxgi.dll", &build_test_dll(&["CreateDXGIFactory"], b"ReShade 6.3"));
    put(g, "winmm.dll", &build_test_dll(&["timeGetTime"], b"shroudtopia.dll"));
    put(g, "version.dll", &build_test_dll(&["GetFileVersionInfoW"], b"a mod's own proxy"));
    put(g, "d3d12.dll", &build_test_dll(&["D3D12CreateDevice"], b"not OptiScaler"));
    assert_eq!(optiscaler_in(g), None, "no OptiScaler DLL: nothing");
    let kinds = |g: &Path| scan_proxies(g).into_iter().map(|s| (s.name, s.class.kind)).collect::<Vec<_>>();
    let before = kinds(g);
    assert_eq!(before, [("winmm.dll".to_string(), ProxyKind::Shroudtopia), ("version.dll".to_string(), ProxyKind::Unknown),
        ("dxgi.dll".to_string(), ProxyKind::ReShade)]);
    // OptiScaler as d3d12.dll (no proxy name of a mod loader): found, and scan_proxies does not list it
    put(g, "d3d12.dll", &build_test_dll(&["D3D12CreateDevice"], b"OptiScaler.ini"));
    assert_eq!(optiscaler_in(g).as_deref(), Some("d3d12.dll"));
    assert_eq!(kinds(g), before);
    std::fs::remove_file(g.join("d3d12.dll")).unwrap();
    // OptiScaler as winmm.dll instead of Shroudtopia: the proxy scan names it (Kiln's installer leaves it in place)
    put(g, "winmm.dll", &build_test_dll(&["timeGetTime"], &utf16("OptiScaler.ini")));
    assert_eq!(optiscaler_in(g).as_deref(), Some("winmm.dll"));
    assert_eq!(kinds(g)[0], ("winmm.dll".to_string(), ProxyKind::OptiScaler));
    let notes = kiln_manage::game::graphics_tools(g);
    assert_eq!(notes.len(), 1);
    assert_eq!((notes[0].engine, notes[0].file.as_str()), (Engine::OptiScaler, "winmm.dll"));
    assert!(notes[0].text.contains("KILN_UI_OVERLAY"), "{}", notes[0].text);
    let json = serde_json::to_value(&notes[0]).unwrap();
    assert_eq!(json["engine"], "optiscaler");
    assert!(!notes[0].kiln_overlay && notes[0].text.contains("steps aside") && notes[0].text.contains("OverlayMenu=false"));
    // its own menu off in its ini: Kiln's overlay draws next to it (kiln.ui decides the same from the same ini)
    put(g, "OptiScaler.ini", b"[Menu]\r\nOverlayMenu=false\r\n");
    let notes = kiln_manage::game::graphics_tools(g);
    assert!(notes[0].kiln_overlay && notes[0].text.contains("draws next to it"), "{}", notes[0].text);
    assert_eq!(serde_json::to_value(&notes[0]).unwrap()["kilnOverlay"], true);
    // Inbox 80: OptiScaler's real DLL carries ReShade's texts (it can load ReShade) long before its own (0.9.5's
    // OptiScaler.dll: "reshade" at 24.59 MB, "optiscaler.ini" at 24.88 MB in UTF-16 and 24.95 MB in ASCII). Under winmm.dll
    // it is OptiScaler, whatever comes first in the file; 29.17's report named it "ReShade (text reshade)".
    for own in [utf16("OptiScaler.ini"), utf16("OptiScaler.log"), b"OptiScaler.ini".to_vec()] {
        let mut text = b"ReShade 6.4 reshade64.dll ReShade.ini ".to_vec();
        text.extend(utf16("ReShade"));
        text.extend(std::iter::repeat_n(0u8, 4096));
        text.extend(&own);
        put(g, "winmm.dll", &build_test_dll(&["timeGetTime", "PlaySoundW"], &text));
        assert_eq!(kinds(g)[0], ("winmm.dll".to_string(), ProxyKind::OptiScaler));
        assert_eq!(optiscaler_in(g).as_deref(), Some("winmm.dll"));
    }
    // ReShade's texts alone under winmm.dll stay ReShade's
    put(g, "winmm.dll", &build_test_dll(&["timeGetTime"], b"ReShade 6.4 reshade64.dll"));
    assert_eq!(kinds(g)[0], ("winmm.dll".to_string(), ProxyKind::ReShade));
    assert_eq!(optiscaler_in(g), None);
    // its .asi build, loaded by Ultimate ASI Loader
    std::fs::remove_file(g.join("winmm.dll")).unwrap();
    put(g, "OptiScaler.asi", &build_test_dll(&["x"], b"OptiScaler.log"));
    assert_eq!(optiscaler_in(g).as_deref(), Some("OptiScaler.asi"));
}

/// OptiScaler.ini as kiln.ui reads it (native/services/ui/overlay_optiscaler.h, its host test has the same cases).
#[test]
fn optiscaler_ini_decides_kilns_overlay() {
    use kiln_manage::engines::OptiScalerIni;
    let stock = OptiScalerIni::parse("; OptiScaler.ini as it ships\n[FrameGen]\nEnabled=auto\nFGInput=auto\nFGOutput=auto\n[Menu]\nOverlayMenu=auto\n");
    assert_eq!(stock, OptiScalerIni { overlay_menu: None, frame_gen: false });
    assert!(!stock.kiln_overlay_draws(), "OptiScaler's menu is on by default");
    let off = OptiScalerIni::parse("\u{feff}[menu]\r\n  overlaymenu = FALSE \r\n");
    assert!(off.kiln_overlay_draws());
    assert_eq!(OptiScalerIni::parse("[Hotfix]\nOverlayMenu=false\n[Menu]\n#OverlayMenu=false\n").overlay_menu, None);
    let fg = OptiScalerIni::parse("[Menu]\nOverlayMenu=false\n[FrameGen]\nFGInput=upscaler\nFGOutput=fsrfg\n");
    assert!(fg.frame_gen && !fg.kiln_overlay_draws(), "frame generation: Kiln steps aside");
    assert!(!OptiScalerIni::parse("[FrameGen]\nFGInput=upscaler\nFGOutput=nofg\n").frame_gen);
    // Inbox 80: its menu off, frame generation set as a nightly's ini has it (CRLF, [FrameGen] before [Menu], Enabled
    // off: the stand-in swapchain and the own present queue are made with Enabled=false too). Every Vulkan output its
    // master knows, with either input, makes frame generation; an output alone or an input alone does not.
    for (input, output) in [("upscaler", "fsrfg"), ("dlssg", "fsrfg"), ("upscaler", "dlssg"), ("Upscaler", "Reprojection")] {
        let ini = format!("[FrameGen]\r\nEnabled=false\r\nFGInput={input}\r\nFGOutput={output}\r\n[Menu]\r\nOverlayMenu=false\r\n");
        let parsed = OptiScalerIni::parse(&ini);
        assert!(parsed.overlay_menu == Some(false) && parsed.frame_gen && !parsed.kiln_overlay_draws(), "{input} -> {output}");
        let note = kiln_manage::engines::optiscaler_overlay_note(&parsed);
        assert!(note.contains("frame generation is set") && note.contains("own queue") && note.contains("FGInput and FGOutput to auto"), "{note}");
    }
    for ini in ["[FrameGen]\nFGInput=auto\nFGOutput=fsrfg\n[Menu]\nOverlayMenu=false\n", "[FrameGen]\nFGInput=upscaler\n[Menu]\nOverlayMenu=false\n",
                "[FrameGen]\nEnabled=true\n[Menu]\nOverlayMenu=false\n"] {
        assert!(OptiScalerIni::parse(ini).kiln_overlay_draws(), "{ini}");
    }
    // the menu-on note says to leave frame generation off too; the drawing note says both are off
    let menu_on = kiln_manage::engines::optiscaler_overlay_note(&stock);
    assert!(menu_on.contains("OverlayMenu=false") && menu_on.contains("FGInput and FGOutput") && !menu_on.contains("go off with it"), "{menu_on}");
    assert!(kiln_manage::engines::optiscaler_overlay_note(&off).contains("no frame generation is set"));
    assert_eq!(OptiScalerIni::parse("[Menu]\nOverlayMenu=false\nOverlayMenu=true\n").overlay_menu, Some(true));
    // no ini: the defaults
    let t = tmp("optiscaler-ini");
    assert_eq!(OptiScalerIni::read(&t.0), OptiScalerIni::default());
    put(&t.0, "optiscaler.INI", b"[Menu]\nOverlayMenu=false\n");
    assert!(OptiScalerIni::read(&t.0).kiln_overlay_draws(), "its name in any case");
    // larger than kiln.ui reads: neither reads it, both take OptiScaler's menu for on
    let big = tmp("optiscaler-ini-big");
    let mut text = b"[Menu]\nOverlayMenu=false\n".to_vec();
    text.resize(kiln_manage::engines::OPTISCALER_INI_MAX as usize + 1, b'\n');
    put(&big.0, "OptiScaler.ini", &text);
    assert_eq!(OptiScalerIni::read(&big.0), OptiScalerIni::default());
    text.truncate(kiln_manage::engines::OPTISCALER_INI_MAX as usize);
    put(&big.0, "OptiScaler.ini", &text);
    assert!(OptiScalerIni::read(&big.0).kiln_overlay_draws(), "up to the limit it is read");
}

/// Enshrouded ModLoader's packages (AndyPimms' loader, SDK ABI 1; github.com/aebischer/Enshrouded-ModLoader) run only
/// from that loader, at a world's start. Imported as plain DLLs, Kiln would load them early and that loader would then
/// refuse every world with the mod switched on (it never starts a DLL already in the process). Its own dinput8.dll
/// stays a DLL mod Kiln chain-loads.
#[test]
fn enshrouded_modloader_packages_are_never_plain_dlls() {
    let t = tmp("modloader");
    let r = &t.0;
    // a kind=mod package as its zip lays it out, a loose mod DLL (its export alone says it), and a kind=library
    // package (no SDK export: only its manifest says it)
    let carry = put(r, "CarryOn/CarryOn.dll", &build_test_dll(&["EnshroudedModInitialize", "EnshroudedModConfigure"], b""));
    put(r, "CarryOn/CarryOn.dll.mod", b"\xef\xbb\xbfENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=1.0.0\r\nabi=1\r\n");
    let lantern = put(r, "loose/BeltLantern.dll", &build_test_dll(&["EnshroudedModInitialize"], b""));
    let lib = put(r, "libs/shared.dll", &build_test_dll(&["SharedThing"], b""));
    put(r, "libs/shared.dll.mod", b"ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1.0.0\nabi=1\n");
    for p in [&carry, &lantern, &lib] {
        let d = detect(p);
        assert_eq!((d.kind, d.engine), (DetectKind::Unknown, None), "{}: {}", p.display(), d.reason);
        assert!(d.reason.contains("a mod for Enshrouded ModLoader") && d.reason.contains("Edit > MODS"), "{}", d.reason);
        assert!(!d.reason.ends_with('.'), "the launcher adds the period: {:?}", d.reason);
        assert!(!d.kind.importable());
    }
    // another .mod file beside a plain DLL changes nothing
    let fps = put(r, "other/fps.dll", &build_test_dll(&["x"], b""));
    put(r, "other/fps.dll.mod", b"[settings]\n");
    assert_eq!(detect(&fps).kind, DetectKind::BareDll);
    // its loader as its release ships it (dist/dinput8.dll): a DLL mod chain-loaded at dllmain, as before
    let loader = put(r, "Enshrouded-ModLoader/dist/dinput8.dll", &build_test_dll(&["DirectInput8Create", "GetdfDIJoystick"], &utf16("mod-loader.log")));
    assert_eq!(detect(&loader).kind, DetectKind::BareDll);
    // importing the mod's folder imports nothing and says why
    let dirs = DataDirs::at(r.join("data"));
    let rep = library::import(&dirs, &[r.join("CarryOn"), lantern.clone()], &mut NoProgress).unwrap();
    assert!(rep.imported.is_empty() && rep.existing.is_empty(), "{rep:?}");
    assert!(!rep.skipped.is_empty(), "{rep:?}");
    assert!(format!("{rep:?}").contains("a mod for Enshrouded ModLoader"), "{rep:?}");
    assert!(library::list(&dirs, None).unwrap().is_empty());
}
