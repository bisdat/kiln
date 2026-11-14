//! Debugger targets for `kiln debug` and the editors (chapter 12a): which process a debugger may attach to, proven by
//! its Windows identity, and the state of an attached session.
//!
//! | module | what |
//! |---|---|
//! | [`errors`] | the stable error codes (`identity_changed`, `pause_consent_required`, ...) |
//! | [`identity`] | the core's descriptor, the checked tuple `DebugIdentity`, `DebugTarget`, `DebugSessionState` |
//! | [`resolver`] | descriptors + the Windows side + the host side -> one target; the check before an adapter starts |
//! | [`windows`] | native Windows: the Windows side in this process (`NativeSource`), the adapter's console and job handling |
//!
//! The session file (`<install>/kiln/state/debug/session.json`) says that a proxy is attached; `kiln reload` refuses
//! while it does (a reload under an attached debugger leaves breakpoints on a dead copy, and such sessions have
//! ended the server).

pub mod errors;
pub mod identity;
pub mod resolver;
pub mod windows;

pub use errors::{DebugCode, DebugError};
pub use identity::{
    DebugIdentity, DebugSessionState, DebugTarget, Descriptor, PrefixIdentity, RunnerContext, SessionMode, SessionState, TargetKind,
};
pub use resolver::{AttachPermit, HostFacts, HostSide, HostSpec, ProcessSource, Resolved};

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// The proxy's session file: who wrote it (to tell a live proxy from a crashed one) and the session's state.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionFile {
    pub format: u32,
    /// The proxy's own PID and start (Unix seconds): both must still match for the file to count.
    pub proxy_pid: u32,
    pub proxy_started: Option<i64>,
    pub state: DebugSessionState,
}

/// `<install>/kiln/state/debug/session.json`.
pub fn session_path(install: &Path) -> PathBuf {
    install.join(crate::paths::game::KILN).join("state").join(identity::DESCRIPTOR_DIR).join(identity::SESSION_FILE)
}

/// Writes this process's session state (atomically).
pub fn write_session(install: &Path, state: &DebugSessionState) -> crate::Result<()> {
    let me = std::process::id();
    let f = SessionFile { format: 1, proxy_pid: me, proxy_started: crate::proc::start_time(me).map(crate::util::unix), state: state.clone() };
    let path = session_path(install);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(crate::util::io_at(dir, "creating"))?;
    }
    crate::util::write_json_atomic(&path, &f)
}

/// Removes the session file when this process wrote it.
pub fn clear_session(install: &Path) {
    let path = session_path(install);
    if crate::util::read_json::<SessionFile>(&path).ok().flatten().is_some_and(|f| f.proxy_pid == std::process::id()) {
        let _ = std::fs::remove_file(&path);
    }
}

/// The attached session of this install, if a proxy that still runs says one is attached (a file left by a proxy that
/// ended, or by another process under the same PID, does not count).
pub fn attached_session(install: &Path) -> Option<SessionFile> {
    let f: SessionFile = crate::util::read_json(&session_path(install)).ok().flatten()?;
    if matches!(f.state.state, SessionState::Detached | SessionState::Ended) {
        return None;
    }
    if !crate::proc::alive(f.proxy_pid) {
        return None;
    }
    let started = crate::proc::start_time(f.proxy_pid).map(crate::util::unix);
    (f.proxy_started.is_some() && started == f.proxy_started).then_some(f)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(s: SessionState) -> DebugSessionState {
        DebugSessionState {
            identity: DebugIdentity {
                install_id: "i".into(),
                prefix_identity: None,
                runner_sha256: None,
                windows_pid: 32,
                creation_time_100ns: "5".into(),
                image_sha256: "a".repeat(64),
                session_nonce: "n".repeat(32),
            },
            state: s,
            session_mode: SessionMode::Hosted,
            reason: None,
            paused_since: None,
            max_pause_ms: None,
        }
    }

    #[test]
    fn a_live_proxy_holds_the_session_and_a_dead_one_does_not() {
        let t = std::env::temp_dir().join(format!("kiln-debug-session-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        assert!(attached_session(&t).is_none());
        write_session(&t, &state(SessionState::Running)).unwrap();
        let f = attached_session(&t).expect("this test process is the proxy");
        assert_eq!(f.state.state, SessionState::Running);
        // JSON names the extension reads
        let v: serde_json::Value = serde_json::from_slice(&std::fs::read(session_path(&t)).unwrap()).unwrap();
        for k in ["identity", "state", "sessionMode", "reason", "pausedSince", "maxPauseMs"] {
            assert!(v["state"].get(k).is_some(), "{k}");
        }
        assert_eq!(v["state"]["state"], "running");
        // over: does not hold
        write_session(&t, &state(SessionState::Detached)).unwrap();
        assert!(attached_session(&t).is_none());
        // written by a process that ended (or a PID now used by another process)
        write_session(&t, &state(SessionState::Paused)).unwrap();
        let mut f: SessionFile = crate::util::read_json(&session_path(&t)).unwrap().unwrap();
        f.proxy_started = f.proxy_started.map(|s| s - 3600);
        crate::util::write_json_atomic(&session_path(&t), &f).unwrap();
        assert!(attached_session(&t).is_none());
        f.proxy_pid = u32::MAX - 3;
        crate::util::write_json_atomic(&session_path(&t), &f).unwrap();
        assert!(attached_session(&t).is_none());
        // clear only removes our own
        clear_session(&t);
        assert!(session_path(&t).is_file(), "another proxy's file stays");
        write_session(&t, &state(SessionState::Running)).unwrap();
        clear_session(&t);
        assert!(!session_path(&t).exists());
        std::fs::remove_dir_all(&t).unwrap();
    }
}
