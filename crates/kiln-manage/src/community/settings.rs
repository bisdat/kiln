//! `community.json` in the launcher's data folder: the player's choices for the Discord features, kept apart from
//! `settings.json` so the features stay one file to read and to remove.
//!
//! ```json
//! { "format": 1, "shown": true, "presence": false, "presencePack": false, "notify": false }
//! ```
//! - `shown` (default true): "Show the Community page" in Settings. Off hides the page, the Deck tab, the reply count
//!   and every "Send to Kiln support", and stops every Discord-related request ([`CommunitySettings::network_allowed`]).
//! - `presence` (default false): Rich Presence ("Show Kiln on my Discord profile").
//! - `presencePack` (default false): "Show my Crucible pack on my Discord profile": with Rich Presence on, the pack's name
//!   and a "Get this modpack" button while a Crucible pack's instance is played (community::presence).
//! - `notify` (default false): "Tell me when Kiln support replies".
//! - `relay` (absent: [`DEFAULT_RELAY`]): the relay's address. Only https on frostdev.io or one of its subdomains is
//!   used ([`relay_base`]): a staging relay can be tried, the session key never goes to another site.

use crate::paths::DataDirs;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// The relay's default address.
pub const DEFAULT_RELAY: &str = "https://frostdev.io/kiln/relay";
/// The site the relay may be on (and its subdomains).
pub const RELAY_SITE: &str = "frostdev.io";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CommunitySettings {
    #[serde(default = "one")]
    pub format: u32,
    #[serde(default = "yes")]
    pub shown: bool,
    #[serde(default)]
    pub presence: bool,
    #[serde(default)]
    pub presence_pack: bool,
    #[serde(default)]
    pub notify: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub relay: Option<String>,
}

fn one() -> u32 {
    1
}
fn yes() -> bool {
    true
}

impl Default for CommunitySettings {
    fn default() -> Self {
        CommunitySettings { format: 1, shown: true, presence: false, presence_pack: false, notify: false, relay: None }
    }
}

/// A change from the Settings page or the Community page: only the named fields change.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SettingsPatch {
    #[serde(default)]
    pub shown: Option<bool>,
    #[serde(default)]
    pub presence: Option<bool>,
    #[serde(default)]
    pub presence_pack: Option<bool>,
    #[serde(default)]
    pub notify: Option<bool>,
}

pub fn path(dirs: &DataDirs) -> PathBuf {
    dirs.root.join("community.json")
}

impl CommunitySettings {
    /// The file, or the defaults when there is none. A broken file reads as the defaults with the page shown and every
    /// switch off, so nothing is sent because of a file the player can't see.
    pub fn load(dirs: &DataDirs) -> CommunitySettings {
        match crate::util::read_json::<CommunitySettings>(&path(dirs)) {
            Ok(Some(s)) => s,
            _ => CommunitySettings::default(),
        }
    }

    pub fn save(&self, dirs: &DataDirs) -> Result<()> {
        std::fs::create_dir_all(&dirs.root).map_err(crate::util::io_at(&dirs.root, "creating"))?;
        crate::util::write_json_atomic(&path(dirs), self)
    }

    /// Applies a change and writes the file.
    pub fn update(dirs: &DataDirs, patch: &SettingsPatch) -> Result<CommunitySettings> {
        let mut s = CommunitySettings::load(dirs);
        if let Some(v) = patch.shown {
            s.shown = v;
        }
        if let Some(v) = patch.presence {
            s.presence = v;
        }
        if let Some(v) = patch.presence_pack {
            s.presence_pack = v;
        }
        if let Some(v) = patch.notify {
            s.notify = v;
        }
        s.save(dirs)?;
        Ok(s)
    }

    /// Whether any Discord-related request may be made: only while the page is shown.
    pub fn network_allowed(&self) -> Result<()> {
        if self.shown {
            Ok(())
        } else {
            Err(hidden())
        }
    }

    /// The relay's base address to use.
    pub fn relay(&self) -> RelayBase {
        self.relay.as_deref().and_then(|r| relay_base(r).ok()).unwrap_or_else(|| relay_base(DEFAULT_RELAY).expect("the default relay address is valid"))
    }
}

/// The refusal while the Community page is hidden.
pub fn hidden() -> Error {
    Error::new(ErrorCode::Refused, "The Community page is turned off in Settings.").with_detail(serde_json::json!({ "kind": "hidden" }))
}

/// A checked relay address: https, frostdev.io or a subdomain of it, port 443, a path without a query.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelayBase {
    pub host: String,
    /// `/kiln/relay`: no trailing slash; empty for the site's root.
    pub path: String,
}

impl RelayBase {
    /// `https://host/path` + `rest` (which starts with `/`).
    pub fn url(&self, rest: &str) -> String {
        format!("https://{}{}{}", self.host, self.path, rest)
    }
}

pub fn relay_base(url: &str) -> Result<RelayBase> {
    let u = crate::net::policy::parse_url(url)?;
    if u.host != RELAY_SITE && !u.host.ends_with(&format!(".{RELAY_SITE}")) {
        return Err(Error::new(ErrorCode::Refused, format!("The relay must be on {RELAY_SITE}.")));
    }
    if u.path_and_query.contains('?') {
        return Err(Error::new(ErrorCode::Refused, "The relay's address may not carry a query."));
    }
    let path = u.path_and_query.trim_end_matches('/').to_string();
    if path.split('/').any(|seg| seg == ".." || seg == ".") {
        return Err(Error::new(ErrorCode::Refused, "The relay's address has a path that is not allowed."));
    }
    Ok(RelayBase { host: u.host, path })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> crate::library::TempDir {
        crate::community::testing::tmp(&format!("settings-{tag}"))
    }

    #[test]
    fn defaults_show_the_page_with_presence_and_notifications_off() {
        let t = tmp("defaults");
        let s = CommunitySettings::load(&DataDirs::at(t.path()));
        assert!(s.shown && !s.presence && !s.notify);
        assert!(!s.presence_pack, "Show my Crucible pack is off until the player turns it on");
        assert_eq!(s.relay().url("/me"), "https://frostdev.io/kiln/relay/me");
        assert!(s.network_allowed().is_ok());
    }

    #[test]
    fn hidden_refuses_every_request_with_its_kind() {
        let s = CommunitySettings { shown: false, ..Default::default() };
        let e = s.network_allowed().unwrap_err();
        assert_eq!(e.code, ErrorCode::Refused);
        assert_eq!(e.detail.unwrap()["kind"], "hidden");
    }

    #[test]
    fn a_patch_changes_only_what_it_names_and_is_kept() {
        let t = tmp("patch");
        let d = DataDirs::at(t.path());
        CommunitySettings::update(&d, &SettingsPatch { presence: Some(true), ..Default::default() }).unwrap();
        let s = CommunitySettings::update(&d, &SettingsPatch { shown: Some(false), ..Default::default() }).unwrap();
        assert!(!s.shown && s.presence && !s.notify);
        assert_eq!(CommunitySettings::load(&d), s);
        let s = CommunitySettings::update(&d, &SettingsPatch { presence_pack: Some(true), ..Default::default() }).unwrap();
        assert!(s.presence_pack && s.presence && !s.shown);
        let raw: serde_json::Value = serde_json::from_slice(&std::fs::read(path(&d)).unwrap()).unwrap();
        assert_eq!(raw["presencePack"], true);
        // a file from before the switch existed reads it as off
        std::fs::write(path(&d), br#"{ "format": 1, "shown": true, "presence": true, "notify": false }"#).unwrap();
        assert!(!CommunitySettings::load(&d).presence_pack);
    }

    #[test]
    fn a_broken_file_reads_as_the_defaults() {
        let t = tmp("broken");
        let d = DataDirs::at(t.path());
        std::fs::write(path(&d), b"{ not json").unwrap();
        assert_eq!(CommunitySettings::load(&d), CommunitySettings::default());
    }

    #[test]
    fn the_relay_must_be_https_on_frostdev_io() {
        assert_eq!(relay_base("https://staging.frostdev.io/relay/").unwrap(), RelayBase { host: "staging.frostdev.io".into(), path: "/relay".into() });
        for bad in ["http://frostdev.io/kiln/relay", "https://frostdev.io.evil.com/x", "https://evilfrostdev.io/x", "https://frostdev.io:8443/x",
                    "https://127.0.0.1/x", "https://frostdev.io/x?y=1", "https://frostdev.io/a/../b"] {
            assert!(relay_base(bad).is_err(), "{bad}");
        }
        // a bad address in the file falls back to the default
        let s = CommunitySettings { relay: Some("https://example.com/relay".into()), ..Default::default() };
        assert_eq!(s.relay().host, "frostdev.io");
    }
}
