//! Startup and restore gate: both command dispatch and every data-folder lookup use it.
use super::*;
use std::sync::{Mutex, OnceLock};

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    pub phase: String,
    pub reasons: Vec<String>,
    pub can_continue: bool,
}

static STATUS: OnceLock<Mutex<Status>> = OnceLock::new();
fn state() -> &'static Mutex<Status> {
    STATUS.get_or_init(|| Mutex::new(Status { phase: if cfg!(test) { "ready" } else { "preparing" }.into(), reasons: vec![], can_continue: false }))
}
pub fn status() -> Status { state().lock().unwrap_or_else(|e| e.into_inner()).clone() }
fn set(phase: &str, reasons: Vec<String>) { *state().lock().unwrap_or_else(|e| e.into_inner()) = Status { phase: phase.into(), reasons, can_continue: false }; }
pub fn refusal() -> Option<Error> {
    let s = status();
    (s.phase != "ready").then(|| Error::new(ErrorCode::Busy, if s.reasons.is_empty() { "Preparing the copy from before the beta.".into() } else { s.reasons.join("\n") }))
}
pub fn command_refusal(cmd: &str) -> Option<Error> {
    if matches!(cmd, "snapshot_status" | "snapshot_continue" | "app_info" | "app_started" | "app_render_frame" | "data_folder_get" | "deck_quit" | "app_open_url" | "task_list" | "task_get" | "task_cancel") { None } else { refusal() }
}
/// A restore of the beta copy stopped half way (its journal is there): only its recovery writes the folder.
pub fn restore_unfinished(root: &Path) -> bool { present(root, super::JOURNAL) }

/// Tests: the gate as every production start has it until [`start`] opens it ("preparing"; a test run's begins
/// "ready", which hid kiln:// and nxm:// links lost before the gate up to 0.2.0-dev.31.3), open again at
/// [`TestGate::open`] or when the guard goes.
#[cfg(test)]
pub(crate) struct TestGate;
#[cfg(test)]
impl TestGate {
    pub(crate) fn preparing() -> TestGate { set("preparing", vec![]); TestGate }
    pub(crate) fn open(&self) { set("ready", vec![]); }
}
#[cfg(test)]
impl Drop for TestGate { fn drop(&mut self) { set("ready", vec![]); } }
// Identical ordinary starts do not mutate beta state and may share the folder.
fn present(root: &Path, rel: &str) -> bool {
    !matches!(std::fs::symlink_metadata(root.join(rel)), Err(e) if e.kind() == std::io::ErrorKind::NotFound)
}
fn startup_work(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> bool {
    present(root, super::JOURNAL) || present(root, "snapshots/before-beta.pending.json") ||
        super::read_last(root).is_none_or(|l| l.format != 1 || l.version != version || l.channel != channel
            || l.beta != beta || l.pending_release || if beta.is_some() { l.copy_complete.is_none() }
                else { l.copy_complete.is_some() || !l.without_restore.is_empty() })
}

fn exclusive(folder: &crate::folder::Folder) -> Result<(), Error> {
    let mut held = folder.lock();
    let result = held.as_mut().ok_or_else(|| Error::new(ErrorCode::Busy,
        "Cannot protect the beta copy without the data-folder lock. Close other launchers and restart this launcher."))
        .and_then(|lock| lock.upgrade());
    match result {
        Ok(true) => Ok(()),
        // Ok(false) guarantees that shared access was reacquired, so the original session may resume.
        Ok(false) => Err(Error::new(ErrorCode::Busy,
            "Close the other launcher before preparing or restoring the beta copy. Restart this launcher to try again.")),
        Err(e) => { set("blocked", vec![e.message.clone()]); Err(e) },
    }
}

fn shared(folder: &crate::folder::Folder) -> Result<(), Error> {
    folder.lock().as_mut().ok_or_else(|| Error::new(ErrorCode::Busy, "The data-folder lock is missing."))?.downgrade()
}

pub fn start(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> Result<(), Error> {
    set("preparing", vec![]);
    if !startup_work(root, version, channel, beta) { set("ready", vec![]); return Ok(()); }
    let folder = crate::folder::current();
    if let Err(e) = exclusive(&folder) { set("blocked", vec![e.message.clone()]); return Err(e); }
    let result = super::recover_restore(root).and_then(|_| super::at_start(root, version, channel, beta));
    // A blocked recovery keeps the OS lock as well as the process-local gate.
    let recovering = present(root, super::JOURNAL);
    if !recovering && let Err(e) = shared(&folder) { set("blocked", vec![e.message.clone()]); return Err(e); }
    match result {
        Ok(_) => { set("ready", vec![]); Ok(()) },
        Err(e) => {
            set("blocked", e.message.lines().map(String::from).collect());
            state().lock().unwrap_or_else(|e| e.into_inner()).can_continue = beta.is_some() && !recovering;
            Err(e)
        },
    }
}
pub fn resume(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> Result<(), Error> {
    let s = {
        let mut g = state().lock().unwrap_or_else(|e| e.into_inner());
        if g.phase != "blocked" || !g.can_continue { return Err(Error::new(ErrorCode::Conflict, "There is no failed beta copy to continue past.")); }
        let saved = g.clone();
        g.phase = "preparing".into();
        saved
    };
    let folder = crate::folder::current();
    let result = exclusive(&folder).and_then(|_| super::continue_without_restore(root, version, channel, beta, s.reasons.clone()));
    if let Err(e) = shared(&folder) { set("blocked", vec![e.message.clone()]); return Err(e); }
    if let Err(e) = result {
        *state().lock().unwrap_or_else(|e| e.into_inner()) = s;
        return Err(e);
    }
    set("ready", vec![]);
    Ok(())
}

pub struct Freeze { previous: Status, folder: std::sync::Arc<crate::folder::Folder>, exclusive: bool }
impl Freeze {
    pub fn keep_blocked(&self, error: &Error) { set("blocked", vec![error.message.clone()]); }
}
impl Drop for Freeze {
    fn drop(&mut self) {
        if status().phase == "preparing" {
            if self.exclusive && let Err(e) = shared(&self.folder) { set("blocked", vec![e.message]); return; }
            *state().lock().unwrap_or_else(|e| e.into_inner()) = self.previous.clone();
        }
    }
}
pub fn freeze() -> Result<Freeze, Error> {
    let previous = crate::commands::app::begin_alone(|running| {
        if !running.is_empty() { return Err(Error::new(ErrorCode::Busy, "Wait for the launcher's tasks before restoring the beta copy.")); }
        if let Some(e) = crate::folder::current().held_error() { return Err(e); }
        let mut g = state().lock().unwrap_or_else(|e| e.into_inner());
        if g.phase != "ready" { return Err(Error::new(ErrorCode::Busy, "The launcher data is held for recovery.")); }
        let saved = g.clone();
        g.phase = "preparing".into();
        Ok(saved)
    })?;
    let mut freeze = Freeze { previous, folder: crate::folder::current(), exclusive: false };
    exclusive(&freeze.folder)?;
    freeze.exclusive = true;
    crate::state::wait_for_writers();
    Ok(freeze)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn failed_copy_blocks_settings_profiles_kept_update_and_log_until_explicit_opt_out() {
        let _serial = crate::folder::test_serial();
        struct Reset;
        impl Drop for Reset { fn drop(&mut self) { set("ready", vec![]); } }
        let _reset = Reset;
        let root = std::env::temp_dir().join(format!("kiln-beta-gate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let folder = std::sync::Arc::new(crate::folder::Folder::open(kiln_manage::paths::DataEnv {
            kiln_data_dir: Some(root.clone().into()), ..Default::default()
        }));
        crate::folder::replace(folder.clone());
        let settings = root.join("settings.json");
        std::fs::File::create(&settings).unwrap().set_len(super::super::MAX_FILE + 1).unwrap();
        assert!(start(&root, "0.2.0-dev.29", "beta", Some(1)).is_err());
        assert_eq!(status().phase, "blocked");
        assert!(crate::state::dirs().is_err());
        assert!(folder.dirs().is_err());
        assert!(crate::commands::data_folder::hold_for_move(&folder).is_err());
        for cmd in ["app_settings_set", "profile_save", "library_rollback", "update_check", "update_install"] {
            assert!(folder.command_refusal(cmd).is_some(), "{cmd}");
        }
        crate::commands::app::launcher_log("test", "must not write while held");
        assert!(!root.join("logs/launcher.log").exists());
        assert!(!root.join("update.json").exists());
        assert!(!root.join("library/kiln-kept.json").exists());
        resume(&root, "0.2.0-dev.29", "beta", Some(1)).unwrap();
        assert_eq!(status().phase, "ready");
        assert!(crate::state::dirs().is_ok());
        assert!(super::super::without_restore(&root)[0].contains("settings.json"));
        assert!(super::super::info(&root).is_none());
        let _ = std::fs::remove_dir_all(root);
    }
    /// 0.2.0-dev.29 to 29.17: the start's render lines went to stderr only and its marker was never read, because both came
    /// before this gate opened. The marker is read from the folder as found; the lines reach launcher.log once it opens.
    #[test]
    fn the_starts_lines_and_markers_do_not_wait_for_the_gate() {
        let _serial = crate::folder::test_serial();
        struct Reset;
        impl Drop for Reset { fn drop(&mut self) { set("ready", vec![]); } }
        let _reset = Reset;
        let (t, folder) = open_test_folder("early-log");
        set("preparing", vec![]);
        assert!(crate::state::dirs().is_err());
        assert_eq!(crate::folder::found_dirs().map(|d| d.root), Some(folder.located().unwrap().dirs.root.clone()));
        let line = format!("early line {}", std::process::id());
        crate::commands::app::launcher_log("render", &line);
        let log = t.0.join("logs").join("launcher.log");
        assert!(!log.exists(), "nothing is written before the gate opens");
        set("ready", vec![]);
        crate::commands::app::flush_early_log();
        crate::commands::app::flush_early_log();
        let text = std::fs::read_to_string(&log).unwrap();
        assert_eq!(text.matches(&format!("render: {line}")).count(), 1, "{text}");
    }

    fn open_test_folder(tag: &str) -> (super::super::tests::Tmp, std::sync::Arc<crate::folder::Folder>) {
        let t = super::super::tests::tmp(tag);
        let folder = std::sync::Arc::new(crate::folder::Folder::open(kiln_manage::paths::DataEnv {
            kiln_data_dir: Some(t.0.clone().into()), ..Default::default()
        }));
        crate::folder::replace(folder.clone());
        set("ready", vec![]);
        (t, folder)
    }

    #[cfg(unix)]
    #[test]
    #[ignore = "child process fixture, invoked by the cross-process lock test"]
    fn shared_lock_peer() {
        let root = std::path::PathBuf::from(std::env::var_os("KILN_TEST_LOCK_ROOT").unwrap());
        let _lock = kiln_manage::data_folder::DataLock::shared(&root).unwrap().unwrap();
        std::fs::write(root.join("peer-ready"), "ready").unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        while !root.join("peer-done").exists() {
            assert!(std::time::Instant::now() < deadline, "parent did not release peer");
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    #[cfg(unix)]
    #[test]
    fn beta_restore_excludes_another_launcher_and_keeps_the_lock_through_handoff() {
        let _serial = crate::folder::test_serial();
        let (t, folder) = open_test_folder("restore-lock");
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "snapshot::startup::tests::shared_lock_peer", "--ignored"])
            .env("KILN_TEST_LOCK_ROOT", &t.0).spawn().unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        while !t.0.join("peer-ready").exists() {
            assert!(std::time::Instant::now() < deadline, "peer never took its shared lock");
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let result = freeze();
        // Release and join the peer even when the assertion below fails on the old implementation.
        std::fs::write(t.0.join("peer-done"), "done").unwrap();
        assert!(child.wait().unwrap().success());
        let error = result.err().expect("a second launcher's shared lock must refuse restore");
        assert_eq!(error.code, ErrorCode::Busy);
        assert!(error.message.contains("other launcher"), "{}", error.message);
        assert_eq!(status().phase, "ready", "a refusal returns to the original session");
        let held = freeze().unwrap();
        assert!(folder.lock().as_ref().unwrap().is_exclusive());
        assert!(kiln_manage::data_folder::DataLock::shared(&t.0).unwrap().is_none());
        drop(held);
        assert_eq!(status().phase, "ready");
        assert!(!folder.lock().as_ref().unwrap().is_exclusive());
        let held = freeze().unwrap();
        held.keep_blocked(&Error::invalid("incomplete restore"));
        drop(held);
        assert!(folder.lock().as_ref().unwrap().is_exclusive());
        assert!(folder.dirs().is_err());
        assert!(kiln_manage::data_folder::DataLock::shared(&t.0).unwrap().is_none());
        set("ready", vec![]);
    }

    #[test]
    fn beta_startup_capture_and_recovery_require_exclusive_access() {
        let _serial = crate::folder::test_serial();
        let (t, folder) = open_test_folder("startup-lock");
        super::super::tests::put(&t.0, "settings.json", "{}");
        let peer = kiln_manage::data_folder::DataLock::shared(&t.0).unwrap().unwrap();
        let result = start(&t.0, "29", "beta", Some(1));
        drop(peer);
        assert!(result.is_err(), "capture must not run across a second launcher's writes");
        assert!(!t.0.join(DIR).exists());
        assert!(!t.0.join(LAST_BUILD).exists());
        assert!(!status().can_continue, "lock refusal cannot be bypassed with opt out");
        start(&t.0, "29", "beta", Some(1)).unwrap();
        assert!(!folder.lock().as_ref().unwrap().is_exclusive());
        // An ordinary second launcher of the same build needs no new capture or marker.
        let peer = kiln_manage::data_folder::DataLock::shared(&t.0).unwrap().unwrap();
        start(&t.0, "29", "beta", Some(1)).unwrap();
        super::super::durable_json(&t.0.join(super::super::JOURNAL), &super::super::RestoreJournal {
            format: 1, files: [("settings.json".into(), "missing".into())].into(),
        }).unwrap();
        assert!(start(&t.0, "29", "dev", None).is_err());
        assert!(!folder.lock().as_ref().unwrap().is_exclusive(), "failed upgrade did not acquire exclusivity");
        drop(peer);
        assert!(start(&t.0, "29", "dev", None).is_err(), "damaged stage remains blocked");
        assert!(folder.lock().as_ref().unwrap().is_exclusive(), "blocked recovery retains exclusivity");
        assert!(!status().can_continue);
        assert!(kiln_manage::data_folder::DataLock::shared(&t.0).unwrap().is_none());
        set("ready", vec![]);
    }

    #[test]
    fn a_restore_lock_error_keeps_local_writers_blocked() {
        let _serial = crate::folder::test_serial();
        let (_t, folder) = open_test_folder("restore-lock-error");
        folder.release_lock();
        assert!(freeze().is_err());
        assert_eq!(status().phase, "blocked", "no safe shared lock is known after a lock error");
        assert!(folder.dirs().is_err());
        set("ready", vec![]);
    }

}
