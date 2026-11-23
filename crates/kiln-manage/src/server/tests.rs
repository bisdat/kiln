//! Server unit tests and opt-in lab tests (the labs run SteamCMD and Wine).
//!
//! `KILN_TEST_SERVER_DIR=~/srv/enshrouded/labs/<name> KILN_TEST_SERVER_PORT=26011 [KILN_TEST_REFLINK_FROM=<install>]
//!  XDG_CONFIG_HOME=<scratch> XDG_STATE_HOME=<scratch> KILN_DATA_DIR=<scratch>
//!  cargo test -p kiln-manage --all-features -- --ignored --nocapture lab_instance`

#[cfg(feature = "server")]
use super::instance::{self, ServerSpec};
use super::instance::{InstanceKind, Scaled, ServerInstance, ServerRunner};
use super::run;
#[cfg(feature = "server")]
use super::ServerState;
use crate::ErrorCode;
use crate::task::Progress;
use std::io::Write;
use std::path::PathBuf;
use std::time::{Duration, Instant};

#[derive(Default)]
struct Stages(Vec<String>);

impl Progress for Stages {
    fn report(&mut self, _: Option<f32>, _: &str) {}

    fn stage(&mut self, id: &str) {
        self.0.push(id.to_owned());
    }
}

/// A fake local server folder (an empty exe, a logs folder) under `root`, run by `runner`.
fn fake_instance(root: &std::path::Path, runner: ServerRunner) -> ServerInstance {
    let dir = root.join("server");
    std::fs::create_dir_all(dir.join("logs")).unwrap();
    std::fs::write(dir.join(super::game::EXE), []).unwrap();
    ServerInstance {
        id: "local".into(),
        name: "Fake server".into(),
        kind: InstanceKind::Local,
        dir: Some(dir),
        port: 15637,
        query_port: 15637,
        wine: None,
        policy: "any".into(),
        source: "launcher".into(),
        remote: None,
        runner,
        prefix: None,
        backup_dir: None,
        kilnctl: None,
        stop_timeout_s: 90,
    }
}

fn scratch(what: &str) -> PathBuf {
    std::env::temp_dir().join(format!("kiln-{what}-{}", crate::util::uuid_v4().unwrap()))
}

/// A start of `instance` through a fake runner that writes `game` to the game log and `kiln` to kiln.log, then waits
/// for the world (the runner "exits" at once, so a start without a ready line fails). Returns the result and the stages.
fn fake_start(instance: &ServerInstance, capture: &std::path::Path, game: Option<&str>, kiln: Option<&str>, duration_s: u64)
    -> (crate::Result<super::ServerStatus>, Vec<String>) {
    let dir = instance.dir.clone().unwrap();
    let mut stages = Stages::default();
    // Restart forwards the start through this progress adapter.
    let mut scaled = Scaled { inner: &mut stages, from: 0.4, to: 1.0 };
    let result = run::start_via(None, instance, &mut scaled, &mut |instance, logs, _, progress| {
        if let Some(line) = game {
            let mut log = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("logs").join(super::game::LOG)).unwrap();
            writeln!(log, "{line}").unwrap();
        }
        if let Some(line) = kiln {
            std::fs::create_dir_all(dir.join("kiln/logs")).unwrap();
            std::fs::write(dir.join("kiln/logs/kiln.log"), format!("{line}\n")).unwrap();
        }
        let spawned = Instant::now() - Duration::from_secs(duration_s);
        run::await_start(instance, logs, capture, spawned, "started fake runner (pid 1)", progress, &mut || Some("fake runner exited".into()))
    });
    (result, stages.0)
}

#[test]
fn start_stages_and_last_successful_duration() {
    let root = scratch("start-stages");
    let instance = fake_instance(&root, ServerRunner::Windows);
    let dir = instance.dir.clone().unwrap();
    let capture = root.join("local.run.log");

    // spawned, but no log written yet: the start is at "process" (a Windows server has no Wine stage)
    let (failed, stages) = fake_start(&instance, &capture, None, None, 1);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "process"]);
    // the game writes its log: the world loads
    let (failed, stages) = fake_start(&instance, &capture, Some("Loading the world"), None, 2);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "process", "world"]);
    assert!(!capture.with_extension("start.json").exists());

    let (started, stages) = fake_start(&instance, &capture, Some(super::game::READY), None, 4);
    assert_eq!(stages, ["preparing", "process", "world", "running"]);
    let started = started.unwrap();
    let duration = started.last_start_s.unwrap();
    assert!(duration >= 4);
    assert_eq!(run::read_last_start(&capture, &dir), Some(duration));
    assert_eq!(run::read_last_start(&capture, &root.join("different-install")), None);
    let wire = serde_json::to_value(&started).unwrap();
    assert_eq!(wire["lastStartS"].as_u64(), Some(duration));

    // The previous ready line is still present. A new log write followed by failure must preserve the record.
    let record = capture.with_extension("start.json");
    let before = std::fs::read(&record).unwrap();
    let (failed, stages) = fake_start(&instance, &capture, Some("Loading the world"), None, duration + 2);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "process", "world"]);
    assert_eq!(std::fs::read(record).unwrap(), before);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn a_wine_start_has_the_wine_stage_and_kilns_line_ends_the_wait() {
    let root = scratch("start-wine");
    let instance = fake_instance(&root, ServerRunner::Kilnctl);
    assert!(run::uses_wine(ServerRunner::Kilnctl) && !run::uses_wine(ServerRunner::Windows) && !run::uses_wine(ServerRunner::Remote));
    let capture = root.join("local.run.log");
    let (failed, stages) = fake_start(&instance, &capture, Some("Loading the world"), None, 1);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "wine", "process", "world"]);
    // Kiln's readiness line before the game wrote anything: the start ends at once
    let (started, stages) = fake_start(&instance, &capture, None, Some("tick: server world 0xb2410080 started (session 1, thread 388)"), 1);
    assert!(started.is_ok(), "{:?}", started.err());
    assert_eq!(stages, ["preparing", "wine", "process", "running"]);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn a_running_world_from_the_game_log_or_this_runs_kiln_log() {
    let root = scratch("world-runs");
    let instance = fake_instance(&root, ServerRunner::Windows);
    let dir = instance.dir.clone().unwrap();
    let game_log = dir.join("logs").join(super::game::LOG);
    let kiln_log = dir.join("kiln/logs/kiln.log");
    let started = std::time::SystemTime::now() - Duration::from_secs(30);
    assert!(!run::world_runs(&instance, &dir, Some(started)), "no logs yet");
    std::fs::write(&game_log, "Switching state from Init to LoadEcsScene after 2.1 s\n").unwrap();
    assert!(!run::world_runs(&instance, &dir, Some(started)), "the world still loads");
    // the last run's kiln.log (written before this process started) says nothing about this run
    std::fs::create_dir_all(kiln_log.parent().unwrap()).unwrap();
    std::fs::write(&kiln_log, "tick: server world 0x1 started (session 1, thread 2)\n").unwrap();
    std::fs::File::options().write(true).open(&kiln_log).unwrap().set_modified(started - Duration::from_secs(600)).unwrap();
    assert!(!run::world_runs(&instance, &dir, Some(started)), "the last run's kiln.log");
    // this run's: the world runs, as the start that ended on it said (the game log has not caught up)
    std::fs::File::options().write(true).open(&kiln_log).unwrap().set_modified(std::time::SystemTime::now()).unwrap();
    assert!(run::world_runs(&instance, &dir, Some(started)), "this run's kiln.log");
    assert!(!run::world_runs(&instance, &dir, None), "no start time: kiln.log cannot be told apart from the last run's");
    std::fs::remove_file(&kiln_log).unwrap();
    std::fs::write(&game_log, format!("Switching state from LoadEcsScene to Run after 31.0 s\n{}\n", super::game::READY)).unwrap();
    assert!(run::world_runs(&instance, &dir, None), "the game log says so");
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn trying_a_create_again_finishes_only_the_half_made_server_in_the_same_folder() {
    let root = scratch("resume");
    let mut half = fake_instance(&root, ServerRunner::Kilnctl);
    let dir = half.dir.clone().unwrap();
    let spec = |dir: PathBuf| super::instance::ServerSpec {
        id: "local".into(), name: "Fake server".into(), dir, port: 15637, reflink_from: None, policy: None, wine: None, skip_steamcmd: false, resume: true,
    };
    assert!(super::instance::resumable(&half, &spec(dir.clone())).is_ok());
    let other = super::instance::resumable(&half, &spec(root.join("elsewhere"))).unwrap_err();
    assert_eq!(other.code, ErrorCode::AlreadyExists);
    assert!(other.message.contains("same folder"), "{}", other.message);
    for runner in [ServerRunner::Steam, ServerRunner::Windows, ServerRunner::Remote] {
        half.runner = runner;
        assert_eq!(super::instance::resumable(&half, &spec(dir.clone())).unwrap_err().code, ErrorCode::AlreadyExists, "{runner:?}");
    }
    std::fs::remove_dir_all(root).unwrap();
}

#[cfg(feature = "server")]
fn print_progress(f: Option<f32>, m: &str) {
    println!("[{}] {m}", f.map(|f| format!("{:3.0}%", f * 100.0)).unwrap_or_else(|| "  ? ".into()));
}

#[test]
#[ignore]
#[cfg(feature = "server")]
fn lab_instance_lifecycle() {
    let dir = PathBuf::from(std::env::var("KILN_TEST_SERVER_DIR").expect("KILN_TEST_SERVER_DIR"));
    let port: u16 = std::env::var("KILN_TEST_SERVER_PORT").expect("KILN_TEST_SERVER_PORT").parse().unwrap();
    let id = dir.file_name().unwrap().to_string_lossy().to_string();
    for v in ["XDG_CONFIG_HOME", "XDG_STATE_HOME", "KILN_DATA_DIR"] {
        let p = std::env::var(v).unwrap_or_else(|_| panic!("{v} must point at a scratch folder"));
        assert!(!p.contains("/.config") && !p.contains("/.local"), "{v}={p}: use a scratch folder, not the user's");
    }
    let mut cb = print_progress;
    let inst = match instance::get(&id) {
        Ok(i) => i,
        Err(_) => instance::create(
            &ServerSpec {
                id: id.clone(),
                name: format!("Kiln launcher test {id}"),
                dir: dir.clone(),
                port,
                reflink_from: std::env::var_os("KILN_TEST_REFLINK_FROM").map(PathBuf::from),
                policy: Some("any".into()),
                wine: None,
                skip_steamcmd: std::env::var_os("KILN_TEST_SKIP_STEAMCMD").is_some(),
                resume: false,
            },
            &mut cb,
        )
        .expect("create"),
    };
    println!("instance: {}", serde_json::to_string(&inst).unwrap());
    assert_eq!(inst.runner, ServerRunner::Kilnctl);
    assert_eq!(inst.query_port, port);
    assert!(instance::list().unwrap().iter().any(|s| s.id == id));
    let st = run::status(&inst).unwrap();
    assert_eq!(st.state, ServerState::Stopped, "{st:?}");
    let t0 = std::time::Instant::now();
    let st = run::start(&inst, &mut cb).expect("start");
    println!("started in {:.1} s: {st:?}", t0.elapsed().as_secs_f64());
    assert_eq!(st.state, ServerState::Running);
    let pid = st.pid.expect("pid");
    let q = super::query::query(&format!("127.0.0.1:{port}"), Duration::from_secs(2)).expect("A2S");
    println!("A2S: {q:?}");
    assert_eq!(q.players, 0);
    assert!(run::start(&inst, &mut cb).is_err(), "a second start is refused");
    let settings = super::settings::read(&inst).unwrap();
    assert!(settings.running && settings.values["queryPort"] == port);
    assert!(super::settings::write(&inst, &serde_json::json!({"slotCount": 3})).is_err(), "no edits while running");
    let st = run::stop(&inst, &mut cb).expect("stop");
    println!("stopped: {st:?}");
    assert_eq!(st.state, ServerState::Stopped);
    assert!(!super::procs::alive(pid));
    let s = super::settings::write(&inst, &serde_json::json!({"slotCount": 3})).expect("edit while stopped");
    assert_eq!(s.values["slotCount"], 3);
    let b = super::backup::create(&inst, &mut cb).expect("backup");
    println!("backup: {b:?}");
    assert!(super::backup::list(&inst).unwrap().iter().any(|x| x.name == b.name));
}

/// Windowless start + console, for any local runner:
/// `KILN_TEST_SERVER_ID=<id> XDG_CONFIG_HOME=<scratch> XDG_STATE_HOME=<scratch> KILN_DATA_DIR=<scratch>
///  [KILN_STEAM_ROOTS=<fake Steam root>] <test binary> --ignored --nocapture lab_console`
/// Starts the server, checks the console shows the run from its start marker with game-log levels, follows it
/// through the stop. Window checks are outside (the server reaches no display: run::headless).
#[test]
#[ignore]
#[cfg(feature = "server")]
fn lab_console_windowless() {
    use super::console::{ConsoleFollower, ConsoleLevel, ConsoleSource, ConsoleUpdate};
    let id = std::env::var("KILN_TEST_SERVER_ID").expect("KILN_TEST_SERVER_ID");
    for v in ["XDG_CONFIG_HOME", "XDG_STATE_HOME", "KILN_DATA_DIR"] {
        let p = std::env::var(v).unwrap_or_else(|_| panic!("{v} must point at a scratch folder"));
        assert!(!p.contains("/.config") && !p.contains("/.local"), "{v}={p}: use a scratch folder, not the user's");
    }
    let mut cb = print_progress;
    let inst = instance::get(&id).expect("instance");
    println!("instance: {} runner {:?} prefix {:?}", inst.id, inst.runner, inst.prefix);
    assert_eq!(run::status(&inst).unwrap().state, ServerState::Stopped, "stop it first");
    let st = run::start(&inst, &mut cb).expect("start");
    assert_eq!(st.state, ServerState::Running);
    let (snap, mut f) = ConsoleFollower::open(&inst, 5000);
    println!("console: {:?} {:?}, {} lines, truncated {}", snap.source, snap.path, snap.lines.len(), snap.truncated);
    for l in snap.lines.iter().take(4) {
        println!("  {:?} {}", l.level, l.text);
    }
    assert_eq!(snap.source, ConsoleSource::Capture);
    assert!(snap.lines[0].text.starts_with("---- kiln-launcher start "), "{:?}", snap.lines[0]);
    assert_eq!(snap.lines[0].level, ConsoleLevel::Meta);
    assert!(snap.lines.iter().any(|l| l.text.contains(super::game::READY)), "the ready line is in the console");
    let warned: Vec<_> = snap.lines.iter().filter(|l| l.level == ConsoleLevel::Warn || l.level == ConsoleLevel::Error).collect();
    println!("{} warn/error lines, e.g. {:?}", warned.len(), warned.first());
    assert!(!warned.is_empty(), "game-log levels reach the captured lines");
    let stopped = run::stop(&inst, &mut cb).expect("stop");
    assert_eq!(stopped.state, ServerState::Stopped);
    let mut after = Vec::new();
    for _ in 0..8 {
        if let ConsoleUpdate::Lines(l) = f.poll() {
            after.extend(l);
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    println!("{} lines followed through the stop, last: {:?}", after.len(), after.last());
    assert!(after.iter().any(|l| l.text.contains("Close Log file")), "the follower saw the shutdown");
}
