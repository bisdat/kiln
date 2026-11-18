//! Rich Presence (PLAN 3.6, 7.4): the Discord app on this PC shows "Playing Kiln / Enshrouded with Kiln / 12 mods on /
//! elapsed" while the game runs with Kiln and the switch is on. Over the app's local IPC socket only: nothing goes to
//! Frostdev, and nothing at all while the switch is off. Never a server, world, character or mod name.

use serde::{Deserialize, Serialize};

/// The art keys in the application's Rich Presence assets (a key Discord doesn't know shows no picture; see
/// [`super::presence`]'s retry without art).
pub const ART_LARGE: &str = "kiln-hearth";
pub const ART_SMALL: &str = "kiln-mark";

/// What the Community page shows about presence.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PresenceStatus {
    /// The switch.
    pub on: bool,
    /// The Discord app's socket is there (only looked for while the switch is on).
    pub discord_running: bool,
    /// Shown on Discord now (the game runs with Kiln).
    pub showing: bool,
}

/// The lines in the launcher's language (en.json `community.presence.*`); the window sends them with the facts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PresenceTexts {
    /// "Enshrouded with Kiln"
    pub details: String,
    /// "{count} mod on"
    pub state_one: String,
    /// "{count} mods on"
    pub state_other: String,
    /// "Get Kiln"
    pub get_kiln: String,
    /// "Join the Discord"
    pub join_discord: String,
}
