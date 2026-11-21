// SPDX-License-Identifier: MIT
//! kiln-eml's archive inspector for Linux is the one runtime file packed and installed executable: the
//! bundle writer gives exactly that path of exactly component eml 0755 in the zip, the installer gives it 0755 on the
//! handle it was written through once its bytes are checked, never trusting a zip's own modes (setuid, write bits), and
//! an earlier install's helper that lost its execute bits gets them back only when it is still Kiln's own file with the
//! receipt's bytes; a file the player had there before (adopted), a changed file and a link are never chmodded. Every
//! other entry keeps its bytes and mode. Windows has no modes: its helper is the .exe, placed like any file.
//!
//! Every file here is synthetic.
#![cfg(feature = "zip")]

use kiln_manage::bundle::{self, CreateOptions};
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::install::{self, InstallOptions, ReplacedAction};
use kiln_manage::pe::build_test_dll;
use kiln_manage::task::NoProgress;
use kiln_manage::{hash, paths::DataDirs};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const HELPER: &str = "kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect";
const WIN_HELPER: &str = "kiln/tools/eml-inspect/windows-x86_64/kiln-eml-inspect.exe";
/// An x86-64 ELF header and some bytes (the installer never runs it).
const ELF: &[u8] =
    b"\x7fELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0 the archive inspector for Linux";

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-sfi-modes-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A stage like tools/package/stage.sh writes: core, eml with kiln-eml's archive inspector for both
/// platforms, sfi. `helper_in`: the component the Linux helper's path is staged in.
fn stage(root: &Path, tag: &str, helper_in: &str) -> PathBuf {
    let s = root.join(format!("stage-{tag}"));
    put(
        &s,
        "kiln-runtime.json",
        br#"{
  "format": 1, "kiln": "0.2.0-dev", "abi": 2, "api": "0.2.0", "dotnet": "10.0.14",
  "components": {
    "core": { "sides": ["server", "client"], "default": true },
    "eml": { "sides": ["server", "client"], "default": true, "requires": ["core"] },
    "sfi": { "sides": ["server", "client"], "default": true, "requires": ["core", "eml"] }
  },
  "config": ["kiln/kiln.json"]
}"#,
    );
    put(
        &s,
        "core/dinput8.dll",
        &build_test_dll(
            &["DirectInput8Create", "GetdfDIJoystick", "KilnProxyInfo"],
            format!("KilnProxy/2 0.2.0-dev+{tag}\0").as_bytes(),
        ),
    );
    put(
        &s,
        "core/kiln/kiln-core.dll",
        format!("core {tag}").as_bytes(),
    );
    put(&s, "core/kiln/kiln.json", b"{}\n");
    put(&s, "core/kiln/host/Kiln.Host.dll", b"host");
    put(&s, "eml/kiln/plugins/kiln-eml.dll", b"eml plugin");
    put(&s, &format!("{helper_in}/{HELPER}"), ELF);
    put(
        &s,
        &format!("eml/{WIN_HELPER}"),
        b"MZ the archive inspector for Windows",
    );
    for d in ["linux-x86_64", "windows-x86_64"] {
        for (n, b) in [
            ("LICENSE.txt", &b"GPL-3"[..]),
            ("NOTICE.txt", b"notice"),
            ("version.json", b"{}"),
            ("archive-inspection.schema.json", b"{}"),
        ] {
            put(&s, &format!("eml/kiln/tools/eml-inspect/{d}/{n}"), b);
        }
    }
    put(&s, "sfi/kiln/plugins/kiln-sfi.dll", b"MZ interpreter");
    s
}

fn bundle_of(t: &Path, tag: &str, helper_in: &str) -> PathBuf {
    bundle::create(
        &stage(t, tag, helper_in),
        &t.join(format!("out-{tag}")),
        &CreateOptions {
            source_date_epoch: 1_793_793_600,
            git: "aaaaaaa".into(),
        },
    )
    .unwrap()
}

fn client(dir: &Path) -> GameInstall {
    GameInstall {
        id: "g-modes".into(),
        kind: GameKind::Client,
        path: dir.to_path_buf(),
        exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID,
        library: None,
        platform: if cfg!(windows) {
            Platform::Windows
        } else {
            Platform::Proton
        },
        prefix: None,
        build_id: None,
        exe_sha256: None,
        steam_flavor: None,
        path_mode: if cfg!(windows) {
            PathMode::Native
        } else {
            PathMode::Copy
        },
        source: "manual".into(),
    }
}

fn game(t: &Path, name: &str) -> PathBuf {
    let g = t.join(name);
    put(&g, "enshrouded.exe", b"MZ");
    g
}

/// Every entry of a zip with its unix mode bits (permission bits and setuid/setgid/sticky).
fn zip_modes(zip: &Path) -> Vec<(String, u32)> {
    let mut a = zip::ZipArchive::new(std::fs::File::open(zip).unwrap()).unwrap();
    (0..a.len())
        .map(|i| {
            let e = a.by_index(i).unwrap();
            (
                e.name().unwrap().to_string(),
                e.unix_mode().unwrap_or(0) & 0o7777,
            )
        })
        .collect()
}

/// The same zip with other unix modes: `mode(name)` for each entry (the bytes, names and order unchanged). Returns the
/// names whose central-directory entry carries setuid/setgid/sticky bits (written by hand: the zip crate drops them, on
/// writing and on reading).
fn with_modes(src: &Path, dst: &Path, mode: impl Fn(&str) -> u32) -> Vec<String> {
    let mut a = zip::ZipArchive::new(std::fs::File::open(src).unwrap()).unwrap();
    let mut w = zip::ZipWriter::new(std::fs::File::create(dst).unwrap());
    for i in 0..a.len() {
        let mut e = a.by_index(i).unwrap();
        let name = e.name().unwrap().to_string();
        let mut data = Vec::new();
        e.read_to_end(&mut data).unwrap();
        let o = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .system(zip::System::Unix)
            .unix_permissions(mode(&name) & 0o777);
        w.start_file(name.as_str(), o).unwrap();
        w.write_all(&data).unwrap();
    }
    w.finish().unwrap();
    // the zip crate keeps only 0o777: the setuid/setgid bits go into the central directory by hand (external attributes,
    // made by Unix), as a hostile zip would carry them
    let mut z = std::fs::read(dst).unwrap();
    let u16_at = |z: &[u8], i: usize| u16::from_le_bytes([z[i], z[i + 1]]) as usize;
    let eocd = (0..=z.len() - 22)
        .rev()
        .find(|&i| z[i..i + 4] == *b"PK\x05\x06")
        .unwrap();
    let count = u16_at(&z, eocd + 10);
    let mut i = u32::from_le_bytes(z[eocd + 16..eocd + 20].try_into().unwrap()) as usize;
    let mut special = Vec::new();
    for _ in 0..count {
        assert_eq!(z[i..i + 4], *b"PK\x01\x02");
        let (n, extra, comment) = (u16_at(&z, i + 28), u16_at(&z, i + 30), u16_at(&z, i + 32));
        let name = String::from_utf8(z[i + 46..i + 46 + n].to_vec()).unwrap();
        let m = mode(&name);
        if m & !0o777 != 0 {
            z[i + 5] = 3;
            z[i + 38..i + 42].copy_from_slice(&((0o100000 | m) << 16).to_le_bytes());
            special.push(name);
        }
        i += 46 + n + extra + comment;
    }
    std::fs::write(dst, z).unwrap();
    special
}

/// Every file below `game`/kiln (no receipt, staging or backups), game-relative, with its permission bits.
#[cfg(unix)]
fn modes(game: &Path) -> Vec<(String, u32)> {
    use std::os::unix::fs::PermissionsExt as _;
    fn walk(root: &Path, dir: &Path, out: &mut Vec<(String, u32)>) {
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            let p = e.path();
            let rel = p
                .strip_prefix(root)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            if rel.starts_with("kiln/backups") || rel.starts_with("kiln/.") {
                continue;
            }
            let m = std::fs::symlink_metadata(&p).unwrap();
            if m.is_dir() {
                walk(root, &p, out);
            } else {
                out.push((rel, m.permissions().mode() & 0o7777));
            }
        }
    }
    let mut v = Vec::new();
    walk(game, &game.join("kiln"), &mut v);
    v.sort();
    v
}

#[cfg(unix)]
fn mode_of(p: &Path) -> u32 {
    use std::os::unix::fs::PermissionsExt as _;
    std::fs::symlink_metadata(p).unwrap().permissions().mode() & 0o7777
}

#[cfg(unix)]
fn set_mode(p: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt as _;
    std::fs::set_permissions(p, std::fs::Permissions::from_mode(mode)).unwrap();
}

/// The writer packs exactly the Linux helper of component eml 0755; every other entry keeps 0644.
#[test]
fn the_bundle_packs_only_the_linux_helper_executable() {
    let t = tmp("pack");
    let zip = bundle_of(&t.0, "a", "eml");
    let modes = zip_modes(&zip);
    assert!(modes.iter().any(|(n, _)| n == HELPER), "{modes:?}");
    for (name, mode) in &modes {
        let want = if name == HELPER { 0o755 } else { 0o644 };
        assert_eq!(*mode, want, "{name}");
    }
    // the same path in another component is no helper: packed like any file
    let zip = bundle_of(&t.0, "core", "core");
    for (name, mode) in zip_modes(&zip) {
        assert_eq!(mode, 0o644, "{name}");
    }
}

/// The installer places both helpers; on Linux and macOS the Linux one is exactly 0755 and nothing else is executable.
#[test]
fn the_install_places_the_helpers_and_makes_only_the_linux_one_executable() {
    let t = tmp("install");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    let zip = bundle_of(&t.0, "a", "eml");
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    // placement: mode-independent on every platform
    assert_eq!(std::fs::read(g.join(HELPER)).unwrap(), ELF);
    assert_eq!(
        std::fs::read(g.join(WIN_HELPER)).unwrap(),
        b"MZ the archive inspector for Windows"
    );
    let r = install::read_receipt(&g).unwrap().unwrap();
    for p in [HELPER, WIN_HELPER] {
        let f = r
            .files
            .iter()
            .find(|f| f.path == p)
            .unwrap_or_else(|| panic!("{p} not in the receipt"));
        assert_eq!(f.component.as_deref(), Some("eml"));
        assert_eq!(f.sha256, hash::sha256_file(&g.join(p)).unwrap().0);
    }
    #[cfg(unix)]
    for (rel, mode) in modes(&g) {
        if rel == HELPER {
            assert_eq!(mode, 0o755, "{rel}");
        } else {
            assert_eq!(mode & 0o7111, 0, "{rel} {mode:o}");
        }
    }
}

/// A zip's own modes are never read: setuid and world-write on the helper, execute bits on a DLL, all ignored.
#[test]
#[cfg(unix)]
fn a_zips_own_modes_are_never_trusted() {
    let t = tmp("zipmodes");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    let clean = bundle_of(&t.0, "a", "eml");
    let forged = t.0.join("forged.zip");
    let special = with_modes(&clean, &forged, |n| {
        if n == HELPER {
            0o6777
        } else if n.ends_with(".dll") {
            0o777
        } else {
            0o666
        }
    });
    assert_eq!(
        special,
        [HELPER],
        "the forged zip carries setuid/setgid on the helper"
    );
    assert!(
        zip_modes(&forged)
            .iter()
            .any(|(n, m)| n.ends_with(".dll") && *m == 0o777)
    );
    install::install(
        &dirs,
        &client(&g),
        &forged,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    for (rel, mode) in modes(&g) {
        if rel == HELPER {
            assert_eq!(
                mode, 0o755,
                "exactly 0755: no setuid, no group or world write"
            );
        } else {
            assert_eq!(mode & 0o7111, 0, "{rel} {mode:o}");
        }
    }
}

/// An earlier install's helper that lost its execute bits gets them back on the next install of the same bytes;
/// a helper that has them is left as it is.
#[test]
#[cfg(unix)]
fn the_installed_helper_gets_its_execute_bits_back() {
    let t = tmp("repair");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    let zip = bundle_of(&t.0, "a", "eml");
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    let before = modes(&g);
    set_mode(&g.join(HELPER), 0o644);
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    assert_eq!(mode_of(&g.join(HELPER)), 0o755);
    assert_eq!(modes(&g), before, "nothing else changed");
    // already executable (here 0711): left as it is
    set_mode(&g.join(HELPER), 0o711);
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    assert_eq!(mode_of(&g.join(HELPER)), 0o711);
}

/// A helper the player had there before Kiln (adopted: the bundle's bytes, not Kiln's file) is never chmodded, at
/// the first install or any later one.
#[test]
#[cfg(unix)]
fn a_helper_that_was_there_before_kiln_is_never_chmodded() {
    let t = tmp("adopted");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    put(&g, HELPER, ELF);
    set_mode(&g.join(HELPER), 0o640);
    let zip = bundle_of(&t.0, "a", "eml");
    let r = install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    assert!(
        r.replaced
            .iter()
            .any(|f| f.path == HELPER && f.action == ReplacedAction::Adopted),
        "{:?}",
        r.replaced
    );
    assert_eq!(mode_of(&g.join(HELPER)), 0o640);
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    assert_eq!(mode_of(&g.join(HELPER)), 0o640, "adopted: never chmodded");
}

/// A helper changed in place is replaced whole by the bundle's (0755) and its changed bytes are kept aside unchanged:
/// no file holding other bytes is ever made executable.
#[test]
#[cfg(unix)]
fn a_changed_helper_is_replaced_never_chmodded_in_place() {
    let t = tmp("changed");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    let zip = bundle_of(&t.0, "a", "eml");
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    std::fs::write(g.join(HELPER), b"#!/bin/sh\necho changed\n").unwrap();
    set_mode(&g.join(HELPER), 0o644);
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    assert_eq!(std::fs::read(g.join(HELPER)).unwrap(), ELF);
    assert_eq!(mode_of(&g.join(HELPER)), 0o755);
    let mut stack = vec![g.clone(), t.0.join("data")];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            let p = e.path();
            let m = std::fs::symlink_metadata(&p).unwrap();
            if m.is_dir() {
                stack.push(p);
            } else if m.is_file() && std::fs::read(&p).unwrap() == b"#!/bin/sh\necho changed\n" {
                assert_eq!(
                    mode_of(&p) & 0o111,
                    0,
                    "{} kept aside, never made executable",
                    p.display()
                );
            }
        }
    }
}

/// A link at the helper's path refuses the install, and what it points at keeps its mode.
#[test]
#[cfg(unix)]
fn a_linked_helper_refuses_the_install_and_its_target_keeps_its_mode() {
    let t = tmp("link");
    let dirs = DataDirs::at(t.0.join("data"));
    let g = game(&t.0, "game");
    let zip = bundle_of(&t.0, "a", "eml");
    install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    let outside = t.0.join("players-file");
    std::fs::write(&outside, ELF).unwrap();
    set_mode(&outside, 0o600);
    std::fs::remove_file(g.join(HELPER)).unwrap();
    std::os::unix::fs::symlink(&outside, g.join(HELPER)).unwrap();
    let e = install::install(
        &dirs,
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap_err();
    assert!(e.message.contains("is a symlink"), "{e:?}");
    assert_eq!(mode_of(&outside), 0o600);
}

/// Windows ignores modes: both helpers are placed with their bytes and nothing else (a forged mode changes nothing).
#[test]
fn packing_and_placement_do_not_depend_on_modes() {
    let t = tmp("modeless");
    let dirs = DataDirs::at(t.0.join("data"));
    let a = game(&t.0, "game-a");
    let b = game(&t.0, "game-b");
    let clean = bundle_of(&t.0, "a", "eml");
    let forged = t.0.join("forged.zip");
    with_modes(&clean, &forged, |_| 0o777);
    install::install(
        &dirs,
        &client(&a),
        &clean,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    install::install(
        &dirs,
        &client(&b),
        &forged,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    for p in [
        HELPER,
        WIN_HELPER,
        "kiln/plugins/kiln-sfi.dll",
        "kiln/plugins/kiln-eml.dll",
    ] {
        assert_eq!(
            std::fs::read(a.join(p)).unwrap(),
            std::fs::read(b.join(p)).unwrap(),
            "{p}"
        );
    }
}
