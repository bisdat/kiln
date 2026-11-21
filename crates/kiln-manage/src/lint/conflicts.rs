//! KL701 data-conflict: two mods of a profile or pack giving one field of one game entry different values in their
//! data packs (content/*.json). kiln.content applies the packs in load order and the later mod wins
//! (docs/kiln/services/content.md, "Order and conflicts"); this is the offline version of the conflict it logs, per
//! section, selector and field. Globs are expanded against the game-data catalog when there is one; without it a glob
//! overlaps a name it matches, and `*` overlaps everything. `@where` conditions are not evaluated (they need the game's
//! values): a conflict with one is "conditional", as is one found through a glob without the catalog.
//!
//! Used by kiln lint (the cross-mod checks of --profile) and by the pack analysis (`modpack::check`), which reports
//! every pair of mods in a pack.

use super::catalog::Catalog;
use super::suggest::glob_ci;
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

/// One data pack file of a mod, parsed.
#[derive(Debug, Clone)]
pub struct PackFile {
    /// Shown in findings (a path).
    pub file: String,
    pub root: Value,
}

/// A mod with its data packs, in load order (the later mod wins).
#[derive(Debug, Clone)]
pub struct ModPacks {
    pub id: String,
    pub files: Vec<PackFile>,
}

/// Who sets the field.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Setter {
    #[serde(rename = "mod")]
    pub mod_id: String,
    pub file: String,
    pub selector: String,
    pub value: Value,
}

/// A field two mods set to different values. `later` wins at run time.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct DataConflict {
    pub section: String,
    pub field: String,
    pub earlier: Setter,
    pub later: Setter,
    /// It depends on the game's values (`@where`) or on a glob checked without the catalog.
    pub conditional: bool,
}

impl DataConflict {
    pub fn message(&self) -> String {
        let sel = if self.earlier.selector == self.later.selector {
            self.later.selector.clone()
        } else {
            format!("{} / {}", self.later.selector, self.earlier.selector)
        };
        format!(
            "{} {sel}.{}: {} sets {}, {} sets {} (the later mod wins: {}){}",
            self.section, self.field, self.earlier.mod_id, short(&self.earlier.value), self.later.mod_id, short(&self.later.value),
            self.later.mod_id, if self.conditional { "; may not touch the same entries (a condition or a glob)" } else { "" }
        )
    }
}

fn short(v: &Value) -> String {
    let s = v.to_string();
    if s.chars().count() > 60 { format!("{}...", s.chars().take(57).collect::<String>()) } else { s }
}

/// At most this many conflicts are reported (a pack of two big "*" rule sets would list thousands).
pub const MAX_CONFLICTS: usize = 500;

/// Reads a mod folder's content/*.json (as kiln.content finds them: one folder, any case of .json, by name). Files
/// that do not parse are skipped (lint reports them on their own).
pub fn read_mod(id: &str, root: &Path) -> ModPacks {
    let mut files = Vec::new();
    if let Ok(rd) = std::fs::read_dir(root.join("content")) {
        let mut v: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.is_file() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("json"))).collect();
        v.sort();
        for p in v {
            if std::fs::metadata(&p).is_ok_and(|m| m.len() > 16 << 20) {
                continue;
            }
            let Ok(b) = std::fs::read(&p) else { continue };
            let text = crate::manifest::decode_text(&b);
            if let Ok(root) = crate::jsonc::parse(&text) {
                let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
                files.push(PackFile { file: format!("content/{name}"), root });
            }
        }
    }
    ModPacks { id: id.to_string(), files }
}

struct Entry<'a> {
    m: usize,
    file: &'a str,
    section: &'a str,
    selector: &'a str,
    field: String,
    value: &'a Value,
    cond: bool,
}

const OPS: [&str; 7] = ["mul", "add", "min", "max", "raiseOnly", "lowerOnly", "set"];

fn is_op(o: &serde_json::Map<String, Value>) -> bool {
    !o.is_empty() && o.keys().all(|k| OPS.contains(&k.as_str()))
}

fn flatten<'a>(prefix: &str, v: &'a Value, out: &mut Vec<(String, &'a Value)>) {
    match v {
        Value::Object(o) if !is_op(o) => {
            for (k, x) in o {
                if k.starts_with('@') {
                    continue;
                }
                let path = if prefix.is_empty() { k.clone() } else { format!("{prefix}.{k}") };
                flatten(&path, x, out);
            }
        }
        _ => {
            if !prefix.is_empty() {
                out.push((prefix.to_string(), v));
            }
        }
    }
}

fn is_glob(s: &str) -> bool {
    s.contains('*') || s.contains('?')
}

/// Do two selectors of one section name a common entry? (overlap, certain)
fn overlap(section: &str, a: &str, b: &str, cat: Option<&Catalog>) -> Option<bool> {
    if a.eq_ignore_ascii_case(b) {
        return Some(true);
    }
    match (is_glob(a), is_glob(b)) {
        (false, false) => None,
        (true, false) => glob_ci(a, b).then_some(true),
        (false, true) => glob_ci(b, a).then_some(true),
        (true, true) => {
            if let Some(s) = cat.and_then(|c| c.section(section)).filter(|s| !s.names_list.is_empty()) {
                s.names_list.iter().any(|n| glob_ci(a, n) && glob_ci(b, n)).then_some(true)
            } else if a == "*" || b == "*" {
                Some(false)
            } else {
                None
            }
        }
    }
}

/// Every field two mods set differently, the earlier mod first. Within one mod later entries win and are no conflict.
pub fn analyze(mods: &[ModPacks], cat: Option<&Catalog>) -> Vec<DataConflict> {
    let mut entries: Vec<Entry> = Vec::new();
    for (mi, m) in mods.iter().enumerate() {
        for f in &m.files {
            let Some(top) = f.root.as_object() else { continue };
            for (section, sv) in top {
                if matches!(section.as_str(), "format" | "options" | "$schema") || section.starts_with('@') {
                    continue;
                }
                let Some(sels) = sv.as_object() else { continue };
                for (selector, body) in sels {
                    let Some(bo) = body.as_object() else { continue };
                    if selector.starts_with('@') || selector == "add" {
                        continue;
                    }
                    let cond = bo.contains_key("@where");
                    let mut fields = Vec::new();
                    flatten("", body, &mut fields);
                    for (field, value) in fields {
                        entries.push(Entry { m: mi, file: &f.file, section, selector, field, value, cond });
                    }
                }
            }
        }
    }
    let mut seen = BTreeSet::new();
    let mut out = Vec::new();
    for (j, b) in entries.iter().enumerate() {
        for a in &entries[..j] {
            if a.m == b.m || a.section != b.section || a.field != b.field || a.value == b.value {
                continue;
            }
            let Some(sure) = overlap(a.section, a.selector, b.selector, cat) else { continue };
            // the earlier mod in load order is the one that loses
            let (e, l) = if a.m < b.m { (a, b) } else { (b, a) };
            let key = (e.m, l.m, e.section.to_string(), e.selector.to_ascii_lowercase(), l.selector.to_ascii_lowercase(), e.field.clone());
            if !seen.insert(key) {
                continue;
            }
            let set = |x: &Entry| Setter { mod_id: mods[x.m].id.clone(), file: x.file.to_string(), selector: x.selector.to_string(), value: x.value.clone() };
            out.push(DataConflict { section: e.section.to_string(), field: e.field.clone(), earlier: set(e), later: set(l), conditional: !sure || e.cond || l.cond });
            if out.len() >= MAX_CONFLICTS {
                return out;
            }
        }
    }
    out
}

/// `analyze` with the catalog lint would use (LintOptions: --catalog, KILN_CATALOG, the launcher's; none with
/// no_catalog). Returns the conflicts and whether a catalog was used.
pub fn analyze_with(mods: &[ModPacks], opts: &super::LintOptions) -> (Vec<DataConflict>, bool) {
    let (cat, _) = if opts.no_catalog { (None, None) } else { super::catalog::locate(opts) };
    (analyze(mods, cat.as_ref()), cat.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn m(id: &str, packs: &[Value]) -> ModPacks {
        ModPacks { id: id.into(), files: packs.iter().enumerate().map(|(i, v)| PackFile { file: format!("content/{i}.json"), root: v.clone() }).collect() }
    }

    #[test]
    fn the_later_mod_wins_and_only_different_values_conflict() {
        let a = m("big-stacks", &[json!({"format": 1, "items": {"Material_T1_Twigs": {"maxStackSize": 1000, "rarity": "Epic"}}})]);
        let b = m("pack.ember", &[json!({"format": 1, "items": {"material_t1_twigs": {"maxStackSize": 600, "rarity": "Epic"}}})]);
        let c = analyze(&[a, b], None);
        assert_eq!(c.len(), 1, "{c:?}");
        assert_eq!((c[0].section.as_str(), c[0].field.as_str()), ("items", "maxStackSize"));
        assert_eq!((c[0].earlier.mod_id.as_str(), c[0].later.mod_id.as_str()), ("big-stacks", "pack.ember"));
        assert!(!c[0].conditional);
        assert!(c[0].message().contains("the later mod wins: pack.ember"), "{}", c[0].message());
    }

    #[test]
    fn globs_ops_nested_fields_and_conditions() {
        let a = m("a", &[json!({"items": {"Material_*": {"maxStackSize": {"mul": 4}}, "Weapon_X": {"damageSetup": {"dmgMod": 1.5}}},
                               "drops": {"*": {"count": {"mul": 2}}}})]);
        let b = m("b", &[json!({"items": {"Material_T1_Wood_Logs": {"maxStackSize": {"mul": 2}}, "Weapon_X": {"damageSetup.dmgMod": 2}},
                               "drops": {"a20e8eae": {"@where": {"x": 1}, "count": {"mul": 3}}}})]);
        let c = analyze(&[a, b], None);
        let fields: Vec<(&str, &str, bool)> = c.iter().map(|x| (x.section.as_str(), x.field.as_str(), x.conditional)).collect();
        assert!(fields.contains(&("items", "maxStackSize", false)), "a glob covering a name: {fields:?}");
        assert!(fields.contains(&("items", "damageSetup.dmgMod", false)), "nested and dotted paths are one field: {fields:?}");
        assert!(fields.contains(&("drops", "count", true)), "@where makes it conditional: {fields:?}");
        assert_eq!(c.len(), 3);
    }

    #[test]
    fn one_mod_alone_new_items_and_unrelated_entries_are_no_conflict() {
        let a = m("a", &[json!({"items": {"X": {"maxStackSize": 5}}}), json!({"items": {"X": {"maxStackSize": 9}}})]);
        assert!(analyze(std::slice::from_ref(&a), None).is_empty(), "within one mod later entries win");
        let b = m("b", &[json!({"items": {"Y": {"maxStackSize": 7}, "add": [{"name": "X"}]}, "options": {"stackGuard": true}})]);
        assert!(analyze(&[a, b], None).is_empty());
        // two globs without the catalog: only "*" with another glob counts, as conditional
        let g1 = m("g1", &[json!({"items": {"Material_*": {"weight": 1}}})]);
        let g2 = m("g2", &[json!({"items": {"Weapon_*": {"weight": 2}}, "recipes": {"*": {"craftingDuration": 3}}})]);
        assert!(analyze(&[g1.clone(), g2.clone()], None).is_empty());
        let g3 = m("g3", &[json!({"items": {"*": {"weight": 3}}})]);
        let c = analyze(&[g1, g3], None);
        assert_eq!(c.len(), 1);
        assert!(c[0].conditional);
    }

    #[test]
    fn kiln_lint_reports_kl701_on_the_linted_mod_against_its_profile() {
        let d = std::env::temp_dir().join(format!("kiln-kl701-{}", std::process::id()));
        let w = |p: &str, t: &str| {
            let f = d.join(p);
            std::fs::create_dir_all(f.parent().unwrap()).unwrap();
            std::fs::write(f, t).unwrap();
        };
        let manifest = |id: &str| format!(r#"{{"id": "{id}", "name": "{id}", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "clients": "mod"}}"#);
        w("mods/big-stacks/kiln.mod.json", &manifest("big-stacks"));
        w("mods/big-stacks/content/a.json", r#"{"format": 1, "items": {"Material_T1_Twigs": {"maxStackSize": 1000}}}"#);
        w("layer/kiln.mod.json", &manifest("pack.p"));
        w("layer/content/b.json", r#"{"format": 1, "items": {"Material_T1_Twigs": {"maxStackSize": 600}}}"#);
        let opts = super::super::LintOptions { no_catalog: true, profile: Some(d.join("mods")), ..Default::default() };
        let r = super::super::lint_paths(&[d.join("layer")], &opts);
        let k: Vec<_> = r.diagnostics.iter().filter(|x| x.code == "KL701").collect();
        assert_eq!(k.len(), 1, "{:#?}", r.diagnostics);
        assert!(k[0].file.ends_with("b.json") && k[0].line == 1 && k[0].message.contains("big-stacks sets 1000"), "{:?}", k[0]);
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn the_catalog_decides_whether_two_globs_meet() {
        let mut cat = Catalog::default();
        let mut s = super::super::catalog::Section::default();
        s.names_list = vec!["Material_T1_Wood_Logs".into(), "Weapon_T1_Sword".into()];
        s.names = s.names_list.iter().cloned().collect();
        cat.sections.insert("items".into(), s);
        let a = m("a", &[json!({"items": {"Material_*": {"weight": 1}}})]);
        let b = m("b", &[json!({"items": {"*_Logs": {"weight": 2}}})]);
        let c = m("c", &[json!({"items": {"Weapon_*": {"weight": 2}}})]);
        assert_eq!(analyze(&[a.clone(), b], Some(&cat)).len(), 1);
        assert!(analyze(&[a, c], Some(&cat)).is_empty());
    }
}
