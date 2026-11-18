//! Known problems (PLAN 7.5): a public list the support session keeps in the site repo, served by the relay at
//! `GET /known-issues`. The launcher keeps the last copy for offline use and matches it against a report's findings on
//! the player's PC, so the fix shows before anything is sent (and sending stays possible).
//!
//! ```json
//! { "format": 1, "issues": [ {
//!     "id": "proton-override", "title": "Steam's launch option is missing",
//!     "fix": "Your Proton build needs one line in Enshrouded's launch options before Kiln can load. ...",
//!     "copy": "WINEDLLOVERRIDES=\"dinput8=n,b\" %command%",
//!     "match": { "findings": ["proton-override", "not-loaded"], "all": false, "text": ["dinput8"], "os": ["linux", "deck"], "kiln": ">=0.2.0-dev.29" },
//!     "added": "2026-11-18", "until": null } ] }
//! ```

use serde::{Deserialize, Serialize};

/// The fix's own button: where it takes the player.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum KnownAction {
    /// Play's mod list ("Show my mods").
    Mods,
    /// The Runtime page.
    Runtime,
    /// The Logs page.
    Logs,
}

/// When an entry applies.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Match {
    /// Finding ids (`report::Finding::id`); any one is enough unless `all`.
    #[serde(default)]
    pub findings: Vec<String>,
    #[serde(default)]
    pub all: bool,
    /// Words that must appear in a finding's text (case-insensitive); any one is enough.
    #[serde(default)]
    pub text: Vec<String>,
    /// `windows`, `linux`, `deck`; empty: every system.
    #[serde(default)]
    pub os: Vec<String>,
    /// A Kiln version range (`>=0.2.0-dev.29, <0.2.0-dev.30`); absent: every version.
    #[serde(default)]
    pub kiln: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KnownIssue {
    pub id: String,
    pub title: String,
    pub fix: String,
    /// A line to copy (a launch option).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub copy: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub action: Option<KnownAction>,
    #[serde(rename = "match", default)]
    pub matches: Match,
    /// Also a banner on Play (an idea for later; kept, not shown yet).
    #[serde(default)]
    pub notice: bool,
    #[serde(default)]
    pub added: Option<String>,
    /// After this date the entry no longer applies.
    #[serde(default)]
    pub until: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KnownList {
    pub format: u32,
    #[serde(default)]
    pub issues: Vec<KnownIssue>,
}

impl KnownList {
    /// Parses the list; entries that break the format's limits are left out (one bad entry never hides the rest).
    pub fn parse(bytes: &[u8]) -> crate::Result<KnownList> {
        let mut list: KnownList = serde_json::from_slice(bytes).map_err(|e| crate::Error::new(crate::ErrorCode::Parse, format!("known-issues.json: {e}")))?;
        if list.format != 1 {
            return Err(crate::Error::new(crate::ErrorCode::Parse, "known-issues.json: unknown format"));
        }
        let short = |s: &str, n: usize| !s.trim().is_empty() && s.chars().count() <= n && !s.chars().any(|c| c.is_control() && c != '\n');
        list.issues.retain(|i| {
            super::relay::check_id(&i.id).is_ok() && short(&i.title, 100) && short(&i.fix, 600)
                && i.copy.as_deref().is_none_or(|c| short(c, 200)) && i.matches.findings.len() <= 20 && i.matches.text.len() <= 20
        });
        list.issues.truncate(200);
        Ok(list)
    }
}
