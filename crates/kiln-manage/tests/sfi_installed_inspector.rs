// SPDX-License-Identifier: MIT
//! The archive inspector the Kiln installed in a game folder brought: kiln-manage takes its path, SHA-256
//! and version from the install receipt and the `version.json` beside it (component eml), refuses anything that is not
//! exactly what that install recorded, runs it through its checked handle, says why when it cannot (no helper for this
//! platform, none installed, a forged or swapped one, no execute permission) and never copies it elsewhere; the
//! launcher's and kilnctl's `use_installed_inspector` makes a kiln-mode activation run it. `restore_executable` proves a
//! file on its own handle before any chmod.
//!
//! The ignored test `the_real_staged_bundle_installs_and_its_helper_answers` installs a real staged bundle
//! (`SFI_REAL_BUNDLE=<kiln-runtime-*.zip>`) into a synthetic client folder and runs its real helper there.
//!
//! Every file here is synthetic.
#![cfg(all(feature = "sfi-scan", feature = "zip"))]

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::bundle::{self, CreateOptions};
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::install::{self, InstallOptions};
use kiln_manage::manifest::RunSide;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::sfi_inspection::{self, EXE_NAME, FRAMING, Installed, PLATFORM, VersionFile};
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

const VERSION: &str = sfi_profile_common::VERSION;
const SCHEMA: &[u8] = b"{\"$id\": \"archive-inspection\"}\n";

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-sfi-installed-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A change to a version.json document.
type Change = Box<dyn Fn(&mut serde_json::Value)>;

fn sha(b: &[u8]) -> String {
    kiln_manage::hash::sha256_hex(b)
}

/// This platform's helper folder (a made-up one where Kiln ships none, so the files still exist).
fn platform() -> &'static str {
    PLATFORM.unwrap_or("linux-x86_64")
}

/// The helper: on Unix a script that prints an inspector's answer of `version` (started by its path: no ELF), elsewhere bytes.
fn helper(version: &str) -> Vec<u8> {
    if cfg!(unix) {
        let mut a = sfi_profile_common::answer();
        a["inspector"]["version"] = version.into();
        format!("#!/bin/sh\ncat <<'EOF'\n{a}\nEOF\n").into_bytes()
    } else {
        b"MZ a stand-in that is never run here".to_vec()
    }
}

fn version_file(exe: &[u8]) -> VersionFile {
    VersionFile {
        format: 1,
        tool: "kiln-eml-inspect".into(),
        version: VERSION.into(),
        platform: platform().into(),
        executable: EXE_NAME.into(),
        sha256: sha(exe),
        schema_version: 1,
        schema: "archive-inspection.schema.json".into(),
        schema_sha256: sha(SCHEMA),
        framing: FRAMING.into(),
        kiln: "0.2.0-dev".into(),
        git: None,
    }
}

/// A stage with core and eml (kiln-eml and its inspector for this platform); `version` is the version.json text,
/// `helper_in` the component of the helper's three files.
fn stage(root: &Path, tag: &str, exe: &[u8], version: &[u8], helper_in: &str) -> PathBuf {
    let s = root.join(format!("stage-{tag}"));
    put(
        &s,
        "kiln-runtime.json",
        br#"{
  "format": 1, "kiln": "0.2.0-dev", "abi": 2, "api": "0.2.0", "dotnet": "10.0.14",
  "components": {
    "core": { "sides": ["server", "client"], "default": true },
    "eml": { "sides": ["server", "client"], "default": true, "requires": ["core"] }
  },
  "config": ["kiln/kiln.json"]
}"#,
    );
    put(
        &s,
        "core/dinput8.dll",
        &build_test_dll(
            &["DirectInput8Create", "GetdfDIJoystick", "KilnProxyInfo"],
            b"KilnProxy/2 0.2.0-dev\0",
        ),
    );
    put(&s, "core/kiln/kiln-core.dll", b"core");
    put(&s, "core/kiln/kiln.json", b"{}\n");
    put(&s, "eml/kiln/plugins/kiln-eml.dll", b"eml plugin");
    let d = format!("{helper_in}/kiln/tools/eml-inspect/{}", platform());
    put(&s, &format!("{d}/{EXE_NAME}"), exe);
    put(&s, &format!("{d}/version.json"), version);
    put(&s, &format!("{d}/archive-inspection.schema.json"), SCHEMA);
    s
}

fn client(dir: &Path) -> GameInstall {
    GameInstall {
        id: "g-installed".into(),
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

/// A client folder with the bundle of `stage` installed.
fn installed_game(t: &Path, tag: &str, stage: &Path) -> PathBuf {
    let g = t.join(format!("game-{tag}"));
    put(&g, "enshrouded.exe", b"MZ");
    let zip = bundle::create(
        stage,
        &t.join(format!("out-{tag}")),
        &CreateOptions {
            source_date_epoch: 1_793_793_600,
            git: "aaaaaaa".into(),
        },
    )
    .unwrap();
    install::install(
        &DataDirs::at(t.join(format!("data-{tag}"))),
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    g
}

fn good(t: &Path, tag: &str) -> PathBuf {
    let exe = helper(VERSION);
    let v = serde_json::to_vec_pretty(&version_file(&exe)).unwrap();
    installed_game(t, tag, &stage(t, tag, &exe, &v, "eml"))
}

fn rel_path(game: &Path, name: &str) -> PathBuf {
    game.join("kiln")
        .join("tools")
        .join("eml-inspect")
        .join(platform())
        .join(name)
}

/// The tool is the receipt's: its path in this platform's folder, the receipt's SHA-256, version.json's version.
#[test]
fn the_installed_tool_is_the_one_the_receipt_recorded() {
    let t = tmp("tool");
    let g = good(&t.0, "a");
    let r = sfi_inspection::installed(&g);
    if PLATFORM.is_none() {
        let e = r.unwrap_err();
        assert!(e.contains("for this platform"), "{e}");
        return;
    }
    let tool = r.unwrap();
    assert_eq!(tool.exe, rel_path(&g, EXE_NAME));
    assert_eq!(tool.version, VERSION);
    let receipt = install::read_receipt(&g).unwrap().unwrap();
    let rel = format!("kiln/tools/eml-inspect/{}/{EXE_NAME}", platform());
    assert_eq!(
        Some(&tool.sha256),
        receipt
            .files
            .iter()
            .find(|f| f.path == rel)
            .map(|f| &f.sha256)
    );
    assert_eq!(tool.sha256, sha(&helper(VERSION)));
}

/// No receipt, or a Kiln installed without the helper: said, no inspector.
#[test]
fn no_receipt_or_no_helper_is_said() {
    if PLATFORM.is_none() {
        return;
    }
    let t = tmp("none");
    let bare = t.0.join("bare");
    put(&bare, "enshrouded.exe", b"MZ");
    let e = sfi_inspection::installed(&bare).unwrap_err();
    assert!(e.contains("not installed"), "{e}");
    let g = good(&t.0, "a");
    let receipt_path = g.join("kiln/.install.json");
    let mut r: serde_json::Value =
        serde_json::from_slice(&std::fs::read(&receipt_path).unwrap()).unwrap();
    r["files"].as_array_mut().unwrap().retain(|f| {
        !f["path"].as_str().unwrap().ends_with(EXE_NAME)
            || !f["path"].as_str().unwrap().contains("eml-inspect")
    });
    std::fs::write(&receipt_path, serde_json::to_vec(&r).unwrap()).unwrap();
    let e = sfi_inspection::installed(&g).unwrap_err();
    assert!(
        e.contains("is not installed with this Kiln") && e.contains("eml"),
        "{e}"
    );
}

/// version.json is bound to the receipt and to the helper: any other bytes, any field that does not describe the
/// receipt's helper, an unknown key, refuse it, each naming why.
#[test]
fn the_version_file_is_bound_to_the_receipt_and_the_helper() {
    if PLATFORM.is_none() {
        return;
    }
    let t = tmp("version");
    let exe = helper(VERSION);
    let cases: Vec<(&str, Change)> = vec![
        (
            "sha256",
            Box::new(|v| v["sha256"] = sha(b"another helper").into()),
        ),
        (
            "platform",
            Box::new(|v| v["platform"] = "windows-arm64".into()),
        ),
        (
            "framing",
            Box::new(|v| v["framing"] = "kiln-eml-inspect archive-set v2".into()),
        ),
        (
            "schemaSha256",
            Box::new(|v| v["schemaSha256"] = sha(b"another schema").into()),
        ),
        ("schemaVersion", Box::new(|v| v["schemaVersion"] = 2.into())),
        ("tool", Box::new(|v| v["tool"] = "kiln-eml-run".into())),
        (
            "executable",
            Box::new(|v| v["executable"] = "kiln-eml-run".into()),
        ),
        ("version", Box::new(|v| v["version"] = "".into())),
        (
            "not a version file this Kiln reads",
            Box::new(|v| v["extra"] = true.into()),
        ),
    ];
    for (i, (why, change)) in cases.iter().enumerate() {
        let mut v = serde_json::to_value(version_file(&exe)).unwrap();
        change(&mut v);
        let tag = format!("v{i}");
        let g = installed_game(
            &t.0,
            &tag,
            &stage(&t.0, &tag, &exe, &serde_json::to_vec(&v).unwrap(), "eml"),
        );
        let e = sfi_inspection::installed(&g).unwrap_err();
        assert!(e.contains(why), "{why}: {e}");
    }
    // the version.json the install placed, changed afterwards: not the receipt's bytes
    let g = good(&t.0, "changed");
    let p = rel_path(&g, "version.json");
    let mut v: serde_json::Value = serde_json::from_slice(&std::fs::read(&p).unwrap()).unwrap();
    v["version"] = "9.9.9".into();
    std::fs::write(&p, serde_json::to_vec_pretty(&v).unwrap()).unwrap();
    let e = sfi_inspection::installed(&g).unwrap_err();
    assert!(e.contains("is not the one this Kiln installed"), "{e}");
}

/// The helper's files in another component (the MIT sfi, core) are no inspector.
#[test]
fn a_helper_of_another_component_is_no_inspector() {
    if PLATFORM.is_none() {
        return;
    }
    let t = tmp("component");
    let exe = helper(VERSION);
    let v = serde_json::to_vec(&version_file(&exe)).unwrap();
    let g = installed_game(&t.0, "core", &stage(&t.0, "core", &exe, &v, "core"));
    let e = sfi_inspection::installed(&g).unwrap_err();
    assert!(e.contains("other than as a file of component eml"), "{e}");
}

/// Linux: the installed helper (made executable by the install) runs, and its answer is read with version.json's
/// version: through `Installed`, as the launcher and kilnctl run it.
#[test]
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn the_installed_helper_runs_and_its_answer_is_read() {
    let t = tmp("runs");
    let g = good(&t.0, "a");
    let facts = sfi_inspection::inspect(&Installed, &g, RunSide::Client).unwrap();
    assert_eq!(facts.inspector_version, VERSION);
    assert_eq!(facts.exe_sha256, sfi_profile_common::EXE);
    // read without resolving it for the game first, an answer is refused (fail closed)
    use kiln_manage::sfi_inspection::Inspect as _;
    let out = Installed.run(&g, RunSide::Client).unwrap();
    assert!(sfi_inspection::facts(&out, RunSide::Client, Installed.version()).is_err());
}

/// Linux: a swapped helper is refused before it starts; one without its execute bit is said as such, and nothing is
/// copied anywhere to run it.
#[test]
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn a_swapped_or_unexecutable_helper_is_said_and_never_copied() {
    use std::os::unix::fs::PermissionsExt as _;
    let t = tmp("swapped");
    let g = good(&t.0, "a");
    let exe = rel_path(&g, EXE_NAME);
    let listing = |root: &Path| {
        let mut v = Vec::new();
        let mut stack = vec![root.to_path_buf()];
        while let Some(d) = stack.pop() {
            for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
                if e.file_type().unwrap().is_dir() {
                    stack.push(e.path())
                } else {
                    v.push(e.path())
                }
            }
        }
        v.sort();
        v
    };
    let before = listing(&t.0);
    std::fs::write(&exe, helper("6.6.6")).unwrap();
    let e = sfi_inspection::inspect(&Installed, &g, RunSide::Client).unwrap_err();
    assert!(e.contains("is not the one this Kiln ships"), "{e}");
    std::fs::write(&exe, helper(VERSION)).unwrap();
    std::fs::set_permissions(&exe, std::fs::Permissions::from_mode(0o644)).unwrap();
    let e = sfi_inspection::inspect(&Installed, &g, RunSide::Client).unwrap_err();
    assert!(e.contains("execute permission"), "{e}");
    assert_eq!(listing(&t.0), before, "nothing copied or written to run it");
    assert_eq!(
        std::fs::metadata(&exe).unwrap().permissions().mode() & 0o777,
        0o644,
        "not chmodded by a run"
    );
}

/// restore_executable: the file proven on its own handle (no link followed, a plain file, the receipt's bytes) before
/// any chmod; nothing else is ever changed.
#[test]
#[cfg(unix)]
fn restore_executable_proves_the_file_on_its_handle() {
    use std::os::unix::fs::PermissionsExt as _;
    let t = tmp("restore");
    let mode = |p: &Path| std::fs::symlink_metadata(p).unwrap().permissions().mode() & 0o7777;
    let f = t.0.join("helper");
    std::fs::write(&f, b"helper bytes").unwrap();
    std::fs::set_permissions(&f, std::fs::Permissions::from_mode(0o640)).unwrap();
    assert!(bundle::restore_executable(&f, &sha(b"other bytes")).is_err());
    assert_eq!(mode(&f), 0o640, "other bytes: unchanged");
    let link = t.0.join("link");
    std::os::unix::fs::symlink(&f, &link).unwrap();
    assert!(bundle::restore_executable(&link, &sha(b"helper bytes")).is_err());
    assert_eq!(mode(&f), 0o640, "a link: its target unchanged");
    let dir = t.0.join("dir");
    std::fs::create_dir(&dir).unwrap();
    assert!(bundle::restore_executable(&dir, &sha(b"")).is_err());
    let fifo = t.0.join("fifo");
    let c = std::ffi::CString::new(fifo.to_str().unwrap()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(c.as_ptr(), 0o600) }, 0);
    assert!(
        bundle::restore_executable(&fifo, &sha(b"")).is_err(),
        "a FIFO: refused without blocking"
    );
    assert!(bundle::restore_executable(&f, &sha(b"helper bytes")).unwrap());
    assert_eq!(mode(&f), 0o755);
    assert!(
        !bundle::restore_executable(&f, &sha(b"helper bytes")).unwrap(),
        "it has them: nothing to do"
    );
}

/// The allowlist: exactly that path of exactly component eml.
#[test]
fn only_the_linux_helper_of_component_eml_is_executable() {
    assert!(bundle::executable(bundle::EXECUTABLE_HELPER, "eml"));
    assert_eq!(
        bundle::EXECUTABLE_HELPER,
        "kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect"
    );
    for (p, c) in [
        (bundle::EXECUTABLE_HELPER, "sfi"),
        (bundle::EXECUTABLE_HELPER, "core"),
        (
            "kiln/tools/eml-inspect/windows-x86_64/kiln-eml-inspect.exe",
            "eml",
        ),
        (
            "Kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect",
            "eml",
        ),
        ("kiln/tools/eml-inspect/linux-x86_64/kiln-eml-run", "eml"),
        ("kiln/plugins/kiln-eml.dll", "eml"),
    ] {
        assert!(!bundle::executable(p, c), "{p} {c}");
    }
}

/// The launcher's and kilnctl's start (`use_installed_inspector`): a kiln-mode activation runs the helper the Kiln
/// installed in that game brought (never the process's stand-in), and a game whose helper is gone gets no plan, with
/// that reason. One test: the inspector is one per process.
#[test]
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn a_kiln_mode_activation_runs_the_installed_helper() {
    use kiln_manage::profile;
    use kiln_manage::sfi_plan::Mode;
    use std::os::unix::fs::PermissionsExt as _;
    let w = sfi_profile_common::world("installed-helper", false);
    profile::sfi::use_installed_inspector();
    // the helper as an install of component eml leaves it: its three files and the receipt naming them
    let exe = helper(VERSION);
    let v = serde_json::to_vec_pretty(&version_file(&exe)).unwrap();
    let d = format!("kiln/tools/eml-inspect/{}", platform());
    let files: Vec<(String, Vec<u8>)> = vec![
        (format!("{d}/{EXE_NAME}"), exe.clone()),
        (format!("{d}/version.json"), v),
        (
            format!("{d}/archive-inspection.schema.json"),
            SCHEMA.to_vec(),
        ),
    ];
    for (rel, b) in &files {
        put(&w.game, rel, b);
    }
    std::fs::set_permissions(
        w.game.join(&files[0].0),
        std::fs::Permissions::from_mode(0o755),
    )
    .unwrap();
    let receipt = serde_json::json!({"format": 1, "installId": "g-installed-helper", "installed": "2026-11-20T00:00:00Z",
        "runtime": {"version": "0.2.0-dev+20261120.aaaaaaa", "components": ["eml"]}, "side": "client",
        "files": files.iter().map(|(p, b)| serde_json::json!({"path": p, "sha256": sha(b), "size": b.len(), "component": "eml"})).collect::<Vec<_>>()});
    put(
        &w.game,
        "kiln/.install.json",
        &serde_json::to_vec_pretty(&receipt).unwrap(),
    );
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"));
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!((s.route, &s.refused), ("interpreter", &None), "{s:?}");
    assert!(s.plan.is_some(), "{s:?}");
    let rec = profile::sfi::record(&w.dirs, &w.inst).active.unwrap();
    assert_eq!(rec.outcome, "written", "{rec:?}");
    assert_eq!(
        rec.inspection.as_ref().map(|i| i.version.as_str()),
        Some(VERSION)
    );
    assert_eq!(
        sfi_profile_common::inspections(&w.game),
        0,
        "the process's stand-in never ran"
    );
    // the helper gone: no plan, and why (the helper the receipt names, never the "no inspector" text)
    let exe_path = w.game.join(&files[0].0);
    std::fs::remove_file(&exe_path).unwrap();
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"));
    let rec = profile::sfi::record(&w.dirs, &w.inst).active.unwrap();
    assert_eq!(rec.code.as_deref(), Some("no-archive-base"), "{rec:?}");
    let why = rec.reason.as_deref().unwrap_or_default();
    assert!(
        why.contains(&exe_path.display().to_string()) && !why.contains(profile::sfi::NO_INSPECTOR),
        "{rec:?}"
    );
    assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none());
    // the helper back, but its version.json is no longer the receipt's: the installed inspector cannot be resolved for
    // this game, so no plan, with installed()'s reason (never the "no inspector" text, never another inspector)
    put(&w.game, &files[0].0, &files[0].1);
    std::fs::set_permissions(&exe_path, std::fs::Permissions::from_mode(0o755)).unwrap();
    let mut forged: serde_json::Value = serde_json::from_slice(&files[1].1).unwrap();
    forged["version"] = "9.9.9".into();
    put(
        &w.game,
        &files[1].0,
        &serde_json::to_vec_pretty(&forged).unwrap(),
    );
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"));
    let rec = profile::sfi::record(&w.dirs, &w.inst).active.unwrap();
    assert_eq!(rec.code.as_deref(), Some("no-archive-base"), "{rec:?}");
    let why = rec.reason.as_deref().unwrap_or_default();
    assert!(
        why.contains("version.json is not the one this Kiln installed")
            && !why.contains(profile::sfi::NO_INSPECTOR),
        "{rec:?}"
    );
    assert_eq!(
        sfi_profile_common::inspections(&w.game),
        0,
        "the process's stand-in never ran"
    );
    assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none());
}

/// A real staged bundle (`SFI_REAL_BUNDLE`): installed into a synthetic client folder, its helper for this platform
/// resolves from the receipt and version.json, starts (on Linux through its checked handle: a real ELF, 0755) and
/// answers in the version its version.json names; the folder holds no game archive, so its answer is a refusal.
#[test]
#[ignore = "needs SFI_REAL_BUNDLE=<a staged kiln-runtime zip>"]
fn the_real_staged_bundle_installs_and_its_helper_answers() {
    let zip = PathBuf::from(std::env::var_os("SFI_REAL_BUNDLE").expect("SFI_REAL_BUNDLE"));
    let t = tmp("real");
    let g = t.0.join("game");
    put(&g, "enshrouded.exe", b"MZ");
    let r = install::install(
        &DataDirs::at(t.0.join("data")),
        &client(&g),
        &zip,
        &InstallOptions::default(),
        &mut NoProgress,
    )
    .unwrap();
    // every default the bundle has is installed (a partial preview stage may lack the hosted runtime: then it says so)
    let m = bundle::read(&zip).unwrap();
    for comp in ["core", "eml", "sfi", "shroudforge"] {
        if comp == "shroudforge" && !m.components.contains_key(comp) {
            println!("this bundle has no component {comp}");
            continue;
        }
        assert!(
            r.runtime.components.iter().any(|c| c == comp),
            "{comp} installed by default: {:?}",
            r.runtime.components
        );
    }
    assert!(g.join("kiln/plugins/kiln-sfi.dll").is_file());
    assert!(!g.join("kiln/plugins/shroudforge/kiln-sfi.dll").exists());
    for p in [
        "linux-x86_64/kiln-eml-inspect",
        "windows-x86_64/kiln-eml-inspect.exe",
    ] {
        let f = r
            .files
            .iter()
            .find(|f| f.path == format!("kiln/tools/eml-inspect/{p}"))
            .unwrap_or_else(|| panic!("{p}"));
        assert_eq!(f.component.as_deref(), Some("eml"));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let m = std::fs::metadata(g.join(bundle::EXECUTABLE_HELPER))
            .unwrap()
            .permissions()
            .mode()
            & 0o7777;
        assert_eq!(m, 0o755);
    }
    let tool = sfi_inspection::installed(&g).unwrap();
    println!(
        "tool: {} {} {}",
        tool.exe.display(),
        tool.sha256,
        tool.version
    );
    let e = sfi_inspection::inspect(&Installed, &g, RunSide::Client).unwrap_err();
    println!("answer: {e}");
    assert!(
        e.starts_with("the archive inspector found no proven game archive base"),
        "{e}"
    );
}
