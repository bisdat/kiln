//! The launcher's optional Discord features (docs/kiln/launcher/discord.md): the Community page's data, the sign-in
//! through frostdev.io, support reports to Kiln support, the player's support posts, known problems, testers and Rich
//! Presence. Everything here is logic without Tauri; launcher/src-tauri/src/commands/community.rs is the glue.
//!
//! The rules every part keeps:
//! - **Off unless asked.** With the page hidden ([`settings::CommunitySettings::shown`] false) nothing here sends a
//!   request. Shown but signed out, the only requests are the public news feed and the known-problems list, and only
//!   when the page is opened. Rich Presence talks to the Discord app on this PC only, and only while it is switched on.
//! - **Nothing leaves without a click** on a screen that shows what goes: a report is prepared first
//!   ([`post::PublicPost`], the zip's file list) and sent as prepared.
//! - **No secret in the launcher.** The only key it holds is the Kiln session key the relay gave it ([`session`]), in
//!   the system's keyring where there is one. Never a Discord token, a bot token, a webhook or a client secret; the
//!   application's client ID ([`CLIENT_ID`]) is public.
//!
//! | module | what |
//! |---|---|
//! | [`settings`] | `community.json`: the page shown or hidden, Rich Presence, reply notifications, the relay's address |
//! | [`relay`] | the relay's endpoints (the contract in docs/kiln/launcher/discord.md), errors as `detail.kind` |
//! | [`session`] | the session key and the account it belongs to: keyring, or an owner-only file |
//! | [`signin`] | the desktop sign-in (a one-time loopback listener) and the Deck's QR pairing, both through the relay |
//! | [`post`] | the public post built from a support report: versions, system, mod count, only the mods a finding names |
//! | [`known`] | the known-problems list: its format and the match against a report's findings |
//! | [`hardware`] | what a tester shares: graphics card, driver, video memory, system, how the game runs; what it can test |
//! | [`testers`] | test plans, the steps for this hardware, results kept until sent |
//! | [`presence`] | Rich Presence over the Discord app's local IPC socket: where it is, the frames, the lines |

pub mod hardware;
pub mod known;
pub mod post;
pub mod presence;
pub mod relay;
pub mod session;
pub mod settings;
pub mod signin;
pub mod testers;
#[cfg(test)]
pub(crate) mod testing;

use serde::{Deserialize, Serialize};

/// The Discord application "Kiln" (public: it names the app for Rich Presence; it is not a secret).
pub const CLIENT_ID: &str = "1570849158431244459";
/// The Kiln Discord's invite.
pub const INVITE: &str = "https://discord.gg/7pjr8SMTfU";
/// Kiln's page (Rich Presence's "Get Kiln").
pub const KILN_PAGE: &str = "https://frostdev.io/kiln";

/// The signed-in player as the relay describes them.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Account {
    /// Discord user id (a snowflake, as a string).
    pub id: String,
    pub name: String,
    /// In the Kiln Discord.
    #[serde(default)]
    pub member: bool,
    #[serde(default)]
    pub tester: bool,
}

impl Account {
    /// Checks what came from the relay before it is kept or shown.
    pub fn validate(&self) -> crate::Result<()> {
        let id_ok = !self.id.is_empty() && self.id.len() <= 24 && self.id.bytes().all(|b| b.is_ascii_digit());
        let name_ok = !self.name.trim().is_empty() && self.name.chars().count() <= 64 && !self.name.chars().any(char::is_control);
        if id_ok && name_ok { Ok(()) } else { Err(crate::Error::new(crate::ErrorCode::Parse, "Kiln support sent an account the launcher cannot use.")) }
    }
}

/// A support post's state on Discord.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PostStatus {
    Waiting,
    Replied,
    Solved,
    Closed,
}

/// Who sees a report's zip.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum ZipAudience {
    /// The staff-only channel (the default).
    #[default]
    Team,
    /// Attached to the public post.
    Public,
    /// Not sent.
    None,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LastReply {
    pub name: String,
    /// A member of the Kiln Team role.
    #[serde(default)]
    pub team: bool,
    pub at: String,
}

/// One of the player's support posts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SupportPost {
    pub id: String,
    pub title: String,
    /// `https://discord.com/channels/<guild>/<thread>`
    pub url: String,
    pub created: String,
    pub status: PostStatus,
    #[serde(default)]
    pub replies: u32,
    /// Replies the player has not seen in the launcher.
    #[serde(default)]
    pub unread: u32,
    #[serde(default)]
    pub last_reply: Option<LastReply>,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub zip: ZipAudience,
}

/// Only Discord links leave the launcher's "Open in Discord".
pub fn is_discord_url(url: &str) -> bool {
    url.strip_prefix("https://discord.com/channels/").is_some_and(|rest| {
        let parts: Vec<&str> = rest.split('/').collect();
        (2..=3).contains(&parts.len()) && parts.iter().all(|p| !p.is_empty() && p.len() <= 24 && p.bytes().all(|b| b.is_ascii_digit()))
    })
}

impl SupportPost {
    pub fn validate(&self) -> crate::Result<()> {
        let short = |s: &str, n: usize| s.chars().count() <= n && !s.chars().any(char::is_control);
        if self.id.is_empty() || !short(&self.id, 64) || !short(&self.title, 120) || !is_discord_url(&self.url) || !short(&self.created, 40)
            || self.tags.len() > 8 || self.tags.iter().any(|t| !short(t, 40))
            || self.last_reply.as_ref().is_some_and(|r| !short(&r.name, 64) || !short(&r.at, 40))
        {
            return Err(crate::Error::new(crate::ErrorCode::Parse, "Kiln support sent a post the launcher cannot show."));
        }
        Ok(())
    }
}

/// The kind of problem a report is about (the send dialog's six choices).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ProblemKind {
    Noload,
    Mod,
    Crash,
    Launcher,
    Server,
    Other,
}

impl ProblemKind {
    /// The `#support` forum tag for the kind (none for "something else").
    pub fn tag(self) -> Option<&'static str> {
        match self {
            ProblemKind::Noload | ProblemKind::Mod => Some("Mods"),
            ProblemKind::Crash => Some("Crash"),
            ProblemKind::Launcher => Some("Launcher"),
            ProblemKind::Server => Some("Server"),
            ProblemKind::Other => None,
        }
    }
}

/// What the player wrote or picked in the send dialog.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportDraft {
    /// One line: the post's title.
    pub title: String,
    pub kind: ProblemKind,
    /// Deck: when it happens (picked).
    #[serde(default)]
    pub when: Option<String>,
    /// More words (optional).
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub zip: ZipAudience,
}

/// The longest title (the post's title; Discord allows 100).
pub const TITLE_MAX: usize = 90;
/// The longest "anything else".
pub const WORDS_MAX: usize = 1500;

impl ReportDraft {
    /// The draft as it may be sent: a title of 1 to [`TITLE_MAX`] characters, words up to [`WORDS_MAX`], no control
    /// characters but line breaks in the words, a `when` from the Deck's list.
    pub fn checked(&self) -> crate::Result<ReportDraft> {
        let title: String = self.title.split_whitespace().collect::<Vec<_>>().join(" ");
        if title.is_empty() {
            return Err(crate::Error::invalid("Write one line first.").with_detail(serde_json::json!({ "kind": "title" })));
        }
        if title.chars().count() > TITLE_MAX || title.chars().any(char::is_control) {
            return Err(crate::Error::invalid("The line is too long.").with_detail(serde_json::json!({ "kind": "title" })));
        }
        let text = self.text.trim().replace("\r\n", "\n");
        if text.chars().count() > WORDS_MAX || text.chars().any(|c| c.is_control() && c != '\n') {
            return Err(crate::Error::invalid("Your words are too long.").with_detail(serde_json::json!({ "kind": "words" })));
        }
        const WHENS: [&str; 6] = ["start", "menu", "load", "play", "join", "other"];
        if self.when.as_deref().is_some_and(|w| !WHENS.contains(&w)) {
            return Err(crate::Error::invalid("Unknown time of the problem."));
        }
        Ok(ReportDraft { title, kind: self.kind, when: self.when.clone(), text, zip: self.zip })
    }
}

/// Where a report was started (the kind it preselects; the relay counts it for nothing but the post's first line).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SendFrom {
    Logs,
    Banner,
    Crash,
    Community,
    Deck,
}

/// News and events for the Community page (public).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct Feed {
    #[serde(default)]
    pub releases: Vec<FeedRelease>,
    #[serde(default)]
    pub events: Vec<FeedEvent>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FeedRelease {
    pub title: String,
    /// RFC 3339.
    pub date: String,
    pub line: String,
    pub url: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FeedEvent {
    pub title: String,
    /// RFC 3339: when it starts.
    pub start: String,
    #[serde(default)]
    pub end: Option<String>,
    /// Where: a channel or a place, in words.
    #[serde(rename = "where", default)]
    pub place: String,
    pub url: String,
}

impl Feed {
    /// Keeps what can be shown: at most three releases and five events, short texts, https links only.
    pub fn cleaned(self) -> Feed {
        let ok = |s: &str, n: usize| !s.trim().is_empty() && s.chars().count() <= n && !s.chars().any(char::is_control);
        let link = |u: &str| u.starts_with("https://") && u.len() <= 300 && !u.chars().any(|c| c.is_whitespace() || c.is_control());
        Feed {
            releases: self.releases.into_iter().filter(|r| ok(&r.title, 80) && ok(&r.date, 40) && r.line.chars().count() <= 200 && link(&r.url)).take(3).collect(),
            events: self.events.into_iter().filter(|e| ok(&e.title, 100) && ok(&e.start, 40) && e.place.chars().count() <= 100 && link(&e.url)).take(5).collect(),
        }
    }
}

/// `base64url` without padding (the sign-in challenge).
pub(crate) fn b64url(bytes: &[u8]) -> String {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::with_capacity(bytes.len() * 4 / 3 + 2);
    for c in bytes.chunks(3) {
        let n = (u32::from(c[0]) << 16) | (u32::from(*c.get(1).unwrap_or(&0)) << 8) | u32::from(*c.get(2).unwrap_or(&0));
        for i in 0..=c.len() {
            out.push(char::from(A[((n >> (18 - 6 * i)) & 63) as usize]));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64url_matches_rfc_4648_vectors() {
        assert_eq!(b64url(b""), "");
        assert_eq!(b64url(b"f"), "Zg");
        assert_eq!(b64url(b"fo"), "Zm8");
        assert_eq!(b64url(b"foo"), "Zm9v");
        assert_eq!(b64url(b"foobar"), "Zm9vYmFy");
        assert_eq!(b64url(&[0xfb, 0xff]), "-_8");
    }

    #[test]
    fn only_discord_channel_links_count_as_post_links() {
        assert!(is_discord_url("https://discord.com/channels/1568454645683195976/1573130884210000003"));
        for bad in ["http://discord.com/channels/1/2", "https://discord.com.evil.io/channels/1/2", "https://discord.com/channels/1",
                    "https://discord.com/channels/1/2/3/4", "https://discord.com/channels/a/2", "https://discord.com/channels//2", "javascript:alert(1)"] {
            assert!(!is_discord_url(bad), "{bad}");
        }
    }

    #[test]
    fn a_draft_needs_one_line_and_keeps_to_its_limits() {
        let d = |title: &str, text: &str| ReportDraft { title: title.into(), kind: ProblemKind::Crash, when: None, text: text.into(), zip: ZipAudience::Team };
        assert_eq!(d("  ", "").checked().unwrap_err().detail.unwrap()["kind"], "title");
        assert_eq!(d("  The game\tcrashes  ", "").checked().unwrap().title, "The game crashes");
        assert!(d(&"x".repeat(TITLE_MAX + 1), "").checked().is_err());
        assert!(d("ok", &"y".repeat(WORDS_MAX + 1)).checked().is_err());
        assert_eq!(d("ok", "line one\r\nline two ").checked().unwrap().text, "line one\nline two");
        assert!(d("ok", "bell\u{7}").checked().is_err());
        let mut w = d("ok", "");
        w.when = Some("dance".into());
        assert!(w.checked().is_err());
        w.when = Some("load".into());
        assert!(w.checked().is_ok());
    }

    #[test]
    fn a_feed_keeps_three_releases_and_https_links() {
        let r = |n: u32, url: &str| FeedRelease { title: format!("Kiln {n}"), date: "2026-11-18T10:00:00Z".into(), line: "x".into(), url: url.into() };
        let f = Feed { releases: vec![r(1, "https://frostdev.io/kiln"), r(2, "javascript:x"), r(3, "https://a.b"), r(4, "https://a.b"), r(5, "https://a.b")], events: vec![] }.cleaned();
        assert_eq!(f.releases.iter().map(|r| r.title.as_str()).collect::<Vec<_>>(), ["Kiln 1", "Kiln 3", "Kiln 4"]);
    }
}
