//! Where launcher updates come from and which ones count: the two sources in order (frostdev.io, then GitHub
//! Releases), the channels (stable, dev, beta), the version rule, and the Kiln fields of an update manifest.
//!
//! A manifest (`latest.json`) is Tauri's static updater format plus optional fields the updater ignores:
//! `platforms.<target>.{sha256, size}` and a `kiln` object (`format`, `channel`, `page`, `runtime`). Formats and
//! the release layout: docs/kiln/launcher/releasing.md.

use kiln_manage::semver::SemVersion;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;

/// The page that offers every build, for installs the launcher cannot update itself.
pub const DOWNLOAD_PAGE: &str = "https://frostdev.io/kiln";

const FROSTDEV: &str = "https://frostdev.io/kiln/updates/{channel}/latest.json";
/// GitHub's "latest" release is the newest stable one (pre-releases and drafts are skipped)...
const GITHUB_STABLE: &str = "https://github.com/frostdev-ops/kiln/releases/latest/download/latest.json";
/// ...so the dev channel has a fixed pre-release, `updates-dev`, whose `latest.json` each dev release replaces.
const GITHUB_DEV: &str = "https://github.com/frostdev-ops/kiln/releases/download/updates-dev/latest.json";
/// The beta channel (test builds of the next release, 0.2.0-dev.29 on) has one too, `updates-beta`.
const GITHUB_BETA: &str = "https://github.com/frostdev-ops/kiln/releases/download/updates-beta/latest.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Channel {
    Stable,
    /// The public release line while there is no stable release (every release up to now is `X.Y.Z-dev.N`): the UI
    /// calls it "Release".
    Dev,
    /// (0.2.0-dev.29) Test builds of the next release, for testers: "Beta (test builds)". A launcher before dev.29
    /// does not know the value and follows its build's channel instead (dev).
    Beta,
}

impl Channel {
    pub fn parse(s: &str) -> Option<Channel> {
        match s {
            "stable" => Some(Channel::Stable),
            "dev" => Some(Channel::Dev),
            "beta" => Some(Channel::Beta),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Channel::Stable => "stable",
            Channel::Dev => "dev",
            Channel::Beta => "beta",
        }
    }

    /// The channel a build belongs to: the one baked in when it was built ([`baked_channel`]: a test build's is beta),
    /// else by its version: a pre-release (`0.2.0-dev.14`, a local `0.2.0-dev`) is dev.
    pub fn of_build(version: &str) -> Channel {
        Channel::of_build_with(baked_channel(), version)
    }

    /// [`Channel::of_build`] with the baked-in channel given (tests).
    pub fn of_build_with(baked: Option<Channel>, version: &str) -> Channel {
        baked.unwrap_or_else(|| match SemVersion::parse(version) {
            Ok(v) if !v.is_prerelease() => Channel::Stable,
            _ => Channel::Dev,
        })
    }

    /// The setting (`updateChannel`), else the build's own channel.
    pub fn effective(setting: Option<&str>, version: &str) -> Channel {
        setting.and_then(Channel::parse).unwrap_or_else(|| Channel::of_build(version))
    }
}

/// The channel this build was made for (build.rs: `KILN_LAUNCHER_CHANNEL`), None when the version decides.
pub fn baked_channel() -> Option<Channel> {
    Channel::parse(env!("KILN_LAUNCHER_BUILD_CHANNEL"))
}

/// This test build's beta number (build.rs: `KILN_LAUNCHER_BETA`, set only with the beta channel), else None.
pub fn baked_beta() -> Option<u32> {
    crate::distribution::beta()
}

/// The id of the build at `version` with `beta`: `0.2.0-dev.29+beta.3` for test build 3, else the version as it is.
pub fn build_id(version: &str, beta: Option<u32>) -> String {
    match beta {
        Some(n) => format!("{}+beta.{n}", version.split('+').next().unwrap_or(version)),
        None => version.to_string(),
    }
}

/// This build's id: its version and the baked-in beta number (what the updater compares and what update.json records).
pub fn this_build(version: &str) -> String {
    build_id(version, baked_beta())
}

/// A build: its version and, for a test build, its beta number (docs/kiln/launcher/releasing.md, "Test builds").
/// Written `0.2.0-dev.29+beta.3`: the beta number is build metadata, which semver precedence ignores and the updater's
/// signed-version check compares (so a test build's file cannot pass for another build). Other build metadata is no
/// beta number and decides nothing, as before.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Build {
    pub version: SemVersion,
    pub beta: Option<u32>,
}

impl Build {
    pub fn parse(id: &str) -> Option<Build> {
        let v = SemVersion::parse(id).ok()?;
        let beta = v
            .build
            .strip_prefix("beta.")
            .filter(|n| !n.is_empty() && !n.starts_with('0') && n.len() <= 9 && n.bytes().all(|c| c.is_ascii_digit()))
            .and_then(|n| n.parse().ok());
        Some(Build { version: v, beta })
    }

    /// The update order: the newer version; at the same version a final build is above its test builds, and a test
    /// build with a higher beta number above one with a lower. Build metadata other than a beta number never decides.
    pub fn order(&self, other: &Build) -> Ordering {
        self.version.compare(&other.version).then(match (self.beta, other.beta) {
            (None, None) => Ordering::Equal,
            (None, Some(_)) => Ordering::Greater,
            (Some(_), None) => Ordering::Less,
            (Some(a), Some(b)) => a.cmp(&b),
        })
    }

    /// May `channel` offer this build? Stable: a release without a pre-release part; dev (the release line): no test
    /// build; beta: test builds and releases (a tester is offered the final build).
    pub fn on(&self, channel: Channel) -> bool {
        // Reserved beta metadata with an invalid number is never a release.
        if self.beta.is_none() && (self.version.build == "beta" || self.version.build.starts_with("beta.")) {
            return false;
        }
        match channel {
            Channel::Stable => !self.version.is_prerelease() && self.beta.is_none(),
            Channel::Dev => self.beta.is_none(),
            Channel::Beta => true,
        }
    }
}

/// One place updates come from. `stable`, `dev` and `beta` are the manifest URLs of the channels.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Source {
    pub name: String,
    pub stable: String,
    pub dev: String,
    pub beta: String,
}

impl Source {
    pub fn url(&self, channel: Channel) -> &str {
        match channel {
            Channel::Stable => &self.stable,
            Channel::Dev => &self.dev,
            Channel::Beta => &self.beta,
        }
    }

    /// The source with `template`'s `{channel}` filled in for each channel.
    fn of_template(name: &str, template: &str) -> Source {
        let at = |c: Channel| template.replace("{channel}", c.as_str());
        Source { name: name.into(), stable: at(Channel::Stable), dev: at(Channel::Dev), beta: at(Channel::Beta) }
    }
}

/// The sources in the order they are tried. Release builds always use frostdev.io, then GitHub. Debug builds and
/// builds with the `update-test-feeds` feature take `KILN_UPDATE_FEEDS` instead, when every URL in it is on the
/// loopback interface (tests run local feeds).
pub fn sources() -> Vec<Source> {
    test_sources().unwrap_or_else(release_sources)
}

pub fn release_sources() -> Vec<Source> {
    vec![
        Source::of_template("frostdev.io", FROSTDEV),
        Source { name: "GitHub".into(), stable: GITHUB_STABLE.into(), dev: GITHUB_DEV.into(), beta: GITHUB_BETA.into() },
    ]
}

#[cfg(any(debug_assertions, feature = "update-test-feeds"))]
fn test_sources() -> Option<Vec<Source>> {
    let v = std::env::var("KILN_UPDATE_FEEDS").ok().filter(|v| !v.trim().is_empty())?;
    match parse_test_feeds(&v) {
        Ok(s) => Some(s),
        Err(e) => {
            eprintln!("KILN_UPDATE_FEEDS ignored: {e}");
            None
        }
    }
}

#[cfg(not(any(debug_assertions, feature = "update-test-feeds")))]
fn test_sources() -> Option<Vec<Source>> {
    None
}

/// `name=url;name=url`, each URL with `{channel}` where the channel goes. Only `http(s)://127.0.0.1`, `localhost`
/// or `[::1]` URLs are taken.
pub fn parse_test_feeds(text: &str) -> Result<Vec<Source>, String> {
    let mut out = Vec::new();
    for part in text.split(';').map(str::trim).filter(|p| !p.is_empty()) {
        let (name, url) = part.split_once('=').ok_or_else(|| format!("{part}: expected name=url"))?;
        if !is_loopback_url(url) {
            return Err(format!("{url}: only loopback URLs are allowed"));
        }
        out.push(Source::of_template(name.trim(), url));
    }
    if out.is_empty() {
        return Err("no feeds".into());
    }
    Ok(out)
}

fn is_loopback_url(url: &str) -> bool {
    let Ok(u) = tauri::Url::parse(url) else { return false };
    if !matches!(u.scheme(), "http" | "https") {
        return false;
    }
    let Some(host) = u.host_str() else { return false };
    host.eq_ignore_ascii_case("localhost")
        || host.trim_start_matches('[').trim_end_matches(']').parse::<std::net::IpAddr>().is_ok_and(|ip| ip.is_loopback())
}

/// Is `remote` an update for a launcher at `current` (build ids: [`this_build`]) on `channel`? Newer in [`Build::order`]
/// (semver precedence: `0.2.0-dev.9 < 0.2.0-dev.10 < 0.2.0`; then the beta number: `0.2.0-dev.29+beta.1 <
/// 0.2.0-dev.29+beta.2 < 0.2.0-dev.29`), and a build the channel may offer ([`Build::on`]): stable never a pre-release,
/// the release line never a test build. Never older: a background check or an Update never goes down. Unparsable
/// versions never are.
pub fn accepts(current: &str, remote: &str, channel: Channel) -> bool {
    let (Some(cur), Some(rem)) = (Build::parse(current), Build::parse(remote)) else { return false };
    rem.on(channel) && rem.order(&cur) == Ordering::Greater
}

/// Is `remote` what a channel switch the player started installs, for a launcher at `current` going to `target`?
/// A build `target` may offer ([`Build::on`]), and: to the beta, only a newer one (a test build older than the
/// release one runs is no test build of anything to come); back from a test build to the release line (stable, dev),
/// also an older one. A release build can only move forward. Only the switch ([`super::switch`]) asks this, never a background check or an Update.
pub fn switch_accepts(current: &str, remote: &str, target: Channel) -> bool {
    let (Some(cur), Some(rem)) = (Build::parse(current), Build::parse(remote)) else { return false };
    if !rem.on(target) {
        return false;
    }
    match target {
        Channel::Beta => rem.order(&cur) == Ordering::Greater,
        Channel::Stable | Channel::Dev => rem.order(&cur) == Ordering::Greater || (cur.beta.is_some() && rem.order(&cur) == Ordering::Less),
    }
}

/// The Kiln fields of a manifest (all optional).
#[derive(Debug, Clone, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KilnFields {
    #[serde(default)]
    pub format: Option<u32>,
    #[serde(default)]
    pub channel: Option<String>,
    /// The download page for this release.
    #[serde(default)]
    pub page: Option<String>,
    /// The Kiln runtime the launcher ships.
    #[serde(default)]
    pub runtime: Option<String>,
    /// (dev.29) A test build's beta number (also in the manifest's `version`, `+beta.<n>`, which is what counts).
    #[serde(default)]
    pub beta: Option<u32>,
}

/// `kiln` from a manifest; a newer `format` than 1 is read as far as it goes.
pub fn kiln_fields(raw: &serde_json::Value) -> KilnFields {
    raw.get("kiln").and_then(|k| serde_json::from_value(k.clone()).ok()).unwrap_or_default()
}

/// The sha256 a manifest lists for the platform entry with this download URL.
pub fn sha256_for(raw: &serde_json::Value, url: &str) -> Option<String> {
    raw.get("platforms")?
        .as_object()?
        .values()
        .find(|p| p.get("url").and_then(|u| u.as_str()) == Some(url))?
        .get("sha256")?
        .as_str()
        .map(|s| s.to_ascii_lowercase())
}

/// Release notes for the notice: the first paragraphs, at most `max` characters, cut at a word.
pub fn short_notes(notes: &str, max: usize) -> String {
    let t = notes.trim();
    if t.chars().count() <= max {
        return t.to_string();
    }
    let cut: String = t.chars().take(max).collect();
    let cut = cut.rsplit_once(char::is_whitespace).map(|(a, _)| a).unwrap_or(&cut);
    format!("{}…", cut.trim_end_matches([',', ';', ':', '.', ' ']))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn malformed_beta_ids_are_never_release_candidates() {
        for id in ["0.2.0-dev.29+beta.0", "0.2.0-dev.29+beta.x", "0.2.0-dev.29+beta.03", "0.2.0-dev.29+beta"] {
            for ch in [Channel::Dev, Channel::Stable, Channel::Beta] {
                assert!(!accepts("0.2.0-dev.28", id, ch), "{id} on {ch:?}");
                assert!(!switch_accepts("0.2.0-dev.30+beta.1", id, ch), "{id} switch to {ch:?}");
            }
        }
    }

    #[test]
    fn a_release_cannot_use_switch_to_rollback_another_release() {
        assert!(!switch_accepts("0.2.0-dev.29", "0.2.0-dev.28", Channel::Dev));
        assert!(!switch_accepts("0.2.1", "0.2.0", Channel::Stable));
    }

    #[test]
    fn sources_are_frostdev_then_github() {
        let s = release_sources();
        assert_eq!(s.iter().map(|s| s.name.as_str()).collect::<Vec<_>>(), ["frostdev.io", "GitHub"]);
        assert_eq!(s[0].url(Channel::Stable), "https://frostdev.io/kiln/updates/stable/latest.json");
        assert_eq!(s[0].url(Channel::Dev), "https://frostdev.io/kiln/updates/dev/latest.json");
        assert_eq!(s[1].url(Channel::Stable), "https://github.com/frostdev-ops/kiln/releases/latest/download/latest.json");
        assert_eq!(s[1].url(Channel::Dev), "https://github.com/frostdev-ops/kiln/releases/download/updates-dev/latest.json");
        assert_eq!(s[0].url(Channel::Beta), "https://frostdev.io/kiln/updates/beta/latest.json");
        assert_eq!(s[1].url(Channel::Beta), "https://github.com/frostdev-ops/kiln/releases/download/updates-beta/latest.json");
        // every release URL is https and carries nothing about the user (no query, no version, no id)
        for src in &s {
            for ch in [Channel::Stable, Channel::Dev, Channel::Beta] {
                let u = tauri::Url::parse(src.url(ch)).unwrap();
                assert_eq!(u.scheme(), "https");
                assert!(u.query().is_none());
            }
        }
    }

    #[test]
    fn test_feeds_must_be_loopback() {
        let s = parse_test_feeds("frostdev.io=http://127.0.0.1:26721/u/{channel}/latest.json; GitHub=http://localhost:26722/{channel}.json").unwrap();
        assert_eq!(s[0].name, "frostdev.io");
        assert_eq!(s[0].url(Channel::Dev), "http://127.0.0.1:26721/u/dev/latest.json");
        assert_eq!(s[1].url(Channel::Stable), "http://localhost:26722/stable.json");
        assert_eq!(s[0].url(Channel::Beta), "http://127.0.0.1:26721/u/beta/latest.json");
        assert!(parse_test_feeds("x=http://[::1]:1/{channel}").is_ok());
        assert!(parse_test_feeds("x=https://frostdev.io/kiln/updates/{channel}/latest.json").is_err());
        assert!(parse_test_feeds("x=http://192.168.1.2/{channel}").is_err());
        assert!(parse_test_feeds("x=file:///tmp/{channel}").is_err());
        assert!(parse_test_feeds("nonsense").is_err());
        assert!(parse_test_feeds(" ; ").is_err());
    }

    #[test]
    fn versions_order_by_semver_and_dev_number() {
        // the dev number counts as a number, not text
        assert!(accepts("0.2.0-dev.9", "0.2.0-dev.10", Channel::Dev));
        assert!(!accepts("0.2.0-dev.10", "0.2.0-dev.9", Channel::Dev));
        assert!(!accepts("0.2.0-dev.10", "0.2.0-dev.10", Channel::Dev));
        // a release is newer than its pre-releases; the next dev cycle is newer than the release
        assert!(accepts("0.2.0-dev.40", "0.2.0", Channel::Dev));
        assert!(accepts("0.2.0", "0.2.1-dev.41", Channel::Dev));
        assert!(!accepts("0.2.1-dev.41", "0.2.0", Channel::Dev));
        // a local build (no dev number) takes any dev release of its version
        assert!(accepts("0.2.0-dev", "0.2.0-dev.1", Channel::Dev));
        // build metadata never decides
        assert!(!accepts("0.2.0-dev.5+20261106.abc1234", "0.2.0-dev.5+20261107.def5678", Channel::Dev));
        assert!(!accepts("0.2.0", "garbage", Channel::Dev));
    }

    #[test]
    fn a_suffix_cannot_put_a_test_build_below_its_release() {
        // why the beta number is not in the pre-release part: with the tree's SemVersion both spellings sort above
        // the final 0.2.0-dev.29, and the hyphen even above 0.2.0-dev.30 (text outranks a number)
        let v = |s: &str| SemVersion::parse(s).unwrap();
        assert_eq!(v("0.2.0-dev.29-beta.1").compare(&v("0.2.0-dev.29")), Ordering::Greater);
        assert_eq!(v("0.2.0-dev.29.beta.1").compare(&v("0.2.0-dev.29")), Ordering::Greater);
        assert_eq!(v("0.2.0-dev.29-beta.1").compare(&v("0.2.0-dev.30")), Ordering::Greater);
        // the build metadata form: equal precedence, so the beta number is ours to order
        assert_eq!(v("0.2.0-dev.29+beta.1").compare(&v("0.2.0-dev.29")), Ordering::Equal);
    }

    #[test]
    fn builds_order_release_then_betas_then_final() {
        // release 28 < beta 29.1 < beta 29.2 < beta 29.10 < final 29 < beta 30.1
        let line = ["0.2.0-dev.28", "0.2.0-dev.29+beta.1", "0.2.0-dev.29+beta.2", "0.2.0-dev.29+beta.10", "0.2.0-dev.29", "0.2.0-dev.30+beta.1"];
        for (i, a) in line.iter().enumerate() {
            for (j, b) in line.iter().enumerate() {
                let (a, b) = (Build::parse(a).unwrap(), Build::parse(b).unwrap());
                assert_eq!(a.order(&b), i.cmp(&j), "{a:?} vs {b:?}");
            }
        }
        let b = Build::parse("0.2.0-dev.29+beta.3").unwrap();
        assert_eq!((b.version.to_string().as_str(), b.beta), ("0.2.0-dev.29+beta.3", Some(3)));
        // other build metadata is no beta number
        for id in ["0.2.0-dev.29+20261115.abc1234", "0.2.0-dev.29+beta", "0.2.0-dev.29+beta.0", "0.2.0-dev.29+beta.03", "0.2.0-dev.29+beta.x", "0.2.0-dev.29+beta.1.2"] {
            assert_eq!(Build::parse(id).unwrap().beta, None, "{id}");
        }
        assert!(Build::parse("0.2.0-dev.29-beta.1+").is_none());
    }

    #[test]
    fn the_beta_channel_offers_newer_betas_and_the_final() {
        // release 28 -> beta 29.1 -> beta 29.2 -> final 29
        assert!(accepts("0.2.0-dev.28", "0.2.0-dev.29+beta.1", Channel::Beta));
        assert!(accepts("0.2.0-dev.29+beta.1", "0.2.0-dev.29+beta.2", Channel::Beta));
        assert!(accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29", Channel::Beta));
        // never down, never the same
        assert!(!accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29+beta.1", Channel::Beta));
        assert!(!accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29+beta.2", Channel::Beta));
        assert!(!accepts("0.2.0-dev.29", "0.2.0-dev.29+beta.3", Channel::Beta), "a late test build of a version that is out");
        assert!(!accepts("0.2.0-dev.29+beta.1", "0.2.0-dev.28", Channel::Beta));
        // the next cycle
        assert!(accepts("0.2.0-dev.29", "0.2.0-dev.30+beta.1", Channel::Beta));
    }

    #[test]
    fn the_release_line_never_offers_a_test_build() {
        for (cur, rem) in [("0.2.0-dev.28", "0.2.0-dev.29+beta.1"), ("0.2.0-dev.29+beta.1", "0.2.0-dev.29+beta.2"), ("0.2.0", "0.2.1-dev.1+beta.1")] {
            assert!(!accepts(cur, rem, Channel::Dev), "{cur} -> {rem}");
            assert!(!accepts(cur, rem, Channel::Stable), "{cur} -> {rem}");
        }
        // a test build that follows the release again is offered the final build of its version, and later ones
        assert!(accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29", Channel::Dev));
        assert!(accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.30", Channel::Dev));
        // but never the older release a background check would have to go down to
        assert!(!accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.28", Channel::Dev));
    }

    #[test]
    fn a_switch_goes_down_only_back_to_the_release() {
        // back from a test build: the release line's newest, also when it is older, never a test build
        assert!(switch_accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.28", Channel::Dev));
        assert!(switch_accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29", Channel::Dev));
        assert!(!switch_accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.29+beta.1", Channel::Dev));
        assert!(!switch_accepts("0.2.0-dev.28", "0.2.0-dev.28", Channel::Dev), "nothing to switch to");
        assert!(switch_accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.28", Channel::Stable) == false, "stable takes no pre-release");
        // to the beta: only newer
        assert!(switch_accepts("0.2.0-dev.28", "0.2.0-dev.29+beta.1", Channel::Beta));
        assert!(switch_accepts("0.2.0-dev.29+beta.1", "0.2.0-dev.29+beta.2", Channel::Beta));
        assert!(!switch_accepts("0.2.0-dev.29", "0.2.0-dev.29+beta.3", Channel::Beta), "an older test build is no switch");
        assert!(!switch_accepts("0.2.0-dev.29", "0.2.0-dev.29", Channel::Beta));
        // and what a background check or an Update takes stays "only newer"
        assert!(!accepts("0.2.0-dev.29+beta.2", "0.2.0-dev.28", Channel::Dev));
        assert!(!switch_accepts("garbage", "0.2.0-dev.28", Channel::Dev));
    }

    #[test]
    fn a_test_build_knows_its_channel_from_what_was_baked_in() {
        // a test build carries the release's version: its channel is the baked-in one, not the version's
        assert_eq!(Channel::of_build_with(Some(Channel::Beta), "0.2.0-dev.29"), Channel::Beta);
        assert_eq!(Channel::of_build_with(Some(Channel::Beta), "0.2.0"), Channel::Beta);
        assert_eq!(Channel::of_build_with(None, "0.2.0-dev.29"), Channel::Dev);
        assert_eq!(Channel::of_build_with(Some(Channel::Dev), "0.2.0"), Channel::Dev);
        // this test binary was built without one (build.rs bakes an empty channel and beta number)
        assert_eq!(baked_channel(), None);
        assert_eq!(baked_beta(), None);
        assert_eq!(this_build("0.2.0-dev.29"), "0.2.0-dev.29");
        assert_eq!(build_id("0.2.0-dev.29", Some(3)), "0.2.0-dev.29+beta.3");
        assert_eq!(build_id("0.2.0-dev.29", None), "0.2.0-dev.29");
    }

    #[test]
    fn stable_takes_only_releases() {
        assert!(!accepts("0.2.0", "0.3.0-dev.2", Channel::Stable));
        assert!(accepts("0.2.0", "0.2.1", Channel::Stable));
        assert!(accepts("0.2.0-dev.40", "0.2.0", Channel::Stable));
        assert!(!accepts("0.3.0-dev.2", "0.2.1", Channel::Stable));
    }

    #[test]
    fn channels_parse_and_print_their_stored_values() {
        // the settings' `updateChannel` values: a 0.2.0-dev.28 launcher's "stable" and "dev" keep their meaning
        for (s, c) in [("stable", Channel::Stable), ("dev", Channel::Dev), ("beta", Channel::Beta)] {
            assert_eq!(Channel::parse(s), Some(c));
            assert_eq!(c.as_str(), s);
            assert_eq!(serde_json::to_value(c).unwrap(), s);
            assert_eq!(serde_json::from_value::<Channel>(s.into()).unwrap(), c);
        }
        for s in ["", "Beta", "release", "nightly", " beta"] {
            assert_eq!(Channel::parse(s), None, "{s:?}");
        }
        // a value this build does not know (a later launcher's) falls back to the build's channel, as before
        assert_eq!(Channel::effective(Some("nightly"), "0.2.0-dev.3"), Channel::Dev);
        assert_eq!(Channel::effective(Some("beta"), "0.2.0-dev.3"), Channel::Beta);
    }

    #[test]
    fn channel_defaults_to_the_build() {
        assert_eq!(Channel::of_build("0.2.0"), Channel::Stable);
        assert_eq!(Channel::of_build("0.2.0-dev.3"), Channel::Dev);
        assert_eq!(Channel::of_build("0.2.0-dev"), Channel::Dev);
        assert_eq!(Channel::effective(None, "0.2.0"), Channel::Stable);
        assert_eq!(Channel::effective(None, "0.2.0-dev.3"), Channel::Dev);
        assert_eq!(Channel::effective(Some("stable"), "0.2.0-dev.3"), Channel::Stable);
        assert_eq!(Channel::effective(Some("dev"), "0.2.0"), Channel::Dev);
        assert_eq!(Channel::effective(Some("nightly"), "0.2.0"), Channel::Stable);
    }

    #[test]
    fn manifest_fields() {
        let raw: serde_json::Value = serde_json::from_str(include_str!("testdata/latest.json")).unwrap();
        // the updater itself reads the manifest (its own fields; ours are ignored)
        let rel: tauri_plugin_updater::RemoteRelease = serde_json::from_value(raw.clone()).unwrap();
        assert_eq!(rel.version.to_string(), "0.2.0-dev.42");
        assert!(rel.notes.as_deref().unwrap().starts_with("Self-update"));
        let appimage = "https://frostdev.io/kiln/downloads/0.2.0-dev.42/kiln-launcher_0.2.0-dev.42_amd64.AppImage";
        assert_eq!(rel.download_url("linux-x86_64-appimage").unwrap().as_str(), appimage);
        assert_eq!(rel.download_url("linux-x86_64").unwrap().as_str(), appimage);
        assert!(rel.download_url("windows-x86_64-nsis").unwrap().as_str().ends_with("_x64-setup.exe"));
        let k = kiln_fields(&raw);
        assert_eq!(k.format, Some(1));
        assert_eq!(k.channel.as_deref(), Some("dev"));
        assert_eq!(k.page.as_deref(), Some("https://frostdev.io/kiln"));
        assert_eq!(k.runtime.as_deref(), Some("0.2.0-dev+20261106.0f1ffa6"));
        assert_eq!(sha256_for(&raw, appimage).as_deref(), Some("9f2c6f0a1c3a0b7f0f8c9a6d1e2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d"));
        assert_eq!(sha256_for(&raw, "https://example.invalid/x"), None);
        assert_eq!(k.beta, None);
        // no kiln object: defaults
        assert_eq!(kiln_fields(&serde_json::json!({"version": "1.0.0"})), KilnFields::default());
        // a test build's manifest: the beta number in the version (what counts) and in the kiln object
        let raw: serde_json::Value = serde_json::from_str(include_str!("testdata/beta/latest.json")).unwrap();
        let rel: tauri_plugin_updater::RemoteRelease = serde_json::from_value(raw.clone()).unwrap();
        assert_eq!(rel.version.to_string(), "0.2.0-dev.29+beta.3");
        assert_eq!(Build::parse(&rel.version.to_string()).unwrap().beta, Some(3));
        let k = kiln_fields(&raw);
        assert_eq!((k.channel.as_deref(), k.beta), (Some("beta"), Some(3)));
        assert!(rel.download_url("linux-x86_64").unwrap().as_str().ends_with("/0.2.0-dev.29-beta.3/linux/kiln-launcher_0.2.0-dev.29-beta.3_amd64.AppImage"));
    }

    #[test]
    fn notes_are_cut_at_a_word() {
        assert_eq!(short_notes("  short  ", 40), "short");
        assert_eq!(short_notes("one two three four five", 12), "one two…");
    }
}
