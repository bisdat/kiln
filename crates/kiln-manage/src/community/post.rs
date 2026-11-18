//! The public post a support report becomes in `#support` (WIREFRAMES S1), built on the player's PC so the check step
//! shows exactly what goes. From the report it takes the findings and the versions: Kiln, the launcher, the game's
//! build, the operating system and the Proton or Wine build, and how many mods are on. It never takes the mod list:
//! a mod is named only when a finding names it.

use super::{ProblemKind, ReportDraft, SendFrom};
use crate::report::{Level, Report};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// At most this many findings go in the post (the relay cuts what would pass Discord's 2000 characters).
pub const FINDINGS_MAX: usize = 12;
/// A finding's text is cut to this many characters.
pub const FINDING_TEXT_MAX: usize = 300;

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

fn cut(s: &str, max: usize) -> String {
    if s.chars().count() <= max { s.to_string() } else { s.chars().take(max - 1).collect::<String>() + "…" }
}

/// The system's `#support` tag: Steam Deck, Windows or Linux.
pub fn os_tag(r: &Report) -> &'static str {
    if r.facts.deck {
        "Steam Deck"
    } else if r.facts.runner.as_deref() == Some("Windows") || r.facts.os.to_ascii_lowercase().starts_with("windows") {
        "Windows"
    } else {
        "Linux"
    }
}

/// The post a report becomes, before the player's words: the findings as the report has them (redacted, at most
/// [`FINDINGS_MAX`]) and the versions. `launcher`: the launcher's version.
pub fn build(r: &Report, launcher: &str) -> PublicPost {
    let f = &r.facts;
    let mut versions: Vec<(String, String)> = vec![
        ("Kiln".into(), f.kiln.clone().unwrap_or_else(|| "not installed".into())),
        ("Launcher".into(), launcher.to_string()),
    ];
    let system = if f.deck && !f.os.to_ascii_lowercase().contains("deck") { format!("{} (Steam Deck)", f.os) } else { f.os.clone() };
    if !system.trim().is_empty() {
        versions.push(("System".into(), system));
    }
    if let Some(run) = f.runner.as_ref().filter(|r| r.as_str() != "Windows") {
        versions.push(("Runs on".into(), run.clone()));
    }
    if let Some(b) = &f.game_build {
        versions.push(("Game build".into(), b.clone()));
    }
    if let Some(n) = f.mods_on {
        versions.push(("Mods".into(), format!("{n} on")));
    }
    PublicPost {
        title: String::new(),
        tags: vec![os_tag(r).to_string()],
        kind: ProblemKind::Other,
        words: String::new(),
        findings: r.findings.iter().take(FINDINGS_MAX).map(|x| PostFinding { id: x.id.clone(), level: x.level, text: cut(&x.text, FINDING_TEXT_MAX) }).collect(),
        versions,
    }
}

impl PublicPost {
    /// The tags with the draft's kind first (at most five; the forum allows that many on a post).
    pub fn tags_for(&self, kind: ProblemKind) -> Vec<String> {
        let mut t: Vec<String> = kind.tag().map(str::to_string).into_iter().collect();
        for x in &self.tags {
            if !t.contains(x) {
                t.push(x.clone());
            }
        }
        t.truncate(5);
        t
    }
}

/// The `draft` part of `POST /reports` (docs/kiln/launcher/discord.md): the player's draft and the post exactly as
/// the check step showed it.
pub fn wire(post: &PublicPost, d: &ReportDraft, from: SendFrom, known: Option<&str>) -> Value {
    let p = PublicPost { tags: post.tags_for(d.kind), ..post.clone() }.with_draft(d);
    let mut v = json!({ "title": d.title, "kind": d.kind, "words": d.text, "zip": d.zip, "from": from, "post": p });
    if let Some(w) = &d.when {
        v["when"] = json!(w);
    }
    if let Some(k) = known {
        v["known"] = json!(k);
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::report::{Finding, PublicFacts};

    fn report() -> Report {
        Report {
            findings: vec![
                Finding { id: "mod-failed".into(), level: Level::Problem, text: "1 mod or DLL failed at the last start: night-sky (incompatible with weather-plus).".into() },
                Finding { id: "crash".into(), level: Level::Warning, text: "Kiln wrote a crash report on 2026-11-18T22:10:04Z: kiln/crash-reports/x.txt (1 in all; the newest is in this report).".into() },
            ],
            facts: PublicFacts { kiln: Some("0.2.0-dev.29.18".into()), tool: "kiln-launcher 0.2.0-dev.29.18".into(), os: "Arch Linux".into(), deck: false,
                runner: Some("GE-Proton10-3".into()), game_build: Some("26404966".into()), mods_on: Some(12) },
            ..Default::default()
        }
    }

    fn draft() -> ReportDraft {
        ReportDraft { title: "Mods don't load since the game update".into(), kind: ProblemKind::Noload, when: None, text: "It worked yesterday.".into(), zip: super::super::ZipAudience::Team }
    }

    #[test]
    fn the_post_names_versions_the_system_and_how_many_mods_are_on() {
        let p = build(&report(), "0.2.0-dev.29.18");
        let v: Vec<(&str, &str)> = p.versions.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        assert_eq!(v, [("Kiln", "0.2.0-dev.29.18"), ("Launcher", "0.2.0-dev.29.18"), ("System", "Arch Linux"), ("Runs on", "GE-Proton10-3"), ("Game build", "26404966"), ("Mods", "12 on")]);
        assert_eq!(p.tags, ["Linux"]);
        assert_eq!(p.findings.len(), 2);
        assert_eq!(p.findings[0].text, report().findings[0].text);
    }

    #[test]
    fn the_mod_list_never_goes_only_the_mods_a_finding_names() {
        // the report's own summary lists every mod; the post takes the findings and the facts, never the summary
        let mut r = report();
        r.summary = "Profile: My mods\n  kiln  big-stacks  1.2.0  on\n  kiln  secret-base-mod  0.1  on\n".into();
        r.files = vec![("game/kiln/profile.json".into(), b"{\"mods\":[\"secret-base-mod\"]}".to_vec())];
        let text = serde_json::to_string(&wire(&build(&r, "1"), &draft(), SendFrom::Logs, None)).unwrap();
        assert!(!text.contains("big-stacks") && !text.contains("secret-base-mod"), "{text}");
        assert!(text.contains("night-sky"), "a mod a finding names is in the post");
        assert!(text.contains("\"12 on\""));
    }

    #[test]
    fn the_deck_and_windows_say_so_and_windows_has_no_runs_on_line() {
        let mut r = report();
        r.facts.deck = true;
        r.facts.os = "SteamOS 3.7".into();
        r.facts.runner = Some("Proton 10.0".into());
        let p = build(&r, "1");
        assert_eq!(p.tags, ["Steam Deck"]);
        assert!(p.versions.contains(&("System".into(), "SteamOS 3.7 (Steam Deck)".into())));
        r.facts.deck = false;
        r.facts.os = "Windows 11".into();
        r.facts.runner = Some("Windows".into());
        let p = build(&r, "1");
        assert_eq!(p.tags, ["Windows"]);
        assert!(!p.versions.iter().any(|(k, _)| k == "Runs on"));
        r.facts.kiln = None;
        r.facts.mods_on = None;
        let p = build(&r, "1");
        assert!(p.versions.contains(&("Kiln".into(), "not installed".into())) && !p.versions.iter().any(|(k, _)| k == "Mods"));
    }

    #[test]
    fn findings_are_capped_and_cut() {
        let mut r = report();
        r.findings = (0..20).map(|i| Finding { id: format!("f{i}"), level: Level::Info, text: "x".repeat(400) }).collect();
        let p = build(&r, "1");
        assert_eq!(p.findings.len(), FINDINGS_MAX);
        assert_eq!(p.findings[0].text.chars().count(), FINDING_TEXT_MAX);
    }

    #[test]
    fn the_wire_draft_is_the_post_as_shown_with_the_kinds_tag_first() {
        let mut d = draft();
        d.when = Some("load".into());
        let v = wire(&build(&report(), "0.2.0"), &d, SendFrom::Banner, Some("proton-override"));
        assert_eq!(v["title"], "Mods don't load since the game update");
        assert_eq!(v["kind"], "noload");
        assert_eq!(v["when"], "load");
        assert_eq!(v["zip"], "team");
        assert_eq!(v["from"], "banner");
        assert_eq!(v["known"], "proton-override");
        assert_eq!(v["post"]["title"], v["title"]);
        assert_eq!(v["post"]["words"], "It worked yesterday.");
        assert_eq!(v["post"]["tags"], json!(["Mods", "Linux"]));
        assert_eq!(v["post"]["versions"][5], json!(["Mods", "12 on"]));
        // "something else" has no kind tag
        d.kind = ProblemKind::Other;
        assert_eq!(wire(&build(&report(), "0.2.0"), &d, SendFrom::Logs, None)["post"]["tags"], json!(["Linux"]));
    }
}
