//! The public post a support report becomes in `#support` (WIREFRAMES S1), built on the player's PC so the check step
//! shows exactly what goes. From the report it takes the findings and the versions: Kiln, the launcher, the game's
//! build, the operating system and the Proton or Wine build, and how many mods are on. It never takes the mod list:
//! a mod is named only when a finding names it.

use super::{ProblemKind, ReportDraft, SendFrom};
use crate::report::{Level, Report};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// At most this many findings go in the post, as the relay takes them (it cuts what would pass Discord's 2000
/// characters with "and N more in the report").
pub const FINDINGS_MAX: usize = 60;
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
    /// The versions line as the relay writes it, a word and a value each: ("Kiln", "0.2.0-dev.29.18"), ("Launcher", ...),
    /// ("", "Arch Linux"), ("", "GE-Proton10-3"), ("game build", "26404966"), ("", "12 mods on"); shown joined with " · ".
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

/// The system's `#support` tags, as the relay picks them: a Steam Deck is "Steam Deck" and "Linux".
pub fn os_tags(r: &Report) -> Vec<String> {
    let t: &[&str] = if r.facts.deck {
        &["Steam Deck", "Linux"]
    } else if r.facts.runner.as_deref() == Some("Windows") || r.facts.os.to_ascii_lowercase().contains("windows") {
        &["Windows"]
    } else {
        &["Linux"]
    };
    t.iter().map(|x| x.to_string()).collect()
}

/// The post a report becomes, before the player's words: the findings as the report has them (redacted, at most
/// [`FINDINGS_MAX`]) and the versions. `launcher`: the launcher's version.
pub fn build(r: &Report, launcher: &str) -> PublicPost {
    // the relay's versions line (relay/post.mjs versionsLine): from report.json's `versions`, the same facts
    let v = r.facts.versions();
    let get = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_string);
    let mut versions: Vec<(String, String)> = Vec::new();
    if let Some(k) = get("kiln") {
        versions.push(("Kiln".into(), k));
    }
    versions.push(("Launcher".into(), get("launcher").unwrap_or_else(|| launcher.to_string())));
    for k in ["os", "runner"] {
        if let Some(x) = get(k) {
            versions.push((String::new(), x));
        }
    }
    if let Some(b) = get("gameBuild") {
        versions.push(("game build".into(), b));
    }
    if let Some(n) = r.facts.mods_on {
        versions.push((String::new(), if n == 1 { "1 mod on".into() } else { format!("{n} mods on") }));
    }
    let mut findings: Vec<PostFinding> = r.findings.iter().take(FINDINGS_MAX).map(|x| PostFinding { id: x.id.clone(), level: x.level, text: cut(&x.text, FINDING_TEXT_MAX) }).collect();
    findings.sort_by_key(|f| f.level);
    PublicPost {
        title: String::new(),
        tags: os_tags(r),
        kind: ProblemKind::Other,
        words: String::new(),
        findings,
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

/// The `draft` part of `POST /reports` as the relay takes it (docs/relay-api.md: exactly `title`, `kind`, `when?`,
/// `text?`, `zip`, `known?`). The relay builds the post from report.json (the zip's, or report.json alone when the
/// player chose that nobody gets the zip: [`crate::report::public_json`]) the same way [`build`] did for the check step.
pub fn wire(d: &ReportDraft, _from: SendFrom, known: Option<&str>) -> Value {
    let mut v = json!({ "title": d.title, "kind": d.kind, "zip": d.zip });
    if !d.text.is_empty() {
        v["text"] = json!(d.text);
    }
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

    fn line(p: &PublicPost) -> String {
        p.versions.iter().map(|(k, v)| if k.is_empty() { v.clone() } else { format!("{k} {v}") }).collect::<Vec<_>>().join(" · ")
    }

    #[test]
    fn the_versions_line_is_the_relays() {
        // relay/post.mjs versionsLine: "Kiln x · Launcher y · <os> · <runner> · game build z · N mods on"
        let p = build(&report(), "0.2.0-dev.29.18");
        assert_eq!(line(&p), "Kiln 0.2.0-dev.29.18 · Launcher 0.2.0-dev.29.18 · Arch Linux · GE-Proton10-3 · game build 26404966 · 12 mods on");
        assert_eq!(p.tags, ["Linux"]);
        assert_eq!(p.findings.len(), 2);
        assert_eq!(p.findings[0].text, report().findings[0].text);
        let mut one = report();
        one.facts.mods_on = Some(1);
        assert!(line(&build(&one, "1")).ends_with("1 mod on"));
    }

    #[test]
    fn the_mod_list_never_goes_only_the_mods_a_finding_names() {
        // the report's own summary lists every mod; the post takes the findings and the facts, never the summary
        let mut r = report();
        r.summary = "Profile: My mods\n  kiln  big-stacks  1.2.0  on\n  kiln  secret-base-mod  0.1  on\n".into();
        r.files = vec![("game/kiln/profile.json".into(), b"{\"mods\":[\"secret-base-mod\"]}".to_vec())];
        let post = serde_json::to_string(&build(&r, "1")).unwrap();
        let alone = String::from_utf8(crate::report::public_json(&r)).unwrap();
        for text in [&post, &alone] {
            assert!(!text.contains("big-stacks") && !text.contains("secret-base-mod"), "{text}");
            assert!(text.contains("night-sky"), "a mod a finding names is in it");
        }
        assert!(post.contains("12 mods on"));
    }

    #[test]
    fn the_deck_and_windows_say_so_and_windows_has_no_runner_word() {
        let mut r = report();
        r.facts.deck = true;
        r.facts.os = "SteamOS 3.7".into();
        r.facts.runner = Some("Proton 10.0".into());
        let p = build(&r, "1");
        assert_eq!(p.tags, ["Steam Deck", "Linux"]);
        assert!(line(&p).contains("SteamOS 3.7 · Proton 10.0"));
        r.facts.deck = false;
        r.facts.os = "Windows 11".into();
        r.facts.runner = Some("Windows".into());
        let p = build(&r, "1");
        assert_eq!(p.tags, ["Windows"]);
        r.facts.kiln = None;
        r.facts.mods_on = None;
        let p = build(&r, "1");
        assert!(!line(&p).contains("Kiln ") && !line(&p).contains("mods on"));
    }

    #[test]
    fn findings_are_capped_cut_and_problems_come_first() {
        let mut r = report();
        r.findings = (0..80).map(|i| Finding { id: format!("f{i}"), level: if i == 79 { Level::Problem } else { Level::Info }, text: "x".repeat(400) }).collect();
        let p = build(&r, "1");
        assert_eq!(p.findings.len(), FINDINGS_MAX);
        assert_eq!(p.findings[0].text.chars().count(), FINDING_TEXT_MAX);
    }

    #[test]
    fn the_wire_draft_has_exactly_the_relays_fields() {
        let mut d = draft();
        d.when = Some("load".into());
        let v = wire(&d, SendFrom::Banner, Some("proton-override"));
        let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
        keys.sort();
        assert_eq!(keys, ["kind", "known", "text", "title", "when", "zip"]);
        assert_eq!((v["title"].as_str(), v["kind"].as_str(), v["text"].as_str(), v["zip"].as_str()), (Some("Mods don't load since the game update"), Some("noload"), Some("It worked yesterday."), Some("team")));
        d.text = String::new();
        d.when = None;
        let v = wire(&d, SendFrom::Logs, None);
        assert!(v.get("text").is_none() && v.get("when").is_none() && v.get("known").is_none());
        // the kind's tag first, then the system's
        assert_eq!(build(&report(), "1").tags_for(ProblemKind::Crash), ["Crash", "Linux"]);
        assert_eq!(build(&report(), "1").tags_for(ProblemKind::Other), ["Linux"]);
    }
}
