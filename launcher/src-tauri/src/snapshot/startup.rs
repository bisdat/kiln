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
    if matches!(cmd, "snapshot_status" | "snapshot_continue" | "app_info" | "app_started" | "data_folder_get" | "deck_quit" | "app_open_url" | "task_list" | "task_get" | "task_cancel") { None } else { refusal() }
}
pub fn start(root: &Path, version: &str, channel: &str, beta: Option<u32>) -> Result<(), Error> {
    set("preparing", vec![]);
    let result = super::recover_restore(root).and_then(|_| super::at_start(root, version, channel, beta));
    match result {
        Ok(_) => { set("ready", vec![]); Ok(()) },
        Err(e) => {
            set("blocked", e.message.lines().map(String::from).collect());
            state().lock().unwrap_or_else(|e| e.into_inner()).can_continue = beta.is_some() && !root.join(super::JOURNAL).exists();
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
    if let Err(e) = super::continue_without_restore(root, version, channel, beta, s.reasons.clone()) {
        *state().lock().unwrap_or_else(|e| e.into_inner()) = s;
        return Err(e);
    }
    set("ready", vec![]);
    Ok(())
}

pub struct Freeze { previous: Status }
impl Freeze {
    pub fn keep_blocked(&self, error: &Error) { set("blocked", vec![error.message.clone()]); }
}
impl Drop for Freeze {
    fn drop(&mut self) {
        if status().phase == "preparing" { *state().lock().unwrap_or_else(|e| e.into_inner()) = self.previous.clone(); }
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
    crate::state::wait_for_writers();
    Ok(Freeze { previous })
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
}
