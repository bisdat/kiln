// SPDX-License-Identifier: MIT
//! The registry stage's plans and a rollback to the previous one (docs/kiln/modding/registries/README.md "Rollback").
//!
//! kiln.content's stage (native/services/content/registry/stage.cpp) writes, under `<game>/kiln/data/content/registry/`:
//! - `plans/<sha256>.json` (format 2): one start's plan: the lowered edits of each mod (`inputs`, IR batches), the values
//!   they resolved to (`values`, keyed `<registry>[<id>].<field>`), and `previous`, the last good plan before it;
//! - `plans/index.json`: `good` (the last plan a world started on without a refused edit, and the good one before it) and
//!   `world` (the last plan a world started on);
//! and `<game>/kiln/state/registry.json` names the start's plan (`plan.hash`, `plan.previous`) and a rollback in effect.
//!
//! A rollback is a request, `rollback.json` beside the plans: the next start runs on that plan's edits instead of the mods'
//! current ones, and every later start too, until the request is cleared. The stage refuses the plan whole (and runs the
//! mods' current edits) when one of its mods is not loaded at the plan's version, when its file is missing or changed, or
//! when one of its edits is refused on this build; it never half-applies one. Nothing here reads or writes a save.
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// The registry report of the last start, relative to the game folder.
pub const REPORT_REL: &str = "kiln/state/registry.json";
/// The stage's data folder (plans, the rollback request), relative to the game folder.
pub const DATA_REL: &str = "kiln/data/content/registry";
/// The rollback request's file name in [`DATA_REL`].
pub const REQUEST_NAME: &str = "rollback.json";
/// Read caps: the stage caps registry.json's rows, not its bytes; a plan holds edits and values.
const MAX_REPORT_BYTES: u64 = 8 << 20;
const MAX_PLAN_BYTES: u64 = 64 << 20;

/// One mod of a plan, with the version it ran at.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanMod {
    pub id: String,
    pub version: String,
}

/// A plan as kilnctl and the launcher show it.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Plan {
    pub hash: String,
    pub side: String,
    pub exe_sha256: String,
    pub kiln: String,
    pub mods: Vec<PlanMod>,
    /// `<registry>[<id>].<field>` -> the value its registry edits resolved to at the stage (restart fields as written).
    pub values: BTreeMap<String, Value>,
    pub previous: Option<String>,
}

/// What the last start's registry.json says about plans.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Current {
    pub plan: Option<String>,
    pub previous: Option<String>,
    /// The report's `rollback` object (`plan`, `from`, `state` applied | refused, `reason`, `changedBack`, `leftOut`).
    pub rollback: Option<Value>,
    pub written: Option<String>,
    pub phase: Option<String>,
}

/// `rollback.json`: the next start runs on `plan` (and every later start, until cleared).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Request {
    pub format: u32,
    pub plan: String,
    /// The plan rolled away from (the stage says what changed back against its values).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from: Option<String>,
    pub requested: String,
    pub by: String,
}

/// One value a rollback changes back. `None`: the plan has no registry edit of it (the value without the registry: the
/// game's own, or what an older path writes).
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Change {
    pub field: String,
    pub from: Option<Value>,
    pub to: Option<Value>,
}

pub fn data_dir(game: &Path) -> PathBuf {
    game.join(DATA_REL)
}

fn is_hash(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn read_capped(path: &Path, cap: u64, what: &str) -> Result<Option<String>> {
    use std::io::Read;
    let meta = match std::fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(Error::new(ErrorCode::Io, format!("{what} could not be read: {e}"))),
    };
    if !meta.is_file() {
        return Err(Error::new(ErrorCode::Io, format!("{what} is not a regular file ({})", path.display())));
    }
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .and_then(|f| f.take(cap + 1).read_to_end(&mut bytes))
        .map_err(|e| Error::new(ErrorCode::Io, format!("{what} could not be read: {e}")))?;
    if bytes.len() as u64 > cap {
        return Err(Error::new(ErrorCode::Io, format!("{what} is larger than {} MiB ({})", cap >> 20, path.display())));
    }
    String::from_utf8(bytes).map(Some).map_err(|_| Error::new(ErrorCode::Parse, format!("{what} is not UTF-8 ({})", path.display())))
}

fn text(v: &Value, key: &str) -> Option<String> {
    v.get(key).and_then(Value::as_str).map(str::to_owned)
}

/// The plans named by the last start's `kiln/state/registry.json`; all `None` when there is none yet.
pub fn current(game: &Path) -> Result<Current> {
    let Some(t) = read_capped(&game.join(REPORT_REL), MAX_REPORT_BYTES, "kiln/state/registry.json")? else {
        return Ok(Current::default());
    };
    let doc: Value = serde_json::from_str(&t).map_err(|e| Error::new(ErrorCode::Parse, format!("kiln/state/registry.json is not JSON: {e}")))?;
    let plan = doc.get("plan").filter(|p| p.is_object());
    Ok(Current {
        plan: plan.and_then(|p| text(p, "hash")),
        previous: plan.and_then(|p| text(p, "previous")),
        rollback: doc.get("rollback").filter(|r| r.is_object()).cloned(),
        written: text(&doc, "written"),
        phase: text(&doc, "phase"),
    })
}

/// `plans/<hash>.json`: a plan a rollback can start on (format 2, with its edits).
pub fn plan(game: &Path, hash: &str) -> Result<Plan> {
    if !is_hash(hash) {
        return Err(Error::invalid(format!("{hash:?} is not a plan hash (64 lower-case hex digits)")));
    }
    let path = data_dir(game).join("plans").join(format!("{hash}.json"));
    let short = &hash[..12];
    let Some(t) = read_capped(&path, MAX_PLAN_BYTES, &format!("plan {short}"))? else {
        return Err(Error::not_found(format!("plan {short} is not in {DATA_REL}/plans (it was removed or never written)")));
    };
    let doc: Value = serde_json::from_str(&t).map_err(|e| Error::new(ErrorCode::Parse, format!("plan {short} is not JSON: {e}")))?;
    if doc.get("format").and_then(Value::as_u64) != Some(2) || !doc.get("inputs").is_some_and(Value::is_array) {
        return Err(Error::invalid(format!(
            "plan {short} was written by an older Kiln and holds no edits to start on (a plan of 0.2.0-dev.31.17 or later is needed)"
        )));
    }
    let key = doc.get("key").cloned().unwrap_or(Value::Null);
    let mods = doc["inputs"].as_array().into_iter().flatten()
        .map(|m| PlanMod { id: text(m, "mod").unwrap_or_default(), version: text(m, "version").unwrap_or_default() })
        .collect();
    let values = doc.get("values").and_then(Value::as_object).map(|o| o.iter().map(|(k, v)| (k.clone(), v.clone())).collect())
        .unwrap_or_default();
    Ok(Plan {
        hash: hash.to_owned(),
        side: text(&key, "side").unwrap_or_default(),
        exe_sha256: text(&key, "exeSha256").unwrap_or_default(),
        kiln: text(&key, "kiln").unwrap_or_default(),
        mods,
        values,
        previous: text(&doc, "previous"),
    })
}

/// Every value that differs between the plan rolled away from and the plan rolled back to, by field.
pub fn changes(from: &Plan, to: &Plan) -> Vec<Change> {
    let mut keys: Vec<&String> = from.values.keys().chain(to.values.keys()).collect();
    keys.sort();
    keys.dedup();
    keys.into_iter()
        .filter_map(|k| {
            let (a, b) = (from.values.get(k), to.values.get(k));
            (a != b).then(|| Change { field: k.clone(), from: a.cloned(), to: b.cloned() })
        })
        .collect()
}

/// The rollback request waiting for (or held by) the next start, if any.
pub fn pending(game: &Path) -> Result<Option<Request>> {
    let Some(t) = read_capped(&data_dir(game).join(REQUEST_NAME), 1 << 16, "the rollback request")? else { return Ok(None) };
    serde_json::from_str(&t).map(Some).map_err(|e| Error::new(ErrorCode::Parse, format!("the rollback request is not readable: {e}")))
}

/// Ask that the next start (and the later ones, until [`clear`]) run on plan `to`. Checks that the plan can be started on
/// at all (it exists, format 2); whether its mods are loaded at its versions is the stage's to decide at that start.
pub fn ask(game: &Path, to: &str, from: Option<&str>, by: &str) -> Result<Request> {
    plan(game, to)?;
    if let Some(f) = from
        && !is_hash(f)
    {
        return Err(Error::invalid(format!("{f:?} is not a plan hash")));
    }
    let r = Request { format: 1, plan: to.to_owned(), from: from.map(str::to_owned), requested: crate::util::now_rfc3339(), by: by.to_owned() };
    let mut body = serde_json::to_vec_pretty(&r)?;
    body.push(b'\n');
    crate::util::write_atomic(&data_dir(game).join(REQUEST_NAME), &body)?;
    Ok(r)
}

/// Remove the request: the next start runs on the mods' current edits again. `false`: there was none.
pub fn clear(game: &Path) -> Result<bool> {
    match std::fs::remove_file(data_dir(game).join(REQUEST_NAME)) {
        Ok(()) => Ok(true),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(e) => Err(Error::new(ErrorCode::Io, format!("the rollback request could not be removed: {e}"))),
    }
}

/// A value as one line of text (a list or element as JSON; "no registry edit" for none).
pub fn value_text(v: Option<&Value>) -> String {
    match v {
        None => "no registry edit (the value without the registry)".to_owned(),
        Some(Value::String(s)) => s.clone(),
        Some(v) => v.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    struct Game(PathBuf);
    impl Game {
        fn new(name: &str) -> Self {
            let root = std::env::temp_dir().join(format!("kiln-registry-plan-{}-{name}", std::process::id()));
            let _ = std::fs::remove_dir_all(&root);
            std::fs::create_dir_all(root.join(DATA_REL).join("plans")).unwrap();
            std::fs::create_dir_all(root.join("kiln/state")).unwrap();
            Self(root)
        }
        fn plan(&self, hash: &str, doc: Value) {
            std::fs::write(data_dir(&self.0).join("plans").join(format!("{hash}.json")), doc.to_string()).unwrap();
        }
    }
    impl Drop for Game {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    fn h(c: char) -> String {
        std::iter::repeat_n(c, 64).collect()
    }
    fn doc(values: Value, previous: Option<&str>) -> Value {
        json!({"format": 2, "key": {"side": "server", "exeSha256": h('e'), "kiln": "0.2.0-test"},
               "inputs": [{"mod": "bigmod", "version": "2.0.0", "batches": []}], "values": values, "previous": previous})
    }

    #[test]
    fn current_reads_the_plan_and_its_previous_from_registry_json() {
        let g = Game::new("current");
        assert_eq!(current(&g.0).unwrap(), Current::default(), "no report yet: nothing named");
        std::fs::write(g.0.join(REPORT_REL), json!({"plan": {"hash": h('b'), "previous": h('a')}, "rollback": null, "phase": "worldLoad"}).to_string()).unwrap();
        let c = current(&g.0).unwrap();
        assert_eq!((c.plan.as_deref(), c.previous.as_deref(), c.rollback, c.phase.as_deref()), (Some(&*h('b')), Some(&*h('a')), None, Some("worldLoad")));
    }

    #[test]
    fn changes_say_every_value_that_goes_back() {
        let g = Game::new("changes");
        g.plan(&h('a'), doc(json!({"items[game:plank].maxStack": 80, "balance[game:balance].levelCap": 60}), None));
        g.plan(&h('b'), doc(json!({"items[game:plank].maxStack": 160, "items[game:logs].maxStack": 30, "balance[game:balance].levelCap": 60}), Some(&h('a'))));
        let (a, b) = (plan(&g.0, &h('a')).unwrap(), plan(&g.0, &h('b')).unwrap());
        assert_eq!(b.previous.as_deref(), Some(&*h('a')));
        assert_eq!(a.mods, vec![PlanMod { id: "bigmod".into(), version: "2.0.0".into() }]);
        assert_eq!(changes(&b, &a), vec![
            Change { field: "items[game:logs].maxStack".into(), from: Some(json!(30)), to: None },
            Change { field: "items[game:plank].maxStack".into(), from: Some(json!(160)), to: Some(json!(80)) },
        ], "levelCap is the same in both: not a change");
    }

    #[test]
    fn a_plan_that_cannot_be_started_on_is_refused_with_why() {
        let g = Game::new("refused");
        assert_eq!(plan(&g.0, "../x").unwrap_err().code, ErrorCode::InvalidArgument);
        assert!(plan(&g.0, &h('c')).unwrap_err().message.contains("not in kiln/data/content/registry/plans"));
        g.plan(&h('d'), json!({"format": 1, "key": {}, "boot": [], "live": []}));
        let older = plan(&g.0, &h('d')).unwrap_err().message;
        assert!(older.contains("older Kiln"));
        // plans format 2 (with their edits) first ship in 0.2.0-dev.31.17: an earlier Kiln's plan has none to start on
        assert!(older.contains("0.2.0-dev.31.17 or later"), "names the first release that writes format-2 plans: {older}");
        assert!(ask(&g.0, &h('d'), None, "test").is_err(), "and no request is written for it");
        assert_eq!(pending(&g.0).unwrap(), None);
    }

    #[test]
    fn ask_writes_the_request_the_stage_reads_and_clear_removes_it() {
        let g = Game::new("ask");
        g.plan(&h('a'), doc(json!({}), None));
        let r = ask(&g.0, &h('a'), Some(&h('b')), "kilnctl").unwrap();
        let on_disk: Value = serde_json::from_str(&std::fs::read_to_string(data_dir(&g.0).join(REQUEST_NAME)).unwrap()).unwrap();
        assert_eq!((on_disk["format"].as_u64(), on_disk["plan"].as_str(), on_disk["from"].as_str(), on_disk["by"].as_str()),
            (Some(1), Some(&*h('a')), Some(&*h('b')), Some("kilnctl")), "the stage's fields: format 1, plan, from");
        assert_eq!(pending(&g.0).unwrap(), Some(r));
        assert!(clear(&g.0).unwrap());
        assert!(!clear(&g.0).unwrap(), "nothing left to clear");
        assert_eq!(pending(&g.0).unwrap(), None);
    }
}
