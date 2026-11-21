// SPDX-License-Identifier: GPL-3.0-or-later
// test infrastructure (the test lock/spawn gate).
//! kiln-eml: the test lock/spawn gate. A process is created only while no thread of the test process holds a file lock
//! (or a descriptor a child must not share), so no child keeps a lock its creator's thread released.
//!
//! Built only for mod-loader-lua's own unit tests (`cfg(test)`) and under the dev-only `test-gate` feature, which only
//! kiln-eml's `[dev-dependencies]` turn on, so kiln-eml's unit tests get the same gate around this crate's locks and
//! their own. No normal build has it: the shipped kiln-eml.dll has no gate, hold or check.
//!
//! A lock taken with `File::try_lock` (flock on Unix, as ShroudForge's own fs2 lock) belongs to the open file
//! description, and a new process starts with a copy of every descriptor its creator has open: it shares those
//! descriptions until its exec closes the copies (every descriptor of the test process is close-on-exec). A child created
//! while another test's thread holds such a lock therefore keeps that lock after the thread released it, and the
//! thread's next `try_lock` of the same file fails ("ShroudForge state is in use", "operation would block"). So every
//! lock the code of a gated test binary takes is taken under a `LockHold`, and `spawn` creates a process only while no
//! thread holds one. A lock taken after the process exists is on a description the child never had. `Command::spawn`
//! returns only once the child has exec'd (or failed to): posix_spawn suspends the caller until then, and std's fork path
//! waits on its close-on-exec error pipe. `bypasses` is the source check both crates' tests run: it names a lock or a
//! process creation that does not go through this gate.
//!
//! The binaries it covers: mod-loader-lua's unit tests and kiln-eml's library unit tests (`cargo test -p kiln-eml
//! --lib`), whose code the two source checks scan (`eml/*/src`, `kiln-eml/src`). Not covered: kiln-eml's integration
//! test binaries (`kiln-eml/tests/*.rs`) and the programs cargo builds for them (`kiln-eml-run`, `kiln-eml-inspect`).
//! A `cargo test` build turns the feature on for those too, so mod-loader-lua's holds are compiled in there, but they
//! never wait: nothing there creates a process through `spawn`, and kiln-eml's own holds are `cfg(test)`, which only
//! its library's unit tests have. Those binaries' locks and processes are their own (`m6_sf_retry` keeps its own fix).
use std::{
    cell::RefCell,
    io,
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Output, Stdio},
    sync::{Condvar, Mutex, MutexGuard, PoisonError},
    thread::{self, ThreadId},
    time::{Duration, Instant},
};

struct Gate {
    /// The threads that hold a lock, with how many each and the thread's name (the test's) for the timeout's message.
    holders: Vec<(ThreadId, usize, Option<String>)>,
    /// Spawns waiting for the holders: meanwhile a thread that holds no lock yet waits for them.
    waiting: usize,
    /// A process is being created.
    spawning: bool,
    /// The threads whose spawn found holders and waits for them (for the gate's own tests).
    blocked: Vec<ThreadId>,
}

static GATE: Mutex<Gate> = Mutex::new(Gate {
    holders: Vec::new(),
    waiting: 0,
    spawning: false,
    blocked: Vec::new(),
});
static GATE_CHANGED: Condvar = Condvar::new();

thread_local! {
    /// For the gate's own test: runs once, after this thread's last hold is released.
    static ON_LAST_RELEASE: RefCell<Option<Box<dyn FnOnce()>>> = const { RefCell::new(None) };
}

fn gate() -> MutexGuard<'static, Gate> {
    GATE.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Held for as long as a descriptor of this process may hold a file lock: taken before the file is opened, dropped
/// after it is closed. A thread that already holds one never waits: its release is what a waiting spawn needs. Not
/// `Send`: a hold counts for the thread that took it, so it is released on that thread (a hold moved to another thread
/// would let that thread's first hold wait for a spawn that waits for the moved one). A lock kept past its function in
/// shared state keeps its hold as a `KeptHold` (`keep`).
#[derive(Debug)]
pub struct LockHold(ThreadId, std::marker::PhantomData<*const ()>);

/// A `LockHold` for a lock kept in shared state past the function that took it (kiln-eml's install lock, kept for the
/// process's life in `GAME_LOCKS`). It still counts for the thread that took it, which therefore may not create a
/// process while it lasts, and it is released wherever it is dropped (it never makes its owner a holder).
#[derive(Debug)]
pub struct KeptHold(ThreadId);

/// Takes a hold for the current thread (waits only for this thread's first, while a spawn waits or runs).
pub fn lock_hold() -> LockHold {
    let me = thread::current().id();
    let mut gate = gate();
    loop {
        if let Some((_, held, _)) = gate.holders.iter_mut().find(|(t, ..)| *t == me) {
            *held += 1;
            break;
        }
        if !gate.spawning && gate.waiting == 0 {
            gate.holders.push((me, 1, thread::current().name().map(str::to_owned)));
            break;
        }
        gate = GATE_CHANGED.wait(gate).unwrap_or_else(PoisonError::into_inner);
    }
    LockHold(me, std::marker::PhantomData)
}

/// Releases one hold of `thread`; true when that was its last.
fn release(thread: ThreadId) -> bool {
    let mut gate = gate();
    if let Some(i) = gate.holders.iter().position(|(t, ..)| *t == thread) {
        gate.holders[i].1 -= 1;
        if gate.holders[i].1 == 0 {
            gate.holders.swap_remove(i);
            GATE_CHANGED.notify_all();
            return true;
        }
    }
    false
}

impl LockHold {
    /// The same hold, for a lock kept in shared state (see `KeptHold`).
    pub fn keep(self) -> KeptHold {
        let thread = self.0;
        std::mem::forget(self);
        KeptHold(thread)
    }
}

impl Drop for LockHold {
    fn drop(&mut self) {
        if release(self.0) {
            if let Some(hook) = ON_LAST_RELEASE.try_with(|h| h.borrow_mut().take()).ok().flatten() {
                hook();
            }
        }
    }
}

impl Drop for KeptHold {
    fn drop(&mut self) {
        release(self.0);
    }
}

/// Runs `hook` once, right after this thread's last `LockHold` is released (for the gate's own test).
pub fn on_last_release(hook: impl FnOnce() + 'static) {
    ON_LAST_RELEASE.with(|h| *h.borrow_mut() = Some(Box::new(hook)));
}

/// Creates the process while no thread of this process holds a file lock, so the child shares none.
pub fn spawn(command: &mut Command) -> io::Result<Child> {
    struct Spawning;
    impl Drop for Spawning {
        fn drop(&mut self) {
            let mut gate = gate();
            gate.waiting -= 1;
            gate.spawning = false;
            GATE_CHANGED.notify_all();
        }
    }
    let me = thread::current().id();
    let mut gate = gate();
    assert!(
        !gate.holders.iter().any(|(t, ..)| *t == me),
        "a process created while its creator holds a file lock would keep that lock"
    );
    gate.waiting += 1;
    let started = Instant::now();
    while gate.spawning || !gate.holders.is_empty() {
        if !gate.blocked.contains(&me) {
            gate.blocked.push(me);
        }
        // A lock is held for one step of a run (or until its test releases it); one held this long was never released.
        if started.elapsed() > Duration::from_secs(300) {
            gate.waiting -= 1;
            gate.blocked.retain(|t| *t != me);
            GATE_CHANGED.notify_all();
            let holders: Vec<_> = gate.holders.iter().map(|(_, n, name)| (name.clone(), *n)).collect();
            panic!("a file lock of this process was never released (holding threads {holders:?})");
        }
        gate = GATE_CHANGED
            .wait_timeout(gate, Duration::from_secs(1))
            .unwrap_or_else(PoisonError::into_inner)
            .0;
    }
    gate.blocked.retain(|t| *t != me);
    gate.spawning = true;
    drop(gate);
    let _spawning = Spawning;
    command.spawn()
}

/// `Command::output` through the gate: stdin closed, stdout and stderr captured (`output`'s defaults).
pub fn output(command: &mut Command) -> io::Result<Output> {
    command.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    spawn(command)?.wait_with_output()
}

/// `Command::status` through the gate.
pub fn status(command: &mut Command) -> io::Result<ExitStatus> {
    spawn(command)?.wait()
}

/// The spawn on `thread` waits for the lock holders (or for another spawn to finish): it has not created its process
/// (for the gate's own tests; another test's spawn, or one about to create its process, does not count).
pub fn blocked(thread: ThreadId) -> bool {
    gate().blocked.contains(&thread)
}

/// How many holds `thread` has, kept ones included, whether or not it still runs (for the gate's own tests).
pub fn holds(thread: ThreadId) -> usize {
    gate().holders.iter().find(|(t, ..)| *t == thread).map_or(0, |(_, n, _)| *n)
}

/// The source check: every file lock in `files` is taken under a `LockHold` taken earlier in the same function, and
/// every process they create goes through `spawn` (or `output`/`status`) above. `mutexes` are the `.lock()` calls
/// that lock a mutex, not a file: (path under `root`, receiver). Returns one `path:line: why` per site that bypasses
/// the gate, and one per `mutexes` entry that locks nothing (a stale entry would let a file lock of that name through).
/// Line-based: a comment line is skipped (a hold named in a comment is no hold), and a function starts at its `fn `
/// line.
pub fn bypasses(root: &Path, files: &[PathBuf], mutexes: &[(&str, &str)]) -> Vec<String> {
    // Built from pieces, so this function's own lines do not match.
    let locks = ["try_lock", "lock", "try_lock_shared", "lock_shared"].map(|m| format!(".{m}()"));
    let spawns = ["spawn", "status", "output"].map(|m| format!(".{m}()"));
    let hold = format!("test_gate::{}()", "lock_hold");
    let comment = |line: &&str| line.trim_start().starts_with("//");
    let mut bypass = Vec::new();
    let mut used = vec![false; mutexes.len()];
    for file in files {
        let text = std::fs::read_to_string(file).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        let name = file.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
        for (i, line) in lines.iter().enumerate() {
            let code = line.trim_start();
            if comment(line) {
                continue;
            }
            if spawns.iter().any(|s| code.contains(s.as_str())) && !name.ends_with("mod-loader-lua/src/test_gate.rs") {
                bypass.push(format!("{name}:{}: a process created outside test_gate::spawn", i + 1));
            }
            if !locks.iter().any(|l| code.contains(l.as_str())) {
                continue;
            }
            if let Some(k) = mutexes.iter().position(|(f, m)| name == *f && code.contains(&format!("{m}.{}()", "lock"))) {
                used[k] = true;
                continue;
            }
            let start = (0..i)
                .rev()
                .find(|&j| {
                    let t = lines[j].trim_start();
                    t.starts_with("fn ") || t.contains(" fn ")
                })
                .unwrap_or(0);
            if !lines[start..i].iter().any(|l| !comment(l) && l.contains(hold.as_str())) {
                bypass.push(format!("{name}:{}: a file lock with no earlier test_gate::lock_hold in its function", i + 1));
            }
        }
    }
    for ((f, m), used) in mutexes.iter().zip(used) {
        if !used {
            bypass.push(format!("{f}: MUTEXES lists `{m}`, which locks nothing there"));
        }
    }
    bypass
}

/// Every `.rs` file under `dir`, recursively.
pub fn sources(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in std::fs::read_dir(dir).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            sources(&path, out);
        } else if path.extension().is_some_and(|e| e == "rs") {
            out.push(path);
        }
    }
}

#[cfg(test)]
mod tests {
    /// The source check itself: it names a file lock with no hold before it in its function (none at all, one only in a
    /// comment, one after the lock), a process created outside the gate and a MUTEXES entry that locks nothing; a lock
    /// under an earlier hold and a listed mutex pass. The sample's lines are built from pieces, so this file's own lines
    /// do not match the check this crate's tests run over it.
    #[test]
    fn the_source_check_names_each_bypass() {
        let hold = format!("let _hold = crate::test_gate::{}();", "lock_hold");
        let lock = |m: &str| format!("f.{m}().unwrap();");
        let text = [
            "fn guarded(f: &File) {".to_owned(),
            hold.clone(),
            lock("try_lock"),
            "}".into(),
            "fn unguarded(f: &File) {".into(),
            format!("// {hold}"),
            lock("lock_shared"),
            "}".into(),
            "fn late(f: &File) {".into(),
            lock("lock"),
            hold.clone(),
            "}".into(),
            format!("fn mutex() {{ M.{}().unwrap(); }}", "lock"),
            format!("fn child() {{ Command::new(\"cp\").{}().unwrap(); }}", "status"),
        ]
        .join("\n");
        let root = std::env::temp_dir().join(format!("kiln-test-gate-scan-{}", std::process::id()));
        std::fs::create_dir_all(root.join("src")).unwrap();
        let sample = root.join("src/sample.rs");
        std::fs::write(&sample, text).unwrap();
        let found = super::bypasses(&root, &[sample], &[("src/sample.rs", "M"), ("src/sample.rs", "N")]);
        let _ = std::fs::remove_dir_all(&root);
        assert_eq!(
            found,
            [
                "src/sample.rs:7: a file lock with no earlier test_gate::lock_hold in its function",
                "src/sample.rs:10: a file lock with no earlier test_gate::lock_hold in its function",
                "src/sample.rs:14: a process created outside test_gate::spawn",
                "src/sample.rs: MUTEXES lists `N`, which locks nothing there",
            ]
        );
    }
}
