// SPDX-License-Identifier: GPL-3.0-or-later
// test infrastructure (the test lock/spawn gate).
//! The test lock/spawn gate in this crate's unit tests: mod-loader-lua's `test_gate`, reached through the dev-only
//! `test-gate` feature of this crate's [dev-dependencies]. This test binary creates processes (runner and plugin tests
//! run it again as a child, the records test runs a copy of it and `cp`) while other test threads take and release
//! file locks: the install lock (`runner::lock_game`), the portrait cache lock (`portrait_export::cache_lock`) and
//! ShroudForge's write lock (mod-loader-lua's `sf_lock`). On Unix a child shares every open file description of its
//! creator until its exec, flock locks included, so a child created while a lock is held keeps it after its holder
//! released it, and the holder's next take fails ("operation would block"). Every lock here is taken under a
//! `test_gate::lock_hold` and every process is created by `test_gate::spawn` (or `output`/`status`), which waits
//! until no thread holds one.
use camino::Utf8PathBuf;
use mod_loader_lua::test_gate;

/// A child the gate creates on another thread. Between its creation and its exec it tells this process it exists,
/// then waits 500 ms, as a child of a loaded machine can; it runs no test.
#[cfg(unix)]
struct SlowChild {
    spawner: std::thread::JoinHandle<std::process::ExitStatus>,
    watcher: std::thread::JoinHandle<()>,
    created: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

#[cfg(unix)]
impl SlowChild {
    fn start() -> Self {
        use std::{
            io::{Read, Write},
            os::unix::process::CommandExt,
            process::{Command, Stdio},
            sync::{Arc, atomic::{AtomicBool, Ordering}},
            time::Duration,
        };
        let (mut reader, writer) = std::io::pipe().unwrap();
        let created = Arc::new(AtomicBool::new(false));
        let watcher = {
            let created = created.clone();
            std::thread::spawn(move || {
                let mut byte = [0];
                if reader.read(&mut byte).unwrap_or(0) == 1 {
                    created.store(true, Ordering::SeqCst);
                }
            })
        };
        let spawner = std::thread::spawn(move || {
            let mut command = Command::new(std::env::current_exe().unwrap());
            // No test has this name: the child exits at once once it has exec'd.
            command
                .args(["--exact", "gate_tests::no_test_has_this_name"])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            // SAFETY: between its creation and its exec the child only calls write(2) and nanosleep(2), which are
            // async-signal-safe.
            unsafe {
                command.pre_exec(move || {
                    let _ = (&writer).write(b"c");
                    std::thread::sleep(Duration::from_millis(500));
                    Ok(())
                });
            }
            test_gate::spawn(&mut command).unwrap().wait().unwrap()
        });
        Self { spawner, watcher, created }
    }

    /// This child's spawn found the lock holders and waits for them: no process is created while a lock is held. Fails
    /// when the child was created instead (it would share the held lock's description).
    fn blocked(&self, what: &str) {
        use std::{sync::atomic::Ordering, time::{Duration, Instant}};
        let started = Instant::now();
        let spawner = self.spawner.thread().id();
        while !self.created.load(Ordering::SeqCst) && !test_gate::blocked(spawner) {
            assert!(started.elapsed() < Duration::from_secs(120), "the spawn never began ({what})");
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(!self.created.load(Ordering::SeqCst), "a process was created while the {what} was held");
    }

    fn finish(self) {
        assert!(self.spawner.join().unwrap().success());
        self.watcher.join().unwrap();
    }
}

fn scratch(tag: &str) -> Utf8PathBuf {
    let dir = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap().join(format!("kiln-eml-gate-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// The install lock run() keeps for the process's life: while it is kept no process is created, and once released (as
/// a test does before it removes its game folder) it is free to take again while another thread creates a process. The
/// second round takes it again under an outer hold, as run() takes it under its own: that take does not wait for a
/// spawn, so no spawn may happen while the outer hold lasts either. Without the gate's hold in `lock_game` the child
/// is created while the lock is held (and keeps it, so the second take fails "would block").
#[cfg(unix)]
#[test]
fn a_child_process_never_keeps_a_released_install_lock() {
    let me = std::thread::current().id();
    for outer in [false, true] {
        let game = scratch(&format!("install-{outer}"));
        let _release = crate::runner::GameLockRelease(game.clone());
        let outer_hold = outer.then(test_gate::lock_hold);
        crate::runner::lock_game(&game).unwrap();
        // The kept install lock's hold counts for this thread (beside the outer one).
        assert_eq!(test_gate::holds(me), 1 + usize::from(outer), "the kept install lock's hold (outer={outer})");
        let child = SlowChild::start();
        child.blocked(&format!("install lock (outer={outer})"));
        crate::runner::release_game_lock(&game);
        assert_eq!(test_gate::holds(me), usize::from(outer), "the released install lock's hold (outer={outer})");
        let again = crate::runner::lock_game(&game);
        crate::runner::release_game_lock(&game);
        assert!(again.is_ok(), "a child process keeps the released install lock (outer={outer}): {:?}", again.err());
        // The spawn waits for this hold: release it before waiting for the child.
        drop(outer_hold);
        child.finish();
        let _ = std::fs::remove_dir_all(game);
    }
}

/// The install lock a thread keeps (GAME_LOCKS) keeps its hold for that thread (`KeptHold`), also once the thread has
/// ended, until it is released, from whatever thread: then exactly that hold goes (no lost or double release). A test
/// that fails while it keeps the lock releases it as it unwinds (`runner::GameLockRelease`). No process is created
/// here, so this runs on every platform.
#[test]
fn a_kept_install_lock_holds_for_its_taker_until_it_is_released() {
    let me = std::thread::current().id();
    let game = scratch("kept");
    let _release = crate::runner::GameLockRelease(game.clone());
    let taker = {
        let game = game.clone();
        std::thread::spawn(move || {
            crate::runner::lock_game(&game).unwrap();
            std::thread::current().id()
        })
    }
    .join()
    .unwrap();
    assert_eq!((test_gate::holds(taker), test_gate::holds(me)), (1, 0), "the kept lock holds for the thread that took it");
    crate::runner::release_game_lock(&game);
    assert_eq!((test_gate::holds(taker), test_gate::holds(me)), (0, 0), "released from another thread");
    // A test thread that fails while it keeps the lock (no panic message: the failure is the point).
    let failed = {
        let game = game.clone();
        std::thread::spawn(move || -> () {
            let _release = crate::runner::GameLockRelease(game.clone());
            crate::runner::lock_game(&game).unwrap();
            assert_eq!(test_gate::holds(std::thread::current().id()), 1);
            std::panic::resume_unwind(Box::new("the test failed"));
        })
    };
    let failed_thread = failed.thread().id();
    assert!(failed.join().is_err());
    assert_eq!(test_gate::holds(failed_thread), 0, "a failed test's install lock is released as it unwinds");
    assert!(crate::runner::lock_game(&game).is_ok(), "and free");
    crate::runner::release_game_lock(&game);
    let _ = std::fs::remove_dir_all(game);
}

/// The same for the portrait cache lock (`portrait_export::cache_lock`): no process is created while one generator
/// holds it, and once released it is free while a process is created.
#[cfg(unix)]
#[test]
fn a_child_process_never_keeps_a_released_portrait_cache_lock() {
    for outer in [false, true] {
        let dir = scratch(&format!("portraits-{outer}"));
        let cache = dir.join("cache");
        let outer_hold = outer.then(test_gate::lock_hold);
        let first = crate::portrait_export::cache_lock(cache.as_std_path()).unwrap();
        let child = SlowChild::start();
        child.blocked(&format!("portrait cache lock (outer={outer})"));
        drop(first);
        let again = crate::portrait_export::cache_lock(cache.as_std_path());
        assert!(again.is_ok(), "a child process keeps the released portrait cache lock (outer={outer}): {:?}", again.err());
        drop(again);
        drop(outer_hold);
        child.finish();
        let _ = std::fs::remove_dir_all(dir);
    }
}

/// Every file lock of this crate is taken under a `test_gate::lock_hold` taken earlier in the same function and
/// every process it creates goes through `test_gate::spawn` (mod-loader-lua's own test checks the EML crates the same
/// way). A new `Mutex` here goes into MUTEXES (by its receiver). And mod-loader-lua's holds around its own locks
/// (`sf_lock`, the install guard probe) are built for this binary too: under `cfg(any(test, feature = "test-gate"))`,
/// never `cfg(test)` alone, which would leave them out of every crate but mod-loader-lua. Source text is not on the
/// Windows runners.
#[cfg(not(windows))]
#[test]
fn every_file_lock_and_process_of_kiln_eml_goes_through_the_gate() {
    use std::path::Path;
    // (path under gpl/kiln-eml/, receiver): mutexes, not files.
    const MUTEXES: &[(&str, &str)] = &[
        ("kiln-eml/src/progress.rs", "self.inner"),
        ("kiln-eml/src/progress.rs", "self.window"),
        ("kiln-eml/src/progress.rs", "CALLS"),
        // the startup pass's state (one per process), and its tests' turn on the run lock
        ("kiln-eml/src/progress.rs", "PASS"),
        ("kiln-eml/src/progress.rs", "RUN_LOCK"),
        ("kiln-eml/src/archive_service/view.rs", "STATE"),
        ("kiln-eml/src/archive_service/view.rs", "self.replies"),
        ("kiln-eml/src/archive_service/errors.rs", "STORE"),
        ("kiln-eml/src/archive_service/attributes.rs", "self.catalog"),
        ("kiln-eml/src/archive_values/arena.rs", "TABLE"),
        ("kiln-eml/src/archive_values/arena.rs", "self.queue"),
        ("kiln-eml/src/runner.rs", "GAME_LOCKS"),
        ("kiln-eml/src/runner.rs", "RUN_LOCK"),
        ("kiln-eml/src/plugin.rs", "EML_LOG"),
        ("kiln-eml/src/plugin.rs", "STATE_JSON"),
        ("kiln-eml/src/plugin.rs", "ADDED"),
        ("kiln-eml/src/plugin_records_tests.rs", "RECORD"),
        ("kiln-eml/src/plugin_records_tests.rs", "PROFILE"),
        ("kiln-eml/src/plugin_records_tests.rs", "LOGGED"),
        ("kiln-eml/src/plugin_records_tests.rs", "GAME"),
        ("kiln-eml/src/kiln_eml_inspect.rs", "std::io::stdout()"),
        ("kiln-eml/src/startup_seam.rs", "STATE"),
    ];
    let here = Path::new(env!("CARGO_MANIFEST_DIR"));
    let root = here.parent().unwrap();
    let mut files = Vec::new();
    test_gate::sources(&here.join("src"), &mut files);
    assert!(files.iter().any(|f| f.ends_with("src/runner.rs")), "the scan reaches this crate's sources");
    let mut bypass = test_gate::bypasses(root, &files, MUTEXES);
    let mut lua = Vec::new();
    test_gate::sources(&root.join("eml/mod-loader-lua/src"), &mut lua);
    assert!(lua.iter().any(|f| f.ends_with("src/ownership.rs")), "the scan reaches mod-loader-lua's sources");
    let gate = format!("{}::", "test_gate");
    for file in &lua {
        let text = std::fs::read_to_string(file).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        let name = file.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
        for (i, pair) in lines.windows(2).enumerate() {
            if pair[0].trim() == "#[cfg(test)]" && pair[1].contains(gate.as_str()) {
                bypass.push(format!("{name}:{}: a test-gate hold built for mod-loader-lua's own tests only", i + 2));
            }
        }
    }
    assert!(bypass.is_empty(), "outside the test gate (mod-loader-lua's test_gate.rs):\n{}", bypass.join("\n"));
}
