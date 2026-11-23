//! Runtime bundles unpacked into a folder (\`kiln-runtime.json\` at its root). The Nexus Mods runtime file carries the
//! bundle as a folder instead of the zip: Nexus cannot scan an archive inside an archive. A folder is held to the zip's
//! rules (\`bundle::verify\`, \`bundle::extract\`): every listed file there with its size and sha256, nothing unlisted,
//! safe names only, no links; and installing it gives what installing the zip gives.
#![cfg(feature = "zip")]

use kiln_manage::bundle::{self, CreateOptions};
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::install::{self, InstallOptions};
use kiln_manage::pe::build_test_dll;
use kiln_manage::task::NoProgress;
use kiln_manage::{ErrorCode, hash, paths::DataDirs};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-bundle-folder-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A staging folder like tools/package/stage.sh writes, with the one executable runtime file (bundle::EXECUTABLE_HELPER).
fn bundle_zip(root: &Path) -> PathBuf {
    let s = root.join("stage");
    put(&s, "kiln-runtime.json", br#"{
  "format": 1, "kiln": "0.2.0-dev", "abi": 2, "api": "0.2.16", "dotnet": "10.0.14",
  "components": {
    "core": { "sides": ["server", "client"], "default": true },
    "sigdb-server": { "sides": ["server"], "default": true, "requires": ["core"] },
    "sigdb-client": { "sides": ["client"], "default": true, "requires": ["core"] },
    "eml": { "sides": ["server", "client"], "default": true, "requires": ["core"] },
    "samples": { "sides": ["server"], "default": false, "requires": ["core"] }
  },
  "config": ["kiln/kiln.json"]
}"#);
    let proxy = build_test_dll(&["DirectInput8Create", "GetdfDIJoystick", "KilnProxyInfo"], b"KilnProxy/2 0.2.0-dev+folder\0");
    put(&s, "core/dinput8.dll", &proxy);
    put(&s, "core/kiln/kiln-core.dll", b"core");
    put(&s, "core/kiln/kiln.json", b"{\n  \"policy\": \"verified-only\"\n}\n");
    put(&s, "core/kiln/host/Kiln.Host.dll", b"host");
    put(&s, "core/kiln/dotnet/shared/Microsoft.NETCore.App/10.0.14/System.Formats.Tar.dll", b"not an archive");
    put(&s, "sigdb-client/kiln/sigdb/client.json", b"{\"symbols\":{}}");
    put(&s, "sigdb-server/kiln/sigdb/server.json", b"{\"symbols\":{}}");
    put(&s, &format!("eml/{}", bundle::EXECUTABLE_HELPER), b"\x7fELF helper");
    put(&s, "eml/kiln/plugins/kiln-eml.dll", b"eml plugin");
    put(&s, "samples/kiln/mods/hello/Hello.dll", b"hello sample");
    bundle::create(&s, &root.join("out"), &CreateOptions { source_date_epoch: 1_793_793_600, git: "f01d3a5".into() }).unwrap()
}

/// The zip's entries at their names below \`dest\`: what tools/release/nexus.py puts in the Nexus runtime file's
/// kiln-runtime-<v>/ folder, and what unzipping the bundle gives a player.
fn unpack(zip: &Path, dest: &Path) -> PathBuf {
    use std::io::Read;
    let mut a = zip::ZipArchive::new(std::fs::File::open(zip).unwrap()).unwrap();
    for i in 0..a.len() {
        let mut e = a.by_index(i).unwrap();
        let name = e.name().unwrap().to_string();
        let mut data = Vec::new();
        e.read_to_end(&mut data).unwrap();
        put(dest, &name, &data);
    }
    dest.to_path_buf()
}

fn folder(t: &Tmp) -> PathBuf {
    let zip = bundle_zip(&t.0);
    unpack(&zip, &t.0.join("kiln-runtime-folder"))
}

fn client(dir: &Path) -> GameInstall {
    GameInstall {
        id: "g-test".into(),
        kind: GameKind::Client,
        path: dir.to_path_buf(),
        exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID,
        library: None,
        platform: Platform::Proton,
        prefix: None,
        build_id: Some("26404966".into()),
        exe_sha256: None,
        steam_flavor: None,
        path_mode: PathMode::Copy,
        source: "manual".into(),
    }
}

/// Every file below \`dir\` (relative '/' path -> bytes and, on Unix, whether it is executable), leaving out the paths
/// that name a time or a random suffix (the backups) and the receipt.
fn tree(dir: &Path) -> BTreeMap<String, (Vec<u8>, bool)> {
    fn walk(root: &Path, d: &Path, out: &mut BTreeMap<String, (Vec<u8>, bool)>) {
        for e in std::fs::read_dir(d).unwrap().flatten() {
            let p = e.path();
            let rel = p.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
            if rel == "kiln/backups" || rel == "kiln/.install.json" {
                continue;
            }
            let ft = e.file_type().unwrap();
            if ft.is_dir() {
                walk(root, &p, out);
            } else {
                #[cfg(unix)]
                let exec = {
                    use std::os::unix::fs::PermissionsExt;
                    std::fs::metadata(&p).unwrap().permissions().mode() & 0o111 != 0
                };
                #[cfg(not(unix))]
                let exec = false;
                out.insert(rel, (std::fs::read(&p).unwrap(), exec));
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(dir, dir, &mut out);
    out
}

fn code(r: kiln_manage::Result<bundle::RuntimeManifest>) -> (ErrorCode, String) {
    let e = r.expect_err("the folder must be refused");
    (e.code, e.message)
}

#[test]
fn a_folder_verifies_and_reads_like_its_zip() {
    let t = tmp("good");
    let zip = bundle_zip(&t.0);
    let dir = unpack(&zip, &t.0.join("kiln-runtime-good"));
    let from_zip = bundle::verify(&zip, &mut NoProgress).unwrap();
    let from_dir = bundle::verify(&dir, &mut NoProgress).expect("an unpacked bundle verifies");
    assert_eq!(from_dir, from_zip);
    assert_eq!(bundle::read(&dir).unwrap(), from_zip);
    // an empty folder that the manifest does not list is fine, as a zip's folder entries are
    std::fs::create_dir_all(dir.join("kiln/empty")).unwrap();
    bundle::verify(&dir, &mut NoProgress).unwrap();
    // extract stages exactly the listed files, checked, from the folder too
    let plan = install::plan(&from_dir, kiln_manage::manifest::RunSide::Client, None).unwrap();
    let staged = t.0.join("staged");
    bundle::extract(&dir, &plan, &staged, &mut NoProgress).unwrap();
    for f in &plan {
        assert_eq!(hash::sha256_file(&staged.join(&f.path)).unwrap().0, f.sha256, "{}", f.path);
    }
    // a folder without kiln-runtime.json at its root is no bundle
    std::fs::remove_file(dir.join(bundle::MANIFEST)).unwrap();
    let (_, msg) = code(bundle::verify(&dir, &mut NoProgress));
    assert!(msg.contains(bundle::MANIFEST), "{msg}");
    assert!(bundle::read(&dir).is_err());
}

#[test]
fn a_missing_file_is_refused() {
    let t = tmp("missing");
    let dir = folder(&t);
    std::fs::remove_file(dir.join("kiln/kiln-core.dll")).unwrap();
    let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
    assert_eq!(c, ErrorCode::Schema, "{msg}");
    assert!(msg.contains("kiln/kiln-core.dll"), "{msg}");
}

#[test]
fn changed_bytes_are_refused() {
    let t = tmp("hash");
    let dir = folder(&t);
    // same size, other bytes
    std::fs::write(dir.join("kiln/kiln-core.dll"), b"CORE").unwrap();
    let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
    assert_eq!(c, ErrorCode::HashMismatch, "{msg}");
    // longer
    std::fs::write(dir.join("kiln/kiln-core.dll"), b"core and more").unwrap();
    assert_eq!(code(bundle::verify(&dir, &mut NoProgress)).0, ErrorCode::HashMismatch);
    // the staged copy is checked the same way
    let m = bundle::read(&dir).unwrap();
    let f: Vec<_> = m.files.iter().filter(|f| f.path == "kiln/kiln-core.dll").cloned().collect();
    let e = bundle::extract(&dir, &f, &t.0.join("staged"), &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch, "{e}");
}

#[test]
fn an_unlisted_file_is_refused_os_folder_files_too() {
    for extra in ["kiln/extra.dll", "extra.txt", ".DS_Store", "kiln/Thumbs.db", "kiln/desktop.ini", "__MACOSX/._dinput8.dll"] {
        let t = tmp(&format!("extra-{}", extra.replace(['/', '.'], "_")));
        let dir = folder(&t);
        put(&dir, extra, b"x");
        let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
        assert_eq!(c, ErrorCode::Schema, "{extra}: {msg}");
        assert!(msg.contains(extra), "{extra}: {msg}");
    }
}

#[cfg(unix)]
#[test]
fn links_and_special_files_are_refused() {
    let t = tmp("links");
    let dir = folder(&t);
    // a listed file as a link to the very bytes it should have
    let copy = t.0.join("core-copy.dll");
    std::fs::copy(dir.join("kiln/kiln-core.dll"), &copy).unwrap();
    std::fs::remove_file(dir.join("kiln/kiln-core.dll")).unwrap();
    std::os::unix::fs::symlink(&copy, dir.join("kiln/kiln-core.dll")).unwrap();
    let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
    assert_eq!(c, ErrorCode::Refused, "{msg}");
    let m = bundle::read(&dir).unwrap();
    let f: Vec<_> = m.files.iter().filter(|f| f.path == "kiln/kiln-core.dll").cloned().collect();
    assert!(bundle::extract(&dir, &f, &t.0.join("staged"), &mut NoProgress).is_err(), "extract never reads through a link");
    std::fs::remove_file(dir.join("kiln/kiln-core.dll")).unwrap();
    std::fs::copy(&copy, dir.join("kiln/kiln-core.dll")).unwrap();
    bundle::verify(&dir, &mut NoProgress).unwrap();

    // a folder of the bundle as a link to a copy of it
    let sigdb = dir.join("kiln/sigdb");
    let moved = t.0.join("sigdb-elsewhere");
    std::fs::rename(&sigdb, &moved).unwrap();
    std::os::unix::fs::symlink(&moved, &sigdb).unwrap();
    assert_eq!(code(bundle::verify(&dir, &mut NoProgress)).0, ErrorCode::Refused);
    let m = bundle::read(&dir).unwrap();
    let f: Vec<_> = m.files.iter().filter(|f| f.path == "kiln/sigdb/client.json").cloned().collect();
    assert!(bundle::extract(&dir, &f, &t.0.join("staged2"), &mut NoProgress).is_err(), "extract never goes through a linked folder");
    std::fs::remove_file(&sigdb).unwrap();
    std::fs::rename(&moved, &sigdb).unwrap();

    // the manifest as a link
    let manifest = dir.join(bundle::MANIFEST);
    let mcopy = t.0.join("manifest-copy.json");
    std::fs::rename(&manifest, &mcopy).unwrap();
    std::os::unix::fs::symlink(&mcopy, &manifest).unwrap();
    assert!(bundle::verify(&dir, &mut NoProgress).is_err());
    std::fs::remove_file(&manifest).unwrap();
    std::fs::rename(&mcopy, &manifest).unwrap();
    bundle::verify(&dir, &mut NoProgress).unwrap();

    // a FIFO: refused, never opened (an open would wait for a writer)
    let fifo = dir.join("kiln/pipe");
    let c = std::ffi::CString::new(fifo.to_str().unwrap()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(c.as_ptr(), 0o600) }, 0);
    assert_eq!(code(bundle::verify(&dir, &mut NoProgress)).0, ErrorCode::Refused);
}

#[cfg(unix)]
#[test]
fn unsafe_names_and_paths_out_of_the_folder_are_refused() {
    let t = tmp("escape");
    // names Windows would read as another path (a backslash, a trailing dot or space): refused like a zip entry's
    for bad in ["kiln\\evil.dll", "evil.dll.", "kiln/evil.dll ", "kiln/a:b"] {
        let dir = unpack(&bundle_zip(&t.0), &t.0.join(format!("kiln-runtime-{}", bad.len())));
        std::fs::write(dir.join(bad), b"x").unwrap();
        let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
        assert_eq!(c, ErrorCode::Refused, "{bad:?}: {msg}");
        std::fs::remove_dir_all(&dir).unwrap();
    }
    // a manifest that lists a path out of the folder: refused before anything is read or written
    let dir = folder(&t);
    let outside = t.0.join("outside.dll");
    std::fs::write(&outside, b"a").unwrap();
    let mut m: serde_json::Value = serde_json::from_slice(&std::fs::read(dir.join(bundle::MANIFEST)).unwrap()).unwrap();
    m["files"].as_array_mut().unwrap().insert(0, serde_json::json!({
        "path": "../outside.dll", "size": 1, "component": "core",
        "sha256": "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"
    }));
    std::fs::write(dir.join(bundle::MANIFEST), serde_json::to_vec_pretty(&m).unwrap()).unwrap();
    let (c, msg) = code(bundle::verify(&dir, &mut NoProgress));
    assert!(matches!(c, ErrorCode::Schema | ErrorCode::Refused), "{msg}");
    assert!(bundle::read(&dir).is_err());
    let game = t.0.join("game");
    put(&game, "enshrouded.exe", b"MZ fake game");
    let dirs = DataDirs::at(t.0.join("data"));
    assert!(install::install(&dirs, &client(&game), &dir, &InstallOptions::default(), &mut NoProgress).is_err());
    assert!(!game.join("dinput8.dll").exists() && !game.join("kiln/.install.json").exists());
    assert_eq!(std::fs::read(&outside).unwrap(), b"a");
}

/// Installing the folder gives what installing the zip gives (files, modes, receipt, replaced files), and uninstalling
/// gives back the same folder. The receipt's runtime.sha256 is the zip's sha256, so a folder install has none.
#[test]
fn install_and_uninstall_from_a_folder_equal_the_zip() {
    let t = tmp("install");
    let zip = bundle_zip(&t.0);
    let dir = unpack(&zip, &t.0.join("kiln-runtime-install"));
    // modes in the folder are never read: the helper goes in executable, nothing else does (bundle::executable)
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir.join(bundle::EXECUTABLE_HELPER), std::fs::Permissions::from_mode(0o644)).unwrap();
        std::fs::set_permissions(dir.join("kiln/kiln-core.dll"), std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    let dirs = DataDirs::at(t.0.join("data"));
    let emf = build_test_dll(&["DirectInput8Create", "emf_loader_identity", "emf_loader_play_once"], b"EMF loader");
    let mut games = Vec::new();
    for name in ["from-zip", "from-folder"] {
        let g = t.0.join(name).join("Enshrouded");
        put(&g, "enshrouded.exe", b"MZ fake game");
        put(&g, "dinput8.dll", &emf);
        put(&g, "kiln/kiln-core.dll", b"core");   // already there with the bundle's bytes: adopted
        put(&g, "kiln/host/Kiln.Host.dll", b"older host"); // other bytes: backed up and replaced
        games.push(g);
    }
    let before = tree(&games[0]);
    assert_eq!(before, tree(&games[1]));
    let rz = install::install(&dirs, &client(&games[0]), &zip, &InstallOptions::default(), &mut NoProgress).unwrap();
    let rd = install::install(&dirs, &client(&games[1]), &dir, &InstallOptions::default(), &mut NoProgress)
        .expect("a folder installs like its zip");
    assert_eq!(rz.runtime.sha256.as_deref(), Some(hash::sha256_file(&zip).unwrap().0.as_str()));
    assert_eq!(rd.runtime.sha256, None, "no zip, no zip sha256");
    let same = |r: &install::InstallReceipt| {
        let mut r = r.clone();
        r.install_id.clear();
        r.installed.clear();
        r.updated = None;
        r.runtime.sha256 = None;
        for x in &mut r.replaced {
            x.backup = x.backup.as_ref().map(|b| b.rsplit('/').next().unwrap().to_string());
        }
        r
    };
    assert_eq!(same(&rd), same(&rz));
    assert!(rd.replaced.iter().any(|x| x.action == install::ReplacedAction::Chained), "{:?}", rd.replaced);
    assert!(rd.replaced.iter().any(|x| x.action == install::ReplacedAction::Adopted), "{:?}", rd.replaced);
    assert!(rd.replaced.iter().any(|x| x.action == install::ReplacedAction::Replaced), "{:?}", rd.replaced);
    let installed = tree(&games[0]);
    assert_eq!(tree(&games[1]), installed);
    #[cfg(unix)]
    {
        assert!(installed[bundle::EXECUTABLE_HELPER].1, "the helper is executable");
        assert!(!installed["kiln/kiln-core.dll"].1, "nothing else is");
    }
    assert_eq!(install::status(&dirs, &client(&games[1])).unwrap().modified, Vec::<String>::new());

    for g in &games {
        install::uninstall(&dirs, &client(g), &mut NoProgress).unwrap();
    }
    let after = tree(&games[0]);
    assert_eq!(tree(&games[1]), after);
    assert_eq!(std::fs::read(games[1].join("dinput8.dll")).unwrap(), emf);
    assert_eq!(std::fs::read(games[1].join("kiln/host/Kiln.Host.dll")).unwrap(), b"older host");
    assert_eq!(std::fs::read(games[1].join("kiln/kiln-core.dll")).unwrap(), b"core");
    for (path, v) in &before {
        assert_eq!(after.get(path), Some(v), "{path} is back as it was");
    }
}

/// The launcher keeps bundles as zips in <data>/runtime/: a folder is refused there with a clear word, never half
/// copied.
#[test]
fn a_folder_is_not_imported_into_the_launchers_runtime_folder() {
    let t = tmp("import");
    let dir = folder(&t);
    let dirs = DataDirs::at(t.0.join("data"));
    let e = bundle::import(&dirs, &dir).unwrap_err();
    assert_eq!(e.code, ErrorCode::Unsupported, "{e}");
    assert!(bundle::list(&dirs).unwrap().is_empty());
    // a folder named like a zip in <data>/runtime/ is skipped by the list, not an error
    unpack(&bundle_zip(&t.0.join("again")), &dirs.runtime().join("kiln-runtime-folder.zip"));
    assert!(bundle::list(&dirs).unwrap().is_empty());
}
