//! game::graphics_step_aside: the tools in a game folder next to which Kiln's graphics tools (kiln.json "graphics")
//! step aside, each kind, names in any case, the game's own nvngx_dlss.dll, and the order. Temp folders and fake DLLs.

use kiln_manage::game::{graphics_step_aside, GraphicsStepAside};
use kiln_manage::pe::build_test_dll;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-gfx-aside-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(&p, data).unwrap();
}
fn aside(kind: &str, file: &str, frame_cap: bool) -> GraphicsStepAside {
    // the tools that take the frame cap take Kiln's upscaler swap too (kiln.ui's kAsideUpscale)
    GraphicsStepAside { kind: kind.into(), file: file.into(), frame_cap, upscaler: frame_cap }
}

#[test]
fn an_empty_folder_and_the_games_own_files_give_nothing() {
    let t = tmp("empty");
    assert!(graphics_step_aside(&t.0).is_empty());
    assert!(graphics_step_aside(&t.0.join("missing")).is_empty(), "a folder that is not there");
    // the game ships nvngx_dlss.dll; OptiScaler.ini alone is a leftover; a folder named like a file is not one
    put(&t.0, "nvngx_dlss.dll", b"x");
    put(&t.0, "OptiScaler.ini", b"[Menu]\n");
    std::fs::create_dir_all(t.0.join("nvngx.dll")).unwrap();
    put(&t.0, "Streamline/other.dll", b"x");
    assert!(graphics_step_aside(&t.0).is_empty());
}

#[test]
fn optiscaler_by_its_contents() {
    let t = tmp("optiscaler");
    put(&t.0, "dxgi.dll", &build_test_dll(&["CreateDXGIFactory"], b"ReShade 6.3"));
    assert!(graphics_step_aside(&t.0).is_empty(), "ReShade's dxgi.dll is not OptiScaler");
    put(&t.0, "dxgi.dll", &build_test_dll(&["CreateDXGIFactory"], b"OptiScaler.ini"));
    assert_eq!(graphics_step_aside(&t.0), [aside("optiscaler", "dxgi.dll", true)]);
}

#[test]
fn streamline_in_the_folder_or_its_streamline_folder() {
    let t = tmp("streamline");
    put(&t.0, "streamline/SL.Interposer.DLL", b"x");
    assert_eq!(graphics_step_aside(&t.0), [aside("streamline", "streamline/SL.Interposer.DLL", true)]);
    put(&t.0, "sl.interposer.dll", b"x");
    assert_eq!(graphics_step_aside(&t.0), [aside("streamline", "sl.interposer.dll", true)], "the game folder's first");
}

#[test]
fn vkfg_dlsstweaks_and_nvngx_in_any_case() {
    let t = tmp("cases");
    put(&t.0, "enshroudedvkfg.INI", b"");
    assert_eq!(graphics_step_aside(&t.0), [aside("vkfg", "enshroudedvkfg.INI", true)]);
    std::fs::remove_file(t.0.join("enshroudedvkfg.INI")).unwrap();
    put(&t.0, "DLSSTweaks.ini", b"");
    assert_eq!(graphics_step_aside(&t.0), [aside("dlsstweaks", "DLSSTweaks.ini", false)]);
    std::fs::remove_file(t.0.join("DLSSTweaks.ini")).unwrap();
    put(&t.0, "_NVNGX.dll", b"x");
    put(&t.0, "nvngx_dlss.dll", b"x");
    assert_eq!(graphics_step_aside(&t.0), [aside("nvngx", "_NVNGX.dll", false)]);
    put(&t.0, "NvNgx.Dll", b"x");
    assert_eq!(graphics_step_aside(&t.0), [aside("nvngx", "NvNgx.Dll", false)], "one entry, nvngx.dll first");
}

#[test]
fn every_kind_in_order_and_as_json() {
    let t = tmp("all");
    put(&t.0, "nvngx.dll", b"x");
    put(&t.0, "dlsstweaks.ini", b"");
    put(&t.0, "EnshroudedVKFG.ini", b"");
    put(&t.0, "Streamline/sl.interposer.dll", b"x");
    put(&t.0, "OptiScaler.asi", &build_test_dll(&["x"], b"OptiScaler.log"));
    let all = graphics_step_aside(&t.0);
    assert_eq!(
        all,
        [
            aside("optiscaler", "OptiScaler.asi", true),
            aside("streamline", "Streamline/sl.interposer.dll", true),
            aside("vkfg", "EnshroudedVKFG.ini", true),
            aside("dlsstweaks", "dlsstweaks.ini", false),
            aside("nvngx", "nvngx.dll", false),
        ]
    );
    let json = serde_json::to_value(&all[0]).unwrap();
    assert_eq!(json, serde_json::json!({ "kind": "optiscaler", "file": "OptiScaler.asi", "frameCap": true, "upscaler": true }));
    // a status from before dev.29.18 (no "upscaler") still reads
    let old: GraphicsStepAside = serde_json::from_value(serde_json::json!({ "kind": "nvngx", "file": "nvngx.dll", "frameCap": false })).unwrap();
    assert!(!old.upscaler);
}

#[test]
fn another_tools_upscaler_dlls_next_to_the_game_take_kilns_upscaler_swap() {
    let t = tmp("upscaler");
    // Kiln's own copies live in kiln/vendor/: not another tool's
    put(&t.0, "kiln/vendor/amd/2.4.0/amd_fidelityfx_loader_vk.dll", b"x");
    put(&t.0, "kiln/vendor/intel/3.0.2/libxess.dll", b"x");
    assert!(graphics_step_aside(&t.0).is_empty());
    put(&t.0, "LIBXESS.dll", b"x");
    let up = GraphicsStepAside { kind: "upscaler".into(), file: "LIBXESS.dll".into(), frame_cap: false, upscaler: true };
    assert_eq!(graphics_step_aside(&t.0), [up.clone()]);
    // one entry, the list's first found; after the others
    put(&t.0, "amd_fidelityfx_vk.dll", b"x");
    put(&t.0, "dlsstweaks.ini", b"");
    let all = graphics_step_aside(&t.0);
    assert_eq!(all, [aside("dlsstweaks", "dlsstweaks.ini", false),
        GraphicsStepAside { file: "amd_fidelityfx_vk.dll".into(), ..up }]);
    // the same names as kiln.ui's list (graphics_aside.h has it from phase B's native side on)
    let h = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../native/services/ui/graphics_aside.h")).unwrap();
    if let Some(start) = h.find("kUpscalerFiles[] = {") {
        let end = start + h[start..].find("};").unwrap();
        let list: Vec<&str> = h[start..end].split('"').skip(1).step_by(2).collect();
        assert_eq!(list, kiln_manage::game::UPSCALER_FILES);
    }
}

#[cfg(unix)]
#[test]
fn a_linked_file_or_folder_counts_as_the_game_sees_it() {
    // Linux mod managers deploy by symbolic links; Wine (and kiln.ui's own check in the game) follows them
    let t = tmp("links");
    let store = tmp("links-store");
    put(&store.0, "dlsstweaks.ini", b"");
    put(&store.0, "Streamline/sl.interposer.dll", b"x");
    std::os::unix::fs::symlink(store.0.join("dlsstweaks.ini"), t.0.join("dlsstweaks.ini")).unwrap();
    assert_eq!(graphics_step_aside(&t.0), [aside("dlsstweaks", "dlsstweaks.ini", false)]);
    std::os::unix::fs::symlink(store.0.join("Streamline"), t.0.join("Streamline")).unwrap();
    assert_eq!(
        graphics_step_aside(&t.0),
        [aside("streamline", "Streamline/sl.interposer.dll", true), aside("dlsstweaks", "dlsstweaks.ini", false)]
    );
    // a link to nothing is no tool
    std::fs::remove_file(t.0.join("dlsstweaks.ini")).unwrap();
    std::os::unix::fs::symlink(store.0.join("gone.ini"), t.0.join("dlsstweaks.ini")).unwrap();
    assert_eq!(graphics_step_aside(&t.0), [aside("streamline", "Streamline/sl.interposer.dll", true)]);
}

/// (dev.29.18) kiln/state/graphics.json (Kiln.Ui): read as the schema's example has it; "Now" only while the game that
/// wrote it runs; not read for a server, nor in another format.
#[test]
fn the_graphics_state_file_is_read_and_judged_against_the_running_game() {
    use kiln_manage::game::{graphics_now, GameInstall, GameKind, PathMode, Platform};
    let t = tmp("state");
    let install = |kind: GameKind| GameInstall {
        id: "g".into(), kind, path: t.0.clone(), exe: "enshrouded.exe".into(), app_id: kiln_manage::CLIENT_APP_ID, library: None,
        platform: Platform::Proton, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy,
        source: "manual".into(),
    };
    assert!(graphics_now(&install(GameKind::Client), None).is_none(), "no file");
    let example = std::fs::read(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../schema/examples/state-graphics.json")).unwrap();
    put(&t.0, "kiln/state/graphics.json", &example);
    let g = graphics_now(&install(GameKind::Client), None).unwrap();
    assert!(!g.current, "the game does not run: the last game's");
    assert_eq!((g.state.upscaler.chosen.as_str(), g.state.upscaler.running.as_str()), ("amd", "amdFsr4"));
    assert_eq!(g.state.upscaler.reason, None);
    assert_eq!(g.state.upscaler.version.as_deref(), Some("4.1.2"));
    assert_eq!(g.state.files["amd"].state, "present");
    assert!(g.state.frame_cap.is_some());
    // the running game's pid: written after it started (this test's own process started before the file was written)
    let me = std::process::id();
    if !cfg!(windows) {
        assert!(graphics_now(&install(GameKind::Client), Some(me)).unwrap().current);
    }
    assert!(graphics_now(&install(GameKind::Server), Some(me)).is_none(), "a server draws no frames");
    // as JSON for the launcher: camelCase, the file as it is
    let json = serde_json::to_value(&g).unwrap();
    assert_eq!(json["state"]["upscaler"]["amdUpscaler"], "auto");
    assert_eq!(json["state"]["frameCap"]["fps"], 141);
    // another format, or not JSON: none
    put(&t.0, "kiln/state/graphics.json", br#"{"format": 2, "pid": 1, "written": "x", "upscaler": {"chosen": "game", "running": "off"}}"#);
    assert!(graphics_now(&install(GameKind::Client), None).is_none());
    put(&t.0, "kiln/state/graphics.json", b"{ half");
    assert!(graphics_now(&install(GameKind::Client), None).is_none());
}
