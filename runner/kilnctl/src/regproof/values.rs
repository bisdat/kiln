// SPDX-License-Identifier: MIT
//! Values diff (registry proof step 4, BRIEF 6.1): every registry entry exported through the schema on the old and on
//! the new build, diffed, with each export's base origin. The base is never assumed to be the game's own data: an export
//! that does not say what it read is refused, a base an archive writer published is named in the diff, and two bases of
//! different origin are flagged. The diff is the reviewer's view of what the game changed (and the changelog players
//! ask for); it holds the game's values and ids, so it stays outside the repository.
//!
//! The export (`"kind": "registry-values"`, format 1; written by the lab's proof producer, which is not part of kilnctl):
//! ```jsonc
//! { "format": 1, "kind": "registry-values", "side": "server", "exeSha256": "<sha256>", "gameBuild": "1287369",
//!   "regmapDigest": "<sha256>",                       // the regmap/common it was read through (regmap::digest)
//!   "base": { "origin": "live", "writer": "none" },     // as state/registry.json "base": origin, writer, token
//!   "registries": { "items": { "game:...": { "maxStack": 20, "power": 31 } } } }
//! ```
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

pub const KIND: &str = "registry-values";

/// What an export read: state/registry.json's base (origin live = kiln.assets' bytes at resources-ready; writer none,
/// kiln-eml, shroudforge-hosted or external; the writer's token).
#[derive(Debug, Clone, Default, PartialEq, Deserialize, Serialize)]
pub struct Base {
    #[serde(default)]
    pub origin: String,
    #[serde(default)]
    pub writer: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub token: Option<String>,
}

pub type Entries = BTreeMap<String, BTreeMap<String, Value>>;

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Export {
    #[serde(default)]
    pub format: u32,
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub side: String,
    #[serde(default)]
    pub exe_sha256: String,
    #[serde(default)]
    pub game_build: String,
    /// the mapping the export was read through ([`super::regmap::digest`])
    #[serde(default)]
    pub regmap_digest: String,
    #[serde(default)]
    pub base: Base,
    #[serde(default)]
    pub registries: BTreeMap<String, Entries>,
}

pub fn parse(text: &str) -> Result<Export> {
    from_value(serde_json::from_str(text).context("not JSON")?)
}

pub fn from_value(v: Value) -> Result<Export> {
    let e: Export = serde_json::from_value(v).context("not a values export")?;
    if e.kind != KIND || e.format != 1 {
        bail!("not a {KIND} file of format 1 (kind {:?}, format {})", e.kind, e.format);
    }
    Ok(e)
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct FieldChange {
    pub registry: String,
    pub entry: String,
    pub field: String,
    pub was: Value,
    pub now: Value,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct EntryRef {
    pub registry: String,
    pub entry: String,
}

#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Side {
    pub exe_sha256: String,
    pub game_build: String,
    pub base: Base,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Diff {
    /// both exports are of the expected side and exes and say what their base is
    pub ok: bool,
    pub problems: Vec<String>,
    /// what the bases mean for the diff
    pub notes: Vec<String>,
    pub old: Side,
    pub new: Side,
    pub changed: Vec<FieldChange>,
    pub added: Vec<EntryRef>,
    pub removed: Vec<EntryRef>,
}

fn side_of(e: &Export) -> Side {
    Side { exe_sha256: e.exe_sha256.clone(), game_build: e.game_build.clone(), base: e.base.clone() }
}

fn base_text(b: &Base) -> String {
    format!("{} (writer {}{})", b.origin, b.writer, b.token.as_ref().map(|t| format!(", token {t}")).unwrap_or_default())
}

/// The base is the game's own data as shipped: kiln.assets' live bytes with no archive writer.
fn vanilla(b: &Base) -> bool {
    b.origin == "live" && b.writer == "none"
}

/// The diff of two exports; `old_exe` and `new_exe` are the builds the proof compares.
pub fn diff(old: &Export, new: &Export, side: &str, old_exe: &str, new_exe: &str) -> Diff {
    let mut d = Diff { old: side_of(old), new: side_of(new), ..Default::default() };
    for (what, e, exe) in [("old", old, old_exe), ("new", new, new_exe)] {
        if e.side != side {
            d.problems.push(format!("the {what} export is of the {} side, this proof is the {side} side's", e.side));
        }
        if !e.exe_sha256.eq_ignore_ascii_case(exe) {
            d.problems.push(format!("the {what} export is of exe {}, not {}", short(&e.exe_sha256), short(exe)));
        }
        let o = e.base.origin.trim();
        let w = e.base.writer.trim();
        if o.is_empty() || o == "none" || o == "unknown" || w.is_empty() || w == "unknown" {
            d.problems.push(format!("the {what} export does not say what its base is (origin {:?}, writer {:?}): a values diff is \
                                     never assumed to be of the game's own data", e.base.origin, e.base.writer));
        } else if !vanilla(&e.base) {
            d.notes.push(format!("the {what} base is the data as {} published it ({}): its values are not only the game's, and a \
                                  change may come from that writer's packages", e.base.writer, base_text(&e.base)));
        }
    }
    if old.base.origin != new.base.origin || old.base.writer != new.base.writer {
        d.notes.push(format!("the two bases differ (old {}, new {}): a value may differ because of the base, not the game build",
                             base_text(&old.base), base_text(&new.base)));
    }
    d.ok = d.problems.is_empty();
    let empty = Entries::new();
    let regs: std::collections::BTreeSet<&String> = old.registries.keys().chain(new.registries.keys()).collect();
    for reg in regs {
        let (a, b) = (old.registries.get(reg).unwrap_or(&empty), new.registries.get(reg).unwrap_or(&empty));
        for (id, fa) in a {
            let Some(fb) = b.get(id) else {
                d.removed.push(EntryRef { registry: reg.clone(), entry: id.clone() });
                continue;
            };
            let fields: std::collections::BTreeSet<&String> = fa.keys().chain(fb.keys()).collect();
            for f in fields {
                let (was, now) = (fa.get(f).unwrap_or(&Value::Null), fb.get(f).unwrap_or(&Value::Null));
                if !super::roundtrip::same_value(was, now) {
                    d.changed.push(FieldChange { registry: reg.clone(), entry: id.clone(), field: f.clone(), was: was.clone(), now: now.clone() });
                }
            }
        }
        for id in b.keys().filter(|id| !a.contains_key(*id)) {
            d.added.push(EntryRef { registry: reg.clone(), entry: id.clone() });
        }
    }
    d
}

fn short(s: &str) -> &str {
    s.get(..12).unwrap_or(s)
}

/// The diff as text for the reviewer (and the players' changelog).
pub fn changelog(d: &Diff) -> String {
    let mut t = format!("Registry values: game build {} ({}) -> {} ({})\nBase: old {}, new {}\n",
        d.old.game_build, short(&d.old.exe_sha256), d.new.game_build, short(&d.new.exe_sha256), base_text(&d.old.base), base_text(&d.new.base));
    for p in &d.problems {
        t.push_str(&format!("PROBLEM: {p}\n"));
    }
    for n in &d.notes {
        t.push_str(&format!("Note: {n}\n"));
    }
    let regs: std::collections::BTreeSet<&String> =
        d.changed.iter().map(|c| &c.registry).chain(d.added.iter().chain(&d.removed).map(|e| &e.registry)).collect();
    if regs.is_empty() {
        t.push_str("No value changed.\n");
    }
    for reg in regs {
        let ch: Vec<&FieldChange> = d.changed.iter().filter(|c| &c.registry == reg).collect();
        let entries: std::collections::BTreeSet<&String> = ch.iter().map(|c| &c.entry).collect();
        let add: Vec<&EntryRef> = d.added.iter().filter(|e| &e.registry == reg).collect();
        let rem: Vec<&EntryRef> = d.removed.iter().filter(|e| &e.registry == reg).collect();
        t.push_str(&format!("\n{reg}: {} changed, {} added, {} removed\n", entries.len(), add.len(), rem.len()));
        for c in ch {
            t.push_str(&format!("  {}  {} {} -> {}\n", c.entry, c.field, c.was, c.now));
        }
        for e in add {
            t.push_str(&format!("  + {}\n", e.entry));
        }
        for e in rem {
            t.push_str(&format!("  - {}\n", e.entry));
        }
    }
    t
}

/// The numbers of the balance registry's entry (the gear constants among them).
pub fn balance(e: &Export) -> BTreeMap<String, f64> {
    let Some(reg) = e.registries.get("balance") else { return BTreeMap::new() };
    let Some(entry) = reg.values().find(|x| x.contains_key("gear.seed")).or_else(|| reg.values().next()) else { return BTreeMap::new() };
    entry.iter().filter_map(|(k, v)| Some((k.clone(), v.as_f64()?))).collect()
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use serde_json::json;

    pub(crate) const OLD: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    pub(crate) const NEW: &str = "1111111111111111111111111111111111111111111111111111111111111111";

    pub(crate) fn export(sha: &str, build: &str, writer: &str, registries: Value) -> Export {
        parse(&json!({"format": 1, "kind": KIND, "side": "server", "exeSha256": sha, "gameBuild": build,
                      "regmapDigest": crate::regproof::regmap::tests::DIGEST, "base": {"origin": "live", "writer": writer}, "registries": registries}).to_string()).unwrap()
    }

    pub(crate) fn old_values() -> Value {
        json!({"gadgets": {"game:a": {"size": 20, "tier": "low", "speed": 1.25},
                           "game:b": {"size": 5, "tier": "high", "speed": 0.8},
                           "game:gone": {"size": 1}},
               "balance": {"game:balance": {"gear.seed": 10, "gear.creep": 0.15, "gear.armorOffset": 23, "gear.armorGradient": 15, "mode": "x"}}})
    }

    fn new_values() -> Value {
        json!({"gadgets": {"game:a": {"size": 40, "tier": "low", "speed": 1.25f32 as f64},
                           "game:b": {"size": 5, "tier": "high", "speed": 0.8, "glow": true},
                           "game:fresh": {"size": 3}},
               "balance": {"game:balance": {"gear.seed": 11, "gear.creep": 0.15, "gear.armorOffset": 23, "gear.armorGradient": 15, "mode": "x"}}})
    }

    #[test]
    fn parses_only_values_exports() {
        assert!(parse(r#"{"format": 1, "kind": "registry-roundtrip"}"#).is_err());
        assert!(parse(r#"{"format": 0, "kind": "registry-values"}"#).is_err());
        assert_eq!(export(OLD, "1", "none", old_values()).registries["gadgets"].len(), 3);
    }

    #[test]
    fn diffs_values_entries_and_fields() {
        let d = diff(&export(OLD, "100", "none", old_values()), &export(NEW, "101", "none", new_values()), "server", OLD, NEW);
        assert!(d.ok, "{:?}", d.problems);
        assert!(d.notes.is_empty(), "two vanilla bases need no note: {:?}", d.notes);
        assert_eq!(d.changed, [
            FieldChange { registry: "balance".into(), entry: "game:balance".into(), field: "gear.seed".into(), was: json!(10), now: json!(11) },
            FieldChange { registry: "gadgets".into(), entry: "game:a".into(), field: "size".into(), was: json!(20), now: json!(40) },
            FieldChange { registry: "gadgets".into(), entry: "game:b".into(), field: "glow".into(), was: Value::Null, now: json!(true) },
        ], "a float within float precision is no change");
        assert_eq!(d.added, [EntryRef { registry: "gadgets".into(), entry: "game:fresh".into() }]);
        assert_eq!(d.removed, [EntryRef { registry: "gadgets".into(), entry: "game:gone".into() }]);
        assert_eq!((d.old.game_build.as_str(), d.new.game_build.as_str()), ("100", "101"));
        let text = changelog(&d);
        assert!(text.contains("game:a  size 20 -> 40"), "{text}");
        assert!(text.contains("+ game:fresh") && text.contains("- game:gone"), "{text}");
        assert!(text.contains("gadgets: 2 changed, 1 added, 1 removed"), "{text}");
    }

    #[test]
    fn the_base_is_never_assumed_to_be_the_games() {
        let mut unknown = export(NEW, "101", "none", new_values());
        unknown.base.origin.clear();
        let d = diff(&export(OLD, "100", "none", old_values()), &unknown, "server", OLD, NEW);
        assert!(!d.ok);
        assert!(d.problems.iter().any(|p| p.contains("new export") && p.contains("base")), "{:?}", d.problems);
        let mut none = export(OLD, "100", "none", old_values());
        none.base.origin = "none".into();
        assert!(!diff(&none, &export(NEW, "101", "none", new_values()), "server", OLD, NEW).ok);
        // writer "none" is the game's own data; a writer nobody can name is refused
        assert!(!diff(&export(OLD, "100", "unknown", old_values()), &export(NEW, "101", "none", new_values()), "server", OLD, NEW).ok);
        // an archive writer's base: named, the diff still made
        let mut eml = export(NEW, "101", "kiln-eml", new_values());
        eml.base.token = Some("first-managed-unclassified".into());
        let d2 = diff(&export(OLD, "100", "kiln-eml", old_values()), &eml, "server", OLD, NEW);
        assert!(d2.ok);
        assert!(d2.notes.iter().any(|n| n.contains("kiln-eml") && n.contains("not only the game")), "{:?}", d2.notes);
        assert!(changelog(&d2).contains("kiln-eml"));
        // two bases of different origin
        let d3 = diff(&export(OLD, "100", "none", old_values()), &export(NEW, "101", "kiln-eml", new_values()), "server", OLD, NEW);
        assert!(d3.ok && d3.notes.iter().any(|n| n.contains("bases differ")), "{:?}", d3.notes);
    }

    #[test]
    fn exports_of_another_side_or_build_are_refused() {
        let mut client = export(NEW, "101", "none", new_values());
        client.side = "client".into();
        assert!(!diff(&export(OLD, "100", "none", old_values()), &client, "server", OLD, NEW).ok);
        let d = diff(&export(OLD, "100", "none", old_values()), &export(OLD, "100", "none", old_values()), "server", OLD, NEW);
        assert!(!d.ok && d.problems.iter().any(|p| p.contains("new export")), "{:?}", d.problems);
    }

    #[test]
    fn a_malformed_exe_hash_is_named_without_a_panic() {
        let odd = export(&format!("a{}", "\u{e9}".repeat(40)), "101", "none", new_values()); // byte 12 inside a character
        let d = diff(&export(OLD, "100", "none", old_values()), &odd, "server", OLD, NEW);
        assert!(!d.ok && d.problems.iter().any(|p| p.contains("new export is of exe")), "{:?}", d.problems);
        assert!(changelog(&d).contains("PROBLEM"));
    }

    #[test]
    fn balance_numbers_carry_the_gear_constants() {
        let b = balance(&export(NEW, "101", "none", new_values()));
        assert_eq!(b["gear.seed"], 11.0);
        assert_eq!(b.len(), 4, "the numbers only: {b:?}");
        assert!(balance(&export(NEW, "101", "none", json!({}))).is_empty());
        // the entry with the gear constants, not the first by name
        let two = export(NEW, "101", "none", json!({"balance": {"game:a_first": {"other": 1}, "game:balance": {"gear.seed": 11}}}));
        assert_eq!(balance(&two)["gear.seed"], 11.0);
    }
}
