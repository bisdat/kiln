// SPDX-License-Identifier: MIT
//! Shared helpers for the routing tests: synthetic game folders, Library packages, a ShroudForge install
//! whose config names another mods folder, kiln.json selections, a snapshot and an inotify tripwire over
//! `<game>/shroudforge`, and a dump of what an activation leaves. Every package and DLL here is synthetic.
#![allow(dead_code)]

use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile::ProfileMod;
use kiln_manage::task::NoProgress;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

pub struct Tmp(pub PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            for d in [
                self.0.join("game/shroudforge"),
                self.0.join("twin/shroudforge"),
            ] {
                if d.exists() {
                    let _ = std::fs::set_permissions(&d, std::fs::Permissions::from_mode(0o755));
                }
            }
        }
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
static N: AtomicUsize = AtomicUsize::new(0);
pub fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!(
        "km-sfi-routing-{tag}-{}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::SeqCst)
    ));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    // (macOS names its temporary folder through a link; Windows' canonical form is the verbatim one, not wanted here)
    Tmp(if cfg!(windows) {
        p
    } else {
        p.canonicalize().unwrap()
    })
}
pub fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

// ---- the selections ----

/// kiln.json selecting Kiln's interpreter, with the release hold lifted as a preview build lifts it.
pub const KILN: &str =
    r#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}}"#;
/// The same without the lift: the core refuses it at boot (held).
pub const KILN_HELD: &str = r#"{"shroudforge": {"backend": "kiln"}}"#;

/// This build's gate list holds Kiln's interpreter (a `kiln-sfi.interpreter` row): kiln.json without "preview" is then
/// held. A list with no such row makes it a normal choice in Settings, and the held cases do not apply.
pub fn interpreter_is_a_preview() -> bool {
    kiln_manage::sfi_release::ReleasePolicy::shipped()
        .rows()
        .unwrap()
        .iter()
        .any(|r| r.name == kiln_manage::sfi_release::INTERPRETER)
}
pub const HOSTED: &str = r#"{"shroudforge": {"backend": "hosted"}}"#;
pub const INSTALLED: &str = r#"{"shroudforge": {"backend": "installed"}}"#;
pub const USE_INSTALLED: &str = r#"{"shroudforge": {"useInstalled": true}}"#;
pub const INVALID: &str =
    r#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "Kiln"}}"#;

pub fn select(game: &Path, kiln_json: Option<&str>) {
    let f = game.join("kiln/kiln.json");
    match kiln_json {
        Some(text) => put(game, "kiln/kiln.json", text.as_bytes()),
        None => {
            let _ = std::fs::remove_file(f);
        }
    }
}

// ---- the game ----

/// A client game folder with Kiln's ShroudForge adapter and its bundled runtime, and (`interpreter`) Kiln's interpreter.
pub fn game(dir: &Path, interpreter: bool) -> PathBuf {
    put(dir, "enshrouded.exe", b"MZ");
    put(dir, "kiln/plugins/kiln-shroudforge.dll", b"adapter");
    put(
        dir,
        "kiln/plugins/shroudforge/shroudforge-runtime.dll",
        b"runtime",
    );
    if interpreter {
        put(dir, "kiln/plugins/kiln-sfi.dll", b"interpreter");
    }
    dir.to_path_buf()
}

/// The install record of a game folder in `t` (Z: through a prefix of its own, or Copy mode).
#[cfg(unix)]
pub fn install(
    t: &Path,
    game: &Path,
    mode: kiln_manage::game::PathMode,
) -> kiln_manage::game::GameInstall {
    use kiln_manage::game::{GameInstall, GameKind, Platform};
    let pfx = t.join(format!(
        "{}-pfx",
        game.file_name().unwrap().to_string_lossy()
    ));
    if !pfx.join("dosdevices/z:").exists() {
        std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
        std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    }
    GameInstall {
        id: format!("g-{}", game.file_name().unwrap().to_string_lossy()),
        kind: GameKind::Client,
        path: game.to_path_buf(),
        exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID,
        library: None,
        platform: Platform::Proton,
        prefix: Some(pfx),
        build_id: None,
        exe_sha256: None,
        steam_flavor: None,
        path_mode: mode,
        source: "manual".into(),
    }
}

/// The install record of a game folder on Windows (its real paths, or Copy mode; Z: is Wine's drive).
#[cfg(windows)]
pub fn install(
    _t: &Path,
    game: &Path,
    mode: kiln_manage::game::PathMode,
) -> kiln_manage::game::GameInstall {
    use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
    assert!(mode != PathMode::Z, "Z: is Wine's drive, not Windows'");
    GameInstall {
        id: format!("g-{}", game.file_name().unwrap().to_string_lossy()),
        kind: GameKind::Client,
        path: game.to_path_buf(),
        exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID,
        library: None,
        platform: Platform::Windows,
        prefix: None,
        build_id: None,
        exe_sha256: None,
        steam_flavor: None,
        path_mode: mode,
        source: "manual".into(),
    }
}

/// ShroudForge installed in the game folder (its runtime, version, state with a startup patch) with a config that names
/// `mods` as its mods folder (None: no config, `<game>/mods`).
pub fn sf_install(game: &Path, mods: Option<&str>) {
    put(game, "shroudforge/shroudforge-runtime.dll", b"sf runtime");
    put(game, "shroudforge/kfc-runtime.dll", b"kfc runtime");
    put(game, "shroudforge/version.json", br#"{"version":"1.7.9"}"#);
    put(
        game,
        "shroudforge/state.json",
        br#"{"assets":{"status":"applied"},"runtime":{"running":false,"updatedAt":1,"active":[]}}"#,
    );
    if let Some(m) = mods {
        put(
            game,
            "shroudforge/config/modloader-config.json",
            format!(
                r#"{{"paths":{{"mods":{},"state":"shroudforge"}}}}"#,
                serde_json::Value::String(m.into())
            )
            .as_bytes(),
        );
    }
}

/// ShroudForge's own loader next to the game (its dbghelp.dll).
pub fn sf_own_loader(game: &Path) {
    put(
        game,
        "dbghelp.dll",
        &build_test_dll(&[], b"EML_LOG_FILE_PATH shroudforge"),
    );
}

// ---- packages ----

/// A ShroudForge package with a setting, optionally an XHL-Core plugin too (its native-plugin.ini and DLL).
pub fn sf_package(root: &Path, id: &str, xhl: bool) -> PathBuf {
    let d = root.join("src").join(id);
    put(
        &d,
        "mod.json",
        format!(r#"{{"id":"{id}","name":"{id}","version":"1.0.0","capabilities":["runtime"]}}"#)
            .as_bytes(),
    );
    put(
        &d,
        "extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"targets":["client","server"],
        "settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#,
    );
    put(&d, "src/mod.lua", b"return {}");
    if xhl {
        put(
            &d,
            "native-plugin.ini",
            b"[Plugin]\nName=Tools\nDll=tools.dll\nEnabled=1\n",
        );
        put(&d, "tools.dll", &build_test_dll(&[], b"tools"));
    }
    d
}

/// A legacy EML package (mod.json and Lua, no extended.mod.json).
pub fn eml_package(root: &Path, id: &str) -> PathBuf {
    let d = root.join("src").join(id);
    put(
        &d,
        "mod.json",
        format!(r#"{{"id":"{id}","name":"{id}","version":"0.2.2","capabilities":["patch"]}}"#)
            .as_bytes(),
    );
    put(&d, "src/mod.lua", b"return {}");
    d
}

/// An EML package that needs ShroudForge's runtime (startup data and a runtime part).
pub fn eml_runtime_package(root: &Path, id: &str) -> PathBuf {
    let d = root.join("src").join(id);
    put(
        &d,
        "mod.json",
        format!(
            r#"{{"id":"{id}","name":"{id}","version":"1.0.0","capabilities":["patch","runtime"]}}"#
        )
        .as_bytes(),
    );
    put(&d, "src/mod.lua", b"return {}");
    d
}

/// An EMF Lua mod, EMF's own layout.
pub fn emf_lua_package(root: &Path, id: &str) -> PathBuf {
    let d = root.join("src").join(id);
    put(&d, "mod.json", format!(r#"{{"id":"{id}","name":"{id}","version":"1.0.0","emf":14,"main":"main.lua","enabled":false}}"#).as_bytes());
    put(&d, "main.lua", b"return {}\n");
    d
}

/// A Greeneye mod, a Flame mod and a SET mod, as their authors ship them.
pub fn other_packages(root: &Path) -> Vec<PathBuf> {
    let ge = root.join("src/worstmap");
    put(&ge, "mod.json", br#"{"id":"worstmap","name":"The Worst Map Mod","version":"1.0.0","dll":"map.dll","side":"client"}"#);
    put(&ge, "map.dll", &build_test_dll(&["GreeneyeModStart"], b""));
    let flame = root.join("src/creative");
    put(
        &flame,
        "mod.json",
        br#"{"id":"creative","name":"Creative Mode","version":"1.4.6"}"#,
    );
    put(&flame, "flame.json", br#"{"dll":"creative.dll"}"#);
    put(
        &flame,
        "creative.dll",
        &build_test_dll(&["FlameStartRuntime"], b""),
    );
    let set = root.join("src/flymode");
    put(&set, "init.lua", b"print('fly')");
    vec![ge, flame, set]
}

/// A plugin loader under a proxy name the game loads itself (game_files: placed in the game folder).
pub fn game_folder_package(root: &Path) -> PathBuf {
    let p = root.join("src/AutolootCoop 1.6.0");
    put(
        &p,
        "powrprof.dll",
        &build_test_dll(
            &["PowerGetActiveScheme", "PowerReadACValue"],
            b"PluginLoader.log plugins\\",
        ),
    );
    put(
        &p,
        "plugins/AutolootCoop.dll",
        &build_test_dll(&[], b"AutolootCoop"),
    );
    put(&p, "AutolootCoop.ini", b"[Autoloot]\nEnabled=1\n");
    p
}

pub fn import(dirs: &DataDirs, src: &Path, kind: ModKind, id: &str) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported
        .iter()
        .chain(&r.existing)
        .find(|x| x.kind == kind && x.id == id)
        .cloned()
        .unwrap_or_else(|| panic!("{kind:?} {id} not imported: {r:?}"))
}

pub fn import_kind(dirs: &DataDirs, src: &Path, kind: ModKind) -> LibraryRef {
    let r = library::import(dirs, &[src.to_path_buf()], &mut NoProgress).unwrap();
    r.imported
        .iter()
        .chain(&r.existing)
        .find(|x| x.kind == kind)
        .cloned()
        .unwrap_or_else(|| panic!("no {kind:?}: {r:?}"))
}

pub fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod {
        id: r.id.clone(),
        version: r.version.clone(),
        hash: r.hash.clone(),
        enabled,
    }
}

// ---- snapshots ----

/// Every entry below `root`: folders, files with their bytes, links with their target; each path with '/' between its
/// parts (on Windows too).
pub fn snapshot(root: &Path) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    fn walk(root: &Path, dir: &Path, out: &mut BTreeMap<String, String>) {
        let mut entries: Vec<_> = std::fs::read_dir(dir)
            .into_iter()
            .flatten()
            .flatten()
            .collect();
        entries.sort_by_key(|e| e.file_name());
        for e in entries {
            let p = e.path();
            let rel = p.strip_prefix(root).unwrap().to_string_lossy().into_owned();
            #[cfg(windows)]
            let rel = rel.replace('\\', "/");
            let meta = std::fs::symlink_metadata(&p).unwrap();
            if meta.file_type().is_symlink() {
                out.insert(
                    rel,
                    format!("-> {}", std::fs::read_link(&p).unwrap().display()),
                );
            } else if meta.is_dir() {
                out.insert(format!("{rel}/"), String::new());
                walk(root, &p, out);
            } else {
                let bytes = std::fs::read(&p).unwrap();
                out.insert(rel, kiln_manage::hash::sha256_hex(&bytes));
            }
        }
    }
    walk(root, root, &mut out);
    out
}

/// A text with the temporary folder of a test written `<T>` (as a host path and as the game's Windows Z: path, each also
/// as JSON spells it: a Windows path doubles its backslashes there).
pub fn normal(text: &str, t: &Path) -> String {
    let host = t.to_string_lossy().into_owned();
    let hj = host.replace('\\', "\\\\");
    let z = format!("Z:{}", host.replace('/', "\\"));
    let zj = z.replace('\\', "\\\\");
    text.replace(&zj, "<T>")
        .replace(&z, "<T>")
        .replace(&hj, "<T>")
        .replace(&host, "<T>")
}

/// JSON with the members that change from run to run (times, a fingerprint over host paths) taken out, and `<T>`.
pub fn stable_json(v: &serde_json::Value, t: &Path) -> String {
    fn strip(v: &mut serde_json::Value) {
        match v {
            serde_json::Value::Object(m) => {
                for k in [
                    "activated",
                    "updated",
                    "created",
                    "written",
                    "fingerprint",
                    "time",
                    "at",
                    "stamp",
                    "pid",
                    "launchId",
                ] {
                    m.remove(k);
                }
                m.values_mut().for_each(strip);
            }
            serde_json::Value::Array(a) => a.iter_mut().for_each(strip),
            _ => {}
        }
    }
    let mut v = v.clone();
    strip(&mut v);
    normal(&serde_json::to_string_pretty(&v).unwrap(), t)
}

/// What a game folder holds, for a comparison: each file with the SHA-256 of its text with `<T>` (JSON files without
/// their run-to-run members), each folder and link.
pub fn dump_tree(game: &Path, t: &Path) -> Vec<String> {
    let mut out = Vec::new();
    for (rel, v) in snapshot(game) {
        let p = game.join(&rel);
        let line = if rel.ends_with('/') || v.starts_with("-> ") {
            format!("{rel} {}", normal(&v, t))
        } else {
            let bytes = std::fs::read(&p).unwrap();
            let text = match serde_json::from_slice::<serde_json::Value>(&bytes) {
                Ok(j) if rel.ends_with(".json") => stable_json(&j, t),
                _ => match std::str::from_utf8(&bytes) {
                    Ok(s) => normal(s, t),
                    Err(_) => kiln_manage::hash::sha256_hex(&bytes),
                },
            };
            if rel.ends_with(".lock") {
                format!("{rel} (lock)")
            } else if rel == "kiln/profile.json" {
                format!("{rel} {text}")
            } else {
                format!("{rel} {}", kiln_manage::hash::sha256_hex(text.as_bytes()))
            }
        };
        out.push(line);
    }
    out
}

// ---- the tripwire ----

/// Every open, listing, change or move of a file or folder below a root, seen through inotify (Linux). A plain
/// `stat` is not seen; what ShroudForge's readers do after one (open its config, its state, its version, list its mods
/// folder) is.
#[cfg(target_os = "linux")]
pub struct Tripwire {
    fd: i32,
    dirs: BTreeMap<i32, PathBuf>,
}

#[cfg(target_os = "linux")]
impl Tripwire {
    pub fn arm(root: &Path) -> Tripwire {
        use std::os::unix::ffi::OsStrExt;
        let fd = unsafe { libc::inotify_init1(libc::IN_NONBLOCK | libc::IN_CLOEXEC) };
        assert!(fd >= 0, "inotify_init1");
        let mut dirs = BTreeMap::new();
        let mut todo = vec![root.to_path_buf()];
        while let Some(d) = todo.pop() {
            let c = std::ffi::CString::new(d.as_os_str().as_bytes()).unwrap();
            let wd = unsafe { libc::inotify_add_watch(fd, c.as_ptr(), libc::IN_ALL_EVENTS) };
            assert!(wd >= 0, "watch {}", d.display());
            for e in std::fs::read_dir(&d).unwrap().flatten() {
                if e.file_type().unwrap().is_dir() {
                    todo.push(e.path());
                }
            }
            dirs.insert(wd, d);
        }
        let mut t = Tripwire { fd, dirs };
        t.events(); // the arming's own listings
        t
    }

    /// The events since the last call, `<path> <mask>`.
    pub fn events(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            let n = unsafe { libc::read(self.fd, buf.as_mut_ptr().cast(), buf.len()) };
            if n <= 0 {
                break;
            }
            let mut i = 0usize;
            while i < n as usize {
                let ev: libc::inotify_event =
                    unsafe { std::ptr::read_unaligned(buf[i..].as_ptr().cast()) };
                let head = std::mem::size_of::<libc::inotify_event>();
                let name = &buf[i + head..i + head + ev.len as usize];
                let name = String::from_utf8_lossy(name.split(|b| *b == 0).next().unwrap_or(&[]))
                    .into_owned();
                let dir = self
                    .dirs
                    .get(&ev.wd)
                    .map(|d| d.display().to_string())
                    .unwrap_or_default();
                out.push(format!("{dir}/{name} {:#x}", ev.mask));
                i += head + ev.len as usize;
            }
        }
        out
    }
}

#[cfg(target_os = "linux")]
impl Drop for Tripwire {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

/// Without inotify: nothing is seen (the snapshot and the folders placed still say what was done).
#[cfg(not(target_os = "linux"))]
pub struct Tripwire;
#[cfg(not(target_os = "linux"))]
impl Tripwire {
    pub fn arm(_root: &Path) -> Tripwire {
        Tripwire
    }
    pub fn events(&mut self) -> Vec<String> {
        Vec::new()
    }
}
