// SPDX-License-Identifier: MIT
//! The registry round-trip suite (registry proof step 3, BRIEF 6.1), kilnctl's half: the lab writes one file per side and
//! generation, kilnctl judges it. For every registry field a lab sample reads the value through the schema, writes a probe
//! value, reads it back through the schema and raw through reflection, reverts, and hashes the entry's resource bytes
//! before the write and after the revert. Live fields are proven in a live generation (kiln.content's store at world
//! load), boot fields in a boot generation (the boot resource patch before the game reads it), read-only fields by a
//! read. kilnctl compares the raw values with the Kiln value itself (per conversion; formulas in formula.rs) and fails
//! closed: a field without a passing sample in its generation, a failing sample, a missing generation or a suite of
//! another build leaves the field unproven.
//!
//! The file (`"kind": "registry-roundtrip"`, format 1; written by the lab's proof producer, which is not part of kilnctl):
//! ```jsonc
//! { "format": 1, "kind": "registry-roundtrip", "side": "server", "exeSha256": "<the exe's sha256>",
//!   "generation": "live",                       // live | boot
//!   "regmapDigest": "<sha256>",                 // the regmap/common the run used (regmap::digest); another one is set aside
//!   "constants": { "gear.seed": 10, "gear.creep": 0.15, "gear.armorOffset": 23, "gear.armorGradient": 15 },
//!   "samples": [ { "field": "items.maxStack", "entry": "game:...", "element": null,   // element: a list element's index
//!                  "read": 20, "probe": 21, "readBack": 21,                          // through the schema
//!                  "rawBefore": [20], "rawAfter": [21],                              // the row's paths, through reflection
//!                  "reverted": true, "bytesBefore": "<sha256>", "bytesAfter": "<sha256>", "error": null } ] }
//! ```
//! Raw values: numbers for int/float/bool members (a bool may be 0/1), a time member's nanoseconds, an enum member's game
//! name, a bitmask member's list of game names, an optional enum's [switch, name]. Entry ids name the game's entries:
//! suites are lab artefacts and stay outside the repository.
use super::formula;
use super::regmap::{RegMap, Row};
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

pub const KIND: &str = "registry-roundtrip";

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Suite {
    #[serde(default)]
    pub format: u32,
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub side: String,
    #[serde(default)]
    pub exe_sha256: String,
    #[serde(default)]
    pub generation: String,
    /// the mapping the lab ran with ([`super::regmap::digest`] of its regmap/common)
    #[serde(default)]
    pub regmap_digest: String,
    #[serde(default)]
    pub constants: BTreeMap<String, f64>,
    #[serde(default)]
    pub samples: Vec<Sample>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Sample {
    pub field: String,
    #[serde(default)]
    pub entry: String,
    #[serde(default)]
    pub element: Option<u64>,
    #[serde(default)]
    pub read: Option<Value>,
    #[serde(default)]
    pub probe: Option<Value>,
    #[serde(default)]
    pub read_back: Option<Value>,
    #[serde(default)]
    pub raw_before: Option<Vec<Value>>,
    #[serde(default)]
    pub raw_after: Option<Vec<Value>>,
    #[serde(default)]
    pub reverted: Option<bool>,
    #[serde(default)]
    pub bytes_before: Option<String>,
    #[serde(default)]
    pub bytes_after: Option<String>,
    #[serde(default)]
    pub error: Option<String>,
}

/// A suite file; refuses another kind or format.
#[cfg(test)]
pub fn parse(text: &str) -> Result<Suite> {
    from_value(serde_json::from_str(text).context("not JSON")?)
}

pub fn from_value(v: Value) -> Result<Suite> {
    let s: Suite = serde_json::from_value(v).context("not a round-trip suite")?;
    if s.kind != KIND || s.format != 1 {
        bail!("not a {KIND} file of format 1 (kind {:?}, format {})", s.kind, s.format);
    }
    Ok(s)
}

/// How the raw values of a field were compared.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Raw {
    /// kilnctl compared the raw member values with the Kiln value
    Compared,
    /// a formula row: formula.rs computes the formula over the raw values
    Formula,
    /// a list, reference or other structured value: proven through the schema, the revert and the bytes only
    SchemaOnly,
}

#[derive(Debug, Clone, Serialize)]
pub struct FieldResult {
    pub ok: bool,
    pub samples: usize,
    pub passed: usize,
    pub raw: Raw,
    /// a step that failed: the mapping is wrong on this build
    pub problems: Vec<String>,
    /// evidence the lab did not give (no sample in the field's generation): the field is unproven, not wrong
    pub missing: Vec<String>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Report {
    /// every generation the regmap needs has a suite of this build
    pub complete: bool,
    /// generations without a suite
    pub missing: Vec<String>,
    /// suites set aside (another side or build, an unknown generation)
    pub problems: Vec<String>,
    pub fields: BTreeMap<String, FieldResult>,
}

/// The suites of this side and exe with a known generation; the others are named in `problems`.
pub fn usable<'a>(side: &str, exe_sha256: &str, regmap_digest: &str, suites: &'a [Suite], problems: &mut Vec<String>) -> Vec<&'a Suite> {
    let mut out = Vec::new();
    for s in suites {
        if s.regmap_digest.is_empty() || !s.regmap_digest.eq_ignore_ascii_case(regmap_digest) {
            problems.push(format!("a {} suite of another mapping (regmapDigest {:?}, this proof's regmap/common is {}) was set aside: \
                                   it proves a mapping this proof does not judge", s.generation,
                                  s.regmap_digest.get(..12).unwrap_or(&s.regmap_digest), regmap_digest.get(..12).unwrap_or(regmap_digest)));
        } else if s.side != side {
            problems.push(format!("a {} suite of the {} side was set aside (this is the {side} side's proof)", s.generation, s.side));
        } else if !s.exe_sha256.eq_ignore_ascii_case(exe_sha256) {
            problems.push(format!("a {} suite of another exe ({}) was set aside", s.generation, s.exe_sha256.get(..12).unwrap_or(&s.exe_sha256)));
        } else if s.generation != "live" && s.generation != "boot" {
            problems.push(format!("a suite of generation {:?} was set aside (live or boot)", s.generation));
        } else {
            out.push(s);
        }
    }
    out
}

/// The gear constants a live suite carries (the balance registry's resolved values in the lab).
pub fn constants(suites: &[&Suite]) -> BTreeMap<String, f64> {
    suites.iter().filter(|s| s.generation == "live" && !s.constants.is_empty()).map(|s| s.constants.clone()).next().unwrap_or_default()
}

fn raw_kind(conv: &str) -> Raw {
    match conv {
        "int" | "float" | "bool" | "time" | "enum" | "flags" | "optionalEnum" => Raw::Compared,
        "formula" => Raw::Formula,
        _ => Raw::SchemaOnly,
    }
}

fn num(v: &Value) -> Option<f64> {
    v.as_f64()
}

fn truthy(v: &Value) -> Option<bool> {
    match v {
        Value::Bool(b) => Some(*b),
        Value::Number(n) => n.as_f64().map(|x| x != 0.0),
        _ => None,
    }
}

/// Within float precision (the game keeps most numbers as 32-bit floats).
fn close(a: f64, b: f64) -> bool {
    (a - b).abs() <= 1e-6 * a.abs().max(b.abs()).max(1.0)
}

pub(super) fn same_value(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => x.as_f64().zip(y.as_f64()).is_some_and(|(x, y)| close(x, y)),
        (Value::Array(x), Value::Array(y)) => x.len() == y.len() && x.iter().zip(y).all(|(x, y)| same_value(x, y)),
        (Value::Object(x), Value::Object(y)) => x.len() == y.len() && x.iter().all(|(k, v)| y.get(k).is_some_and(|w| same_value(v, w))),
        _ => a == b,
    }
}

fn names(v: &Value) -> Option<std::collections::BTreeSet<String>> {
    v.as_array()?.iter().map(|x| x.as_str().map(str::to_string)).collect()
}

/// Two Kiln values of a field are the same (a set of flags in any order).
fn same(conv: &str, a: &Value, b: &Value) -> bool {
    if conv == "flags" {
        return names(a).is_some_and(|x| Some(x) == names(b));
    }
    same_value(a, b)
}

/// How the raw member values differ from a Kiln value, per conversion; None when they agree.
fn raw_mismatch(row: &Row, raw: &[Value], kiln: &Value) -> Option<String> {
    let first = raw.first().unwrap_or(&Value::Null);
    let agree = match row.conv.as_str() {
        "int" => num(first).is_some() && num(first) == num(kiln),
        "float" => num(first).zip(num(kiln)).is_some_and(|(a, b)| close(a, b)),
        "bool" => truthy(first).is_some() && truthy(first) == kiln.as_bool(),
        "time" => num(first).zip(num(kiln)).is_some_and(|(ns, s)| (ns / 1e9 - s).abs() <= 1e-9 * s.abs().max(1.0)),
        "enum" => first.as_str().is_some() && first.as_str() == kiln.as_str().and_then(|k| row.values.get(k)).map(String::as_str),
        "flags" => {
            let want: Option<std::collections::BTreeSet<String>> =
                kiln.as_array().and_then(|a| a.iter().map(|k| k.as_str().and_then(|k| row.values.get(k)).cloned()).collect());
            want.is_some() && names(first) == want
        }
        "optionalEnum" => match kiln.as_str() {
            Some("none") => truthy(first) == Some(false),
            Some(k) => truthy(first) == Some(true) && raw.get(1).and_then(Value::as_str) == row.values.get(k).map(String::as_str),
            None => false,
        },
        _ => true,
    };
    (!agree).then(|| format!("values {} do not give {kiln}", Value::Array(raw.to_vec())))
}

fn show(v: &Option<Value>) -> String {
    v.as_ref().map(Value::to_string).unwrap_or_else(|| "nothing".into())
}

fn sample_problems(row: &Row, x: &Sample, raw: Raw) -> Vec<String> {
    let mut e = if x.entry.is_empty() { "(no entry)".to_string() } else { x.entry.clone() };
    if let Some(i) = x.element {
        e.push_str(&format!("[{i}]"));
    }
    let mut p = Vec::new();
    if let Some(err) = &x.error {
        p.push(format!("{e}: the lab reported an error: {err}"));
        return p;
    }
    let Some(read) = &x.read else {
        p.push(format!("{e}: no value was read through the schema"));
        return p;
    };
    let n = row.paths.len();
    let raw_check = |what: &str, v: &Option<Vec<Value>>, kiln: &Value, p: &mut Vec<String>| match v {
        None => p.push(format!("{e}: no raw values were read {what} the write")),
        Some(v) if v.len() != n => p.push(format!("{e}: {} raw values {what} the write, the row has {n} paths", v.len())),
        Some(v) => {
            if raw == Raw::Compared {
                if let Some(m) = raw_mismatch(row, v, kiln) {
                    p.push(format!("{e}: {what} the write the raw member {m}"));
                }
            }
        }
    };
    raw_check("before", &x.raw_before, read, &mut p);
    if !row.writable() {
        if x.probe.is_some() {
            p.push(format!("{e}: a read-only field was written"));
        }
        return p;
    }
    let Some(probe) = &x.probe else {
        p.push(format!("{e}: no probe value was written"));
        return p;
    };
    if same(&row.conv, probe, read) {
        p.push(format!("{e}: the probe equals the value before the write: the write proves nothing"));
    }
    match &x.read_back {
        Some(b) if same(&row.conv, b, probe) => {}
        b => p.push(format!("{e}: read back {} after writing {probe}", show(b))),
    }
    raw_check("after", &x.raw_after, probe, &mut p);
    // structured values are proven through Kiln's own mapping both ways: the game's members must at least have moved
    if raw == Raw::SchemaOnly && !same(&row.conv, probe, read) {
        if let (Some(a), Some(b)) = (&x.raw_before, &x.raw_after) {
            if a.len() == n && a == b {
                p.push(format!("{e}: the raw members did not change after a write that changed the value"));
            }
        }
    }
    if x.reverted != Some(true) {
        p.push(format!("{e}: the write was not reverted"));
    }
    match (&x.bytes_before, &x.bytes_after) {
        (Some(a), Some(b)) if !a.is_empty() && a.eq_ignore_ascii_case(b) => {}
        (Some(_), Some(_)) => p.push(format!("{e}: the resource bytes after the revert differ from before")),
        _ => p.push(format!("{e}: the resource bytes were not hashed before the write and after the revert")),
    }
    p
}

/// Every mapped row of the regmap against the suites of this side and exe.
pub fn check(map: &RegMap, side: &str, exe_sha256: &str, regmap_digest: &str, suites: &[Suite]) -> Report {
    let mut r = Report::default();
    let good = usable(side, exe_sha256, regmap_digest, suites, &mut r.problems);
    let rows: Vec<(String, &Row)> = super::regmap::qualified(map).into_iter().filter(|(_, _, _, row)| row.has_mapping()).map(|(q, _, _, row)| (q, row)).collect();
    let needs_boot = rows.iter().any(|(_, row)| row.backend == "boot");
    for g in ["live", "boot"] {
        if (g == "live" || needs_boot) && !good.iter().any(|s| s.generation == g) {
            r.missing.push(g.to_string());
        }
    }
    r.complete = r.missing.is_empty();
    for (q, row) in rows {
        let want = match row.backend.as_str() {
            "boot" => Some("boot"),
            "live" => Some("live"),
            _ => None,
        };
        let raw = raw_kind(&row.conv);
        let (mut problems, mut samples, mut passed, mut in_generation) = (Vec::new(), 0, 0, false);
        for s in &good {
            for x in s.samples.iter().filter(|x| x.field == q) {
                samples += 1;
                let p = sample_problems(row, x, raw);
                if p.is_empty() {
                    passed += 1;
                    in_generation |= want.is_none_or(|g| g == s.generation);
                }
                problems.extend(p.into_iter().map(|m| format!("{} generation: {m}", s.generation)));
            }
        }
        let where_ = want.map(|g| format!(" in a {g} generation")).unwrap_or_default();
        let mut missing = Vec::new();
        if samples == 0 {
            missing.push(format!("no round-trip sample{where_}: the field is not proven on this build"));
        } else if !in_generation && problems.is_empty() {
            missing.push(format!("no round-trip sample{where_} (only in another generation): the field is not proven on this build"));
        }
        r.fields.insert(q, FieldResult { ok: problems.is_empty() && missing.is_empty(), samples, passed, raw, problems, missing });
    }
    r
}

/// The formula samples of a field (raw values as numbers), for formula.rs.
pub fn formula_samples(suites: &[&Suite], field: &str) -> Vec<formula::Sample> {
    let nums = |v: &Option<Vec<Value>>| v.as_ref().map(|v| v.iter().filter_map(Value::as_f64).collect()).unwrap_or_default();
    suites
        .iter()
        .flat_map(|s| s.samples.iter().filter(|x| x.field == field))
        .map(|x| formula::Sample {
            entry: x.entry.clone(),
            read: x.read.as_ref().and_then(Value::as_f64),
            probe: x.probe.as_ref().and_then(Value::as_f64),
            raw_before: nums(&x.raw_before),
            raw_after: nums(&x.raw_after),
        })
        .collect()
}


#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::regproof::regmap::tests::{DIGEST, sample};
    use serde_json::json;

    pub(crate) const SHA: &str = "1111111111111111111111111111111111111111111111111111111111111111";

    pub(crate) fn good_sample(field: &str) -> Value {
        let bytes = "ab".repeat(32);
        let base = json!({"field": field, "entry": "game:thing", "reverted": true, "bytesBefore": bytes, "bytesAfter": bytes});
        let mut s = base.as_object().unwrap().clone();
        let extra = match field {
            "gadgets.size" => json!({"read": 20, "probe": 21, "readBack": 21, "rawBefore": [20], "rawAfter": [21]}),
            "gadgets.tier" => json!({"read": "low", "probe": "high", "readBack": "high", "rawBefore": ["Low"], "rawAfter": ["High"]}),
            "gadgets.power" => json!({"read": 30, "probe": 44, "readBack": 44, "rawBefore": [1.525, 1.25, 4], "rawAfter": [2.225, 1.25, 4]}),
            "gadgets.label" => json!({"read": true, "rawBefore": [1], "reverted": null, "bytesAfter": null}),
            "plans.parts" => json!({"read": [{"item": "game:a", "count": 2}], "probe": [{"item": "game:a", "count": 3}],
                                    "readBack": [{"item": "game:a", "count": 3}], "rawBefore": [[7]], "rawAfter": [[7, 1]]}),
            "plans.parts[].count" => json!({"element": 0, "read": 2, "probe": 5, "readBack": 5, "rawBefore": [2], "rawAfter": [5]}),
            "plans.cap" => json!({"read": 1.5, "probe": 2.25, "readBack": 2.25, "rawBefore": [1.5], "rawAfter": [2.25]}),
            _ => json!({}),
        };
        for (k, v) in extra.as_object().unwrap() {
            s.insert(k.clone(), v.clone());
        }
        Value::Object(s)
    }

    pub(crate) fn suite(generation: &str, fields: &[&str]) -> Suite {
        let samples: Vec<Value> = fields.iter().map(|f| good_sample(f)).collect();
        parse(&json!({"format": 1, "kind": KIND, "side": "server", "exeSha256": SHA, "generation": generation, "regmapDigest": DIGEST,
                      "constants": {"gear.seed": 10, "gear.creep": 0.15, "gear.armorOffset": 23, "gear.armorGradient": 15},
                      "samples": samples}).to_string()).unwrap()
    }

    pub(crate) fn full() -> Vec<Suite> {
        vec![suite("live", &["gadgets.size", "gadgets.tier", "gadgets.power", "gadgets.label", "plans.parts", "plans.parts[].count"]),
             suite("boot", &["plans.cap"])]
    }

    fn map_with_float_cap() -> RegMap {
        let mut m = sample();
        m.get_mut("plans").unwrap().rows.get_mut("cap").unwrap().conv = "float".into();
        m
    }

    fn edit(suites: &mut [Suite], field: &str, f: impl Fn(&mut Sample)) {
        for s in suites {
            for x in &mut s.samples {
                if x.field == field {
                    f(x);
                }
            }
        }
    }

    #[test]
    fn a_suite_of_another_mapping_is_set_aside() {
        // a lab run with another regmap/common (an older deploy, a row fixed since) proves nothing about this one
        let mut other = full();
        other[0].regmap_digest = "e2".repeat(32);
        other[1].regmap_digest.clear();
        let r = check(&map_with_float_cap(), "server", SHA, DIGEST, &other);
        assert!(!r.complete, "{r:?}");
        assert_eq!(r.problems.len(), 2, "{:?}", r.problems);
        assert!(r.problems.iter().all(|p| p.contains("another mapping")), "{:?}", r.problems);
        assert!(r.fields.values().all(|f| !f.ok));
    }

    #[test]
    fn parses_only_round_trip_suites() {
        assert!(parse(r#"{"format": 1, "kind": "registry-values"}"#).is_err());
        assert!(parse(r#"{"format": 2, "kind": "registry-roundtrip"}"#).is_err());
        assert!(parse("[").is_err());
        assert_eq!(full()[0].samples.len(), 6);
    }

    #[test]
    fn a_complete_suite_proves_every_mapped_field() {
        let r = check(&map_with_float_cap(), "server", SHA, DIGEST, &full());
        assert!(r.complete, "{r:?}");
        for (q, f) in &r.fields {
            assert!(f.ok, "{q}: {f:?}");
        }
        assert_eq!(r.fields.len(), 7, "{:?}", r.fields.keys().collect::<Vec<_>>());
        assert_eq!(r.fields["gadgets.size"].raw, Raw::Compared);
        assert_eq!(r.fields["gadgets.power"].raw, Raw::Formula);
        assert_eq!(r.fields["plans.parts"].raw, Raw::SchemaOnly);
        assert!(!r.fields.contains_key("gadgets.glow"), "an unmapped row is not proven by a lab");
    }

    #[test]
    fn a_missing_generation_or_another_build_fails_closed() {
        let only_live = vec![full().remove(0)];
        let r = check(&map_with_float_cap(), "server", SHA, DIGEST, &only_live);
        assert!(!r.complete);
        assert_eq!(r.missing, ["boot"]);
        assert!(!r.fields["plans.cap"].ok);
        assert!(r.fields["plans.cap"].missing.iter().any(|p| p.contains("boot generation")), "{:?}", r.fields["plans.cap"]);
        assert!(r.fields["plans.cap"].problems.is_empty(), "missing evidence is not a failed step: {:?}", r.fields["plans.cap"]);
        // the boot field proven in a live generation proves nothing about the boot back-end
        let mut wrong = full();
        wrong[1].generation = "live".into();
        assert!(!check(&map_with_float_cap(), "server", SHA, DIGEST, &wrong).fields["plans.cap"].ok);
        // a suite of another exe or side is set aside
        let mut other = full();
        other[0].exe_sha256 = "22".repeat(32);
        other[1].side = "client".into();
        let r2 = check(&map_with_float_cap(), "server", SHA, DIGEST, &other);
        assert!(!r2.complete && r2.problems.len() == 2, "{:?}", r2.problems);
        assert!(r2.fields.values().all(|f| !f.ok));
        // a field without a sample
        let mut thin = full();
        thin[0].samples.retain(|s| s.field != "gadgets.tier");
        let r3 = check(&map_with_float_cap(), "server", SHA, DIGEST, &thin);
        assert!(r3.complete);
        assert!(r3.fields["gadgets.tier"].missing.iter().any(|p| p.contains("no round-trip sample")));
        assert!(!r3.fields["gadgets.tier"].ok && r3.fields["gadgets.tier"].problems.is_empty());
    }

    #[test]
    fn each_failing_step_leaves_the_field_unproven() {
        type Edit = fn(&mut Sample);
        let cases: Vec<(&str, Edit, &str)> = vec![
            ("gadgets.size", |s| s.read_back = Some(json!(20)), "read back"),
            ("gadgets.size", |s| s.raw_after = Some(vec![json!(22)]), "raw"),
            ("gadgets.size", |s| s.raw_before = Some(vec![json!(19)]), "raw"),
            ("gadgets.size", |s| s.bytes_after = Some("cd".repeat(32)), "bytes"),
            ("gadgets.size", |s| s.reverted = Some(false), "revert"),
            ("gadgets.size", |s| s.error = Some("the write was refused".into()), "refused"),
            ("gadgets.size", |s| s.probe = Some(json!(20)), "proves nothing"),
            ("gadgets.size", |s| s.raw_after = None, "raw"),
            ("gadgets.tier", |s| s.raw_after = Some(vec![json!("Low")]), "raw"),
            ("gadgets.label", |s| s.probe = Some(json!(false)), "read-only"),
            ("gadgets.label", |s| s.raw_before = Some(vec![json!(0)]), "raw"),
            ("plans.parts", |s| s.bytes_before = None, "bytes"),
        ];
        for (field, f, word) in cases {
            let mut suites = full();
            edit(&mut suites, field, f);
            let r = check(&map_with_float_cap(), "server", SHA, DIGEST, &suites);
            let x = &r.fields[field];
            assert!(!x.ok, "{field} ({word}) should fail: {x:?}");
            assert!(x.problems.iter().any(|p| p.contains(word)), "{field}: {word:?} not in {:?}", x.problems);
            assert!(r.fields.iter().filter(|(q, _)| q.as_str() != field).all(|(_, o)| o.ok), "only {field} fails");
        }
    }

    /// A list or reference row is not compared value by value, so a write through a wrong conversion and a read back
    /// through the same one would pass: the game's members must at least have changed.
    #[test]
    fn a_schema_only_write_must_change_the_raw_members() {
        let mut suites = full();
        edit(&mut suites, "plans.parts", |s| s.raw_after = s.raw_before.clone());
        let r = check(&map_with_float_cap(), "server", SHA, DIGEST, &suites);
        assert!(!r.fields["plans.parts"].ok);
        assert!(r.fields["plans.parts"].problems.iter().any(|p| p.contains("did not change")), "{:?}", r.fields["plans.parts"]);
    }

    #[test]
    fn floats_compare_within_float_precision_and_formulas_go_to_formula_rs() {
        let mut suites = full();
        edit(&mut suites, "plans.cap", |s| s.raw_after = Some(vec![json!(2.25f32 as f64 + 1e-8)]));
        assert!(check(&map_with_float_cap(), "server", SHA, DIGEST, &suites).fields["plans.cap"].ok);
        edit(&mut suites, "plans.cap", |s| s.raw_after = Some(vec![json!(2.3)]));
        assert!(!check(&map_with_float_cap(), "server", SHA, DIGEST, &suites).fields["plans.cap"].ok);
        let owned = full();
        let mut why = Vec::new();
        let all = usable("server", SHA, DIGEST, &owned, &mut why);
        assert_eq!((all.len(), why.len()), (2, 0));
        assert_eq!(constants(&all)["gear.creep"], 0.15);
        let fs = formula_samples(&all, "gadgets.power");
        assert_eq!(fs, [formula::Sample { entry: "game:thing".into(), read: Some(30.0), probe: Some(44.0),
                                         raw_before: vec![1.525, 1.25, 4.0], raw_after: vec![2.225, 1.25, 4.0] }]);
    }

    #[test]
    fn time_flags_and_optional_enums_compare_raw() {
        let rows = r#"{"regmap": 1, "registry": "kit",
            "entries": {"kind": "resource", "resourceType": "demo::Kit"},
            "rows": {
              "burn":  {"paths": ["burnTime"], "conv": "time", "read": "now", "backend": "live"},
              "kinds": {"paths": ["mask"], "conv": "flags", "read": "now", "backend": "live", "values": {"a": "A", "b": "B"}},
              "fixed": {"paths": ["on", "which"], "conv": "optionalEnum", "read": "now", "backend": "live", "values": {"none": "", "a": "A", "b": "B"}}}}"#;
        let mut map = RegMap::new();
        map.insert("kit".into(), crate::regproof::regmap::parse(rows, "kit").unwrap());
        let b = "ab".repeat(32);
        let mk = |samples: Value| parse(&json!({"format": 1, "kind": KIND, "side": "server", "exeSha256": SHA, "generation": "live",
                                                "regmapDigest": DIGEST, "samples": samples}).to_string()).unwrap();
        let ok = mk(json!([
            {"field": "kit.burn", "read": 1.5, "probe": 2, "readBack": 2, "rawBefore": [1500000000i64], "rawAfter": [2000000000i64], "reverted": true, "bytesBefore": b, "bytesAfter": b},
            {"field": "kit.kinds", "read": ["a"], "probe": ["a", "b"], "readBack": ["b", "a"], "rawBefore": [["A"]], "rawAfter": [["B", "A"]], "reverted": true, "bytesBefore": b, "bytesAfter": b},
            {"field": "kit.fixed", "read": "none", "probe": "b", "readBack": "b", "rawBefore": [false, "A"], "rawAfter": [true, "B"], "reverted": true, "bytesBefore": b, "bytesAfter": b}]));
        let r = check(&map, "server", SHA, DIGEST, &[ok]);
        assert_eq!(r.fields.len(), 3);
        for (q, f) in &r.fields {
            assert!(f.ok, "{q}: {f:?}");
        }
        let bad = mk(json!([
            {"field": "kit.burn", "read": 1.5, "probe": 2, "readBack": 2, "rawBefore": [1500000000], "rawAfter": [2000000001000i64], "reverted": true, "bytesBefore": b, "bytesAfter": b},
            {"field": "kit.kinds", "read": ["a"], "probe": ["a", "b"], "readBack": ["a", "b"], "rawBefore": [["A"]], "rawAfter": [["A"]], "reverted": true, "bytesBefore": b, "bytesAfter": b},
            {"field": "kit.fixed", "read": "none", "probe": "b", "readBack": "b", "rawBefore": [true, "A"], "rawAfter": [true, "B"], "reverted": true, "bytesBefore": b, "bytesAfter": b}]));
        let r2 = check(&map, "server", SHA, DIGEST, &[bad]);
        assert_eq!(r2.fields.len(), 3);
        for (q, f) in &r2.fields {
            assert!(!f.ok, "{q} should fail: {f:?}");
        }
    }
}
