//! The posts' refresh every 5 minutes and the reply notification (step 6 of the Discord work fills this in).

use kiln_manage::community::SupportPost;

/// Posts with replies the player has not seen and that were not unread before.
pub fn new_replies(before: &[SupportPost], after: &[SupportPost]) -> Vec<SupportPost> {
    after.iter().filter(|p| p.unread > before.iter().find(|b| b.id == p.id).map(|b| b.unread).unwrap_or(0)).cloned().collect()
}

pub fn notify(_app: &tauri::AppHandle, _p: &SupportPost) {}

pub fn wake() {}
