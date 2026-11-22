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
//!    in its generation and (a formula row) the formula check passed. Everything else is refused per entry at run time.
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
    pub old_refl: Option<refl::TypeDb>,
    pub new_refl: Option<refl::TypeDb>,
    /// regmap_check.py's record of the new build (its rows, layouts and aliases)
    pub candidate: Option<Value>,
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
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct RowVerdict {
    /// ok | changed | unmapped (regmap/README.md)
    pub state: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub why: Vec<String>,
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
    &s[..s.len().min(12)]
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
    p.roundtrip = roundtrip::check(&inp.map, &inp.side, &inp.new_exe_sha, &inp.suites);
    if !p.roundtrip.complete {
        p.blockers.push(format!("the round-trip suite is incomplete: no {} generation of this build (the lab's proof producer writes one per generation)",
                                p.roundtrip.missing.join(" and no ")));
    }
    p.notes.extend(p.roundtrip.problems.iter().map(|x| format!("round trip: {x}")));
    // 2. formulas
    let mut set_aside = Vec::new();
    let usable = roundtrip::usable(&inp.side, &inp.new_exe_sha, &inp.suites, &mut set_aside);
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
    // 5. aliases
    let aliases = inp.candidate.as_ref().map(ids::Aliases::of_record).unwrap_or_default();
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
            let r = ids::check(prev.as_ref().unwrap_or(&empty), now, &aliases, inp.native_ids.as_ref());
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
        let state = if !row.has_mapping() {
            "unmapped"
        } else {
            match cand_rows.get(&q).map(String::as_str) {
                Some("ok") => {
                    match p.refl.as_ref().map(|r| r.rows.get(&q)) {
                        Some(Some(d)) if d.status == refl::Status::Changed => {
                            if accepted.contains(&q) {
                                p.notes.push(format!("{q}: its reflection change was reviewed (--accept)"));
                            } else {
                                why.extend(d.changes.iter().map(|c| format!("reflection: {c}")));
                                why.extend(d.proposals.iter().map(|x| format!("proposal: {} -> {} ({})", x.was, x.now, x.why)));
                            }
                        }
                        Some(None) => why.push("reflection: not diffed".into()),
                        _ => {}
                    }
                    match p.roundtrip.fields.get(&q) {
                        Some(f) if f.ok => {}
                        Some(f) => why.extend(f.problems.iter().map(|x| format!("round trip: {x}"))),
                        None => why.push("round trip: not checked".into()),
                    }
                    if row.conv == "formula" {
                        match p.formula.rows.get(&q) {
                            Some(f) if f.ok => {}
                            Some(f) => why.extend(f.problems.iter().map(|x| format!("formula: {x}"))),
                            None => why.push("formula: not checked".into()),
                        }
                    }
                    if why.is_empty() { "ok" } else { "changed" }
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
        p.rows.insert(q, RowVerdict { state: state.into(), why });
    }
    p.accepted = accepted.into_iter().cloned().collect();
    p.verified = p.blockers.is_empty();
    p
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
        parts.push(format!("rows ok {ok}/{n}"));
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
        t.push_str(&format!("  {q}: {}\n", r.state));
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
/// copy for the repository into `out`. Refuses an unverified proof and never replaces a verified record.
pub fn mark(kiln_dir: &Path, p: &Proof, record: &Value, out: &Path) -> Result<PathBuf> {
    if !p.verified || record.get("verified").and_then(Value::as_bool) != Some(true) {
        bail!("the proof is not verified: nothing is marked ({})", p.blockers.first().map(String::as_str).unwrap_or("?"));
    }
    let dst = kiln_dir.join("regmap").join(&p.side).join(format!("{}.json", p.exe_sha256));
    if let Ok(text) = std::fs::read_to_string(&dst) {
        if serde_json::from_str::<Value>(&text).ok().and_then(|v| v.get("verified")?.as_bool()) == Some(true) {
            bail!("{} is already verified; not replaced", dst.display());
        }
    }
    let text = serde_json::to_string_pretty(record)? + "\n";
    ids::write_whole(&dst, &text)?;
    ids::write_whole(&out.join(format!("verified-regmap-{}-{}.json", p.side, p.exe_sha256)), &text)?;
    Ok(dst)
}

// ---- making and finding the inputs ---------------------------------------------------------------------------------

/// Where the inputs come from. Paths given win over what kilnctl would make or find.
#[derive(Debug, Clone, Default)]
pub struct Gather {
    pub side: String,
    /// the Kiln tree: regmap/common, schema/, tools/
    pub tree: PathBuf,
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

fn python(tree: &Path, script: &str, args: &[&std::ffi::OsStr], out: &Path, log: &str) -> Result<std::process::ExitStatus> {
    let tool = tree.join(script);
    if !tool.is_file() {
        bail!("{} not found in the Kiln tree", tool.display());
    }
    let logf = std::fs::File::create(out.join(log)).with_context(|| format!("creating {}", out.join(log).display()))?;
    let err = logf.try_clone()?;
    Command::new("python3")
        .arg("-I")
        .arg(&tool)
        .args(args)
        .env("KILN_OUT", out.join("kfc"))
        .stdout(logf)
        .stderr(err)
        .status()
        .with_context(|| format!("running python3 {script}"))
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
        if e.file_type()?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst).with_context(|| format!("copying {}", src.display()))?;
        }
    }
    Ok(())
}

/// The candidate record and the new build's id table from regmap_check.py --record (in a scratch copy of the tree's
/// regmap/ and schema/, so nothing is written into the tree).
fn run_record(g: &Gather, exe: &Path, new_sha: &str, old_sha: Option<&str>, prev_ids: Option<&ids::IdTable>, problems: &mut Vec<String>)
    -> Result<(Option<Value>, Option<ids::IdTable>)> {
    let root = g.out.join("root");
    let _ = std::fs::remove_dir_all(&root);
    copy_tree(&g.tree.join("regmap"), &root.join("regmap"))?;
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
    if let Some(prev) = old_sha.map(|s| g.tree.join("regmap").join(&g.side).join(format!("{s}.json"))).filter(|p| p.is_file()) {
        args.extend(["--previous".into(), prev.into_os_string()]);
    }
    if let Some(t) = prev_ids {
        let p = g.out.join("ids-prev.json");
        ids::write_whole(&p, &t.to_json())?;
        args.extend(["--ids-in".into(), p.into_os_string()]);
    }
    let refs: Vec<&std::ffi::OsStr> = args.iter().map(|a| a.as_os_str()).collect();
    let st = python(&g.tree, "tools/registrygen/regmap_check.py", &refs, &g.out, "regmap_check.log")?;
    let rec = root.join("regmap").join(&g.side).join(format!("{new_sha}.json"));
    if !rec.is_file() {
        problems.push(format!("regmap_check.py --record wrote no record ({st}; {})", g.out.join("regmap_check.log").display()));
        return Ok((None, None));
    }
    let table = if have_entries && ids_out.is_file() {
        Some(ids::IdTable::parse(&std::fs::read_to_string(&ids_out)?, &g.side, new_sha)?)
    } else {
        None
    };
    Ok((Some(read_json(&rec)?), table))
}

/// A lab output by its content: a round-trip suite, a values export or an id table.
fn classify(p: &Path, inp: &mut Inputs, notes: &mut Vec<String>) {
    let Ok(v) = read_json(p) else { return };
    let kind = v.get("kind").and_then(Value::as_str).unwrap_or_default();
    let sha = v.get("exeSha256").and_then(Value::as_str).unwrap_or_default().to_ascii_lowercase();
    let old = inp.old_exe_sha.clone().unwrap_or_default();
    let text = v.to_string();
    match kind {
        roundtrip::KIND => match roundtrip::parse(&text) {
            Ok(s) => inp.suites.push(s),
            Err(e) => inp.problems.push(format!("{}: {e:#}", p.display())),
        },
        values::KIND => match values::parse(&text) {
            Ok(e) if sha == inp.new_exe_sha => inp.values_new = Some(e),
            Ok(e) if !old.is_empty() && sha == old => inp.values_old = Some(e),
            Ok(_) => notes.push(format!("{}: a values export of another build, set aside", p.display())),
            Err(e) => inp.problems.push(format!("{}: {e:#}", p.display())),
        },
        _ if v.get("format").is_some() && v.get("registries").is_some_and(Value::is_object) => {
            match ids::IdTable::parse(&text, &inp.side, &sha) {
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
        match db {
            Some(Ok(d)) if which == "new" => inp.new_refl = Some(d),
            Some(Ok(d)) => inp.old_refl = Some(d),
            Some(Err(e)) => inp.problems.push(format!("the {which} reflection: {e:#}")),
            None => {}
        }
    }
    // the previous build's table: given, else the store's
    if let Some(p) = &g.prev_ids {
        inp.prev_ids = Some(ids::IdTable::parse(&std::fs::read_to_string(p)?, &g.side, inp.old_exe_sha.as_deref().unwrap_or_default())?);
    }
    // the lab's outputs
    let mut notes = Vec::new();
    for e in &g.evidence {
        if e.is_dir() {
            let reg = e.join("content/registry");
            if reg.is_dir() {
                for h in ids::harvest(e, &store)? {
                    if h.differs {
                        notes.push(format!("the store's id table of {} {} differs from the one in {} (the store's is kept)", h.side, short(&h.exe_sha256), e.display()));
                    }
                }
                for p in json_files(&reg).into_iter().chain(json_files(&reg.join("proof"))) {
                    classify(&p, &mut inp, &mut notes);
                }
            } else {
                for p in json_files(e) {
                    classify(&p, &mut inp, &mut notes);
                }
            }
        } else if e.is_file() {
            classify(e, &mut inp, &mut notes);
        } else {
            inp.problems.push(format!("evidence {} does not exist", e.display()));
        }
    }
    if let Some(old) = inp.old_exe_sha.clone() {
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
            let (rec, table) = run_record(g, exe, &inp.new_exe_sha, inp.old_exe_sha.as_deref(), inp.prev_ids.as_ref(), &mut inp.problems)?;
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
    pub fn put_values(&self, e: &values::Export, text: &str) -> Result<PathBuf> {
        let p = self.values_path(&e.side, &e.exe_sha256);
        ids::write_whole(&p, text)?;
        Ok(p)
    }
}

/// After a proof: the new build's id table and values export into the store (the next update's previous build), the id
/// table into a game's Kiln data folder when that game runs this build and the alias check passed.
pub fn keep(p: &Proof, inp: &Inputs, store: &ids::Store, data_dir: Option<&Path>) -> Result<Vec<String>> {
    let mut lines = Vec::new();
    if let Some(t) = p.table.as_ref().or(inp.new_ids.as_ref()) {
        lines.push(format!("ids       kept {}", store.put(t)?.display()));
        if let (Some(d), Some(a)) = (data_dir, &p.aliases) {
            if a.ok {
                lines.push(format!("ids       deployed {}", ids::deploy(t, d)?.display()));
            }
        }
    }
    if let Some(e) = &inp.values_new {
        if p.values.as_ref().is_none_or(|d| d.ok) {
            let text = serde_json::to_string(&serde_json::json!({"format": 1, "kind": values::KIND, "side": e.side, "exeSha256": e.exe_sha256,
                "gameBuild": e.game_build, "base": e.base, "registries": e.registries}))?;
            lines.push(format!("values    kept {}", store.put_values(e, &text)?.display()));
        }
    }
    Ok(lines)
}

// ---- the command and kilnctl update -------------------------------------------------------------------------------

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
fn run(g: &Gather, inst: Option<&crate::config::Instance>, mark_it: bool) -> Result<(Proof, Option<Value>, Vec<String>)> {
    let inp = gather(g)?;
    let p = prove(&inp, &crate::util::fmt_utc(std::time::SystemTime::now()));
    let rec = inp.candidate.as_ref().map(|c| record(&p, c));
    write_outputs(&p, rec.as_ref(), &g.out)?;
    let mut lines = vec![p.summary()];
    let store = g.store.clone().unwrap_or_else(|| ids::Store { dir: ids::Store::default_dir() });
    let runs_it = inst.filter(|i| crate::util::sha256(&i.exe()).is_ok_and(|s| s == p.exe_sha256));
    lines.extend(keep(&p, &inp, &store, runs_it.map(|i| i.dir.join("kiln/data")).as_deref())?);
    if mark_it {
        match (inst, &rec) {
            (Some(i), Some(r)) if p.verified => lines.push(format!("regmap    marked verified: {}", mark(&crate::kiln::kiln_dir(i), &p, r, &g.out)?.display())),
            _ => lines.push(format!("regmap    not marked: {}", p.blockers.first().map(String::as_str).unwrap_or("no candidate record"))),
        }
    }
    lines.push(format!("regproof  {} (report {})", if p.verified { "VERIFIED" } else { "not verified" }, g.out.join("report.txt").display()));
    Ok((p, rec, lines))
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
        side: a.side.clone(), tree, out: PathBuf::new(), store: None, new_exe: a.new_exe, old_exe: a.old_exe, new_refl: a.new_refl,
        old_refl: a.old_refl, record: a.record, ids: a.ids, prev_ids: a.prev_ids, evidence: a.evidence, game_build: a.game_build,
        steam_build: a.steam_build, first_build: a.first_build, accept: a.accept,
    };
    if let Some(i) = &inst {
        if g.new_exe.is_none() && g.new_refl.is_none() && g.record.is_none() {
            g.new_exe = Some(i.exe());
        }
        if g.old_exe.is_none() && g.old_refl.is_none() && !g.first_build {
            let new_sha = g.new_exe.as_deref().map(crate::util::sha256).transpose()?.unwrap_or_default();
            g.old_exe = crate::sigport::baseline_exe(i, &new_sha).map(|(p, _)| p);
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
    let (p, _, lines) = run(&g, inst.as_ref(), a.mark)?;
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
pub fn flow(inst: &crate::config::Instance, old: Option<&Path>, new: &Path, mark_it: bool) -> (String, Vec<String>) {
    if !inst.kiln.source.join("regmap/common").is_dir() {
        return ("no regmap in the Kiln tree; skipped".into(), Vec::new());
    }
    let g = Gather {
        side: "server".into(), tree: inst.kiln.source.clone(), out: default_out(&inst.name, Some(new)), new_exe: Some(new.to_path_buf()),
        old_exe: old.map(Path::to_path_buf), evidence: instance_evidence(inst), steam_build: crate::steam::buildid(&inst.dir),
        ..Default::default()
    };
    match run(&g, Some(inst), mark_it) {
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
            old_refl: Some(db(old_types(), OLD)),
            new_refl: Some(db(old_types(), SHA)),
            candidate: Some(candidate()),
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
        // a failed round trip
        let mut rt = with_power_samples(inputs());
        rt.suites[0].samples.retain(|s| s.field != "gadgets.size");
        let p2 = prove(&rt, "T");
        assert_eq!(p2.rows["gadgets.size"].state, "changed");
        assert!(p2.rows["gadgets.size"].why[0].starts_with("round trip:"), "{:?}", p2.rows["gadgets.size"]);
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
        let dst = mark(&kiln, &p, &rec, &out).unwrap();
        assert_eq!(dst, kiln.join(format!("regmap/server/{SHA}.json")));
        assert_eq!(read_json(&dst).unwrap()["verified"], true);
        assert!(out.join(format!("verified-regmap-server-{SHA}.json")).is_file());
        assert!(mark(&kiln, &p, &rec, &out).is_err(), "a verified record is never replaced");
        // an unverified proof marks nothing
        let mut bad = inp.clone();
        bad.native_ids = None;
        let pb = prove(&bad, "T");
        let kiln2 = root.join("game2/kiln");
        assert!(mark(&kiln2, &pb, &record(&pb, &candidate()), &out).is_err());
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
        let lines = keep(&p, &inp, &store, Some(&data)).unwrap();
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
        keep(&pb, &bad, &store, Some(&data2)).unwrap();
        assert!(!data2.exists());
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
        let suite = json!({"format": 1, "kind": "registry-roundtrip", "side": "server", "exeSha256": SHA, "generation": "live", "samples": []});
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
        assert_eq!(inp.suites.len(), 1);
        assert!(inp.values_new.is_some() && inp.values_old.is_some(), "the old export from the store");
        assert_eq!(inp.native_ids.as_ref().unwrap().exe_sha256, SHA);
        assert_eq!(inp.prev_ids.as_ref().unwrap().exe_sha256, OLD);
        assert!(store.get("server", OLD).is_some(), "the data folder's tables were harvested into the store");
        assert!(inp.problems.is_empty(), "{:?}", inp.problems);
        // outputs inside the tree are refused
        let bad = Gather { out: tree.join("out"), ..g.clone() };
        assert!(gather(&bad).is_err());
        assert!(!tree.join("out").exists());
        let _ = std::fs::remove_dir_all(&root);
    }
}
