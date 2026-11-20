//! `native-plugin.ini` is two loaders' file (inbox 92, zaunfink's Smart Quick Stack report). ShroudForge's DLL Manager
//! reads `[Plugin] Dll=` as a path inside the package (`Dll=bin/x.dll`, Unified Tweaks) and loads any DLL with the
//! Windows loader. XHL-Core 2.1.0 reads the same file in every `<game>/mods/<folder>/`, refuses a `Dll` value with a
//! path in it, and refuses a DLL without its plugin exports (`XhlNativePluginQuery`, `XhlNativePluginStart`,
//! `XhlNativePluginRequestStop`). Only a folder XHL-Core would run is an XHL-Core plugin.
use kiln_manage::library::{self, DetectKind};
use kiln_manage::pe::build_test_dll;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-xhl-classify-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn kind(dir: &Path) -> (DetectKind, String) {
    let d = library::detect(dir).unwrap();
    (d.kind, d.reason)
}
const MOD_JSON: &[u8] = br#"{"id":"smart-quick-stack","name":"Smart Quick Stack","version":"0.16.1-beta"}"#;
const XHL_EXPORTS: [&str; 3] = ["XhlNativePluginQuery", "XhlNativePluginStart", "XhlNativePluginRequestStop"];

#[test]
fn a_shroudforge_native_plugin_ini_package_is_no_xhl_core_plugin() {
    // Unified Tweaks' shape without its Lua and extended.mod.json: the DLL below the package, as ShroudForge's docs show
    let t = tmp("sf-path");
    let d = t.0.join("smart-quick-stack");
    put(&d, "mod.json", MOD_JSON);
    put(&d, "native-plugin.ini", b"[Plugin]\r\nEnabled=1\r\nDll=bin/smart_quick_stack.dll\r\n");
    put(&d, "bin/smart_quick_stack.dll", &build_test_dll(&["DllMain"], b"a plain DLL mod"));
    let (k, why) = kind(&d);
    assert_ne!(k, DetectKind::Xhl, "XHL-Core refuses a Dll value with a path in it: {why}");
    assert_eq!(k, DetectKind::ShroudForge, "ShroudForge's DLL Manager loads it from mods/<folder>: {why}");

    // the DLL beside the ini, but without XHL-Core's plugin exports
    let t = tmp("sf-bare");
    let d = t.0.join("smart-quick-stack");
    put(&d, "mod.json", MOD_JSON);
    put(&d, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=SmartQuickStack.dll\n");
    put(&d, "SmartQuickStack.dll", &build_test_dll(&["DllMain"], b"a plain DLL mod"));
    put(&d, "enstash.ini", b"sort_at_start = on\n");
    let (k, why) = kind(&d);
    assert_eq!(k, DetectKind::ShroudForge, "{why}");

    // no mod.json: a DLL mod with its files, which Kiln loads itself
    let t = tmp("bare");
    let d = t.0.join("smart-quick-stack");
    put(&d, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=SmartQuickStack.dll\n");
    put(&d, "SmartQuickStack.dll", &build_test_dll(&["DllMain"], b"a plain DLL mod"));
    put(&d, "enstash.ini", b"sort_at_start = on\n");
    let (k, why) = kind(&d);
    assert_eq!(k, DetectKind::BareDll, "{why}");
}

#[test]
fn xhl_core_plugins_stay_xhl_core_plugins() {
    // XHL's 2.1.0 mods: mod.json naming the mod, native-plugin.ini, xhl-plugin.json and the plugin DLL
    let t = tmp("xhl-210");
    let d = t.0.join("XHL-Vein-Mining");
    put(&d, "mod.json", br#"{"id":"XHL-Vein-Mining","name":"XHL Vein Mining","version":"2.1.0"}"#);
    put(&d, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=XHL-Vein-Mining.dll\n");
    put(&d, "xhl-plugin.json", br#"{"schema":1,"id":"xhl.vein-mining","version":"2.1.0"}"#);
    put(&d, "XHL-Vein-Mining.dll", &build_test_dll(&XHL_EXPORTS, b"XHL plugin"));
    assert_eq!(kind(&d).0, DetectKind::Xhl);

    // an XHL plugin without xhl-plugin.json: its exports say it
    let t = tmp("xhl-exports");
    let d = t.0.join("XHL-Free-Climb");
    put(&d, "native-plugin.ini", b"[Plugin]\nEnabled=1\nDll=XHL-Free-Climb.dll\n[Climbing]\nspeed=2\n");
    put(&d, "XHL-Free-Climb.dll", &build_test_dll(&XHL_EXPORTS, b"XHL plugin"));
    assert_eq!(kind(&d).0, DetectKind::Xhl);

    // xhl-plugin.json says it even when the DLL cannot be read as a PE file
    let t = tmp("xhl-json");
    let d = t.0.join("XHL-Remote-Chest");
    put(&d, "mod.json", br#"{"id":"XHL-Remote-Chest","name":"XHL Remote Chest","version":"2.1.0"}"#);
    put(&d, "native-plugin.ini", b"[Plugin]\nDll=XHL-Remote-Chest.dll\n");
    put(&d, "xhl-plugin.json", b"{}");
    put(&d, "XHL-Remote-Chest.dll", b"MZ");
    assert_eq!(kind(&d).0, DetectKind::Xhl);
}
