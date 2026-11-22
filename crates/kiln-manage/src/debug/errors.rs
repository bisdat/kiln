//! The stable error codes of `kiln debug` and its helper (chapter 12a §5): every refusal carries one code and, where
//! the code alone does not say what differed, a short reason (`creation_time`, `nonce`, `steam_library`, ...).
//!
//! The codes are a contract with editors (the VS Code extension shows them) and with the helper protocol: add new ones
//! at the end, never rename one.

use serde::{Deserialize, Serialize};
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DebugCode {
    /// Refused on purpose: a server in a Steam library, developer debugging off, several processes match.
    Refused,
    /// Nothing runs there (or only a stale descriptor of a process that ended).
    NotRunning,
    /// The process, its pipe or its descriptor is not the one that was resolved (PID reused, restarted, other nonce).
    IdentityChanged,
    /// The process runs but its .NET host has not started (or does not run at all).
    RuntimeNotReady,
    /// netcoredbg or the `debug-inspect` helper is missing, or the helper's hash does not match.
    ToolMissing,
    /// The process cannot be opened with this user's rights (elevated game, other user).
    PrivilegeMismatch,
    /// The runner contexts of one prefix disagree (Proton): no first match is taken.
    AmbiguousRunner,
    /// The Windows side cannot prove the identity (a query or pipe operation missing under Wine).
    UnsupportedIdentity,
    /// A client is paused only with explicit consent (`--allow-client-pause`).
    PauseConsentRequired,
    /// The thing named does not exist (server instance prefix, Wine prefix).
    NotFound,
    /// Not possible for this target (remote server, a folder that is neither a lab nor an instance).
    Unsupported,
    /// A helper or the server did not answer in time.
    Timeout,
}

impl DebugCode {
    pub fn as_str(self) -> &'static str {
        match self {
            DebugCode::Refused => "refused",
            DebugCode::NotRunning => "not_running",
            DebugCode::IdentityChanged => "identity_changed",
            DebugCode::RuntimeNotReady => "runtime_not_ready",
            DebugCode::ToolMissing => "tool_missing",
            DebugCode::PrivilegeMismatch => "privilege_mismatch",
            DebugCode::AmbiguousRunner => "ambiguous_runner",
            DebugCode::UnsupportedIdentity => "unsupported_identity",
            DebugCode::PauseConsentRequired => "pause_consent_required",
            DebugCode::NotFound => "not_found",
            DebugCode::Unsupported => "unsupported",
            DebugCode::Timeout => "timeout",
        }
    }
}

impl fmt::Display for DebugCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// `{code, message, reason?}`. The CLI prints it as `{"ok":false,"error":{"code","message","detail":{"reason"}}}`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DebugError {
    pub code: DebugCode,
    /// One sentence for the user.
    pub message: String,
    /// What differed or why, as a short stable token; None when the code says it all.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

impl DebugError {
    pub fn new(code: DebugCode, message: impl Into<String>) -> Self {
        DebugError { code, message: message.into(), reason: None }
    }

    pub fn because(mut self, reason: &str) -> Self {
        self.reason = Some(reason.to_string());
        self
    }

    /// The `detail` object of the CLI's error envelope: `{"reason": ...}`, None without a reason.
    pub fn detail(&self) -> Option<serde_json::Value> {
        self.reason.as_ref().map(|r| serde_json::json!({ "reason": r }))
    }

    pub fn refused(message: impl Into<String>, reason: &str) -> Self {
        DebugError::new(DebugCode::Refused, message).because(reason)
    }

    pub fn identity_changed(message: impl Into<String>, reason: &str) -> Self {
        DebugError::new(DebugCode::IdentityChanged, message).because(reason)
    }
}

impl fmt::Display for DebugError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for DebugError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_are_snake_case_and_stable() {
        for (c, s) in [
            (DebugCode::Refused, "refused"),
            (DebugCode::NotRunning, "not_running"),
            (DebugCode::IdentityChanged, "identity_changed"),
            (DebugCode::RuntimeNotReady, "runtime_not_ready"),
            (DebugCode::ToolMissing, "tool_missing"),
            (DebugCode::PrivilegeMismatch, "privilege_mismatch"),
            (DebugCode::AmbiguousRunner, "ambiguous_runner"),
            (DebugCode::UnsupportedIdentity, "unsupported_identity"),
            (DebugCode::PauseConsentRequired, "pause_consent_required"),
        ] {
            assert_eq!(c.as_str(), s);
            assert_eq!(serde_json::to_value(c).unwrap(), serde_json::json!(s));
        }
        let e = DebugError::identity_changed("x", "creation_time");
        assert_eq!(e.detail().unwrap()["reason"], "creation_time");
        assert!(DebugError::new(DebugCode::ToolMissing, "y").detail().is_none());
    }
}
