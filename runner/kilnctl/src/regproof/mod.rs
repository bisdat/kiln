// SPDX-License-Identifier: MIT
//! The registry proof after a game update (registry P2 slice 4; BRIEF section 6.1): what `kilnctl update` (after
//! sigport and the lab gate) and `kilnctl regproof` check before a regmap record (`regmap/<side>/<exe sha256>.json`)
//! says a Kiln field is mapped on a new game build.
//!
//! 1. reflection member and enum diff of the old and new exe ([`refl`]);
//! 2. formula checks ([`formula`]);
//! 3. the round-trip suite of the lab, live and boot generations ([`roundtrip`]);
//! 4. the values diff of the old and new build with each base's origin ([`values`]);
//! 5. the alias check and the per-build id tables ([`ids`]);
//! 6. the fail-closed mark: the record is `verified` only when every step ran with its inputs; a row is `ok` only when
//!    regmap_check.py resolved it, the reflection carried it over (or a human reviewed the change), its round trip passed
//!    in its generation and (a formula row) the formula check passed. A row whose only gap is evidence the lab did not
//!    give (no sample in its generation, no formula constants) is `unproven`, with its reason in the record's `reasons`;
//!    a row the build changed or a lab step disproved is `changed`. Both are refused per entry at run time.
//!
//! Inputs kilnctl makes itself with the tools of the Kiln tree (the exe is only read, never run): the reflection dumps
//! (`tools/kfc/kfc.py types --closure`), the build's entries (`tools/registrygen/regmap_entries.py`) and the candidate
//! record with this build's id table (`tools/registrygen/regmap_check.py --record --ids-out`). Inputs from the lab
//! (evidence): round-trip suites, values exports and kiln.content's own id table, found by content in the given files and
//! folders (a Kiln data folder: `content/registry/` and `content/registry/proof/`). Outputs go to a folder outside the
//! repository; id tables and values exports are kept per build in the maintainer folder ([`ids::Store`]).
pub mod formula;
pub mod ids;
pub mod refl;
pub mod regmap;
pub mod roundtrip;
pub mod values;

use anyhow::{Context, Result, bail};
use serde::Serialize;
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Everything one proof judges, already read.
#[derive(Debug, Clone, Default)]
pub struct Inputs {
    pub side: String,
    pub new_exe_sha: String,
    pub old_exe_sha: Option<String>,
    pub map: regmap::RegMap,
    /// [`regmap::digest`] of the regmap/common judged: lab files of another mapping prove nothing here
    pub regmap_digest: String,
    pub old_refl: Option<refl::TypeDb>,
    pub new_refl: Option<refl::TypeDb>,
    /// regmap_check.py's record of the new build (its rows, layouts and aliases)
    pub candidate: Option<Value>,
    /// the old build's record (the tree's, else the instance's): the removal history and pins carry through it
    pub previous: Option<Value>,
    /// the new build's id table as regmap_check.py computed it (with the previous build's table and the aliases)
    pub new_ids: Option<ids::IdTable>,
    pub prev_ids: Option<ids::IdTable>,
    /// kiln.content's own table of the new build, written at the lab's start
    pub native_ids: Option<ids::IdTable>,
    pub suites: Vec<roundtrip::Suite>,
    pub values_old: Option<values::Export>,
    pub values_new: Option<values::Export>,
    /// no earlier build to compare with: waives the old reflection, the old values and the previous id table
    pub first_build: bool,
    /// rows whose reflection change a human reviewed (`--accept`)
    pub accepted: Vec<String>,
    /// what went wrong while the inputs were made or read
    pub problems: Vec<String>,
    /// what the reader should know about how the inputs were made
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct RowVerdict {
    /// ok | changed | unproven | unmapped (regmap/README.md)
    pub state: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub why: Vec<String>,
    /// an unproven row's reason, in Kiln words (the record's `reasons`: players and mod authors read it)
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Proof {
    pub when: String,
    pub side: String,
    pub exe_sha256: String,
    pub old_exe_sha256: Option<String>,
    pub first_build: bool,
    pub refl: Option<refl::ReflDiff>,
    pub formula: formula::Report,
    pub roundtrip: roundtrip::Report,
    pub values: Option<values::Diff>,
    pub aliases: Option<ids::AliasReport>,
    /// the new build's table as checked and kept (regmap_check.py's with kiln.content's entries it does not cover)
    #[serde(skip)]
    pub table: Option<ids::IdTable>,
    pub rows: BTreeMap<String, RowVerdict>,
    pub accepted: Vec<String>,
    /// why the record may not be verified (empty: verified)
    pub blockers: Vec<String>,
    pub notes: Vec<String>,
    pub verified: bool,
}

const MAX_WHY: usize = 8;

fn short(s: &str) -> &str {
    s.get(..12).unwrap_or(s)
}

/// Runs every step on the inputs and decides each row and the record.
pub fn prove(inp: &Inputs, when: &str) -> Proof {
    let mut p = Proof {
        when: when.to_string(),
        side: inp.side.clone(),
        exe_sha256: inp.new_exe_sha.clone(),
        old_exe_sha256: inp.old_exe_sha.clone(),
        first_build: inp.first_build,
        ..Default::default()
    };
    p.blockers.extend(inp.problems.iter().cloned());
    p.notes.extend(inp.notes.iter().cloned());
    // the candidate record
    let mut cand_rows = BTreeMap::new();
    match &inp.candidate {
        None => p.blockers.push("no candidate record: regmap_check.py --record did not run on the new exe".into()),
        Some(c) => {
            if !c.get("exeSha256").and_then(Value::as_str).is_some_and(|s| s.eq_ignore_ascii_case(&inp.new_exe_sha)) {
                p.blockers.push(format!("the candidate record is not of the new exe {}", short(&inp.new_exe_sha)));
            }
            if c.get("side").and_then(Value::as_str) != Some(inp.side.as_str()) {
                p.blockers.push(format!("the candidate record is not of the {} side", inp.side));
            }
            if c.get("verified").and_then(Value::as_bool) != Some(true) {
                p.blockers.push("regmap_check.py did not find every registry's root type in the new exe's reflection".into());
            }
            if let Some(o) = c.get("rows").and_then(Value::as_object) {
                cand_rows = o.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or_default().to_string())).collect();
            }
        }
    }
    if inp.old_exe_sha.as_deref().is_some_and(|o| o.eq_ignore_ascii_case(&inp.new_exe_sha)) {
        p.blockers.push(format!("the old and the new build are the same exe ({}): nothing to compare it with", short(&inp.new_exe_sha)));
    }
    // 1. reflection
    if inp.new_refl.is_none() {
        p.blockers.push("no reflection dump of the new exe".into());
    }
    match (&inp.old_refl, &inp.new_refl) {
        (Some(o), Some(n)) => p.refl = Some(refl::diff(&inp.map, o, n)),
        (None, _) if inp.first_build => p.notes.push("first build: no previous build's reflection to compare with; each row is proven by the lab alone".into()),
        (None, _) => p.blockers.push("no reflection dump of the old exe (the build the regmap was last proven on)".into()),
        _ => {}
    }
    // 3. round trip (before the formulas: they use its samples)
    p.roundtrip = roundtrip::check(&inp.map, &inp.side, &inp.new_exe_sha, &inp.regmap_digest, &inp.suites);
    if !p.roundtrip.complete {
        p.blockers.push(format!("the round-trip suite is incomplete: no {} generation of this build (the lab's proof producer writes one per generation)",
                                p.roundtrip.missing.join(" and no ")));
    }
    p.notes.extend(p.roundtrip.problems.iter().map(|x| format!("round trip: {x}")));
    // 2. formulas
    let mut set_aside = Vec::new();
    let usable = roundtrip::usable(&inp.side, &inp.new_exe_sha, &inp.regmap_digest, &inp.suites, &mut set_aside);
    let formula_rows: Vec<(String, &regmap::Row)> = regmap::qualified(&inp.map)
        .into_iter()
        .filter(|(_, _, _, row)| row.has_mapping() && row.conv == "formula")
        .map(|(q, _, _, row)| (q, row))
        .collect();
    let samples: BTreeMap<String, Vec<formula::Sample>> =
        formula_rows.iter().map(|(q, _)| (q.clone(), roundtrip::formula_samples(&usable, q))).collect();
    let now_c = inp.values_new.as_ref().and_then(|e| formula::constants_of(&values::balance(e))).or_else(|| formula::constants_of(&roundtrip::constants(&usable)));
    let before_c = inp.values_old.as_ref().and_then(|e| formula::constants_of(&values::balance(e)));
    p.formula = formula::check(&formula_rows, now_c, before_c, &samples);
    p.notes.extend(p.formula.notes.iter().cloned());
    // 4. values
    match (&inp.values_old, &inp.values_new) {
        (_, None) => p.blockers.push("no values export of the new build (the lab's proof producer writes it)".into()),
        (old, Some(new)) => {
            let first = old.is_none() && inp.first_build;
            if old.is_none() && !inp.first_build {
                p.blockers.push("no values export of the old build (kilnctl keeps each proven build's export in the maintainer folder)".into());
            }
            if let Some(old) = old.as_ref().or(first.then_some(new)) {
                let old_sha = if first { inp.new_exe_sha.clone() } else { inp.old_exe_sha.clone().unwrap_or_else(|| old.exe_sha256.clone()) };
                if !new.regmap_digest.eq_ignore_ascii_case(&inp.regmap_digest) {
                    p.blockers.push(format!("values: the new export was read through another mapping (regmapDigest {:?}, this proof's \
                                             regmap/common is {})", new.regmap_digest.get(..12).unwrap_or(&new.regmap_digest), short(&inp.regmap_digest)));
                }
                if !first && !old.regmap_digest.eq_ignore_ascii_case(&inp.regmap_digest) {
                    p.notes.push("values: the old export was read through another regmap/common (rows that changed between the two can \
                                  differ for that reason)".into());
                }
                let d = values::diff(old, new, &inp.side, &old_sha, &inp.new_exe_sha);
                p.blockers.extend(d.problems.iter().map(|x| format!("values: {x}")));
                p.notes.extend(d.notes.iter().map(|x| format!("values: {x}")));
                if first {
                    p.notes.push("first build: no previous build's values to diff".into());
                } else {
                    p.values = Some(d);
                }
            }
        }
    }
    // 5. aliases (the removal history and pins of the old build's record carry forward)
    let aliases = inp.candidate.as_ref().map(ids::Aliases::of_record).unwrap_or_default();
    let previous_aliases = inp.previous.as_ref().map(ids::Aliases::of_record).unwrap_or_default();
    if inp.previous.is_none() && !inp.first_build {
        p.blockers.push("no regmap record of the old build (in the tree or the instance's kiln/regmap): the removal history and pins \
                         carry forward only through it".into());
    }
    match (&inp.prev_ids, &inp.new_ids) {
        (_, None) => p.blockers.push("no id table of the new build (regmap_check.py --ids-out over this build's entries)".into()),
        (None, Some(_)) if !inp.first_build => {
            p.blockers.push("no id table of the previous build (kilnctl keeps one per build in the maintainer folder; kiln.content writes \
                             the running build's into its data folder)".into())
        }
        (prev, Some(computed)) => {
            let now = &ids::merged(computed, inp.native_ids.as_ref());
            if !now.exe_sha256.eq_ignore_ascii_case(&inp.new_exe_sha) || now.side != inp.side {
                p.blockers.push(format!("the new id table is of {} {}, not of the new exe", now.side, short(&now.exe_sha256)));
            }
            if inp.native_ids.is_none() {
                p.blockers.push("kiln.content's own id table of the new build is not in the evidence (it writes it at the lab's start): \
                                 the two implementations were not compared".into());
            }
            let empty = ids::IdTable { side: inp.side.clone(), ..Default::default() };
            let r = ids::check(prev.as_ref().unwrap_or(&empty), now, &aliases, &previous_aliases, inp.native_ids.as_ref());
            p.blockers.extend(r.problems.iter().take(20).map(|x| format!("ids: {x}")));
            if r.problems.len() > 20 {
                p.blockers.push(format!("ids: {} more problems (aliases.json)", r.problems.len() - 20));
            }
            p.notes.extend(r.notes.iter().map(|x| format!("ids: {x}")));
            p.aliases = Some(r);
            p.table = Some(now.clone());
        }
    }
    // 6. each row
    let accepted: BTreeSet<&String> = inp.accepted.iter().collect();
    for (q, _, _, row) in regmap::qualified(&inp.map) {
        let mut why = Vec::new();
        let mut reason = None;
        let state = if !row.has_mapping() {
            "unmapped"
        } else {
            match cand_rows.get(&q).map(String::as_str) {
                Some("ok") => {
                    // hard: the mapping is wrong on this build (Changed); missing: the lab gave no evidence (Unproven)
                    let (mut hard, mut missing) = (Vec::new(), Vec::new());
                    match p.refl.as_ref().map(|r| r.rows.get(&q)) {
                        Some(Some(d)) if d.status == refl::Status::Changed => {
                            if accepted.contains(&q) {
                                p.notes.push(format!("{q}: its reflection change was reviewed (--accept)"));
                            } else {
                                hard.extend(d.changes.iter().map(|c| format!("reflection: {c}")));
                                hard.extend(d.proposals.iter().map(|x| format!("proposal: {} -> {} ({})", x.was, x.now, x.why)));
                            }
                        }
                        Some(None) => hard.push("reflection: not diffed".into()),
                        _ => {}
                    }
                    match p.roundtrip.fields.get(&q) {
                        Some(f) if f.ok => {}
                        Some(f) => {
                            hard.extend(f.problems.iter().map(|x| format!("round trip: {x}")));
                            missing.extend(f.missing.iter().map(|x| format!("round trip: {x}")));
                        }
                        None => hard.push("round trip: not checked".into()),
                    }
                    if row.conv == "formula" {
                        match p.formula.rows.get(&q) {
                            Some(f) if f.ok => {}
                            Some(f) => {
                                hard.extend(f.problems.iter().map(|x| format!("formula: {x}")));
                                missing.extend(f.missing.iter().map(|x| format!("formula: {x}")));
                            }
                            None => hard.push("formula: not checked".into()),
                        }
                    }
                    if !hard.is_empty() {
                        why = hard;
                        why.extend(missing);
                        "changed"
                    } else if !missing.is_empty() {
                        reason = Some(unproven_reason(&missing));
                        why = missing;
                        "unproven"
                    } else {
                        "ok"
                    }
                }
                Some(s @ ("changed" | "unmapped")) => {
                    why.push(format!("regmap_check.py: {s} on this build (its log names why)"));
                    s
                }
                _ => {
                    why.push("not in the candidate record".into());
                    "unmapped"
                }
            }
        };
        if why.len() > MAX_WHY {
            let more = why.len() - MAX_WHY;
            why.truncate(MAX_WHY);
            why.push(format!("... {more} more (proof.json)"));
        }
        p.rows.insert(q, RowVerdict { state: state.into(), why, reason });
    }
    p.accepted = accepted.into_iter().cloned().collect();
    p.verified = p.blockers.is_empty();
    p
}

/// An unproven row's reason as players and mod authors read it (kiln.content: "Unproven: <field> is not proven for game
/// build <b> (<reason>)"): Kiln words, no game names.
fn unproven_reason(missing: &[String]) -> String {
    let no_sample = missing.iter().any(|m| m.contains("no round-trip sample"));
    if !no_sample && missing.iter().any(|m| m.contains("constants")) {
        "its formula's constants were not read on this build yet".into()
    } else {
        "no lab round trip of it on this build yet".into()
    }
}

impl Proof {
    /// (rows ok, rows with a mapping)
    pub fn ok_rows(&self) -> (usize, usize) {
        // a row unmapped in regmap/common carries no reason here; every other row has a mapping
        let mapped = self.rows.values().filter(|r| r.state != "unmapped" || !r.why.is_empty()).count();
        (self.rows.values().filter(|r| r.state == "ok").count(), mapped)
    }

    /// One line: what the proof covered (the record's "proof").
    pub fn summary(&self) -> String {
        let mut parts = Vec::new();
        match &self.refl {
            Some(r) => {
                let n = |s: refl::Status| r.rows.iter().filter(|(q, d)| !q.ends_with(".(entries)") && d.status == s).count();
                parts.push(format!("reflection {} same, {} carried, {} new, {} changed", n(refl::Status::Same), n(refl::Status::Carried),
                                   n(refl::Status::New), n(refl::Status::Changed)));
            }
            None => parts.push(if self.first_build { "first build (no reflection diff)".into() } else { "no reflection diff".into() }),
        }
        let f = &self.formula.rows;
        parts.push(format!("formulas {}/{}", f.values().filter(|x| x.ok).count(), f.len()));
        let rt = &self.roundtrip.fields;
        parts.push(format!("round trip {}/{} fields{}", rt.values().filter(|x| x.ok).count(), rt.len(),
                           if self.roundtrip.complete { String::new() } else { format!(" (no {})", self.roundtrip.missing.join(", ")) }));
        if let Some(v) = &self.values {
            parts.push(format!("values {} changed, {} added, {} removed (base {}/{} -> {}/{})", v.changed.len(), v.added.len(), v.removed.len(),
                               v.old.base.origin, v.old.base.writer, v.new.base.origin, v.new.base.writer));
        }
        if let Some(a) = &self.aliases {
            parts.push(format!("ids {} kept, {} added, {} removed, {} GUID changes", a.kept, a.added.len(), a.removed.len(), a.guid_changes.len()));
        }
        let (ok, n) = self.ok_rows();
        let unproven = self.rows.values().filter(|r| r.state == "unproven").count();
        parts.push(format!("rows ok {ok}/{n}{}", if unproven > 0 { format!(" ({unproven} unproven)") } else { String::new() }));
        if !self.accepted.is_empty() {
            parts.push(format!("reviewed: {}", self.accepted.join(", ")));
        }
        let mut s = format!("kilnctl regproof ({}): {}", self.when, parts.join("; "));
        if !self.verified {
            s.push_str(&format!("; NOT verified: {}", self.blockers.first().map(String::as_str).unwrap_or("?")));
        }
        s
    }
}

/// The record to write: the candidate with each row's proven state, `verified` and what the proof covered.
pub fn record(p: &Proof, candidate: &Value) -> Value {
    let mut r = candidate.clone();
    if let Some(o) = r.as_object_mut() {
        o.insert("rows".into(), Value::Object(p.rows.iter().map(|(q, v)| (q.clone(), Value::String(v.state.clone()))).collect()));
        let reasons: serde_json::Map<String, Value> = p.rows.iter()
            .filter_map(|(q, v)| Some((q.clone(), Value::String(v.reason.clone().filter(|_| v.state == "unproven")?))))
            .collect();
        if reasons.is_empty() {
            o.remove("reasons");
        } else {
            o.insert("reasons".into(), Value::Object(reasons));
        }
        o.insert("verified".into(), Value::Bool(p.verified));
        o.insert("proof".into(), Value::String(p.summary()));
    }
    r
}

/// The human's view: what was proven, what blocks the record, each row that is not ok and why.
pub fn report_text(p: &Proof) -> String {
    let mut t = format!("{}\n\nside {}, new exe {}, old exe {}\n", p.summary(), p.side, p.exe_sha256,
                        p.old_exe_sha256.as_deref().unwrap_or("(none)"));
    t.push_str(&format!("\nrecord: {}\n", if p.verified { "VERIFIED (every step ran with its inputs)" } else { "NOT verified" }));
    for b in &p.blockers {
        t.push_str(&format!("  blocker: {b}\n"));
    }
    let bad: Vec<(&String, &RowVerdict)> = p.rows.iter().filter(|(_, r)| r.state != "ok").collect();
    t.push_str(&format!("\nrows not ok ({}; each is refused per entry at run time):\n", bad.len()));
    for (q, r) in bad {
        t.push_str(&format!("  {q}: {}{}\n", r.state, r.reason.as_ref().map(|x| format!(" ({x})")).unwrap_or_default()));
        for w in &r.why {
            t.push_str(&format!("      {w}\n"));
        }
    }
    if !p.notes.is_empty() {
        t.push_str("\nnotes:\n");
        for n in &p.notes {
            t.push_str(&format!("  {n}\n"));
        }
    }
    t
}

/// What a human has to do next.
pub fn next_text(p: &Proof) -> String {
    let mut v = Vec::new();
    for b in &p.blockers {
        v.push(format!("- {b}"));
    }
    for (q, r) in p.rows.iter().filter(|(_, r)| r.state == "unproven") {
        v.push(format!("- {q}: unproven: {} (kept off; the lab's proof producer has to sample it)", r.reason.as_deref().unwrap_or("no lab evidence")));
    }
    for (q, r) in p.rows.iter().filter(|(_, r)| r.state == "changed") {
        if r.why.iter().any(|w| w.starts_with("reflection") || w.starts_with("proposal")) {
            v.push(format!("- review {q}: fix its row in regmap/common (proposals in report.txt) or rerun with --accept {q} when the change is harmless"));
        } else {
            v.push(format!("- {q}: {}", r.why.first().map(String::as_str).unwrap_or("not proven")));
        }
    }
    if v.is_empty() {
        "nothing left to do: the record is verified (kilnctl regproof --mark writes it into the instance; commit the copy \
         verified-regmap-*.json as regmap/<side>/<exe sha256>.json)\n".into()
    } else {
        v.join("\n") + "\n"
    }
}

/// Writes proof.json, report.txt, NEXT.txt, the step reports and the record into `out`.
pub fn write_outputs(p: &Proof, record: Option<&Value>, out: &Path) -> Result<()> {
    std::fs::create_dir_all(out).with_context(|| format!("creating {}", out.display()))?;
    let w = |name: &str, text: String| ids::write_whole(&out.join(name), &text);
    w("proof.json", serde_json::to_string_pretty(p)? + "\n")?;
    if let Some(r) = &p.refl {
        w("refl-diff.json", serde_json::to_string_pretty(r)? + "\n")?;
    }
    w("roundtrip.json", serde_json::to_string_pretty(&p.roundtrip)? + "\n")?;
    w("formula.json", serde_json::to_string_pretty(&p.formula)? + "\n")?;
    if let Some(v) = &p.values {
        w("values-diff.json", serde_json::to_string_pretty(v)? + "\n")?;
        w("values-changelog.txt", values::changelog(v))?;
    }
    if let Some(a) = &p.aliases {
        w("aliases.json", serde_json::to_string_pretty(a)? + "\n")?;
    }
    if let Some(r) = record {
        w(&format!("regmap-{}-{}.json", p.side, p.exe_sha256), serde_json::to_string_pretty(r)? + "\n")?;
    }
    w("report.txt", report_text(p))?;
    w("NEXT.txt", next_text(p))?;
    Ok(())
}

/// Writes a verified record into a Kiln folder (`<kiln>/regmap/<side>/<exe sha256>.json`, what kiln.content reads) and a
/// copy for the repository into `out`. Refuses an unverified proof. A verified record already there is kept unless
/// `replace` (a fuller proof of the same build); the replaced one stays beside it as `.json.bak`.
/// What [`mark`] did.
#[derive(Debug, Clone, PartialEq)]
pub enum Marked {
    Written(PathBuf),
    /// a verified record of the build was there and `replace` was not given: left as it is
    Kept(PathBuf),
}

pub fn mark(kiln_dir: &Path, p: &Proof, record: &Value, out: &Path, replace: bool) -> Result<Marked> {
    if !p.verified || record.get("verified").and_then(Value::as_bool) != Some(true) {
        bail!("the proof is not verified: nothing is marked ({})", p.blockers.first().map(String::as_str).unwrap_or("?"));
    }
    let dst = kiln_dir.join("regmap").join(&p.side).join(format!("{}.json", p.exe_sha256));
    if let Ok(text) = std::fs::read_to_string(&dst) {
        if serde_json::from_str::<Value>(&text).ok().and_then(|v| v.get("verified")?.as_bool()) == Some(true) {
            if !replace {
                return Ok(Marked::Kept(dst));
            }
            std::fs::rename(&dst, dst.with_extension("json.bak")).with_context(|| format!("keeping {} as .bak", dst.display()))?;
        }
    }
    let text = serde_json::to_string_pretty(record)? + "\n";
    ids::write_whole(&dst, &text)?;
    ids::write_whole(&out.join(format!("verified-regmap-{}-{}.json", p.side, p.exe_sha256)), &text)?;
    Ok(Marked::Written(dst))
}

// ---- making and finding the inputs ---------------------------------------------------------------------------------

/// Where the inputs come from. Paths given win over what kilnctl would make or find.
#[derive(Debug, Clone, Default)]
pub struct Gather {
    pub side: String,
    /// the Kiln tree: regmap/common, schema/, tools/
    pub tree: PathBuf,
    /// the Kiln folder the game runs (`<game>/kiln`): its regmap/common must be the tree's
    pub deployed: Option<PathBuf>,
    pub out: PathBuf,
    pub store: Option<ids::Store>,
    pub new_exe: Option<PathBuf>,
    pub old_exe: Option<PathBuf>,
    pub new_refl: Option<PathBuf>,
    pub old_refl: Option<PathBuf>,
    pub record: Option<PathBuf>,
    pub ids: Option<PathBuf>,
    pub prev_ids: Option<PathBuf>,
    /// files and folders with lab outputs (round-trip suites, values exports, kiln.content's id tables); a Kiln data
    /// folder's content/registry and content/registry/proof are read, and its id tables kept in the store
    pub evidence: Vec<PathBuf>,
    pub game_build: Option<String>,
    pub steam_build: Option<String>,
    pub first_build: bool,
    pub accept: Vec<String>,
}

fn read_json(p: &Path) -> Result<Value> {
    let t = std::fs::read_to_string(p).with_context(|| format!("reading {}", p.display()))?;
    serde_json::from_str(&t).with_context(|| format!("{} is not JSON", p.display()))
}

/// How long one tool may run (a reflection dump or an archive read takes seconds to a few minutes).
const PYTHON_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30 * 60);

/// Runs a tool of the Kiln tree: isolated (`-I`), writing no bytecode into the tree (`-B`), its output into `out/<log>`,
/// stopped (by its own pid) after [`PYTHON_TIMEOUT`].
fn python(tree: &Path, script: &str, args: &[&std::ffi::OsStr], out: &Path, log: &str) -> Result<std::process::ExitStatus> {
    let tool = tree.join(script);
    if !tool.is_file() {
        bail!("{} not found in the Kiln tree", tool.display());
    }
    let logf = std::fs::File::create(out.join(log)).with_context(|| format!("creating {}", out.join(log).display()))?;
    let err = logf.try_clone()?;
    let mut child = Command::new("python3")
        .args(["-I", "-B"])
        .arg(&tool)
        .args(args)
        .env("KILN_OUT", out.join("kfc"))
        .env("PYTHONDONTWRITEBYTECODE", "1")
        .stdout(logf)
        .stderr(err)
        .spawn()
        .with_context(|| format!("running python3 {script}"))?;
    let t0 = std::time::Instant::now();
    loop {
        if let Some(st) = child.try_wait()? {
            return Ok(st);
        }
        if t0.elapsed() > PYTHON_TIMEOUT {
            let _ = child.kill();
            let _ = child.wait();
            bail!("python3 {script} ran longer than {} minutes and was stopped", PYTHON_TIMEOUT.as_secs() / 60);
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}

/// The registries' root types (and a drop table's naming component): the closure kilnctl dumps.
pub fn roots(map: &regmap::RegMap) -> Vec<String> {
    let mut v: BTreeSet<String> = BTreeSet::new();
    for f in map.values() {
        if !f.entries.resource_type.is_empty() {
            v.insert(f.entries.resource_type.clone());
        }
        if let Some((ty, _)) = f.entries.name_from.rsplit_once('.') {
            v.insert(ty.to_string());
        }
    }
    v.into_iter().collect()
}

/// The reflection of an exe (tools/kfc/kfc.py types --closure; the exe is read as data).
fn dump_refl(g: &Gather, map: &regmap::RegMap, exe: &Path, which: &str) -> Result<refl::TypeDb> {
    let out = g.out.join(format!("refl-{which}.json"));
    let closure = roots(map).join(",");
    let st = python(&g.tree, "tools/kfc/kfc.py",
                    &["--exe".as_ref(), exe.as_os_str(), "types".as_ref(), "--closure".as_ref(), closure.as_ref(), "--json-out".as_ref(), out.as_os_str()],
                    &g.out, &format!("refl-{which}.log"))?;
    if !st.success() {
        bail!("kfc.py types failed ({st}; {})", g.out.join(format!("refl-{which}.log")).display());
    }
    refl::TypeDb::parse(&std::fs::read_to_string(&out)?).with_context(|| format!("{}", out.display()))
}

fn copy_tree(from: &Path, to: &Path) -> Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from).with_context(|| format!("reading {}", from.display()))?.flatten() {
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if std::fs::metadata(&src)?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst).with_context(|| format!("copying {}", src.display()))?;
        }
    }
    Ok(())
}

/// The candidate record and the new build's id table from regmap_check.py --record, in a scratch copy of the tree's
/// regmap/common and schema/ (nothing is written into the tree). No build record is copied: only the record this run
/// writes can be the candidate.
fn run_record(g: &Gather, exe: &Path, new_sha: &str, previous: Option<&Path>, prev_ids: Option<&ids::IdTable>, problems: &mut Vec<String>,
              notes: &mut Vec<String>) -> Result<(Option<Value>, Option<ids::IdTable>)> {
    let root = g.out.join("root");
    let _ = std::fs::remove_dir_all(&root);
    copy_tree(&g.tree.join("regmap/common"), &root.join("regmap/common"))?;
    copy_tree(&g.tree.join("schema"), &root.join("schema"))?;
    let entries = g.out.join("entries.json");
    let st = python(&g.tree, "tools/registrygen/regmap_entries.py", &["--exe".as_ref(), exe.as_os_str(), "--out".as_ref(), entries.as_os_str()],
                    &g.out, "entries.log");
    let have_entries = matches!(&st, Ok(s) if s.success()) && entries.is_file();
    if !have_entries {
        problems.push(format!("regmap_entries.py did not read the new build's entries (the .kfc must be beside the exe; {})",
                              g.out.join("entries.log").display()));
    }
    let game_build = g.game_build.clone().or_else(|| {
        exe.parent().and_then(kiln_manage::catalog::kfc_version).map(|v| v.split('|').next().unwrap_or_default().to_string())
    }).unwrap_or_default();
    let mut args: Vec<std::ffi::OsString> = ["--root".into(), root.clone().into_os_string(), "--exe".into(), exe.as_os_str().to_owned(),
        "--record".into(), "--side".into(), g.side.clone().into(), "--game-build".into(), game_build.into(),
        "--steam-build".into(), g.steam_build.clone().unwrap_or_default().into()].into();
    let ids_out = g.out.join("ids-new.json");
    if have_entries {
        args.extend(["--entries".into(), entries.into_os_string(), "--ids-out".into(), ids_out.clone().into_os_string()]);
    }
    if let Some(prev) = previous {
        args.extend(["--previous".into(), prev.as_os_str().to_owned()]);
    }
    if let Some(t) = prev_ids {
        let p = g.out.join("ids-prev.json");
        ids::write_whole(&p, &t.to_json())?;
        args.extend(["--ids-in".into(), p.into_os_string()]);
    }
    let refs: Vec<&std::ffi::OsStr> = args.iter().map(|a| a.as_os_str()).collect();
    let st = python(&g.tree, "tools/registrygen/regmap_check.py", &refs, &g.out, "regmap_check.log")?;
    let rec = root.join("regmap").join(&g.side).join(format!("{new_sha}.json"));
    let log = g.out.join("regmap_check.log");
    if !rec.is_file() {
        problems.push(format!("regmap_check.py --record wrote no record ({st}; {})", log.display()));
        return Ok((None, None));
    }
    match st.code() {
        Some(0) => {}
        // exit 1 after the record: rows with errors, listed unmapped in it
        Some(1) => notes.push(format!("regmap_check.py reported errors in rows (they are unmapped in the candidate; {})", log.display())),
        _ => {
            problems.push(format!("regmap_check.py --record failed ({st}; {}): its record is not taken", log.display()));
            return Ok((None, None));
        }
    }
    let table = if have_entries && ids_out.is_file() {
        Some(ids::IdTable::parse(&std::fs::read_to_string(&ids_out)?, &g.side, new_sha)?)
    } else {
        None
    };
    Ok((Some(read_json(&rec)?), table))
}

/// The largest lab file kilnctl reads (a values export of every registry is tens of MiB).
const LAB_FILE_MAX: u64 = 256 << 20;

/// A lab output by its content: a round-trip suite, a values export or an id table. `strict` (a file under proof/, or one
/// given by name): a file that cannot be read is a problem, not skipped.
fn classify(p: &Path, inp: &mut Inputs, notes: &mut Vec<String>, strict: bool) {
    match std::fs::metadata(p) {
        Ok(m) if m.len() > LAB_FILE_MAX => {
            inp.problems.push(format!("{}: larger than {} MiB, not read", p.display(), LAB_FILE_MAX >> 20));
            return;
        }
        Err(e) => {
            inp.problems.push(format!("{}: {e}", p.display()));
            return;
        }
        _ => {}
    }
    let v = match read_json(p) {
        Ok(v) => v,
        Err(e) if strict => {
            inp.problems.push(format!("{e:#} (a lab file that cannot be read proves nothing)"));
            return;
        }
        Err(_) => return,
    };
    let kind = v.get("kind").and_then(Value::as_str).unwrap_or_default().to_string();
    let sha = v.get("exeSha256").and_then(Value::as_str).unwrap_or_default().to_ascii_lowercase();
    let old = inp.old_exe_sha.clone().unwrap_or_default();
    match kind.as_str() {
        roundtrip::KIND => match roundtrip::from_value(v) {
            Ok(s) => inp.suites.push(s),
            Err(e) => inp.problems.push(format!("{}: {e:#}", p.display())),
        },
        values::KIND => match values::from_value(v) {
            Ok(e) if sha == inp.new_exe_sha => inp.values_new = Some(e),
            Ok(e) if !old.is_empty() && sha == old => inp.values_old = Some(e),
            Ok(_) => notes.push(format!("{}: a values export of another build, set aside", p.display())),
            Err(e) => inp.problems.push(format!("{}: {e:#}", p.display())),
        },
        _ if v.get("format").is_some() && v.get("registries").is_some_and(Value::is_object) => {
            match ids::IdTable::from_value(&v, &inp.side, &sha) {
                Ok(t) if t.side == inp.side && sha == inp.new_exe_sha => inp.native_ids = Some(t),
                Ok(t) if t.side == inp.side && !old.is_empty() && sha == old && inp.prev_ids.is_none() => inp.prev_ids = Some(t),
                _ => {}
            }
        }
        _ => {}
    }
}

fn json_files(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|p| p.is_file() && p.extension().is_some_and(|x| x == "json")).collect();
    v.sort();
    v
}

/// Makes and finds every input. Never writes into the Kiln tree.
pub fn gather(g: &Gather) -> Result<Inputs> {
    let store = g.store.clone().unwrap_or_else(|| ids::Store { dir: ids::Store::default_dir() });
    ids::guard(&g.out, &g.tree)?;
    ids::guard(&store.dir, &g.tree)?;
    std::fs::create_dir_all(&g.out).with_context(|| format!("creating {}", g.out.display()))?;
    let map = regmap::load(&g.tree)?;
    let mut inp = Inputs { side: g.side.clone(), map, first_build: g.first_build, accepted: g.accept.clone(), ..Default::default() };
    inp.regmap_digest = regmap::digest(&g.tree)?;
    // the game runs its deployed regmap/common: the tree's must be the same, or the proof judges a mapping no lab ran
    if let Some(k) = &g.deployed {
        match regmap::differences(&g.tree, k) {
            Ok(d) if d.is_empty() => {}
            Ok(d) => inp.problems.push(format!("the tree's regmap/common differs from the deployed {} ({}): redeploy Kiln, or prove \
                                                with --tree set to the Kiln the game runs", k.join("regmap/common").display(), d.join(", "))),
            Err(e) => inp.problems.push(format!("the deployed regmap/common cannot be compared with the tree's: {e:#}")),
        }
    }
    // which builds
    let sha_of = |exe: &Option<PathBuf>, dump: &Option<PathBuf>| -> Result<Option<String>> {
        if let Some(e) = exe {
            return Ok(Some(crate::util::sha256(e)?));
        }
        if let Some(d) = dump {
            return Ok(Some(refl::TypeDb::parse(&std::fs::read_to_string(d)?)?.exe_sha256).filter(|s| !s.is_empty()));
        }
        Ok(None)
    };
    let mut new_sha = sha_of(&g.new_exe, &g.new_refl)?;
    if new_sha.is_none() {
        if let Some(r) = &g.record {
            new_sha = read_json(r)?.get("exeSha256").and_then(Value::as_str).map(str::to_string);
        }
    }
    inp.new_exe_sha = new_sha.context("which build? pass the new exe (or its reflection dump, or its candidate record)")?.to_ascii_lowercase();
    inp.old_exe_sha = sha_of(&g.old_exe, &g.old_refl)?.map(|s| s.to_ascii_lowercase());
    // the reflection
    for (which, dump, exe) in [("new", &g.new_refl, &g.new_exe), ("old", &g.old_refl, &g.old_exe)] {
        let db = match (dump, exe) {
            (Some(p), _) => Some(std::fs::read_to_string(p).map_err(anyhow::Error::from).and_then(|t| refl::TypeDb::parse(&t))
                .with_context(|| format!("{}", p.display()))),
            (None, Some(e)) => Some(dump_refl(g, &inp.map, e, which)),
            _ => None,
        };
        let want = if which == "new" { Some(inp.new_exe_sha.clone()) } else { inp.old_exe_sha.clone() };
        if let (Some(Ok(d)), Some(w)) = (&db, &want) {
            if !d.exe_sha256.is_empty() && !d.exe_sha256.eq_ignore_ascii_case(w) {
                inp.problems.push(format!("the {which} reflection dump is of exe {}, not the {which} exe {}", short(&d.exe_sha256), short(w)));
            }
        }
        match db {
            Some(Ok(d)) if which == "new" => inp.new_refl = Some(d),
            Some(Ok(d)) => inp.old_refl = Some(d),
            Some(Err(e)) => inp.problems.push(format!("the {which} reflection: {e:#}")),
            None => {}
        }
    }
    // the previous build's table: given, else the store's
    if let Some(p) = &g.prev_ids {
        let t = ids::IdTable::parse(&std::fs::read_to_string(p)?, &g.side, inp.old_exe_sha.as_deref().unwrap_or_default())?;
        match &inp.old_exe_sha {
            Some(old) if !t.exe_sha256.eq_ignore_ascii_case(old) || t.side != g.side => {
                inp.problems.push(format!("the previous build's id table {} is of {} {}, not of the old exe {}", p.display(), t.side, short(&t.exe_sha256), short(old)));
            }
            None => inp.problems.push("a previous build's id table without an old build (--old-exe or --old-refl) cannot be checked".into()),
            _ => {}
        }
        inp.prev_ids = Some(t);
    }
    // --first-build skips the id history: refused where this side already has a proven build
    if g.first_build {
        let mut earlier: Vec<String> = [Some(g.tree.clone()), g.deployed.clone()].into_iter().flatten()
            .flat_map(|r| json_files(&r.join("regmap").join(&g.side)))
            .filter_map(|p| read_json(&p).ok()?.get("exeSha256")?.as_str().map(str::to_string))
            .filter(|s| !s.eq_ignore_ascii_case(&inp.new_exe_sha))
            .collect();
        earlier.extend(json_files(&store.dir.join(&g.side)).into_iter()
            .filter_map(|p| p.file_stem()?.to_str()?.strip_prefix(&format!("ids-{}-", g.side)).map(str::to_string))
            .filter(|s| !s.eq_ignore_ascii_case(&inp.new_exe_sha)));
        earlier.sort();
        earlier.dedup();
        if !earlier.is_empty() {
            inp.problems.push(format!("--first-build, but the {} side already has proven builds ({}): the id history would be skipped; \
                                       prove against the last of them instead", g.side,
                                      earlier.iter().map(|s| short(s)).collect::<Vec<_>>().join(", ")));
        }
    }
    // the lab's outputs
    let mut notes = Vec::new();
    for e in &g.evidence {
        if e.is_dir() {
            let reg = e.join("content/registry");
            if reg.is_dir() {
                let harvested = ids::harvest(e, &store)?;
                inp.problems.extend(harvested.problems);
                for h in harvested.tables {
                    if h.differs {
                        notes.push(format!("the store's id table of {} {} differs from the one in {} (the store's is kept)", h.side, short(&h.exe_sha256), e.display()));
                    }
                }
                for p in json_files(&reg) {
                    classify(&p, &mut inp, &mut notes, false);
                }
                for p in json_files(&reg.join("proof")) {
                    classify(&p, &mut inp, &mut notes, true);
                }
            } else {
                for p in json_files(e) {
                    classify(&p, &mut inp, &mut notes, false);
                }
            }
        } else if e.is_file() {
            classify(e, &mut inp, &mut notes, true);
        } else {
            inp.problems.push(format!("evidence {} does not exist", e.display()));
        }
    }
    // the old build's record: the tree's, else the instance's (marked there, not committed yet)
    let mut previous_path = None;
    if let Some(old) = inp.old_exe_sha.clone() {
        let rel = Path::new("regmap").join(&g.side).join(format!("{old}.json"));
        if let Some(p) = [Some(g.tree.clone()), g.deployed.clone()].into_iter().flatten().map(|r| r.join(&rel)).find(|p| p.is_file()) {
            match read_json(&p) {
                Ok(v) if v.get("exeSha256").and_then(Value::as_str).is_some_and(|s| s.eq_ignore_ascii_case(&old))
                    && v.get("side").and_then(Value::as_str) == Some(g.side.as_str()) => {
                    inp.previous = Some(v);
                    previous_path = Some(p);
                }
                Ok(_) => inp.problems.push(format!("{} is not the record of the old build", p.display())),
                Err(e) => inp.problems.push(format!("the old build's record: {e:#}")),
            }
        }
        if inp.prev_ids.is_none() {
            inp.prev_ids = store.get(&g.side, &old).transpose()?;
        }
        if inp.values_old.is_none() {
            inp.values_old = store.get_values(&g.side, &old).transpose()?;
        }
    }
    // the candidate record and this build's table
    match (&g.record, &g.new_exe) {
        (Some(r), _) => inp.candidate = Some(read_json(r)?),
        (None, Some(exe)) => {
            let (rec, table) = run_record(g, exe, &inp.new_exe_sha, previous_path.as_deref(), inp.prev_ids.as_ref(), &mut inp.problems,
                                          &mut inp.notes)?;
            inp.candidate = rec;
            inp.new_ids = table;
        }
        _ => {}
    }
    if let Some(p) = &g.ids {
        inp.new_ids = Some(ids::IdTable::parse(&std::fs::read_to_string(p)?, &g.side, &inp.new_exe_sha)?);
    }
    inp.problems.retain(|x| !x.is_empty());
    if !notes.is_empty() {
        std::fs::write(g.out.join("gather-notes.txt"), notes.join("\n") + "\n")?;
    }
    Ok(inp)
}

impl ids::Store {
    pub fn values_path(&self, side: &str, exe_sha256: &str) -> PathBuf {
        self.dir.join(side).join(format!("values-{side}-{exe_sha256}.json"))
    }
    pub fn get_values(&self, side: &str, exe_sha256: &str) -> Option<Result<values::Export>> {
        let p = self.values_path(side, exe_sha256);
        p.is_file().then(|| std::fs::read_to_string(&p).map_err(anyhow::Error::from).and_then(|t| values::parse(&t)))
    }
    /// Keeps a proven build's export; None when the store already holds one (never overwritten).
    pub fn put_values(&self, e: &values::Export, text: &str) -> Result<Option<PathBuf>> {
        let p = self.values_path(&e.side, &e.exe_sha256);
        if p.exists() {
            return Ok(None);
        }
        ids::write_whole(&p, text)?;
        Ok(Some(p))
    }
}

/// After a verified proof: the new build's id table and values export into the store (the next update's previous build;
/// never overwritten), the id table into a game's Kiln data folder when that game runs this build. An unverified proof
/// or a dry run keeps and deploys nothing.
pub fn keep(p: &Proof, inp: &Inputs, store: &ids::Store, data_dir: Option<&Path>, dry: bool) -> Result<Vec<String>> {
    if dry {
        return Ok(vec!["ids       nothing kept (dry run)".into()]);
    }
    if !p.verified {
        return Ok(vec!["ids       nothing kept: the proof is not verified".into()]);
    }
    let mut lines = Vec::new();
    if let Some(t) = &p.table {
        lines.push(match store.put(t)? {
            Some(path) => format!("ids       kept {}", path.display()),
            None => format!("ids       the store already holds this build's table ({}); kept as it is", store.path(&t.side, &t.exe_sha256).display()),
        });
        if let Some(d) = data_dir {
            lines.push(format!("ids       deployed {}", ids::deploy(t, d)?.display()));
        }
    }
    if let Some(e) = &inp.values_new {
        let text = serde_json::to_string(&serde_json::json!({"format": 1, "kind": values::KIND, "side": e.side, "exeSha256": e.exe_sha256,
            "gameBuild": e.game_build, "regmapDigest": e.regmap_digest, "base": e.base, "registries": e.registries}))?;
        lines.push(match store.put_values(e, &text)? {
            Some(path) => format!("values    kept {}", path.display()),
            None => "values    the store already holds this build's export; kept as it is".into(),
        });
    }
    Ok(lines)
}

// ---- the command and kilnctl update -------------------------------------------------------------------------------

/// The newest cached exe (`<state>/exe-cache/<sha>.exe`) other than `except` whose build has a verified regmap record in
/// the instance's Kiln folder or the tree: the build the regmap was last proven on.
pub fn regmap_baseline(cache: &Path, kiln_dir: &Path, tree: &Path, side: &str, except: &str) -> Option<(PathBuf, String)> {
    let mut v: Vec<(std::time::SystemTime, PathBuf, String)> = std::fs::read_dir(cache)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let sha = e.file_name().to_string_lossy().strip_suffix(".exe")?.to_string();
            if sha.len() != 64 || sha.eq_ignore_ascii_case(except) || !(record_verified(kiln_dir, side, &sha) || record_verified(tree, side, &sha)) {
                return None;
            }
            Some((e.metadata().ok()?.modified().ok()?, e.path(), sha))
        })
        .collect();
    v.sort();
    v.pop().map(|(_, p, s)| (p, s))
}

/// A Kiln folder holds a verified record of this build (`<kiln>/regmap/<side>/<exe sha256>.json`).
pub fn record_verified(kiln_dir: &Path, side: &str, exe_sha256: &str) -> bool {
    read_json(&kiln_dir.join("regmap").join(side).join(format!("{exe_sha256}.json"))).ok().and_then(|v| v.get("verified")?.as_bool()) == Some(true)
}

/// `kilnctl regproof`'s arguments.
#[derive(Debug, Clone, Default)]
pub struct CliArgs {
    pub name: Option<String>,
    pub side: String,
    pub tree: Option<PathBuf>,
    pub new_exe: Option<PathBuf>,
    pub old_exe: Option<PathBuf>,
    pub new_refl: Option<PathBuf>,
    pub old_refl: Option<PathBuf>,
    pub record: Option<PathBuf>,
    pub ids: Option<PathBuf>,
    pub prev_ids: Option<PathBuf>,
    pub evidence: Vec<PathBuf>,
    pub game_build: Option<String>,
    pub steam_build: Option<String>,
    pub out: Option<PathBuf>,
    pub first_build: bool,
    pub accept: Vec<String>,
    pub mark: bool,
    pub replace: bool,
}

/// A server instance's Kiln data folder and its gate copy's (where a lab boot of the new exe wrote its outputs).
fn instance_evidence(inst: &crate::config::Instance) -> Vec<PathBuf> {
    [inst.dir.join("kiln/data"), crate::util::with_suffix(&inst.dir, ".gate").join("kiln/data")].into_iter().filter(|p| p.is_dir()).collect()
}

fn default_out(name: &str, exe: Option<&Path>) -> PathBuf {
    let sha = exe.and_then(|e| crate::util::sha256(e).ok()).map(|s| s[..12].to_string()).unwrap_or_else(|| "build".into());
    crate::util::state_dir().join("regproof").join(name).join(format!("{sha}-{}", crate::util::stamp(std::time::SystemTime::now())))
}

/// Gathers, proves, writes the outputs, keeps the tables, marks when asked and verified. Returns the proof and the record.
fn run(g: &Gather, inst: Option<&crate::config::Instance>, mark_it: bool, replace: bool, dry: bool) -> Result<(Proof, Option<Value>, Vec<String>)> {
    let inp = gather(g)?;
    let p = prove(&inp, &crate::util::fmt_utc(std::time::SystemTime::now()));
    let rec = inp.candidate.as_ref().map(|c| record(&p, c));
    write_outputs(&p, rec.as_ref(), &g.out)?;
    let mut lines = vec![p.summary()];
    let store = g.store.clone().unwrap_or_else(|| ids::Store { dir: ids::Store::default_dir() });
    let runs_it = inst.filter(|i| crate::util::sha256(&i.exe()).is_ok_and(|s| s == p.exe_sha256));
    lines.extend(keep(&p, &inp, &store, runs_it.map(|i| i.dir.join("kiln/data")).as_deref(), dry)?);
    if mark_it && !dry {
        match (inst, &rec) {
            (Some(i), Some(r)) if p.verified => lines.push(match mark(&crate::kiln::kiln_dir(i), &p, r, &g.out, replace)? {
                Marked::Written(d) => format!("regmap    marked verified: {}", d.display()),
                Marked::Kept(d) => format!("regmap    {} is already verified and was kept (--replace replaces it)", d.display()),
            }),
            _ => lines.push(format!("regmap    not marked: {}", p.blockers.first().map(String::as_str).unwrap_or("no candidate record"))),
        }
    }
    lines.push(format!("regproof  {} (report {})", if p.verified { "VERIFIED" } else { "not verified" }, g.out.join("report.txt").display()));
    Ok((p, rec, lines))
}

/// The new build's sha from what names it: the exe, else its reflection dump, else its candidate record.
fn new_sha_hint(g: &Gather) -> Result<Option<String>> {
    if let Some(e) = &g.new_exe {
        return Ok(Some(crate::util::sha256(e)?));
    }
    if let Some(d) = &g.new_refl {
        let sha = refl::TypeDb::parse(&std::fs::read_to_string(d)?)?.exe_sha256;
        if !sha.is_empty() {
            return Ok(Some(sha));
        }
    }
    Ok(match &g.record {
        Some(r) => read_json(r)?.get("exeSha256").and_then(Value::as_str).map(str::to_string),
        None => None,
    })
}

/// `kilnctl regproof`: exit 0 when the record is verified (and marked, with --mark), else 1.
pub fn cli(a: CliArgs) -> Result<i32> {
    if a.side != "server" && a.side != "client" {
        bail!("--side is server or client");
    }
    let inst = a.name.as_deref().map(crate::config::load).transpose()?;
    if inst.is_some() && a.side != "server" {
        bail!("an instance is a server; for the client side pass --tree and the client's exe and lab outputs");
    }
    if a.mark && inst.is_none() {
        bail!("--mark writes the record into an instance's Kiln folder: name the instance");
    }
    let tree = a.tree.clone().or_else(|| inst.as_ref().map(|i| i.kiln.source.clone())).context("which Kiln tree? pass --tree (or an instance)")?;
    let mut g = Gather {
        side: a.side.clone(), tree, deployed: inst.as_ref().map(crate::kiln::kiln_dir), out: PathBuf::new(), store: None, new_exe: a.new_exe, old_exe: a.old_exe, new_refl: a.new_refl,
        old_refl: a.old_refl, record: a.record, ids: a.ids, prev_ids: a.prev_ids, evidence: a.evidence, game_build: a.game_build,
        steam_build: a.steam_build, first_build: a.first_build, accept: a.accept,
    };
    if let Some(i) = &inst {
        if g.new_exe.is_none() && g.new_refl.is_none() && g.record.is_none() {
            g.new_exe = Some(i.exe());
        }
        if g.old_exe.is_none() && g.old_refl.is_none() && !g.first_build {
            let new_sha = new_sha_hint(&g)?.context("which build is new? pass --new-exe (or --new-refl / --record)")?;
            g.old_exe = regmap_baseline(&crate::sigport::cache_dir(), &crate::kiln::kiln_dir(i), &g.tree, "server", &new_sha).map(|(p, _)| p);
        }
        if g.evidence.is_empty() {
            g.evidence = instance_evidence(i);
        }
        if g.steam_build.is_none() {
            g.steam_build = crate::steam::buildid(&i.dir);
        }
    }
    g.out = match a.out {
        Some(o) => o,
        None => default_out(a.name.as_deref().unwrap_or(&g.side), g.new_exe.as_deref()),
    };
    let (p, _, lines) = run(&g, inst.as_ref(), a.mark, a.replace, false)?;
    for l in lines {
        println!("{l}");
    }
    for b in &p.blockers {
        println!("          - {b}");
    }
    Ok(if p.verified && (!a.mark || inst.is_some()) { 0 } else { 1 })
}

/// The registry proof inside `kilnctl update` / `kilnctl port`, right after the lab gate (whose copy holds the lab's
/// outputs): (summary, lines for NEXT.txt). Marks only when `mark` (the sigdb port was complete and the gate green) and
/// the proof is verified. A Kiln tree without regmap/common is skipped.
pub fn flow(inst: &crate::config::Instance, new: &Path, mark_it: bool, dry: bool) -> (String, Vec<String>) {
    if !inst.kiln.source.join("regmap/common").is_dir() {
        return ("no regmap in the Kiln tree; skipped".into(), Vec::new());
    }
    let new_sha = crate::util::sha256(new).unwrap_or_default();
    if record_verified(&crate::kiln::kiln_dir(inst), "server", &new_sha) {
        return (format!("the instance already has a verified regmap record of {}; skipped (kilnctl regproof --mark --replace re-proves it)",
                        short(&new_sha)), Vec::new());
    }
    // the build the regmap was last proven on (not sigport's baseline, which may never have had a regmap record)
    let old = regmap_baseline(&crate::sigport::cache_dir(), &crate::kiln::kiln_dir(inst), &inst.kiln.source, "server", &new_sha).map(|(p, _)| p);
    let g = Gather {
        side: "server".into(), tree: inst.kiln.source.clone(), deployed: Some(crate::kiln::kiln_dir(inst)),
        out: default_out(&inst.name, Some(new)), new_exe: Some(new.to_path_buf()),
        old_exe: old, evidence: instance_evidence(inst), steam_build: crate::steam::buildid(&inst.dir),
        ..Default::default()
    };
    match run(&g, Some(inst), mark_it, false, dry) {
        Ok((p, _, lines)) => {
            for l in &lines[1..] {
                println!("{l}");
            }
            let next = if p.verified {
                Vec::new()
            } else {
                vec![format!("registry proof not verified (every registry field stays refused on this build): {}; see {}",
                             p.blockers.first().map(String::as_str).unwrap_or("?"), g.out.join("NEXT.txt").display())]
            };
            let (ok, n) = p.ok_rows();
            (format!("{} ({ok}/{n} rows ok; {})", if p.verified { "verified" } else { "not verified" }, g.out.display()), next)
        }
        Err(e) => (format!("failed: {e:#}"), vec![format!("the registry proof failed to run: {e:#}")]),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::regproof::ids::tests::table;
    use crate::regproof::refl::tests::{db, old_types};
    use crate::regproof::regmap::tests::sample;
    use crate::regproof::roundtrip::tests::{SHA, full};
    use crate::regproof::values::tests::{OLD, export, old_values};
    use serde_json::json;

    fn candidate() -> Value {
        json!({"regmap": 1, "side": "server", "exeSha256": SHA, "gameBuild": "101", "steamBuild": "9", "verified": true,
               "proof": "regmap_check.py --exe", "layouts": {"demo::Gadget": "0x00001111"},
               "rows": {"gadgets.size": "ok", "gadgets.tier": "ok", "gadgets.power": "ok", "gadgets.label": "ok", "gadgets.glow": "unmapped",
                        "plans.parts": "ok", "plans.parts[].count": "ok", "plans.cap": "ok"},
               "aliases": {"renames": [], "removals": [], "suffixes": []}})
    }

    fn map() -> regmap::RegMap {
        let mut m = sample();
        m.get_mut("plans").unwrap().rows.get_mut("cap").unwrap().conv = "float".into();
        m
    }

    /// A proof where every step has its inputs and passes.
    fn inputs() -> Inputs {
        let mut new_values = old_values();
        new_values["gadgets"]["game:a"]["size"] = json!(40);
        Inputs {
            side: "server".into(),
            new_exe_sha: SHA.into(),
            old_exe_sha: Some(OLD.into()),
            map: map(),
            regmap_digest: crate::regproof::regmap::tests::DIGEST.into(),
            old_refl: Some(db(old_types(), OLD)),
            new_refl: Some(db(old_types(), SHA)),
            candidate: Some(candidate()),
            previous: Some(json!({"regmap": 1, "side": "server", "exeSha256": OLD, "verified": true, "rows": {},
                                  "aliases": {"renames": [], "removals": [], "suffixes": []}})),
            new_ids: Some(table(SHA, &[("g1", "game:a"), ("g2", "game:b")])),
            prev_ids: Some(table(OLD, &[("g1", "game:a"), ("g2", "game:b")])),
            native_ids: Some(table(SHA, &[("g1", "game:a"), ("g2", "game:b")])),
            suites: full(),
            values_old: Some(export(OLD, "100", "none", old_values())),
            values_new: Some(export(SHA, "101", "none", new_values)),
            ..Default::default()
        }
    }

    fn with_power_samples(mut inp: Inputs) -> Inputs {
        // the round trip's formula sample, with raw values a correct game would hold
        let c = formula::Constants::default();
        for s in &mut inp.suites {
            for x in &mut s.samples {
                if x.field == "gadgets.power" {
                    x.raw_before = Some(vec![json!(formula::dmg_mod_for(30, 4, 1.25, &c)), json!(1.25), json!(4)]);
                    x.raw_after = Some(vec![json!(formula::dmg_mod_for(44, 4, 1.25, &c) as f32 as f64), json!(1.25), json!(4)]);
                }
            }
        }
        inp
    }

    #[test]
    fn every_step_green_verifies_the_record_and_proves_every_mapped_row() {
        let inp = with_power_samples(inputs());
        let p = prove(&inp, "T");
        assert!(p.verified, "{:?}", p.blockers);
        for (q, r) in &p.rows {
            let want = if q == "gadgets.glow" { "unmapped" } else { "ok" };
            assert_eq!(r.state, want, "{q}: {r:?}");
        }
        let rec = record(&p, inp.candidate.as_ref().unwrap());
        assert_eq!(rec["verified"], true);
        assert_eq!(rec["rows"]["plans.cap"], "ok");
        assert_eq!(rec["aliases"], candidate()["aliases"], "the candidate's aliases are kept");
        assert_eq!(rec["layouts"], candidate()["layouts"]);
        let s = rec["proof"].as_str().unwrap();
        assert!(s.starts_with("kilnctl regproof (T): reflection 7 same"), "{s}");
        assert!(s.contains("rows ok 7/7"), "{s}");
        assert!(s.contains("values 1 changed"), "{s}");
        assert!(next_text(&p).starts_with("nothing left to do"));
    }

    #[test]
    fn a_missing_step_input_keeps_the_record_unverified() {
        let base = with_power_samples(inputs());
        type Strip = fn(&mut Inputs);
        let cases: Vec<(Strip, &str)> = vec![
            (|i| i.candidate = None, "candidate record"),
            (|i| i.old_refl = None, "old exe"),
            (|i| i.new_refl = None, "new exe"),
            (|i| i.suites.truncate(1), "boot generation"),
            (|i| i.values_new = None, "values export of the new build"),
            (|i| i.values_old = None, "values export of the old build"),
            (|i| i.new_ids = None, "id table of the new build"),
            (|i| i.prev_ids = None, "previous build"),
            (|i| i.native_ids = None, "kiln.content's own id table"),
            (|i| i.problems.push("kfc.py types failed".into()), "kfc.py"),
            (|i| i.values_new.as_mut().unwrap().base.origin.clear(), "base"),
            (|i| i.candidate.as_mut().unwrap()["verified"] = json!(false), "root type"),
            (|i| {
                // the entry is gone from the build: from both tables
                i.new_ids = Some(table(SHA, &[("g1", "game:a")]));
                i.native_ids = Some(table(SHA, &[("g1", "game:a")]));
            }, "removed"),
        ];
        for (strip, word) in cases {
            let mut inp = base.clone();
            strip(&mut inp);
            let p = prove(&inp, "T");
            assert!(!p.verified, "{word}: should block");
            assert!(p.blockers.iter().any(|b| b.contains(word)), "{word:?} not in {:?}", p.blockers);
            assert_eq!(record(&p, &candidate())["verified"], false);
            assert!(next_text(&p).contains(word), "{word}");
        }
    }

    #[test]
    fn a_row_failing_a_step_is_changed_alone() {
        // the formula check fails (the member written is not what the formula gives), the rest stays proven
        let mut inp = with_power_samples(inputs());
        for x in inp.suites.iter_mut().flat_map(|s| s.samples.iter_mut()).filter(|x| x.field == "gadgets.power") {
            x.raw_after = Some(vec![json!(2.3), json!(1.25), json!(4)]);
        }
        let p = prove(&inp, "T");
        assert!(p.verified, "{:?}", p.blockers);
        assert_eq!(p.rows["gadgets.power"].state, "changed");
        assert!(p.rows["gadgets.power"].why.iter().any(|w| w.starts_with("formula:")), "{:?}", p.rows["gadgets.power"]);
        assert_eq!(p.rows["gadgets.size"].state, "ok");
        // regmap_check's own verdict stays
        let mut inp = with_power_samples(inputs());
        inp.candidate.as_mut().unwrap()["rows"]["gadgets.tier"] = json!("unmapped");
        assert_eq!(prove(&inp, "T").rows["gadgets.tier"].state, "unmapped");
        // a failed round trip (the value read back is not the probe)
        let mut rt = with_power_samples(inputs());
        for x in rt.suites[0].samples.iter_mut().filter(|x| x.field == "gadgets.size") {
            x.read_back = Some(json!(20));
        }
        let p2 = prove(&rt, "T");
        assert_eq!(p2.rows["gadgets.size"].state, "changed");
        assert!(p2.rows["gadgets.size"].why[0].starts_with("round trip:"), "{:?}", p2.rows["gadgets.size"]);
        assert_eq!(p2.rows["gadgets.size"].reason, None);
    }

    /// Owner ruling (coordinator, 2026-11-22): a row that only lacks lab evidence is Unproven with its reason and stays
    /// off; Changed ("Unmapped: ... a Kiln update is needed") is for a mapping that is wrong on the build.
    #[test]
    fn a_row_only_lacking_lab_samples_is_unproven_not_changed() {
        let mut inp = with_power_samples(inputs());
        for s in &mut inp.suites {
            s.samples.retain(|x| x.field != "gadgets.size" && x.field != "gadgets.power");
        }
        let p = prove(&inp, "T");
        assert!(p.verified, "the suite is complete; two fields have no sample: {:?}", p.blockers);
        for q in ["gadgets.size", "gadgets.power"] {
            let r = &p.rows[q];
            assert_eq!(r.state, "unproven", "{q}: {r:?}");
            assert!(r.reason.as_deref().is_some_and(|x| x.contains("lab round trip")), "{q}: {r:?}");
            assert!(r.why.iter().any(|w| w.contains("no round-trip sample")), "{q}: {r:?}");
        }
        let rec = record(&p, &candidate());
        assert_eq!(rec["rows"]["gadgets.size"], "unproven");
        assert_eq!(rec["rows"]["gadgets.tier"], "ok");
        assert!(rec["reasons"]["gadgets.power"].as_str().is_some_and(|x| x.contains("lab round trip")), "{rec}");
        assert!(rec["reasons"].get("gadgets.tier").is_none(), "only unproven rows carry a reason");
        assert!(p.summary().contains("2 unproven"), "{}", p.summary());
        assert!(next_text(&p).contains("gadgets.size: unproven"), "{}", next_text(&p));
        assert!(report_text(&p).contains("gadgets.power: unproven"), "{}", report_text(&p));
        // the formula's constants missing as well: still evidence, still unproven
        let mut nc = inp.clone();
        nc.values_new.as_mut().unwrap().registries.remove("balance");
        for s in &mut nc.suites {
            s.constants.clear();
        }
        assert_eq!(prove(&nc, "T").rows["gadgets.power"].state, "unproven");
        // a reflection change wins over missing samples: that mapping is wrong, not just unproven
        let mut n = old_types();
        let at = n.iter().position(|t| t["name"] == "demo::Gadget").unwrap();
        n[at]["fields"][0]["name"] = json!("maxStack");
        inp.new_refl = Some(db(n, SHA));
        let p2 = prove(&inp, "T");
        assert_eq!(p2.rows["gadgets.size"].state, "changed");
        assert_eq!(record(&p2, &candidate())["rows"]["gadgets.size"], "changed");
        // a record without unproven rows has no reasons member
        let all = prove(&with_power_samples(inputs()), "T");
        assert!(record(&all, &candidate()).get("reasons").is_none());
    }

    #[test]
    fn the_old_builds_record_carries_the_removal_history() {
        // no record of the old build: removals made then could be given again unseen
        let mut inp = with_power_samples(inputs());
        inp.previous = None;
        let p = prove(&inp, "T");
        assert!(!p.verified && p.blockers.iter().any(|b| b.contains("record of the old build")), "{:?}", p.blockers);
        // the old build retired game:z; the candidate lost that and the new build gives game:z out again
        let mut reuse = with_power_samples(inputs());
        reuse.previous.as_mut().unwrap()["aliases"]["removals"] = json!([{"registry": "gadgets", "id": "game:z", "removedIn": "100"}]);
        for t in [reuse.new_ids.as_mut().unwrap(), reuse.native_ids.as_mut().unwrap()] {
            t.registries.get_mut("gadgets").unwrap().insert("g9".into(), "game:z".into());
        }
        let p = prove(&reuse, "T");
        assert!(!p.verified && p.blockers.iter().any(|b| b.contains("game:z")), "{:?}", p.blockers);
        // a first build has no previous record to carry
        let mut first = with_power_samples(inputs());
        first.previous = None;
        first.first_build = true;
        first.old_refl = None;
        first.values_old = None;
        first.prev_ids = None;
        first.old_exe_sha = None;
        assert!(prove(&first, "T").verified);
    }

    #[test]
    fn the_default_old_build_has_a_verified_regmap_record() {
        let root = scratch("baseline");
        let (cache, kiln, tree) = (root.join("cache"), root.join("game/kiln"), root.join("tree"));
        std::fs::create_dir_all(&cache).unwrap();
        let (a, b, c) = ("a".repeat(64), "b".repeat(64), "c".repeat(64));
        let at = |sha: &str, secs: u64| {
            let p = cache.join(format!("{sha}.exe"));
            std::fs::write(&p, "MZ").unwrap();
            std::fs::File::options().write(true).open(&p).unwrap()
                .set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(secs)).unwrap();
        };
        at(&a, 1_000);
        at(&b, 2_000);
        at(&c, 3_000); // the newest, but only sigdb-verified: no regmap record
        let rec = |dir: &Path, sha: &str| {
            let p = dir.join(format!("regmap/server/{sha}.json"));
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, json!({"regmap": 1, "verified": true}).to_string()).unwrap();
        };
        rec(&kiln, &a);
        rec(&tree, &b);
        assert_eq!(regmap_baseline(&cache, &kiln, &tree, "server", &c).map(|(_, s)| s), Some(b.clone()));
        assert_eq!(regmap_baseline(&cache, &kiln, &tree, "server", &b).map(|(_, s)| s), Some(a.clone()), "never the new build");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_old_and_the_new_build_are_two_builds() {
        let mut inp = with_power_samples(inputs());
        inp.old_exe_sha = Some(SHA.into());
        let p = prove(&inp, "T");
        assert!(!p.verified && p.blockers.iter().any(|b| b.contains("same exe")), "{:?}", p.blockers);
    }

    #[test]
    fn a_values_export_of_another_mapping_proves_nothing() {
        let mut inp = with_power_samples(inputs());
        inp.values_new.as_mut().unwrap().regmap_digest = "e2".repeat(32);
        let p = prove(&inp, "T");
        assert!(!p.verified);
        assert!(p.blockers.iter().any(|b| b.contains("another mapping")), "{:?}", p.blockers);
    }

    #[test]
    fn a_reflection_change_needs_a_review() {
        let mut inp = with_power_samples(inputs());
        let mut n = old_types();
        let at = n.iter().position(|t| t["name"] == "demo::Part").unwrap();
        n[at] = json!({"name": "demo::Part", "kind": refl::K_STRUCT, "size": 4, "layout": 7, "inner": null,
                       "fields": [{"name": "amount", "type": "demo::u32", "offset": 0}]});
        inp.new_refl = Some(db(n, SHA));
        let p = prove(&inp, "T");
        assert!(p.verified, "a changed row is refused alone: {:?}", p.blockers);
        assert_eq!(p.rows["plans.parts[].count"].state, "changed");
        assert!(next_text(&p).contains("review plans.parts[].count"), "{}", next_text(&p));
        inp.accepted = vec!["plans.parts[].count".into()];
        let p2 = prove(&inp, "T");
        assert_eq!(p2.rows["plans.parts[].count"].state, "ok");
        assert!(p2.summary().contains("reviewed: plans.parts[].count"));
    }

    #[test]
    fn a_first_build_needs_no_previous_build() {
        let mut inp = with_power_samples(inputs());
        inp.old_refl = None;
        inp.values_old = None;
        inp.prev_ids = None;
        inp.old_exe_sha = None;
        assert!(!prove(&inp, "T").verified);
        inp.first_build = true;
        let p = prove(&inp, "T");
        assert!(p.verified, "{:?}", p.blockers);
        assert!(p.summary().contains("first build"));
        assert!(p.values.is_none());
        // the new export's origin is still checked
        inp.values_new.as_mut().unwrap().base.writer.clear();
        assert!(!prove(&inp, "T").verified);
    }

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("kilnctl-regproof-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn outputs_and_the_mark_fail_closed() {
        let root = scratch("mark");
        let out = root.join("out");
        let kiln = root.join("game/kiln");
        let inp = with_power_samples(inputs());
        let p = prove(&inp, "T");
        let rec = record(&p, &candidate());
        write_outputs(&p, Some(&rec), &out).unwrap();
        for f in ["proof.json", "refl-diff.json", "roundtrip.json", "formula.json", "values-diff.json", "values-changelog.txt", "aliases.json",
                  "report.txt", "NEXT.txt"] {
            assert!(out.join(f).is_file(), "{f}");
        }
        assert!(out.join(format!("regmap-server-{SHA}.json")).is_file());
        let dst = kiln.join(format!("regmap/server/{SHA}.json"));
        assert_eq!(mark(&kiln, &p, &rec, &out, false).unwrap(), Marked::Written(dst.clone()));
        assert_eq!(read_json(&dst).unwrap()["verified"], true);
        assert!(out.join(format!("verified-regmap-server-{SHA}.json")).is_file());
        // a verified record is kept unless the caller replaces it (a fuller proof of the same build); kept is no error
        let mut newer = rec.clone();
        newer["proof"] = json!("a later proof");
        assert_eq!(mark(&kiln, &p, &newer, &out, false).unwrap(), Marked::Kept(dst.clone()));
        assert_ne!(read_json(&dst).unwrap()["proof"], "a later proof");
        assert_eq!(mark(&kiln, &p, &newer, &out, true).unwrap(), Marked::Written(dst.clone()));
        assert_eq!(read_json(&dst).unwrap()["proof"], "a later proof");
        assert_eq!(read_json(&dst.with_extension("json.bak")).unwrap()["verified"], true, "the replaced record is kept as .bak");
        // an unverified proof marks nothing
        let mut bad = inp.clone();
        bad.native_ids = None;
        let pb = prove(&bad, "T");
        let kiln2 = root.join("game2/kiln");
        assert!(mark(&kiln2, &pb, &record(&pb, &candidate()), &out, true).is_err());
        assert!(!kiln2.exists());
        assert!(report_text(&pb).contains("NOT verified"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn keep_stores_the_new_builds_table_and_values_and_deploys_the_table() {
        let root = scratch("keep");
        let store = ids::Store { dir: root.join("store") };
        let inp = with_power_samples(inputs());
        let p = prove(&inp, "T");
        let data = root.join("game/kiln/data");
        let lines = keep(&p, &inp, &store, Some(&data), false).unwrap();
        assert_eq!(lines.len(), 3, "{lines:?}");
        assert_eq!(store.get("server", SHA).unwrap().unwrap(), inp.new_ids.clone().unwrap());
        assert!(ids::data_path(&data, "server", SHA).is_file());
        let v = store.get_values("server", SHA).unwrap().unwrap();
        assert_eq!(v.registries["gadgets"]["game:a"]["size"], 40);
        // a failed alias check deploys nothing
        let mut bad = inp.clone();
        bad.new_ids = Some(table(SHA, &[("g1", "game:a")]));
        bad.native_ids = Some(table(SHA, &[("g1", "game:a")]));
        let pb = prove(&bad, "T");
        let data2 = root.join("game2/kiln/data");
        keep(&pb, &bad, &store, Some(&data2), false).unwrap();
        assert!(!data2.exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn keep_keeps_only_verified_proofs_never_overwrites_and_skips_dry_runs() {
        let root = scratch("keep2");
        let store = ids::Store { dir: root.join("store") };
        let data = root.join("game/kiln/data");
        // an unverified proof keeps nothing: its table would be the next update's "previous build"
        let mut unverified = with_power_samples(inputs());
        unverified.native_ids = None;
        let pu = prove(&unverified, "T");
        assert!(!pu.verified);
        keep(&pu, &unverified, &store, Some(&data), false).unwrap();
        assert!(store.get("server", SHA).is_none() && store.get_values("server", SHA).is_none() && !data.exists());
        // a dry run keeps and deploys nothing either
        let inp = with_power_samples(inputs());
        let p = prove(&inp, "T");
        keep(&p, &inp, &store, Some(&data), true).unwrap();
        assert!(store.get("server", SHA).is_none() && !data.exists());
        // kept once, never overwritten
        keep(&p, &inp, &store, Some(&data), false).unwrap();
        let first = std::fs::read_to_string(store.path("server", SHA)).unwrap();
        let mut other = p.clone();
        other.table.as_mut().unwrap().registries.get_mut("gadgets").unwrap().insert("g3".into(), "game:c".into());
        let lines = keep(&other, &inp, &store, None, false).unwrap();
        assert_eq!(std::fs::read_to_string(store.path("server", SHA)).unwrap(), first);
        assert!(lines.iter().any(|l| l.contains("already")), "{lines:?}");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A Kiln tree with fake python tools (regmap_entries.py exits 1; regmap_check.py as `check` says) and a verified
    /// record of the build already in it, as the shipped build has; returns (gather, exe sha).
    fn fake_tools(root: &Path, check: &str) -> (Gather, String) {
        let tree = root.join("kiln");
        let w = |p: &Path, t: &str| {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, t).unwrap();
        };
        w(&tree.join("regmap/common/gadgets.json"), r#"{"regmap": 1, "registry": "gadgets",
            "entries": {"kind": "resource", "resourceType": "demo::Gadget"}, "rows": {"size": {"paths": ["stackLimit"], "conv": "int", "backend": "live"}}}"#);
        w(&tree.join("schema/registry/gadgets.json"), "{}");
        let exe = root.join("game/server.bin");
        w(&exe, "MZ synthetic");
        let sha = crate::util::sha256(&exe).unwrap();
        w(&tree.join(format!("regmap/server/{sha}.json")), &json!({"regmap": 1, "side": "server", "exeSha256": sha, "verified": true,
            "rows": {"gadgets.size": "ok"}, "stale": true}).to_string());
        w(&tree.join("tools/registrygen/regmap_entries.py"), "import sys\nsys.exit(1)\n");
        w(&tree.join("tools/registrygen/regmap_check.py"), check);
        w(&root.join("dumps/new.json"), &json!({"exe_sha256": sha, "types": []}).to_string());
        w(&root.join("dumps/old.json"), &json!({"exe_sha256": OLD, "types": []}).to_string());
        let g = Gather {
            side: "server".into(), tree, out: root.join("out"), store: Some(ids::Store { dir: root.join("store") }), new_exe: Some(exe),
            new_refl: Some(root.join("dumps/new.json")), old_refl: Some(root.join("dumps/old.json")), ..Default::default()
        };
        (g, sha)
    }

    /// regmap_check.py writing this run's record (`fresh`) into --root, then exiting with `code`.
    fn writing_check(code: u8) -> String {
        format!("import sys, json, hashlib, pathlib\na = sys.argv\nroot = pathlib.Path(a[a.index('--root') + 1])\n\
                 side = a[a.index('--side') + 1]\nsha = hashlib.sha256(open(a[a.index('--exe') + 1], 'rb').read()).hexdigest()\n\
                 p = root / 'regmap' / side / (sha + '.json')\np.parent.mkdir(parents=True, exist_ok=True)\n\
                 p.write_text(json.dumps({{'regmap': 1, 'side': side, 'exeSha256': sha, 'verified': True, 'rows': {{'gadgets.size': 'ok'}}, 'fresh': True}}))\n\
                 sys.exit({code})\n")
    }

    #[test]
    fn a_failed_regmap_check_never_leaves_an_old_record_as_the_candidate() {
        let root = scratch("m2");
        // exits 1 and writes nothing: the tree's old record must not stand in
        let (g, _) = fake_tools(&root, "import sys\nsys.exit(1)\n");
        let inp = gather(&g).unwrap();
        assert!(inp.candidate.is_none(), "{:?}", inp.candidate);
        assert!(inp.problems.iter().any(|p| p.contains("wrote no record")), "{:?}", inp.problems);
        // writes a record but fails: not a candidate either
        let (g, _) = fake_tools(&root, &writing_check(3));
        let inp = gather(&g).unwrap();
        assert!(inp.candidate.is_none(), "{:?}", inp.candidate);
        assert!(inp.problems.iter().any(|p| p.contains("failed") && p.contains("3")), "{:?}", inp.problems);
        // exit 1 with this run's record: errors in rows (listed unmapped); the fresh record is the candidate
        let (g, _) = fake_tools(&root, &writing_check(1));
        let inp = gather(&g).unwrap();
        assert_eq!(inp.candidate.as_ref().and_then(|c| c.get("fresh")), Some(&json!(true)), "{:?}", inp.candidate);
        assert!(inp.notes.iter().any(|n| n.contains("errors")), "{:?}", inp.notes);
        // nothing of python lands in the tree (python3 -B)
        assert!(!g.tree.join("tools/registrygen/__pycache__").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// Inputs named by file are tied to their builds; --first-build is refused where the side has proven builds; lab files
    /// that cannot be read are named.
    #[test]
    fn gather_ties_inputs_to_their_builds_and_names_bad_lab_files() {
        let root = scratch("ties");
        let (g, sha) = fake_tools(&root, &writing_check(0));
        // a reflection dump of another exe beside the new exe
        let mismatch = gather(&Gather { new_refl: Some(g.old_refl.clone().unwrap()), ..g.clone() }).unwrap();
        assert!(mismatch.problems.iter().any(|p| p.contains("reflection dump") && p.contains("new")), "{:?}", mismatch.problems);
        // a previous table of another build
        let prev = root.join("prev.json");
        std::fs::write(&prev, json!({"format": 1, "side": "server", "exeSha256": sha, "registries": {"gadgets": {}}}).to_string()).unwrap();
        let wrong = gather(&Gather { prev_ids: Some(prev), ..g.clone() }).unwrap();
        assert!(wrong.problems.iter().any(|p| p.contains("previous build's id table")), "{:?}", wrong.problems);
        // --first-build where this side already has a proven build (the tree's record of the build itself does not count)
        std::fs::write(g.tree.join(format!("regmap/server/{OLD}.json")), json!({"regmap": 1, "side": "server", "exeSha256": OLD, "verified": true}).to_string()).unwrap();
        let first = gather(&Gather { first_build: true, old_refl: None, ..g.clone() }).unwrap();
        assert!(first.problems.iter().any(|p| p.contains("--first-build")), "{:?}", first.problems);
        // a truncated suite and an oversized file under proof/ are named, not skipped
        let data = root.join("gate/kiln/data/content/registry/proof");
        std::fs::create_dir_all(&data).unwrap();
        std::fs::write(data.join("live.json"), "{\"format\": 1, \"kind\": \"registry-round").unwrap();
        std::fs::File::create(data.join("huge.json")).unwrap().set_len(300 << 20).unwrap();
        let bad = gather(&Gather { evidence: vec![root.join("gate/kiln/data")], ..g.clone() }).unwrap();
        assert!(bad.problems.iter().any(|p| p.contains("live.json")), "{:?}", bad.problems);
        assert!(bad.problems.iter().any(|p| p.contains("huge.json") && p.contains("MiB")), "{:?}", bad.problems);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn gather_finds_lab_outputs_by_content_and_never_writes_into_the_tree() {
        let root = scratch("gather");
        let tree = root.join("kiln");
        std::fs::create_dir_all(tree.join("regmap/common")).unwrap();
        std::fs::create_dir_all(tree.join(".git")).unwrap();
        std::fs::write(tree.join("regmap/common/gadgets.json"), r#"{"regmap": 1, "registry": "gadgets",
            "entries": {"kind": "resource", "resourceType": "demo::Gadget"}, "rows": {"size": {"paths": ["stackLimit"], "conv": "int", "backend": "live"}}}"#).unwrap();
        let w = |p: &Path, v: &Value| {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, v.to_string()).unwrap();
        };
        let dumps = root.join("dumps");
        w(&dumps.join("new.json"), &json!({"exe_sha256": SHA, "types": []}));
        w(&dumps.join("old.json"), &json!({"exe_sha256": OLD, "types": []}));
        w(&dumps.join("record.json"), &candidate());
        // a lab's Kiln data folder: kiln.content's id tables, the producer's suite and export
        let data = root.join("gate/kiln/data");
        ids::deploy(&table(SHA, &[("g1", "game:a")]), &data).unwrap();
        ids::deploy(&table(OLD, &[("g1", "game:a")]), &data).unwrap();
        let suite = json!({"format": 1, "kind": "registry-roundtrip", "side": "server", "exeSha256": SHA, "generation": "live",
                           "regmapDigest": regmap::digest(&tree).unwrap(), "samples": []});
        w(&data.join("content/registry/proof/rt-live.json"), &suite);
        w(&data.join("content/registry/proof/values.json"),
          &json!({"format": 1, "kind": "registry-values", "side": "server", "exeSha256": SHA, "base": {"origin": "live", "writer": "none"}, "registries": {}}));
        let store = ids::Store { dir: root.join("store") };
        store.put_values(&export(OLD, "100", "none", old_values()), &json!({"format": 1, "kind": "registry-values", "side": "server",
            "exeSha256": OLD, "base": {"origin": "live", "writer": "none"}, "registries": {}}).to_string()).unwrap();
        let g = Gather {
            side: "server".into(), tree: tree.clone(), out: root.join("out"), store: Some(store.clone()),
            new_refl: Some(dumps.join("new.json")), old_refl: Some(dumps.join("old.json")), record: Some(dumps.join("record.json")),
            evidence: vec![data.clone()], ..Default::default()
        };
        let inp = gather(&g).unwrap();
        assert_eq!((inp.new_exe_sha.as_str(), inp.old_exe_sha.as_deref()), (SHA, Some(OLD)));
        assert_eq!(inp.regmap_digest, regmap::digest(&tree).unwrap());
        assert_eq!(inp.suites.len(), 1);
        assert!(inp.values_new.is_some() && inp.values_old.is_some(), "the old export from the store");
        assert_eq!(inp.native_ids.as_ref().unwrap().exe_sha256, SHA);
        assert_eq!(inp.prev_ids.as_ref().unwrap().exe_sha256, OLD);
        assert!(store.get_any("server", OLD).is_some(), "the data folder's tables were harvested into the store");
        assert!(inp.problems.is_empty(), "{:?}", inp.problems);
        // the game runs another regmap/common than the tree's: the proof would judge a mapping no lab ran
        let kiln = root.join("game/kiln");
        std::fs::create_dir_all(kiln.join("regmap/common")).unwrap();
        std::fs::copy(tree.join("regmap/common/gadgets.json"), kiln.join("regmap/common/gadgets.json")).unwrap();
        assert!(gather(&Gather { deployed: Some(kiln.clone()), ..g.clone() }).unwrap().problems.is_empty());
        // the old build's record only in the instance (marked, not committed yet): found there
        std::fs::create_dir_all(kiln.join("regmap/server")).unwrap();
        std::fs::write(kiln.join(format!("regmap/server/{OLD}.json")),
                       json!({"regmap": 1, "side": "server", "exeSha256": OLD, "verified": true, "rows": {}}).to_string()).unwrap();
        let found = gather(&Gather { deployed: Some(kiln.clone()), ..g.clone() }).unwrap();
        assert_eq!(found.previous.as_ref().and_then(|p| p["exeSha256"].as_str()), Some(OLD), "{:?}", found.problems);
        std::fs::write(kiln.join("regmap/common/gadgets.json"), "{}").unwrap();
        let stale = gather(&Gather { deployed: Some(kiln.clone()), ..g.clone() }).unwrap();
        assert!(stale.problems.iter().any(|p| p.contains("deployed") && p.contains("gadgets.json")), "{:?}", stale.problems);
        // outputs inside the tree are refused
        let bad = Gather { out: tree.join("out"), ..g.clone() };
        assert!(gather(&bad).is_err());
        assert!(!tree.join("out").exists());
        let _ = std::fs::remove_dir_all(&root);
    }
}
