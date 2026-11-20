//! Three-way merge of a pack's config overrides (Crucible, docs/kiln/launcher/crucible.md "Updates").
//!
//! An override is a set of leaves: a key path and the value the pack gives it (None: the pack removes the key). JSON
//! overrides are merge patches (RFC 7386: objects recurse, null removes, anything else is a leaf), INI overrides set
//! keys (`section.key` or a key before any section), a "replace" override is one leaf, the whole file. The live file is
//! the player's. Per key: the pack changed it and the player did not -> take the pack's; the player changed it and the
//! pack did not -> keep the player's; both, differently -> a conflict the player decides.
//!
//! Edits keep comments and layout (crate::jsonc, crate::config::ini).

use crate::jsonc::{self, PathSeg};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

/// How an override applies to its file.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum OverrideMode {
    /// A JSON merge patch (RFC 7386) onto a JSON/JSONC file.
    MergePatch,
    /// INI keys onto an INI file (other keys, comments and order stay).
    Ini,
    /// The whole file.
    Replace,
}

impl OverrideMode {
    /// The mode for a settings file by its extension: .json -> merge-patch, .ini -> ini, anything else -> replace.
    pub fn for_path(path: &str) -> OverrideMode {
        let lower = path.to_ascii_lowercase();
        if lower.ends_with(".json") {
            OverrideMode::MergePatch
        } else if lower.ends_with(".ini") {
            OverrideMode::Ini
        } else {
            OverrideMode::Replace
        }
    }
}

/// A key path (JSON: member names; INI: [section, key] or [key]; replace: empty).
pub type Key = Vec<String>;
/// The leaves of an override: what the pack sets each key to (None: removes it).
pub type Leaves = BTreeMap<Key, Option<Value>>;

/// Shown to people: `a.b`, `section.key`, `(whole file)`.
pub fn key_text(k: &[String]) -> String {
    if k.is_empty() { "(whole file)".into() } else { k.join(".") }
}

fn parse_err(file: &str, e: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::Parse, format!("{file}: {e}"))
}

/// The leaves of an override file's text.
pub fn leaves(mode: OverrideMode, file: &str, text: &str) -> Result<Leaves> {
    let mut out = Leaves::new();
    match mode {
        OverrideMode::MergePatch => {
            let v = jsonc::parse_tree(text).map_err(|e| parse_err(file, e))?.to_value();
            let Value::Object(_) = v else { return Err(Error::new(ErrorCode::Schema, format!("{file}: a merge patch must be one {{ ... }} object"))) };
            fn walk(v: &Value, at: &mut Key, out: &mut Leaves) {
                match v {
                    Value::Object(m) if !m.is_empty() => {
                        for (k, x) in m {
                            at.push(k.clone());
                            walk(x, at, out);
                            at.pop();
                        }
                    }
                    Value::Null => {
                        out.insert(at.clone(), None);
                    }
                    other => {
                        out.insert(at.clone(), Some(other.clone()));
                    }
                }
            }
            walk(&v, &mut Vec::new(), &mut out);
            out.remove(&Vec::new());
        }
        OverrideMode::Ini => {
            if let Value::Object(m) = crate::config::ini::values(text) {
                for (k, v) in m {
                    match v {
                        Value::Object(keys) => {
                            for (key, x) in keys {
                                out.insert(vec![k.clone(), key], Some(x));
                            }
                        }
                        x => {
                            out.insert(vec![k], Some(x));
                        }
                    }
                }
            }
        }
        OverrideMode::Replace => {
            out.insert(Vec::new(), Some(Value::String(text.to_string())));
        }
    }
    Ok(out)
}

/// The value a live file holds at a key (None: absent, or the file is missing).
pub fn value_at(mode: OverrideMode, text: Option<&str>, key: &[String]) -> Option<Value> {
    let text = text?;
    match mode {
        OverrideMode::MergePatch => {
            let root = jsonc::parse_tree(text).ok()?;
            let segs: Vec<PathSeg> = key.iter().map(|k| PathSeg::Key(k.clone())).collect();
            root.at(&segs).or_else(|| root.at_ci(&segs)).map(|n| n.to_value())
        }
        OverrideMode::Ini => {
            let v = crate::config::ini::values(text);
            let mut at = &v;
            for k in key {
                at = at.get(k)?;
            }
            Some(at.clone())
        }
        OverrideMode::Replace => Some(Value::String(text.to_string())),
    }
}

/// `text` (None: no file yet) with the edits applied, comments and layout kept.
pub fn apply(mode: OverrideMode, file: &str, text: Option<&str>, edits: &[(Key, Option<Value>)]) -> Result<String> {
    match mode {
        OverrideMode::MergePatch => {
            let mut t = match text {
                Some(t) if !t.trim().is_empty() => t.to_string(),
                _ => "{\n}\n".to_string(),
            };
            let root = jsonc::parse_tree(&t).map_err(|e| parse_err(file, e))?;
            if !root.is_object() {
                return Err(Error::new(ErrorCode::Schema, format!("{file}: the file must hold one {{ ... }} object")));
            }
            for (k, v) in edits {
                let segs: Vec<PathSeg> = k.iter().map(|k| PathSeg::Key(k.clone())).collect();
                t = match v {
                    Some(v) => jsonc::set_value(&t, &segs, v)?,
                    None => jsonc::remove_value(&t, &segs)?,
                };
            }
            Ok(t)
        }
        OverrideMode::Ini => {
            let mut t = text.unwrap_or_default().to_string();
            for (k, v) in edits {
                let path = k.join(".");
                t = match v {
                    Some(v) => crate::config::ini::set(&t, &path, &crate::config::ini::text_of(v)),
                    None => crate::config::ini::remove(&t, &path),
                };
            }
            Ok(t)
        }
        OverrideMode::Replace => {
            let mut t = text.unwrap_or_default().to_string();
            for (_, v) in edits {
                t = match v {
                    Some(Value::String(s)) => s.clone(),
                    Some(other) => other.to_string(),
                    None => String::new(),
                };
            }
            Ok(t)
        }
    }
}

/// Which side a conflict takes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Pick {
    /// The pack's new value.
    Pack,
    /// The player's value (as the file holds it now).
    Mine,
}

/// A key both the pack and the player changed, differently.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeyConflict {
    /// `a.b` / `section.key` / `(whole file)`.
    pub key: String,
    /// What the pack's installed version set (None: removed it).
    pub base: Option<Value>,
    /// What the new version sets (None: removes it).
    pub pack: Option<Value>,
    /// What the file holds now (None: absent).
    pub mine: Option<Value>,
}

/// The outcome of one key-by-key merge.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Merged {
    /// Keys to write with the pack's new value.
    pub take: Vec<(Key, Option<Value>)>,
    /// Keys the pack changed and the player had not (shown as "updated by the pack").
    pub taken: Vec<String>,
    /// Keys the player changed and the pack did not touch, or the pack no longer sets (shown as "your change kept").
    pub kept: Vec<String>,
    pub conflicts: Vec<(Key, KeyConflict)>,
}

/// The three-way merge of one file: `base` = the installed version's leaves (None: it had no override for the file),
/// `theirs` = the new version's (None: it has none), `mine` = the live value at a key.
pub fn three_way(base: Option<&Leaves>, theirs: Option<&Leaves>, mine: &dyn Fn(&[String]) -> Option<Value>) -> Merged {
    let empty = Leaves::new();
    let (b, t) = (base.unwrap_or(&empty), theirs.unwrap_or(&empty));
    let keys: BTreeSet<&Key> = b.keys().chain(t.keys()).collect();
    let mut out = Merged::default();
    for k in keys {
        let m = mine(k);
        match (b.get(k), t.get(k)) {
            // the pack no longer sets it: whatever the player has stays
            (Some(_), None) => {}
            // a key the pack sets for the first time
            (None, Some(tv)) => {
                if m != *tv {
                    out.take.push((k.clone(), tv.clone()));
                    out.taken.push(key_text(k));
                }
            }
            (Some(bv), Some(tv)) => {
                if bv == tv {
                    if m != *tv {
                        out.kept.push(key_text(k));
                    }
                } else if m == *bv {
                    out.take.push((k.clone(), tv.clone()));
                    out.taken.push(key_text(k));
                } else if m != *tv {
                    out.conflicts.push((k.clone(), KeyConflict { key: key_text(k), base: bv.clone(), pack: tv.clone(), mine: m }));
                }
            }
            (None, None) => {}
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn l(mode: OverrideMode, text: &str) -> Leaves {
        leaves(mode, "f", text).unwrap()
    }

    #[test]
    fn merge_patch_leaves_and_apply_keep_comments() {
        let p = l(OverrideMode::MergePatch, r#"{ "range": 12, "loot": { "smash": false, "old": null }, "list": [1, 2] }"#);
        assert_eq!(p.get(&vec!["range".to_string()]), Some(&Some(json!(12))));
        assert_eq!(p.get(&vec!["loot".to_string(), "old".to_string()]), Some(&None));
        assert_eq!(p.get(&vec!["list".to_string()]), Some(&Some(json!([1, 2]))));
        let live = r#"{
  // the player's note
  "range": 20,
  "loot": { "old": 1 }
}
"#;
        let edits: Vec<(Key, Option<Value>)> = p.into_iter().collect();
        let out = apply(OverrideMode::MergePatch, "f.json", Some(live), &edits).unwrap();
        assert!(out.contains("// the player's note"), "{out}");
        let v = jsonc::parse_tree(&out).unwrap().to_value();
        assert_eq!(v, json!({"range": 12, "loot": {"smash": false}, "list": [1, 2]}));
        // no file yet
        let out = apply(OverrideMode::MergePatch, "f.json", None, &[(vec!["a".into(), "b".into()], Some(json!(true)))]).unwrap();
        assert_eq!(jsonc::parse_tree(&out).unwrap().to_value(), json!({"a": {"b": true}}));
    }

    #[test]
    fn ini_leaves_and_apply() {
        let p = l(OverrideMode::Ini, "top = 1\n[Loot]\nrange = 12\n");
        assert_eq!(p.len(), 2);
        let live = "; comment\ntop = 0\n[Loot]\nrange = 20\nother = x\n";
        let out = apply(OverrideMode::Ini, "f.ini", Some(live), &p.into_iter().collect::<Vec<_>>()).unwrap();
        assert!(out.contains("; comment") && out.contains("other = x"), "{out}");
        assert_eq!(value_at(OverrideMode::Ini, Some(&out), &["Loot".into(), "range".into()]), Some(json!("12")));
        assert_eq!(value_at(OverrideMode::Ini, Some(&out), &["top".into()]), Some(json!("1")));
    }

    #[test]
    fn three_way_takes_keeps_and_reports_conflicts() {
        let base = l(OverrideMode::MergePatch, r#"{ "a": 1, "b": 1, "c": 1, "d": 1, "e": 1 }"#);
        let theirs = l(OverrideMode::MergePatch, r#"{ "a": 2, "b": 1, "c": 2, "d": 2, "f": 5 }"#);
        let live = json!({ "a": 1, "b": 9, "c": 7, "d": 2, "e": 4 });
        let mine = |k: &[String]| live.get(&k[0]).cloned();
        let m = three_way(Some(&base), Some(&theirs), &mine);
        // a: only the pack changed it; f: new from the pack
        assert_eq!(m.taken, ["a", "f"]);
        // b: only the player changed it
        assert_eq!(m.kept, ["b"]);
        // c: both, differently; d: both, the same way (nothing to do); e: the pack stopped setting it
        assert_eq!(m.conflicts.len(), 1);
        let (_, c) = &m.conflicts[0];
        assert_eq!((c.key.as_str(), c.base.clone(), c.pack.clone(), c.mine.clone()), ("c", Some(json!(1)), Some(json!(2)), Some(json!(7))));
        assert_eq!(m.take, vec![(vec!["a".to_string()], Some(json!(2))), (vec!["f".to_string()], Some(json!(5)))]);
    }

    #[test]
    fn replace_is_one_leaf() {
        let base = l(OverrideMode::Replace, "x");
        let theirs = l(OverrideMode::Replace, "y");
        let m = three_way(Some(&base), Some(&theirs), &|_| Some(json!("x")));
        assert_eq!(m.taken, ["(whole file)"]);
        let m = three_way(Some(&base), Some(&theirs), &|_| Some(json!("mine")));
        assert_eq!(m.conflicts.len(), 1);
        assert_eq!(apply(OverrideMode::Replace, "a.txt", Some("old"), &m.take).unwrap(), "old");
    }
}
