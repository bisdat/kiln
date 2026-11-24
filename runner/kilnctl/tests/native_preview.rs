//! Unbound `kilnctl profile show`: no implicit selected game or installed runtime. Synthetic mods and receipt only.
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::path::PathBuf;
use std::process::Command;

struct Scratch(PathBuf);
impl Drop for Scratch {
    fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); }
}

#[test]
fn unbound_profile_show_uses_cli_build_not_an_installed_games_receipt() {
    let t = Scratch(std::env::temp_dir().join(format!("kilnctl-native-preview-{}", std::process::id())));
    std::fs::create_dir_all(&t.0).unwrap();
    let dirs = DataDirs::at(t.0.join("data"));
    let mut refs: Vec<LibraryRef> = Vec::new();
    for (id, floor) in [("current", format!(">={}", kiln_manage::VERSION)), ("future", ">=999.0.0-dev.1".into())] {
        let src = t.0.join(id);
        std::fs::create_dir_all(&src).unwrap();
        std::fs::write(src.join("native.json"), serde_json::to_string(&serde_json::json!({
            "format":1, "id":id, "version":"1.0.0", "phase":"early", "dlls":["mod.dll"], "kiln":floor
        })).unwrap()).unwrap();
        std::fs::write(src.join("mod.dll"), kiln_manage::pe::build_test_dll(&["kiln_native_init"], b"synthetic")).unwrap();
        refs.extend(library::import(&dirs, &[src], &mut NoProgress).unwrap().imported);
    }
    let mut p = profile::create(&dirs, "Unbound preview", None).unwrap();
    p.mods.native = refs.iter().filter(|r| r.kind == ModKind::Native).map(|r| ProfileMod {
        id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true,
    }).collect();
    let p = profile::save(&dirs, &p).unwrap();
    let game = t.0.join("other-game");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    std::fs::write(game.join("kiln/.install.json"), serde_json::to_string(&serde_json::json!({
        "format":1, "installId":"synthetic-other", "installed":"2020-01-01T00:00:00Z",
        "runtime":{"version":"999.0.0-dev.1"}, "side":"client", "files":[]
    })).unwrap()).unwrap();
    // It really is a matching game-bound preview, but the CLI below is deliberately unbound.
    let bound = profile::resolve_preview_in(&dirs, &p, kiln_manage::manifest::RunSide::Client, Some(&game)).unwrap();
    assert!(bound.native.iter().find(|n| n.id == "future").unwrap().enabled);
    let out = Command::new(env!("CARGO_BIN_EXE_kilnctl"))
        .env("KILN_DATA_DIR", &dirs.root)
        .env("HOME", t.0.join("home"))
        .env("XDG_DATA_HOME", t.0.join("xdg-data"))
        .env("XDG_STATE_HOME", t.0.join("xdg-state"))
        .env("XDG_CONFIG_HOME", t.0.join("xdg-config"))
        .env("USERPROFILE", t.0.join("home"))
        .env("APPDATA", t.0.join("appdata"))
        .args(["profile", "show", &p.id])
        .output().unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    assert!(out.status.success(), "{text}");
    let current = text.lines().find(|l| l.contains("native current ")).unwrap();
    assert!(!current.contains("requires"), "{text}");
    let future = text.lines().find(|l| l.contains("native future ")).unwrap();
    assert!(future.contains(">=999.0.0-dev.1"), "{text}");
    assert!(future.contains(kiln_manage::VERSION), "{text}");
    assert!(!text.contains("installed runtime compatibility is unknown"), "unbound, not a receipt failure: {text}");
}
