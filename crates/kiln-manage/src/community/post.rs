//! The public post a support report becomes in `#support` (WIREFRAMES S1), built on the player's PC so the check step
//! shows exactly what goes. From the report it takes the findings and the versions: Kiln, the launcher, the game's
//! build, the operating system and the Proton or Wine build, and how many mods are on. It never takes the mod list:
//! a mod is named only when a finding names it.

use super::{ProblemKind, ReportDraft};
use crate::report::Level;
use serde::{Deserialize, Serialize};

/// One finding as the post shows it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PostFinding {
    pub id: String,
    pub level: Level,
    pub text: String,
}

/// The post as the check step draws it and as the relay posts it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublicPost {
    pub title: String,
    /// `#support` tags: the kind's, then the system's (at most five).
    pub tags: Vec<String>,
    pub kind: ProblemKind,
    /// The player's words (may be empty).
    pub words: String,
    pub findings: Vec<PostFinding>,
    /// ("Kiln", "0.2.0-dev.29.18"), ("Launcher", ...), ("System", ...), ("Runs on", ...), ("Game build", ...), ("Mods", "12 on").
    pub versions: Vec<(String, String)>,
}

impl PublicPost {
    /// The draft's words go in as they are (the relay quotes them, with every mention off).
    pub fn with_draft(mut self, d: &ReportDraft) -> PublicPost {
        self.title = d.title.clone();
        self.kind = d.kind;
        self.words = d.text.clone();
        self
    }
}
