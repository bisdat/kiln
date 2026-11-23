// SPDX-License-Identifier: MIT
//! `kilnctl client activate` and `play-once` in kiln mode, with the real binary and scratch folders: they
//! run kiln_manage's own activation and play-once write, then print every warning the check before Play would show
//! (here the interpreter that does not run: held, or not installed), and ask nothing; a hosted game prints what it printed before. Nothing here starts the
//! game, Steam, Wine or a server.
use kiln_manage::paths::DataDirs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const HELD: &str = r#"{"shroudforge": {"backend": "kiln"}}"#;
const HOSTED: &str = r#"{"shroudforge": {"backend": "hosted"}}"#;

fn scratch(name: &str) -> PathBuf {
    let t = std::env::temp_dir().join(format!("kilnctl-v811-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&t);
    std::fs::create_dir_all(&t).unwrap();
    t
}

fn kilnctl(t: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_kilnctl"))
        .env("KILN_DATA_DIR", t.join("data"))
        .env("HOME", t.join("home"))
        .env("XDG_DATA_HOME", t.join("xdg-data"))
        .env("XDG_STATE_HOME", t.join("xdg-state"))
        .env("XDG_CONFIG_HOME", t.join("xdg-config"))
        .env_remove("KILN_SHROUDFORGE")
        .env_remove("KILN_SHROUDFORGE_RUNTIME")
        .args(args)
        .output()
        .unwrap()
}

fn text(o: &Output) -> String {
    format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr))
}

/// A client folder with Kiln's folder and `kiln_json`, and a profile (no mods) in the data folder.
fn client(t: &Path, kiln_json: &str) -> (PathBuf, String) {
    let g = t.join("game");
    std::fs::create_dir_all(g.join("kiln")).unwrap();
    std::fs::write(g.join("enshrouded.exe"), b"MZ").unwrap();
    std::fs::write(g.join("kiln/kiln.json"), kiln_json).unwrap();
    let p = kiln_manage::profile::create(&DataDirs::at(t.join("data")), "cli", None).unwrap();
    (g, p.id)
}

/// Why a kiln-mode start of the scratch client runs no ShroudForge package: the interpreter is held while this build's
/// gate list (native/core/gates/sfi.json) has its row, as kiln-manage's routing tests read it; else it is a normal
/// choice, and the scratch folder has no kiln/plugins/kiln-sfi.dll.
fn kiln_mode_refusal() -> &'static str {
    let policy = kiln_manage::sfi_release::ReleasePolicy::shipped();
    let held = policy.rows().unwrap().iter().any(|r| r.name == kiln_manage::sfi_release::INTERPRETER);
    if held { "interpreter-held" } else { "interpreter-missing" }
}

#[test]
fn activate_and_play_once_print_the_warnings_of_a_kiln_mode_start() {
    let t = scratch("held");
    let (g, id) = client(&t, HELD);
    let dir = g.to_string_lossy().into_owned();
    let code = kiln_mode_refusal();
    let o = kilnctl(&t, &["client", "activate", &id, "--dir", &dir]);
    let out = text(&o);
    assert!(o.status.success(), "{out}");
    assert!(out.contains("activated profile"), "{out}");
    assert!(out.contains(&format!("WARNING: no ShroudForge package runs this start ({code})")), "{out}");
    assert!(out.contains("a headless start asks nobody"), "{out}");
    let o = kilnctl(&t, &["client", "play-once", "profile", "--profile", &id, "--dir", &dir]);
    let out = text(&o);
    assert!(o.status.success(), "{out}");
    assert!(out.contains(&format!("({code})")) && out.contains("wrote "), "{out}");
    assert!(g.join("kiln/play-once.ini").is_file(), "the start goes on: {out}");
    let _ = std::fs::remove_dir_all(&t);
}

#[test]
fn a_hosted_client_prints_no_warning() {
    let t = scratch("hosted");
    let (g, id) = client(&t, HOSTED);
    let dir = g.to_string_lossy().into_owned();
    for args in [vec!["client", "activate", &id, "--dir", &dir], vec!["client", "play-once", "vanilla", "--dir", &dir]] {
        let o = kilnctl(&t, &args);
        let out = text(&o);
        assert!(o.status.success(), "{out}");
        assert!(!out.contains("WARNING") && !out.contains("ShroudForge warning"), "{out}");
    }
    let _ = std::fs::remove_dir_all(&t);
}
