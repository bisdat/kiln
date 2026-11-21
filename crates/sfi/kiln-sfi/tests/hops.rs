// SPDX-License-Identifier: MIT
//! Hops through the real worker and lifecycle host, with the
//! fake host's `post_world` queue standing in for the world thread: this test's
//! thread runs the posted trampolines, as the server tick does before its Pre.
//! `KilnApi.session` is global in the fake, so every worker test holds
//! `session_lock()`. The CAS race test drives `HopCell` alone (and runs under TSan);
//! the late-trampoline test runs under ASan.
mod support;

use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use kiln_sfi::hop::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::worker::*;
use serde_json::json;
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::time::{Duration, Instant};
use support::*;

const SERVER: i32 = KILN_WORLD_SERVER;
/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
static FLAGS: LazyLock<Mutex<HashSet<(KilnOwner, String)>>> = LazyLock::new(Default::default);
static EFFECTS: LazyLock<Mutex<HashMap<KilnOwner, u32>>> = LazyLock::new(Default::default);
/// When each hop call returned to the worker (wake latency), and how. Noted in Rust
/// right after the call, so a count-hook check before the next Lua `record` cannot
/// hide it.
type Returns = HashMap<KilnOwner, Vec<(Instant, String)>>;
static RETURNED: LazyLock<Mutex<Returns>> = LazyLock::new(Default::default);

fn note(owner: KilnOwner, text: String) {
    RECORDS.lock().unwrap().entry(owner).or_default().push(text);
}
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
fn set_flag(owner: KilnOwner, name: &str) {
    FLAGS.lock().unwrap().insert((owner, name.to_owned()));
}
fn effects(owner: KilnOwner) -> u32 {
    EFFECTS.lock().unwrap().get(&owner).copied().unwrap_or(0)
}

const PACKAGE: &str = "local n = 0 \
    return { update_interval_ms = 8, on_update = function() \
      if flag('one') and n == 0 then n = 1 record(hop_value(21)) end \
      if flag('block') and n == 0 then n = 1 record(hop_block()) end \
      if flag('many') and n < 20 then n = n + 1 record(hop_value(n)) end \
      if flag('three') and n < 3 then n = n + 1 record(hop_value(n)) end \
    end, on_unload = function() record('unload') end }";

struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(PACKAGE.as_bytes().to_vec())
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct NoCleanup;
impl Cleanup for NoCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}

fn outcome<T: std::fmt::Display>(result: Result<T, HopError>) -> String {
    match result {
        Ok(value) => format!("done:{value}"),
        Err(error) => format!("err:{error:?}"),
    }
}

struct HopPlan;
impl PlanSource for HopPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        let mut registry = Registry::default();
        let source: Rc<dyn PackageSource> = Rc::new(Source);
        let mut parsed = package::Package::parse(
            json!({"id": "a", "name": "a", "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        registry
            .insert(
                PackageRecord::new(
                    "a".into(),
                    parsed,
                    RuntimePackage::new("a".to_owned(), source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
        let register: Register = Box::new(move |lua, env, _, ports| {
            env.set(
                "record",
                lua.create_function(move |_, text: String| {
                    note(owner, text);
                    Ok(())
                })?,
            )?;
            env.set(
                "flag",
                lua.create_function(move |_, name: String| {
                    Ok(FLAGS.lock().unwrap().contains(&(owner, name)))
                })?,
            )?;
            let hops = ports.hops.clone();
            env.set(
                "hop_value",
                lua.create_function(move |_, value: i64| {
                    let result = hops.call(SERVER as u32, move || {
                        *EFFECTS.lock().unwrap().entry(owner).or_default() += 1;
                        value * 2
                    });
                    let at = Instant::now();
                    let result = outcome(result);
                    RETURNED
                        .lock()
                        .unwrap()
                        .entry(owner)
                        .or_default()
                        .push((at, result.clone()));
                    Ok(result)
                })?,
            )?;
            let hops = ports.hops.clone();
            env.set(
                "hop_block",
                lua.create_function(move |_, ()| {
                    let result = hops.call(SERVER as u32, move || {
                        note(owner, "closure:running".into());
                        while !FLAGS.lock().unwrap().contains(&(owner, "release".into())) {
                            std::thread::sleep(Duration::from_millis(1));
                        }
                        *EFFECTS.lock().unwrap().entry(owner).or_default() += 1;
                        7
                    });
                    Ok(outcome(result))
                })?,
            )?;
            Ok(())
        });
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry,
            plan: vec!["a".into()],
            admitted: ["a".to_owned()].into(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: Some(register),
            compose: Default::default(),
        })
    }
}

struct Run {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    shared: Arc<WorkerShared>,
}
impl Run {
    fn finished(&self) -> bool {
        self.pool.task(0).wait_finished(WAIT)
    }
    fn wait_record(&self, text: &str) {
        assert!(
            eventually(WAIT, || records(self.owner).iter().any(|r| r == text)),
            "never saw {text}: {:?}",
            records(self.owner)
        );
    }
    fn wait_posted(&self, count: usize) {
        assert!(
            eventually(WAIT, || posted(self.owner, SERVER) >= count),
            "no hop posted: {:?}",
            records(self.owner)
        );
    }
    fn outstanding(&self) -> u64 {
        let (posted, returned) = self.shared.hop_counts();
        posted - returned
    }
}

fn start() -> Run {
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<HopPlan>)));
    let api = api(true);
    assert!(worker.bind(unsafe { Host::read(&*api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    Run {
        owner,
        pool,
        shared,
    }
}

// ---- the protocol through the worker ----

#[test]
fn done_path() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "one");
    run.wait_posted(1);
    let took = run_posted(run.owner, SERVER);
    run.wait_record("done:42");
    assert_eq!(effects(run.owner), 1);
    assert_eq!(run.outstanding(), 0);
    assert!(
        took[0] < Duration::from_millis(5),
        "trampoline took {:?}",
        took[0]
    );
    run.shared.request_stop();
    assert!(run.finished());
}

/// Turns the fake core's `world.lifecycle` back on when dropped (under the session lock).
struct LifecycleBack;
impl Drop for LifecycleBack {
    fn drop(&mut self) {
        set_lifecycle(true);
    }
}

#[test]
fn a_core_without_world_lifecycle_refuses_every_hop_at_its_start() {
    let _session = session_lock();
    let _back = LifecycleBack;
    set_session(SERVER, 1);
    set_lifecycle(false);
    let run = start();
    set_flag(run.owner, "three");
    assert!(
        eventually(WAIT, || records(run.owner).len() >= 3),
        "{:?}",
        records(run.owner)
    );
    // SF's unavailable shape at once, nothing posted, no wait, a status counter.
    assert_eq!(records(run.owner)[..3], ["err:Unavailable"; 3]);
    assert_eq!(posts_total(run.owner), 0, "no post_world call");
    assert_eq!(run.shared.hop_counts(), (0, 0));
    assert_eq!(run.shared.status().hops_refused_no_lifecycle, 3);
    assert_eq!(effects(run.owner), 0);
    run.shared.request_stop();
    assert!(run.finished());
}

#[test]
fn a_session_that_ends_before_the_run_cancels_and_the_late_trampoline_has_no_effect() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "one");
    run.wait_posted(1);
    // The lifecycle subscriber sees STOPPING: it flips the floor and unparks.
    fire_world(run.owner, KILN_WORLD_STOPPING, SERVER, 1);
    run.wait_record("err:SessionEnded");
    assert_eq!(run.outstanding(), 1, "the posted function has not run yet");
    run_posted(run.owner, SERVER);
    assert_eq!(effects(run.owner), 0, "no effect");
    assert_eq!(run.outstanding(), 0);
    run.shared.request_stop();
    assert!(run.finished());
}

#[test]
fn a_trampoline_that_runs_in_a_new_session_cancels_with_no_effect() {
    let _session = session_lock();
    for _ in 0..8 {
        set_session(SERVER, 1);
        let run = start();
        set_flag(run.owner, "one");
        run.wait_posted(1);
        // A new world before the posted function ran (the post queue is not
        // session-bound): it must not touch the new world.
        set_session(SERVER, 2);
        run_posted(run.owner, SERVER);
        run.wait_record("err:SessionEnded");
        assert_eq!(effects(run.owner), 0, "no effect in the new session");
        run.shared.request_stop();
        assert!(run.finished());
    }
}

#[test]
fn a_stop_while_pending_abandons_and_a_late_trampoline_drops_only_its_reference() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "one");
    run.wait_posted(1);
    run.shared.request_stop();
    run.wait_record("err:Stopping");
    assert!(
        run.finished(),
        "the worker left; done took its reference back"
    );
    assert_eq!(run.outstanding(), 1);
    // The worker, its Lua state and its cell reference are gone; the posted box
    // holds the cell's last reference and one of the shared state's.
    let before = Arc::strong_count(&run.shared);
    // The world ticks again later: the trampoline loses the CAS, drops only its own
    // box (cell and shared state stay valid for it), changes nothing.
    let took = run_posted(run.owner, SERVER);
    assert!(took[0] < Duration::from_millis(5));
    assert_eq!(effects(run.owner), 0);
    assert_eq!(run.outstanding(), 0);
    assert_eq!(
        Arc::strong_count(&run.shared),
        before - 1,
        "the box held one reference and the late trampoline dropped exactly it"
    );
}

#[test]
fn the_wait_itself_cancels_a_hop_whose_session_moved_on_without_a_stopping() {
    let _session = session_lock();
    // 2: a newer session already shows in KilnApi.session; 0: the world is gone.
    // No STOPPING reached the subscriber and the world never ran its queue, so only
    // the wait's own check of KilnApi.session can end the hop .
    for next in [2u32, 0] {
        set_session(SERVER, 1);
        let run = start();
        set_flag(run.owner, "one");
        run.wait_posted(1);
        set_session(SERVER, next);
        run.wait_record("err:SessionEnded");
        assert_eq!(
            run.shared.world_stopped(SERVER as u32),
            0,
            "no STOPPING seen"
        );
        assert_eq!(run.outstanding(), 1, "the trampoline has not run");
        run_posted(run.owner, SERVER);
        assert_eq!(effects(run.owner), 0, "the late trampoline had no effect");
        assert_eq!(run.outstanding(), 0);
        run.shared.request_stop();
        assert!(run.finished());
    }
}

#[test]
fn an_exit_ends_a_pending_hop_wait_inside_the_park() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "one");
    run.wait_posted(1);
    std::thread::sleep(Duration::from_millis(2)); // the worker parks in the wait
    // The pool's cancellation (exit here; revoke and drain signal the same way), not
    // kiln-sfi's stop: the wait abandons the hop and the body leaves fast.
    let exit = run.pool.exit(Duration::from_millis(500));
    assert!(
        exit.returned,
        "the hop wait held the body past the park: {exit:?}"
    );
    assert!(exit.waited < Duration::from_millis(250), "{exit:?}");
    let returned = RETURNED.lock().unwrap()[&run.owner].clone();
    assert_eq!(returned.len(), 1);
    assert_eq!(returned[0].1, "err:Stopping");
    assert!(
        !records(run.owner).contains(&"unload".to_owned()),
        "fast leave"
    );
    // No world ticks after an exit; if one did, its trampoline has no effect.
    assert_eq!(run.outstanding(), 1);
    run_posted(run.owner, SERVER);
    assert_eq!(effects(run.owner), 0);
    assert_eq!(run.outstanding(), 0);
    eprintln!("exit with a pending hop: body left in {:?}", exit.waited);
}

#[test]
fn a_stop_while_running_waits_for_done() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "block");
    run.wait_posted(1);
    let owner = run.owner;
    let world = std::thread::spawn(move || run_posted(owner, SERVER));
    run.wait_record("closure:running");
    run.shared.request_stop();
    std::thread::sleep(Duration::from_millis(60));
    assert!(
        !records(run.owner)
            .iter()
            .any(|r| r.starts_with("done:") || r.starts_with("err:")),
        "the worker kept waiting for the running closure: {:?}",
        records(run.owner)
    );
    set_flag(run.owner, "release");
    world.join().unwrap();
    run.wait_record("done:7");
    assert!(run.finished());
    assert_eq!(effects(run.owner), 1);
}

#[test]
fn a_refused_post_answers_unavailable_at_once() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    refuse_posts(run.owner, true);
    set_flag(run.owner, "one");
    run.wait_record("err:Unavailable");
    assert_eq!(posted(run.owner, SERVER), 0);
    assert_eq!(run.outstanding(), 0, "the refused box was taken back");
    refuse_posts(run.owner, false);
    run.shared.request_stop();
    assert!(run.finished());

    // No live session at all: unavailable too, nothing posted.
    set_session(SERVER, 0);
    let run = start();
    set_flag(run.owner, "one");
    run.wait_record("err:Unavailable");
    assert_eq!(posted(run.owner, SERVER), 0);
    run.shared.request_stop();
    assert!(run.finished());
}

#[test]
fn the_leak_is_bounded_to_one_cell_per_never_run_hop() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "three");
    for session in 1..=3u32 {
        run.wait_posted(session as usize);
        fire_world(run.owner, KILN_WORLD_STOPPING, SERVER, session);
        set_session(SERVER, session + 1);
        fire_world(run.owner, KILN_WORLD_STARTED, SERVER, session + 1);
        assert!(eventually(WAIT, || {
            records(run.owner)
                .iter()
                .filter(|r| *r == "err:SessionEnded")
                .count()
                == session as usize
        }));
    }
    run.shared.request_stop();
    assert!(run.finished());
    // The world never ticks again: three posted functions never run.
    assert_eq!(run.outstanding(), 3);
    assert_eq!(forget_posted(run.owner), 3);
    assert_eq!(effects(run.owner), 0);
}

#[test]
fn the_tick_is_never_blocked_by_a_trampoline() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "many");
    let mut took = Vec::new();
    for count in 1..=10 {
        run.wait_posted(1);
        took.extend(run_posted(run.owner, SERVER));
        run.wait_record(&format!("done:{}", count * 2));
    }
    let slowest = took.iter().max().unwrap();
    assert!(
        *slowest < Duration::from_millis(5),
        "slowest trampoline {slowest:?}"
    );
    run.shared.request_stop();
    assert!(run.finished());
    eprintln!("trampolines: slowest {slowest:?} of {}", took.len());
}

#[test]
fn a_hop_wait_wakes_on_the_trampoline_not_on_the_16_ms_poll() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let run = start();
    set_flag(run.owner, "many");
    let mut latencies = Vec::new();
    for count in 1..=20usize {
        run.wait_posted(1);
        // Let the worker settle into its park first.
        std::thread::sleep(Duration::from_millis(2));
        let ran = Instant::now();
        run_posted(run.owner, SERVER);
        assert!(eventually(WAIT, || {
            RETURNED.lock().unwrap().get(&run.owner).map_or(0, Vec::len) >= count
        }));
        let returned = RETURNED.lock().unwrap()[&run.owner][count - 1].0;
        latencies.push(returned.saturating_duration_since(ran));
    }
    latencies.sort();
    let median = latencies[latencies.len() / 2];
    assert!(
        median < Duration::from_millis(3),
        "median wake {median:?} (a lost unpark waits for the 16 ms poll): {latencies:?}"
    );
    run.shared.request_stop();
    assert!(run.finished());
    eprintln!(
        "hop wake: median {median:?}, max {:?}",
        latencies.last().unwrap()
    );
}

// ---- the cell's CAS races, alone (also run under TSan) ----

#[test]
fn a_trampoline_returns_while_the_worker_never_looks() {
    // The world thread never waits on the worker. The worker (the
    // cell's waiter: the thread that made it) is held on a channel it leaves only
    // after the world thread's run has returned, so it never looks at the cell,
    // never takes the result and never parks on it while the run goes on. A run
    // that waited for the worker in any way could not return, and the test would
    // say so at the liveness bound. No clock decides anything here: how long the
    // run takes on a loaded machine is not the property.
    let (cell_tx, cell_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel::<()>();
    let worker = std::thread::spawn(move || {
        let cell = HopCell::new(0, 1, || 7u32);
        cell_tx.send(cell.clone()).unwrap();
        release_rx.recv().unwrap();
        // Only now, after the run returned, does the worker look.
        let state = cell.state();
        (state, cell.take())
    });
    let cell = cell_rx.recv().unwrap();
    let (returned_tx, returned_rx) = std::sync::mpsc::channel();
    let world = std::thread::spawn(move || {
        returned_tx.send(cell.run(|_, _| true)).unwrap();
    });
    let state = returned_rx
        .recv_timeout(WAIT)
        .expect("the world thread waited for the worker");
    assert_eq!(state, HopState::Done);
    world.join().unwrap();
    release_tx.send(()).unwrap();
    let (seen, taken) = worker.join().unwrap();
    assert_eq!(seen, HopState::Done, "Done was left for the worker to find");
    assert_eq!(taken, Some(Ok(7)));
}

#[test]
fn the_world_side_of_a_hop_has_no_wait_in_it() {
    // The test above proves that the run never waits for the worker unboundedly. A
    // bounded wait (a park or a sleep of a few ms, a spin until the worker looks)
    // would pass it, and only a clock could see it, which a loaded machine makes
    // useless. So the world side's own code, `HopCell::run` and `finish`, must not
    // hold any waiting primitive or a loop: its only steps are the CAS, the session
    // check, the closure, the store and the unpark.
    // It reads code only: comments and literals are blanked first, so a brace in a
    // comment cannot end a body early and hide what follows it. And it reaches
    // through calls: every call in the two bodies must be one of those steps, so a
    // wait moved into a helper (or any new call) fails here until someone has
    // looked at it and added it below.
    let source = code_only(include_str!("../src/hop.rs"));
    let body = |name: &str| {
        let sig = format!("fn {name}(");
        assert_eq!(source.matches(&sig).count(), 1, "one {sig} in hop.rs");
        let at = source.find(&sig).unwrap();
        let open = at + source[at..].find('{').unwrap();
        let mut depth = 0usize;
        for (i, c) in source[open..].char_indices() {
            match c {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        return source[open..=open + i].to_owned();
                    }
                }
                _ => {}
            }
        }
        panic!("{sig} has no end");
    };
    // The steps the world side takes (`take` is the work cell's Option::take).
    let steps: HashSet<&str> = [
        "compare_exchange",
        "is_err",
        "state",
        "catch_unwind",
        "AssertUnwindSafe",
        "live",
        "unwrap_or",
        "get",
        "take",
        "map",
        "work",
        "Ok",
        "Some",
        "Err",
        "finish",
        "store",
        "unpark",
    ]
    .into();
    for name in ["run", "finish"] {
        let code = body(name);
        let mut words: Vec<(usize, &str)> = Vec::new();
        let mut start = None;
        for (i, c) in code.char_indices().chain([(code.len(), ' ')]) {
            if c.is_alphanumeric() || c == '_' {
                start.get_or_insert(i);
            } else if let Some(s) = start.take() {
                words.push((s, &code[s..i]));
            }
        }
        for (_, word) in &words {
            assert!(
                ![
                    "park",
                    "park_timeout",
                    "sleep",
                    "wait",
                    "wait_timeout",
                    "wait_while",
                    "yield_now",
                    "spin_loop",
                    "recv",
                    "recv_timeout",
                    "lock",
                    "Mutex",
                    "RwLock",
                    "Condvar",
                    "Barrier",
                    "loop",
                    "while",
                    "for",
                ]
                .contains(word),
                "the world side of a hop must not wait: `{word}` in fn {name}"
            );
        }
        // A call is a word followed by `(`, or a macro: `!` and a bracket.
        for (start, word) in &words {
            let after = &code[start + word.len()..];
            let call = after.trim_start().starts_with('(')
                || (after.starts_with('!') && after[1..].trim_start().starts_with(['(', '[', '{']));
            if call {
                assert!(
                    steps.contains(word),
                    "fn {name} calls `{word}`: check that it cannot wait, then add it to the steps"
                );
            }
        }
        // Not trivially empty: the steps that must be there are found.
        let needed: &[&str] = if name == "run" {
            &["compare_exchange", "self.finish("]
        } else {
            &["self.waiter.unpark()", ".store("]
        };
        for needed in needed {
            assert!(code.contains(needed), "fn {name} has no {needed}: {code}");
        }
    }
}

/// `source` with its comments and string and char literals blanked (lines kept), so a
/// scan of the code sees neither their braces nor their words.
fn code_only(source: &str) -> String {
    let chars: Vec<char> = source.chars().collect();
    let mut out = String::with_capacity(source.len());
    let blank = |out: &mut String, c: char| out.push(if c == '\n' { '\n' } else { ' ' });
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        let next = chars.get(i + 1).copied();
        if c == '/' && next == Some('/') {
            while i < chars.len() && chars[i] != '\n' {
                blank(&mut out, chars[i]);
                i += 1;
            }
        } else if c == '/' && next == Some('*') {
            let mut depth = 0;
            while i < chars.len() {
                if chars[i] == '/' && chars.get(i + 1) == Some(&'*') {
                    depth += 1;
                    blank(&mut out, chars[i]);
                    i += 1;
                } else if chars[i] == '*' && chars.get(i + 1) == Some(&'/') {
                    depth -= 1;
                    blank(&mut out, chars[i]);
                    i += 1;
                }
                blank(&mut out, chars[i]);
                i += 1;
                if depth == 0 {
                    break;
                }
            }
        } else if c == '"' {
            blank(&mut out, c);
            i += 1;
            while i < chars.len() && chars[i] != '"' {
                if chars[i] == '\\' {
                    blank(&mut out, chars[i]);
                    i += 1;
                }
                if i < chars.len() {
                    blank(&mut out, chars[i]);
                    i += 1;
                }
            }
            if i < chars.len() {
                blank(&mut out, chars[i]);
                i += 1;
            }
        } else if c == '\'' && (next == Some('\\') || chars.get(i + 2) == Some(&'\'')) {
            // a char literal (a lifetime has no closing quote two characters on)
            blank(&mut out, c);
            i += 1;
            if chars.get(i) == Some(&'\\') {
                blank(&mut out, chars[i]);
                i += 1;
            }
            while i < chars.len() {
                let end = chars[i] == '\'';
                blank(&mut out, chars[i]);
                i += 1;
                if end {
                    break;
                }
            }
        } else {
            out.push(c);
            i += 1;
        }
    }
    out
}

#[test]
fn a_panicking_session_check_still_ends_the_run() {
    // The worker waits for a Running hop whatever stops it, so Running must always
    // end: a check that panics is "not live", and the closure never runs.
    let ran = Arc::new(AtomicBool::new(false));
    let flag = ran.clone();
    let cell = HopCell::new(0, 1, move || flag.store(true, Ordering::SeqCst));
    assert_eq!(
        cell.run(|_, _| panic!("session check")),
        HopState::Cancelled
    );
    assert_eq!(cell.state(), HopState::Cancelled);
    assert!(!ran.load(Ordering::SeqCst));
}

#[test]
fn trampoline_abandon_and_cancel_races_leave_exactly_one_outcome() {
    const ROUNDS: usize = 2000;
    let mut tally = [0u32; 5];
    for round in 0..ROUNDS {
        let ran = Arc::new(AtomicU32::new(0));
        let counter = ran.clone();
        let cell = HopCell::new(0, 1, move || {
            counter.fetch_add(1, Ordering::SeqCst);
            round
        });
        let live = round % 3 != 0;
        let go = AtomicBool::new(false);
        let (abandoned, cancelled) = (AtomicBool::new(false), AtomicBool::new(false));
        let mut early = None;
        // This thread is the waiter: it sees the outcome and takes the result while
        // the others may still run (the release/acquire pair under test). Explicit
        // joins (pthread_join) afterwards give TSan the happens-before that scope's
        // own wait, in uninstrumented std, hides from it.
        std::thread::scope(|scope| {
            let (cell, abandoned, cancelled) = (&cell, &abandoned, &cancelled);
            let go = &go;
            let spin = move || while !go.load(Ordering::Acquire) {};
            let handles = [
                scope.spawn(move || {
                    spin();
                    cell.run(|_, _| live);
                }),
                scope.spawn(move || {
                    spin();
                    abandoned.store(cell.abandon(), Ordering::SeqCst);
                }),
                scope.spawn(move || {
                    spin();
                    cancelled.store(cell.cancel(), Ordering::SeqCst);
                }),
            ];
            go.store(true, Ordering::Release);
            let seen = loop {
                let state = cell.state();
                if !matches!(state, HopState::Pending | HopState::Running) {
                    break state;
                }
                std::hint::spin_loop();
            };
            if seen == HopState::Done {
                early = cell.take();
            }
            for handle in handles {
                handle.join().unwrap();
            }
        });
        let state = cell.state();
        tally[state as usize] += 1;
        let (abandoned, cancelled) = (abandoned.into_inner(), cancelled.into_inner());
        assert!(!(abandoned && cancelled), "two winners");
        match state {
            HopState::Done => {
                assert!(!abandoned && !cancelled);
                assert_eq!(ran.load(Ordering::SeqCst), 1);
                assert_eq!(early, Some(Ok(round)), "taken by the waiter as Done showed");
                assert_eq!(cell.take(), None, "taken once");
            }
            HopState::Abandoned => {
                assert!(abandoned);
                assert_eq!(ran.load(Ordering::SeqCst), 0);
            }
            HopState::Cancelled => {
                assert_eq!(ran.load(Ordering::SeqCst), 0);
                assert!(
                    cancelled || !live,
                    "cancelled by the worker or the session check"
                );
            }
            other => panic!("left in {other:?}"),
        }
    }
    eprintln!("race outcomes [pending, running, done, cancelled, abandoned]: {tally:?}");
}

// ---- submit_in follow-ups ----

#[test]
fn a_follow_up_runs_after_the_session_s_post_edge_or_is_cancelled_with_its_done() {
    let _session = session_lock();
    set_session(SERVER, 1);
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = api(true);
    let shared = WorkerShared::new(WorkerConfig {
        owner,
        host: unsafe { Host::read(&*api) },
        factory: empty_runtime,
        inbox: InboxConfig::DEFAULT,
    });
    let tasks = unsafe { TasksTable::read(tasks_table()) }.unwrap();
    let ran = Arc::new(AtomicU64::new(0));
    let dones = Arc::new(Mutex::new(Vec::new()));

    let (r, d) = (ran.clone(), dones.clone());
    let first = submit_after_step(
        &shared,
        &tasks,
        SERVER as u32,
        1,
        "follow-up",
        move |run| {
            assert!(!run.cancelled());
            r.fetch_add(1, Ordering::SeqCst);
        },
        move |state| d.lock().unwrap().push(state),
    )
    .unwrap();
    let submitted = pool.submissions();
    assert_eq!(
        submitted.last().unwrap().flags,
        KILN_TASK_AFTER_STEP as u32,
        "after the Post edge, no LongRunning thread"
    );
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(ran.load(Ordering::SeqCst), 0, "waits for the edge");
    pool.post_edge(SERVER);
    assert!(pool.task(0).wait_finished(WAIT));
    assert_eq!(ran.load(Ordering::SeqCst), 1);
    assert_eq!(*dones.lock().unwrap(), vec![KILN_TASK_COMPLETED as u32]);
    assert_ne!(first, 0);
    assert!(
        pool.task(0).released.load(Ordering::SeqCst),
        "done released the record (it counts toward the task owner's 1,024 until then)"
    );

    let (r, d) = (ran.clone(), dones.clone());
    submit_after_step(
        &shared,
        &tasks,
        SERVER as u32,
        1,
        "follow-up",
        move |_| {
            r.fetch_add(1, Ordering::SeqCst);
        },
        move |state| d.lock().unwrap().push(state),
    )
    .unwrap();
    pool.end_session(SERVER, 1);
    assert!(pool.task(1).wait_finished(WAIT));
    assert_eq!(ran.load(Ordering::SeqCst), 1, "never ran");
    assert_eq!(
        *dones.lock().unwrap(),
        vec![KILN_TASK_COMPLETED as u32, KILN_TASK_CANCELLED as u32],
        "done delivered: nothing leaks"
    );
    assert!(pool.task(1).released.load(Ordering::SeqCst));

    // The handle cancels a waiting follow-up: done once, record released, and a
    // later edge of the same session does not start it.
    let (r, d) = (ran.clone(), dones.clone());
    let third = submit_after_step(
        &shared,
        &tasks,
        SERVER as u32,
        0,
        "follow-up",
        move |_| {
            r.fetch_add(1, Ordering::SeqCst);
        },
        move |state| d.lock().unwrap().push(state),
    )
    .unwrap();
    assert_eq!(tasks.cancel(owner, third).1, KILN_TASK_CANCELLED as u32);
    assert!(pool.task(2).wait_finished(WAIT));
    pool.post_edge(SERVER);
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(
        ran.load(Ordering::SeqCst),
        1,
        "a cancelled follow-up never runs"
    );
    assert_eq!(dones.lock().unwrap().len(), 3);
    assert!(pool.task(2).released.load(Ordering::SeqCst));
    assert_eq!(tasks.cancel(owner, third).0, KILN_ERR_NOT_FOUND, "released");

    // Another session than the live one: refused NO_WORLD, nothing transferred.
    let refused = submit_after_step(&shared, &tasks, SERVER as u32, 9, "x", |_| {}, |_| {});
    assert_eq!(
        refused,
        Err((KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_NO_WORLD as u32))
    );
}
