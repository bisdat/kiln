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
//!
//! An entry matches a report ([`best`]) when every part of its `match` that is given holds: one of its finding ids is
//! among the report's findings (all of them with `all`), one of its words is in a finding's text, the system is one of
//! `os` (a Steam Deck counts as `deck` and `linux`), Kiln's version is in the `kiln` range (the manifests' range
//! syntax), and `until` (a date) has not passed. An entry that names neither findings nor words never matches: the list
//! cannot put a fix in front of every report. The first matching entry in the list's order wins.

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
    /// Words that must appear in a finding's text (case-insensitive); any one is enough. A single string is one word.
    #[serde(default, deserialize_with = "one_or_many")]
    pub text: Vec<String>,
    /// `windows`, `linux`, `deck`; empty: every system.
    #[serde(default)]
    pub os: Vec<String>,
    /// A Kiln version range (`>=0.2.0-dev.29, <0.2.0-dev.30`), or `{min, max}` (both kept: `>=min <=max`); absent:
    /// every version.
    #[serde(default, deserialize_with = "range")]
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
    /// The fix's button: `"mods"`, or `{label, to}` with `to` `play` (the mods on Play), `runtime` or `logs`.
    #[serde(default, skip_serializing_if = "Option::is_none", deserialize_with = "action")]
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
    /// 1 (absent: 1).
    #[serde(default = "one")]
    pub format: u32,
    #[serde(default)]
    pub issues: Vec<KnownIssue>,
}

fn one() -> u32 {
    1
}

fn one_or_many<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Vec<String>, D::Error> {
    Ok(match serde_json::Value::deserialize(d)? {
        serde_json::Value::String(s) => vec![s],
        serde_json::Value::Array(a) => a.into_iter().filter_map(|v| v.as_str().map(str::to_string)).collect(),
        _ => Vec::new(),
    })
}

fn range<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    Ok(match serde_json::Value::deserialize(d)? {
        serde_json::Value::String(s) => Some(s),
        serde_json::Value::Object(o) => {
            let part = |k: &str, op: &str| o.get(k).and_then(|v| v.as_str()).map(|v| format!("{op}{v}"));
            let r: Vec<String> = [part("min", ">="), part("max", "<=")].into_iter().flatten().collect();
            (!r.is_empty()).then(|| r.join(" "))
        }
        _ => None,
    })
}

fn action<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<KnownAction>, D::Error> {
    let v = serde_json::Value::deserialize(d)?;
    let to = match &v {
        serde_json::Value::String(s) => s.as_str(),
        serde_json::Value::Object(o) => o.get("to").and_then(|t| t.as_str()).unwrap_or(""),
        _ => "",
    };
    Ok(match to {
        "mods" | "play" => Some(KnownAction::Mods),
        "runtime" => Some(KnownAction::Runtime),
        "logs" => Some(KnownAction::Logs),
        _ => None,
    })
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

/// The systems a report's facts count as.
fn systems(f: &crate::report::PublicFacts) -> Vec<&'static str> {
    if f.deck {
        vec!["deck", "linux"]
    } else if f.runner.as_deref() == Some("Windows") || f.os.to_ascii_lowercase().starts_with("windows") {
        vec!["windows"]
    } else {
        vec!["linux"]
    }
}

/// `until` ("2026-12-01" or RFC 3339) has passed at `now` (unix seconds).
fn expired(until: Option<&str>, now: i64) -> bool {
    let Some(u) = until.map(str::trim).filter(|u| !u.is_empty()) else { return false };
    let secs = crate::util::parse_rfc3339(u).or_else(|| crate::util::parse_rfc3339(&format!("{u}T23:59:59Z")));
    secs.is_some_and(|s| s < now)
}

impl KnownIssue {
    /// Whether this entry is about the report.
    pub fn matches(&self, r: &crate::report::Report, now: i64) -> bool {
        let m = &self.matches;
        if m.findings.is_empty() && m.text.is_empty() {
            return false;
        }
        if expired(self.until.as_deref(), now) {
            return false;
        }
        if !m.findings.is_empty() {
            let has = |id: &String| r.findings.iter().any(|f| &f.id == id);
            let ok = if m.all { m.findings.iter().all(has) } else { m.findings.iter().any(has) };
            if !ok {
                return false;
            }
        }
        if !m.text.is_empty() {
            let texts: Vec<String> = r.findings.iter().map(|f| f.text.to_lowercase()).collect();
            if !m.text.iter().any(|w| { let w = w.trim().to_lowercase(); !w.is_empty() && texts.iter().any(|t| t.contains(&w)) }) {
                return false;
            }
        }
        if !m.os.is_empty() {
            let sys = systems(&r.facts);
            if !m.os.iter().any(|o| sys.contains(&o.to_ascii_lowercase().as_str())) {
                return false;
            }
        }
        if let Some(range) = m.kiln.as_deref().map(str::trim).filter(|x| !x.is_empty()) {
            let (Ok(range), Some(v)) = (crate::semver::VersionRange::try_parse(range), r.facts.kiln.as_deref().and_then(crate::semver::SemVersion::try_parse)) else { return false };
            if !range.satisfies(&v) {
                return false;
            }
        }
        true
    }
}

/// The known problem the report is about, if any: the first matching entry.
pub fn best<'a>(list: &'a KnownList, r: &crate::report::Report, now: i64) -> Option<&'a KnownIssue> {
    list.issues.iter().find(|i| i.matches(r, now))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::report::{Finding, Level, PublicFacts, Report};

    const NOW: i64 = 1_763_480_000; // 2025-11-18

    fn report(ids: &[(&str, &str)], deck: bool) -> Report {
        Report {
            findings: ids.iter().map(|(id, text)| Finding { id: id.to_string(), level: Level::Problem, text: text.to_string() }).collect(),
            facts: PublicFacts { kiln: Some("0.2.0-dev.29.18".into()), os: if deck { "SteamOS 3.7".into() } else { "Arch Linux".into() }, deck, runner: Some("GE-Proton10-3".into()), ..Default::default() },
            ..Default::default()
        }
    }

    fn list(json: &str) -> KnownList {
        KnownList::parse(json.as_bytes()).unwrap()
    }

    const LIST: &str = r#"{ "format": 1, "issues": [
        { "id": "night-sky-weather-plus", "title": "Night Sky and Weather Plus crash together", "fix": "Turn one of them off until Night Sky 0.4 is out.", "action": "mods",
          "match": { "findings": ["mod-failed", "crash"], "all": true, "text": ["night-sky"] } },
        { "id": "proton-override", "title": "Steam's launch option is missing", "fix": "Copy the line into Steam's launch options.", "copy": "WINEDLLOVERRIDES=\"dinput8=n,b\" %command%",
          "match": { "findings": ["proton-override", "not-loaded"], "os": ["linux"] } },
        { "id": "old-runtime", "title": "This Kiln is too old", "fix": "Update Kiln.", "match": { "findings": ["newer-mods"], "kiln": "<0.2.0-dev.29" } },
        { "id": "gone", "title": "Fixed long ago", "fix": "x", "until": "2025-01-01", "match": { "findings": ["crash"] } },
        { "id": "everything", "title": "Catch-all", "fix": "x", "match": { "os": ["linux"] } }
    ] }"#;

    #[test]
    fn a_report_meets_the_first_entry_whose_findings_and_words_it_has() {
        let l = list(LIST);
        let r = report(&[("mod-failed", "night-sky 0.3.1 failed at the last start"), ("crash", "Kiln wrote a crash report")], false);
        assert_eq!(best(&l, &r, NOW).unwrap().id, "night-sky-weather-plus");
        // all: one of the two findings is not enough
        let r = report(&[("mod-failed", "night-sky failed")], false);
        assert!(best(&l, &r, NOW).is_none());
        // the word must be in a finding's text
        let r = report(&[("mod-failed", "big-stacks failed"), ("crash", "x")], false);
        assert!(best(&l, &r, NOW).is_none());
    }

    #[test]
    fn the_system_counts_and_a_deck_is_linux_too() {
        let l = list(LIST);
        assert_eq!(best(&l, &report(&[("not-loaded", "Kiln did not start")], false), NOW).unwrap().id, "proton-override");
        assert_eq!(best(&l, &report(&[("not-loaded", "Kiln did not start")], true), NOW).unwrap().id, "proton-override");
        let mut w = report(&[("not-loaded", "Kiln did not start")], false);
        w.facts.runner = Some("Windows".into());
        w.facts.os = "Windows 11".into();
        assert!(best(&l, &w, NOW).is_none(), "the Proton fix is not shown on Windows");
    }

    #[test]
    fn a_kiln_range_a_passed_date_and_an_entry_without_findings_never_match_wrongly() {
        let l = list(LIST);
        assert!(best(&l, &report(&[("newer-mods", "x")], false), NOW).is_none(), "0.2.0-dev.29.18 is not below 0.2.0-dev.29");
        let mut old = report(&[("newer-mods", "x")], false);
        old.facts.kiln = Some("0.2.0-dev.27".into());
        assert_eq!(best(&l, &old, NOW).unwrap().id, "old-runtime");
        // "gone" is past its date; "everything" names no finding
        assert!(best(&l, &report(&[("crash", "x")], false), NOW).is_none());
        assert!(l.issues.iter().find(|i| i.id == "gone").unwrap().matches(&report(&[("crash", "x")], false), 1_700_000_000), "before its date it did");
    }

    #[test]
    fn the_relays_shapes_read_too_a_single_word_a_min_max_range_an_action_object() {
        let l = list(r#"{ "issues": [
            { "id": "night", "title": "Night Sky", "fix": "Turn it off.", "action": { "label": "Show my mods", "to": "play" },
              "match": { "findings": ["mod-failed"], "text": "night-sky", "kiln": { "min": "0.2.0-dev.29" } } }
        ], "updated": "2026-11-18T12:00:00Z" }"#);
        let i = &l.issues[0];
        assert_eq!((l.format, i.action, i.matches.text.clone(), i.matches.kiln.clone()), (1, Some(KnownAction::Mods), vec!["night-sky".to_string()], Some(">=0.2.0-dev.29".to_string())));
        assert!(i.matches(&report(&[("mod-failed", "night-sky failed")], false), NOW));
    }

    #[test]
    fn the_list_keeps_good_entries_and_drops_broken_ones() {
        let l = list(r#"{ "format": 1, "issues": [
            { "id": "ok", "title": "Fine", "fix": "Do this.", "match": { "findings": ["crash"] } },
            { "id": "bad id!", "title": "x", "fix": "y", "match": { "findings": ["crash"] } },
            { "id": "notitle", "title": "", "fix": "y", "match": { "findings": ["crash"] } }
        ] }"#);
        assert_eq!(l.issues.iter().map(|i| i.id.as_str()).collect::<Vec<_>>(), ["ok"]);
        assert!(KnownList::parse(br#"{ "format": 2, "issues": [] }"#).is_err());
        assert!(KnownList::parse(b"not json").is_err());
    }
}
