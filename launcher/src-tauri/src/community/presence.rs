//! Rich Presence's thread (step 3 of the Discord work fills this in).

use kiln_manage::community::presence::{PresenceStatus, PresenceTexts};
use kiln_manage::community::settings::CommunitySettings;

pub fn settings_changed(_s: &CommunitySettings) {}

pub fn status(s: &CommunitySettings) -> PresenceStatus {
    PresenceStatus { on: s.presence, discord_running: false, showing: false }
}

pub fn facts(_mods: Option<u32>, _texts: Option<PresenceTexts>) {}
