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

#[test]
fn start_stages_and_last_successful_duration() {
    let root = std::env::temp_dir().join(format!("kiln-start-stages-{}", crate::util::uuid_v4().unwrap()));
    let dir = root.join("server");
    let log_dir = dir.join("logs");
    let capture = root.join("local.run.log");
    let game_log = log_dir.join(super::game::LOG);
    std::fs::create_dir_all(&log_dir).unwrap();
    std::fs::write(dir.join(super::game::EXE), []).unwrap();
    let instance = ServerInstance {
        id: "local".into(),
        name: "Fake server".into(),
        kind: InstanceKind::Local,
        dir: Some(dir.clone()),
        port: 15637,
        query_port: 15637,
        wine: None,
        policy: "any".into(),
        source: "launcher".into(),
        remote: None,
        runner: ServerRunner::Windows,
        prefix: None,
        backup_dir: None,
        kilnctl: None,
        stop_timeout_s: 90,
    };

    let try_start = |ready, duration_s| {
        let mut stages = Stages::default();
        // Restart forwards the start through this progress adapter.
        let mut scaled = Scaled { inner: &mut stages, from: 0.4, to: 1.0 };
        let result = run::start_via(None, &instance, &mut scaled, &mut |instance, logs, _, progress| {
            let mut log = std::fs::OpenOptions::new().create(true).append(true).open(&game_log).unwrap();
            if ready {
                writeln!(log, "{}", super::game::READY).unwrap();
            } else {
                writeln!(log, "Loading the world").unwrap();
            }
            let spawned = Instant::now() - Duration::from_secs(duration_s);
            run::await_start(instance, logs, &capture, spawned, "started fake runner (pid 1)", progress, &mut || {
                Some("fake runner exited".into())
            })
        });
        (result, stages.0)
    };

    let (failed, stages) = try_start(false, 2);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "wine", "process", "world"]);
    assert!(!capture.with_extension("start.json").exists());

    let (started, stages) = try_start(true, 4);
    assert_eq!(stages, ["preparing", "wine", "process", "world", "running"]);
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
    let (failed, stages) = try_start(false, duration + 2);
    assert_eq!(failed.unwrap_err().code, ErrorCode::Io);
    assert_eq!(stages, ["preparing", "wine", "process", "world"]);
    assert_eq!(std::fs::read(record).unwrap(), before);
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
