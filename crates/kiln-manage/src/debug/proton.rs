//! A game client under Proton (chapter 12a §4 step 3): which runner and environment reach its prefix. `/proc` is read
//! for **runner context only**, never to pick the debuggee: the processes whose environment names exactly the install's
//! prefix (or its compat data), whose image is this install's game, and whose runner resolves to a Wine/Proton build.
//! Every such context must agree on the runner and on the allowlisted environment; otherwise `ambiguous_runner`, never
//! the first match. Which Windows process is debugged is the descriptor's and the helper's business.

use super::errors::{DebugCode, DebugError};
use super::identity::{PrefixIdentity, RunnerContext, host_path_of};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// One process of the game in the prefix, as /proc shows it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Context {
    pub pid: u32,
    /// The canonical runner executable (`<build>/bin/wine` above the process's preloader).
    pub runner: PathBuf,
    /// Its allowlisted environment (runner_env::ALLOWED).
    pub env: BTreeMap<String, String>,
    pub birth: RunnerContext,
}

/// The agreed runner of an install's prefix.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Runner {
    pub runner: PathBuf,
    pub runner_sha256: String,
    pub env: BTreeMap<String, String>,
    pub prefix: PrefixIdentity,
    /// Every agreeing context, by PID (display: the first).
    pub contexts: Vec<Context>,
}

fn canon(p: &Path) -> PathBuf {
    p.canonicalize().unwrap_or_else(|_| crate::util::clean(p))
}

fn os_path(v: &str) -> PathBuf {
    PathBuf::from(v)
}

/// The kernel truncates comm to 15 bytes.
fn comm_of(exe: &str) -> String {
    exe.bytes().take(15).map(char::from).collect()
}

fn win_basename(s: &str) -> &str {
    s.rsplit(['\\', '/']).next().unwrap_or(s)
}

/// The process is this install's game: named after its exe (comm or argv[0]), and argv[0] maps to the install's exe
/// (through the prefix's drives) or its working folder is the install's folder.
fn is_the_game(dir: &Path, install_dir: &Path, exe: &str, prefix: &Path) -> bool {
    let comm = std::fs::read_to_string(dir.join("comm")).unwrap_or_default();
    let cmd = std::fs::read(dir.join("cmdline")).unwrap_or_default();
    let argv0 = cmd.split(|&b| b == 0).next().map(|a| String::from_utf8_lossy(a).into_owned()).unwrap_or_default();
    let named = comm.trim_end().eq_ignore_ascii_case(&comm_of(exe)) || win_basename(&argv0).eq_ignore_ascii_case(exe);
    if !named {
        return false;
    }
    let want_exe = canon(&install_dir.join(exe));
    if let Some(host) = host_path_of(&argv0, Some(prefix))
        && crate::steam::same_path(&canon(&host), &want_exe)
    {
        return true;
    }
    std::fs::read_link(dir.join("cwd")).is_ok_and(|c| crate::steam::same_path(&canon(&c), &canon(install_dir)))
}

/// The prefix the process's environment names: WINEPREFIX, else STEAM_COMPAT_DATA_PATH's `pfx`.
fn named_prefix(env: &BTreeMap<String, String>) -> Option<PathBuf> {
    env.get("WINEPREFIX").map(|p| os_path(p)).or_else(|| env.get("STEAM_COMPAT_DATA_PATH").map(|c| os_path(c).join("pfx")))
}

/// Every runner context of the install's game in `prefix`, read from `proc_root` (`/proc`, or a fixture tree).
/// Contexts whose runner cannot be resolved from here are counted in the second value.
pub fn contexts(proc_root: &Path, install_dir: &Path, exe: &str, prefix: &Path) -> (Vec<Context>, usize) {
    let want = canon(prefix);
    let mut out = Vec::new();
    let mut unresolved = 0;
    let Ok(rd) = std::fs::read_dir(proc_root) else { return (out, 0) };
    let mut pids: Vec<u32> = rd.flatten().filter_map(|e| e.file_name().to_str()?.parse().ok()).collect();
    pids.sort_unstable();
    for pid in pids {
        let dir = proc_root.join(pid.to_string());
        let Ok(block) = std::fs::read(dir.join("environ")) else { continue };
        let env = super::runner_env::allowlisted(&block);
        if !named_prefix(&env).is_some_and(|p| canon(&p) == want) {
            continue;
        }
        if !is_the_game(&dir, install_dir, exe, prefix) {
            continue;
        }
        // the process's image as this host sees it: a path that does not exist here (a container's) has no runner here,
        // and walking up from it would find whatever wine the host has
        let runner = std::fs::read_link(dir.join("exe")).ok().filter(|e| e.exists()).and_then(|e| super::resolver::wine_above(&e)).map(|r| canon(&r));
        let birth = RunnerContext::read(proc_root, pid);
        match (runner, birth) {
            (Some(runner), Some(birth)) => out.push(Context { pid, runner, env, birth }),
            _ => unresolved += 1,
        }
    }
    (out, unresolved)
}

/// The one runner every context of the install's game in `prefix` agrees on (chapter 12a §4 step 3).
pub fn find_runner(proc_root: &Path, install_dir: &Path, exe: &str, prefix: &Path) -> Result<Runner, DebugError> {
    let prefix_id = PrefixIdentity::of(prefix)?;
    let (found, unresolved) = contexts(proc_root, install_dir, exe, &prefix_id.path);
    if found.is_empty() {
        if unresolved > 0 {
            return Err(DebugError::new(
                DebugCode::UnsupportedIdentity,
                format!("the game runs in {} but its Wine/Proton build cannot be reached from here (a container?): no runner to start the helper with", prefix_id.path.display()),
            )
            .because("runner_unreachable"));
        }
        return Err(DebugError::new(DebugCode::NotRunning, format!("{} is not running in its prefix {}", install_dir.join(exe).display(), prefix_id.path.display())).because("no_runner_context"));
    }
    let first = &found[0];
    for c in &found[1..] {
        if c.runner != first.runner {
            return Err(DebugError::new(
                DebugCode::AmbiguousRunner,
                format!("processes of the game in {} run under two Wine builds ({} for PID {}, {} for PID {}); stop the extra one", prefix_id.path.display(), first.runner.display(), first.pid, c.runner.display(), c.pid),
            )
            .because("runner"));
        }
        if c.env != first.env {
            let differs: Vec<&String> = first.env.keys().chain(c.env.keys()).filter(|k| first.env.get(*k) != c.env.get(*k)).collect();
            return Err(DebugError::new(
                DebugCode::AmbiguousRunner,
                format!("processes of the game in {} run with different Wine environments ({differs:?} differ between PIDs {} and {})", prefix_id.path.display(), first.pid, c.pid),
            )
            .because("environment"));
        }
    }
    if unresolved > 0 {
        return Err(DebugError::new(
            DebugCode::AmbiguousRunner,
            format!("{unresolved} process(es) of the game in {} have a runner that cannot be read: not every context agrees", prefix_id.path.display()),
        )
        .because("runner_unreadable"));
    }
    let runner_sha256 = super::resolver::runner_sha256(&first.runner)?;
    Ok(Runner { runner: first.runner.clone(), runner_sha256, env: first.env.clone(), prefix: prefix_id, contexts: found })
}

#[cfg(test)]
pub(crate) mod fixture {
    //! A fake /proc tree and fake Wine/Proton builds.
    use std::path::{Path, PathBuf};

    pub struct Tree {
        pub root: PathBuf,
    }

    impl Tree {
        pub fn new(tag: &str) -> Tree {
            let root = std::env::temp_dir().join(format!("kiln-proton-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&root);
            std::fs::create_dir_all(root.join("proc/sys/kernel/random")).unwrap();
            std::fs::write(root.join("proc/sys/kernel/random/boot_id"), "boot-1\n").unwrap();
            Tree { root }
        }
        pub fn proc(&self) -> PathBuf {
            self.root.join("proc")
        }
        /// A runner build: `<name>/files/bin/wine` and its preloader. Returns the preloader.
        pub fn runner(&self, name: &str, content: &[u8]) -> PathBuf {
            let b = self.root.join(name).join("files");
            std::fs::create_dir_all(b.join("bin")).unwrap();
            std::fs::create_dir_all(b.join("lib/wine/x86_64-unix")).unwrap();
            let wine = b.join("bin/wine");
            std::fs::write(&wine, content).unwrap();
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                std::fs::set_permissions(&wine, std::fs::Permissions::from_mode(0o755)).unwrap();
            }
            let pre = b.join("lib/wine/x86_64-unix/wine64-preloader");
            std::fs::write(&pre, b"").unwrap();
            pre
        }
        /// An install folder with its exe, and a prefix with Z: -> /.
        pub fn install(&self, name: &str) -> (PathBuf, PathBuf) {
            let dir = self.root.join(name).join("Enshrouded");
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(dir.join("enshrouded.exe"), b"MZ").unwrap();
            let pfx = self.root.join(name).join("compatdata/1203620/pfx");
            std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
            #[cfg(unix)]
            std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
            (dir, pfx)
        }
        /// A process: comm, argv[0], environment, stat (with `start` ticks), exe and cwd links.
        pub fn process(&self, pid: u32, comm: &str, argv0: &str, env: &[(&str, &str)], start: u64, exe: &Path, cwd: &Path) {
            let d = self.proc().join(pid.to_string());
            std::fs::create_dir_all(&d).unwrap();
            std::fs::write(d.join("comm"), format!("{comm}\n")).unwrap();
            std::fs::write(d.join("cmdline"), format!("{argv0}\0")).unwrap();
            let block: Vec<u8> = env.iter().flat_map(|(k, v)| format!("{k}={v}\0").into_bytes()).collect();
            std::fs::write(d.join("environ"), block).unwrap();
            std::fs::write(d.join("stat"), format!("{pid} ({comm}) S 1 {pid} {pid} 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 {start} 0 0")).unwrap();
            #[cfg(unix)]
            {
                std::os::unix::fs::symlink(exe, d.join("exe")).unwrap();
                std::os::unix::fs::symlink(cwd, d.join("cwd")).unwrap();
            }
        }
    }

    impl Drop for Tree {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    pub fn z(p: &Path) -> String {
        crate::winpath::unix_to_z(p).unwrap()
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::fixture::*;
    use super::*;

    fn game_env(pfx: &Path, extra: &[(&'static str, &'static str)]) -> Vec<(String, String)> {
        let mut v = vec![
            ("WINEPREFIX".to_string(), pfx.display().to_string()),
            ("STEAM_COMPAT_DATA_PATH".to_string(), pfx.parent().unwrap().display().to_string()),
            ("LD_PRELOAD".to_string(), "/steam/ubuntu12_64/gameoverlayrenderer.so".to_string()),
            ("WINEDLLOVERRIDES".to_string(), "dinput8=n,b".to_string()),
        ];
        v.extend(extra.iter().map(|(k, x)| (k.to_string(), x.to_string())));
        v
    }

    fn put(t: &Tree, pid: u32, comm: &str, dir: &Path, env: &[(String, String)], start: u64, pre: &Path) {
        let env: Vec<(&str, &str)> = env.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        t.process(pid, comm, &z(&dir.join("enshrouded.exe")), &env, start, pre, dir);
    }

    #[test]
    fn the_one_game_in_the_prefix_gives_the_runner_and_never_ld_preload() {
        let t = Tree::new("one");
        let pre = t.runner("proton-10", b"wine 10");
        let (dir, pfx) = t.install("lib");
        put(&t, 4242, "enshrouded.exe", &dir, &game_env(&pfx, &[]), 5000, &pre);
        // not the game: wineserver and a launcher in the same prefix; the game of another prefix
        let env = game_env(&pfx, &[]);
        let env: Vec<(&str, &str)> = env.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        t.process(4200, "wineserver", "/proton/files/bin/wineserver", &env, 4000, &pre, &dir);
        t.process(4210, "explorer.exe", r"C:\windows\system32\explorer.exe", &env, 4100, &pre, &pfx);
        let (dir2, pfx2) = t.install("other");
        put(&t, 4300, "enshrouded.exe", &dir2, &game_env(&pfx2, &[]), 6000, &pre);
        let r = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap();
        assert_eq!(r.contexts.len(), 1);
        assert_eq!(r.contexts[0].pid, 4242);
        assert_eq!(r.contexts[0].birth, RunnerContext { linux_boot_id: "boot-1".into(), pid: 4242, start_ticks: 5000 });
        assert_eq!(r.runner, t.root.join("proton-10/files/bin/wine").canonicalize().unwrap());
        assert_eq!(r.runner_sha256, crate::hash::sha256_hex(b"wine 10"));
        assert!(!r.env.contains_key("LD_PRELOAD"));
        assert_eq!(r.env.get("WINEDLLOVERRIDES").map(String::as_str), Some("dinput8=n,b"));
        assert_eq!(r.prefix.path, pfx.canonicalize().unwrap());
        // the other prefix's game is the other install's, never this one's
        let r2 = find_runner(&t.proc(), &dir2, "enshrouded.exe", &pfx2).unwrap();
        assert_eq!((r2.contexts.len(), r2.contexts[0].pid), (1, 4300));
        assert_ne!(r.prefix, r2.prefix);
    }

    #[test]
    fn two_game_images_in_one_prefix_must_agree() {
        let t = Tree::new("two");
        let pre = t.runner("proton-10", b"wine 10");
        let (dir, pfx) = t.install("lib");
        put(&t, 100, "enshrouded.exe", &dir, &game_env(&pfx, &[]), 10, &pre);
        // the same runner and environment; only LD_PRELOAD (not taken) differs
        let mut env2 = game_env(&pfx, &[]);
        env2[2].1 = "/something/else.so".into();
        put(&t, 200, "enshrouded.exe", &dir, &env2, 20, &pre);
        let r = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap();
        assert_eq!(r.contexts.iter().map(|c| c.pid).collect::<Vec<_>>(), vec![100, 200]);
        // another runner build for one of them
        let other = t.runner("proton-9", b"wine 9");
        std::fs::remove_dir_all(t.proc().join("200")).unwrap();
        put(&t, 200, "enshrouded.exe", &dir, &game_env(&pfx, &[]), 20, &other);
        let e = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::AmbiguousRunner, Some("runner")));
        // the same runner, another allowlisted environment
        std::fs::remove_dir_all(t.proc().join("200")).unwrap();
        put(&t, 200, "enshrouded.exe", &dir, &game_env(&pfx, &[("WINEFSYNC", "1")]), 20, &pre);
        let e = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::AmbiguousRunner, Some("environment")));
        assert!(e.message.contains("WINEFSYNC"), "{}", e.message);
    }

    #[test]
    fn a_name_with_spaces_and_parentheses_does_not_shift_stat() {
        let t = Tree::new("comm");
        let pre = t.runner("proton-10", b"wine 10");
        let (dir, pfx) = t.install("lib");
        let env = game_env(&pfx, &[]);
        let env: Vec<(&str, &str)> = env.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        // comm "enshrouded.exe" in comm, but the stat line carries a hostile name
        t.process(7, "enshrouded.exe", &z(&dir.join("enshrouded.exe")), &env, 0, &pre, &dir);
        std::fs::write(t.proc().join("7/stat"), "7 (a) b) (c S) S 1 7 7 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 31337 0 0").unwrap();
        let r = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap();
        assert_eq!(r.contexts[0].birth.start_ticks, 31337);
    }

    #[test]
    fn nothing_running_or_nothing_reachable() {
        let t = Tree::new("none");
        let (dir, pfx) = t.install("lib");
        let e = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap_err();
        assert_eq!(e.code, DebugCode::NotRunning);
        // the game runs, but its exe link points into a container path that does not exist here
        put(&t, 9, "enshrouded.exe", &dir, &game_env(&pfx, &[]), 1, Path::new("/run/pressure-vessel/x/wine64-preloader"));
        let e = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::UnsupportedIdentity, Some("runner_unreachable")));
        // one reachable, one not: not every context agrees
        let pre = t.runner("proton-10", b"wine 10");
        put(&t, 10, "enshrouded.exe", &dir, &game_env(&pfx, &[]), 2, &pre);
        let e = find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::AmbiguousRunner, Some("runner_unreadable")));
        // the prefix named only through STEAM_COMPAT_DATA_PATH counts too
        std::fs::remove_dir_all(t.proc().join("9")).unwrap();
        std::fs::remove_dir_all(t.proc().join("10")).unwrap();
        let only_compat = vec![("STEAM_COMPAT_DATA_PATH".to_string(), pfx.parent().unwrap().display().to_string())];
        put(&t, 11, "enshrouded.exe", &dir, &only_compat, 3, &pre);
        assert_eq!(find_runner(&t.proc(), &dir, "enshrouded.exe", &pfx).unwrap().contexts[0].pid, 11);
    }
}
