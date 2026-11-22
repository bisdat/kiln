// SPDX-License-Identifier: MIT
//! Per-build id tables and the alias check (registry proof step 5, BRIEF 6.1; DECISIONS D3).
//!
//! An id table maps each registry entry's GUID to its Kiln id (`game:<stable name>`) on one game build. It holds the
//! game's names, so it never goes into the repository: kilnctl keeps one per build in a maintainer folder
//! (`$KILN_REGMAP_IDS`, default `<state>/regmap-ids/<side>/ids-<side>-<exe sha256>.json`) and deploys it to kiln.content's
//! data folder (`<game>/kiln/data/content/registry/`, where kiln.content writes the running build's table itself when
//! it is missing). The next update reads the previous build's table from there, so drop-table and clash ids stay stable
//! across records (regmap_check.py --ids-in).
//!
//! The alias check: every id of the previous build resolves on the new one (the same GUID keeps the same id), an entry
//! the new build no longer has is listed as removed in the record, a removed id is never given again, a GUID change keeps
//! its id only where the record pins it, every pin holds, and kiln.content's own table (written in the lab) agrees with
//! regmap_check's on every entry the record does not pin.
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// GUID (lower case; `#<number>` for an entry without one) -> Kiln id, per registry.
pub type Tables = BTreeMap<String, BTreeMap<String, String>>;

#[derive(Debug, Clone, Default, PartialEq)]
pub struct IdTable {
    pub side: String,
    pub exe_sha256: String,
    /// kiln.content's model version when kiln.content wrote it
    pub decoder: Option<String>,
    pub registries: Tables,
}

impl IdTable {
    /// kiln.content's form (`{"format": 1, "side", "exeSha256", "decoder", "registries"}`) or regmap_check.py --ids-out's
    /// (`{<registry>: {<guid>: <id>}}`, then side and exe come from the caller).
    pub fn parse(text: &str, side: &str, exe_sha256: &str) -> Result<IdTable> {
        let v: Value = serde_json::from_str(text).context("not JSON")?;
        let obj = v.as_object().context("an id table is a JSON object")?;
        let native = obj.contains_key("format") && obj.get("registries").is_some_and(Value::is_object);
        let text_of = |k: &str, d: &str| obj.get(k).and_then(Value::as_str).unwrap_or(d).to_string();
        let (side, exe_sha256, decoder, regs) = if native {
            (text_of("side", side), text_of("exeSha256", exe_sha256), obj.get("decoder").and_then(Value::as_str).map(str::to_string), &obj["registries"])
        } else {
            (side.to_string(), exe_sha256.to_string(), None, &v)
        };
        let mut registries = Tables::new();
        for (reg, t) in regs.as_object().context("no registries")? {
            let t = t.as_object().with_context(|| format!("{reg}: not an object of GUID -> id"))?;
            let mut m = BTreeMap::new();
            for (g, id) in t {
                let id = id.as_str().with_context(|| format!("{reg} {g}: the id is not text"))?;
                m.insert(g.to_ascii_lowercase(), id.to_string());
            }
            registries.insert(reg.clone(), m);
        }
        Ok(IdTable { side, exe_sha256, decoder, registries })
    }

    /// kiln.content's form.
    pub fn to_json(&self) -> String {
        let mut o = serde_json::Map::new();
        o.insert("format".into(), 1.into());
        o.insert("side".into(), self.side.clone().into());
        o.insert("exeSha256".into(), self.exe_sha256.clone().into());
        if let Some(d) = &self.decoder {
            o.insert("decoder".into(), d.clone().into());
        }
        o.insert("source".into(), "kilnctl".into());
        o.insert("registries".into(), serde_json::to_value(&self.registries).unwrap_or_default());
        Value::Object(o).to_string()
    }
}

pub fn file_name(side: &str, exe_sha256: &str) -> String {
    format!("ids-{side}-{exe_sha256}.json")
}

/// The maintainer folder of id tables.
#[derive(Debug, Clone)]
pub struct Store {
    pub dir: PathBuf,
}

impl Store {
    /// `$KILN_REGMAP_IDS`, else `<state>/regmap-ids`.
    pub fn default_dir() -> PathBuf {
        match std::env::var_os("KILN_REGMAP_IDS") {
            Some(d) if !d.is_empty() => PathBuf::from(d),
            _ => crate::util::state_dir().join("regmap-ids"),
        }
    }
    pub fn path(&self, side: &str, exe_sha256: &str) -> PathBuf {
        self.dir.join(side).join(file_name(side, exe_sha256))
    }
    pub fn get(&self, side: &str, exe_sha256: &str) -> Option<Result<IdTable>> {
        let p = self.path(side, exe_sha256);
        p.is_file().then(|| read_table(&p, side, exe_sha256))
    }
    pub fn put(&self, t: &IdTable) -> Result<PathBuf> {
        let p = self.path(&t.side, &t.exe_sha256);
        write_whole(&p, &t.to_json())?;
        Ok(p)
    }
}

/// Where kiln.content keeps a build's table in a game's Kiln data folder (`<game>/kiln/data`).
pub fn data_path(data_dir: &Path, side: &str, exe_sha256: &str) -> PathBuf {
    data_dir.join("content/registry").join(file_name(side, exe_sha256))
}

fn read_table(p: &Path, side: &str, exe_sha256: &str) -> Result<IdTable> {
    let text = std::fs::read_to_string(p).with_context(|| format!("reading {}", p.display()))?;
    IdTable::parse(&text, side, exe_sha256).with_context(|| format!("{} is not an id table", p.display()))
}

/// A file written whole, then renamed over the old one.
pub fn write_whole(p: &Path, text: &str) -> Result<()> {
    let dir = p.parent().context("no folder")?;
    std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    let tmp = dir.join(format!(".{}.tmp{}", p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(), std::process::id()));
    std::fs::write(&tmp, text).with_context(|| format!("writing {}", tmp.display()))?;
    std::fs::rename(&tmp, p).with_context(|| format!("renaming {} to {}", tmp.display(), p.display()))
}

/// Writes the table into a game's Kiln data folder (whole, then renamed over).
pub fn deploy(t: &IdTable, data_dir: &Path) -> Result<PathBuf> {
    let p = data_path(data_dir, &t.side, &t.exe_sha256);
    write_whole(&p, &t.to_json())?;
    Ok(p)
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Harvested {
    pub side: String,
    pub exe_sha256: String,
    /// copied into the store (it had none for this build)
    pub stored: bool,
    /// the store's table for this build differs (the store's is kept)
    pub differs: bool,
}

/// Every table kiln.content wrote into a data folder, kept in the store.
pub fn harvest(data_dir: &Path, store: &Store) -> Result<Vec<Harvested>> {
    let dir = data_dir.join("content/registry");
    let Ok(rd) = std::fs::read_dir(&dir) else { return Ok(Vec::new()) };
    let mut names: Vec<String> = rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    names.sort();
    let mut out = Vec::new();
    for n in names {
        let Some((side, sha)) = n.strip_prefix("ids-").and_then(|x| x.strip_suffix(".json")).and_then(|x| x.split_once('-')) else { continue };
        let t = read_table(&dir.join(&n), side, sha)?;
        let (stored, differs) = match store.get(side, sha) {
            None => {
                store.put(&t)?;
                (true, false)
            }
            Some(Ok(have)) => (false, have.registries != t.registries),
            Some(Err(_)) => (false, true),
        };
        out.push(Harvested { side: side.into(), exe_sha256: sha.into(), stored, differs });
    }
    Ok(out)
}

/// Refuses a folder for id tables or proof outputs inside the Kiln tree or any Kiln checkout (a folder with `.git`
/// and `regmap/common`): ids and values never go into the repository.
pub fn guard(dir: &Path, tree: &Path) -> Result<()> {
    let d = absolute(dir);
    if !tree.as_os_str().is_empty() && d.starts_with(absolute(tree)) {
        bail!("{} is inside the Kiln tree {}: id tables and values hold the game's names and never go into the repository \
               (set KILN_REGMAP_IDS or --out to a folder outside it)", d.display(), tree.display());
    }
    if let Some(a) = d.ancestors().find(|a| a.join(".git").exists() && a.join("regmap/common").is_dir()) {
        bail!("{} is inside a Kiln checkout ({}): id tables and values hold the game's names and never go into the repository",
              d.display(), a.display());
    }
    Ok(())
}

/// An absolute path with the existing part's links resolved (the rest may not exist yet).
fn absolute(p: &Path) -> PathBuf {
    let p = if p.is_absolute() { p.to_path_buf() } else { std::env::current_dir().unwrap_or_default().join(p) };
    let (mut base, mut rest) = (p.clone(), Vec::new());
    while !base.exists() {
        match (base.parent(), base.file_name()) {
            (Some(parent), Some(name)) => {
                rest.push(name.to_os_string());
                base = parent.to_path_buf();
            }
            _ => break,
        }
    }
    let mut c = base.canonicalize().unwrap_or(base);
    for n in rest.iter().rev() {
        c.push(n);
    }
    c
}

#[derive(Debug, Clone, Default, PartialEq, Deserialize, Serialize)]
pub struct Pin {
    pub registry: String,
    pub guid: String,
    pub id: String,
}

#[derive(Debug, Clone, Default, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Removal {
    pub registry: String,
    pub id: String,
    #[serde(default)]
    pub removed_in: String,
}

/// A regmap record's `aliases`.
#[derive(Debug, Clone, Default, PartialEq, Deserialize, Serialize)]
pub struct Aliases {
    #[serde(default)]
    pub renames: Vec<Pin>,
    #[serde(default)]
    pub suffixes: Vec<Pin>,
    #[serde(default)]
    pub removals: Vec<Removal>,
}

impl Aliases {
    pub fn of_record(record: &Value) -> Aliases {
        serde_json::from_value(record.get("aliases").cloned().unwrap_or(Value::Null)).unwrap_or_default()
    }
}

#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AliasReport {
    pub ok: bool,
    pub problems: Vec<String>,
    pub notes: Vec<String>,
    pub kept: usize,
    pub added: Vec<String>,
    pub removed: Vec<String>,
    pub guid_changes: Vec<String>,
}

/// The new build's table against the previous build's, the candidate record's aliases and (when the lab wrote one)
/// kiln.content's own table of the new build.
pub fn check(prev: &IdTable, now: &IdTable, aliases: &Aliases, native: Option<&IdTable>) -> AliasReport {
    use std::collections::BTreeSet;
    let mut r = AliasReport::default();
    let pins: BTreeMap<(String, String), String> =
        aliases.renames.iter().chain(&aliases.suffixes).map(|p| ((p.registry.clone(), p.guid.to_ascii_lowercase()), p.id.clone())).collect();
    let removed: BTreeMap<(String, String), String> =
        aliases.removals.iter().map(|x| ((x.registry.clone(), x.id.clone()), x.removed_in.clone())).collect();
    let empty = BTreeMap::new();
    // every id of the previous build
    for (reg, pt) in &prev.registries {
        let nt = match now.registries.get(reg) {
            Some(t) if !t.is_empty() || pt.is_empty() => t,
            _ => {
                r.problems.push(format!("{reg}: the new build's table has no entries (they were not read on the new build)"));
                continue;
            }
        };
        let by_id: BTreeMap<&String, &String> = nt.iter().map(|(g, i)| (i, g)).collect();
        for (g, id) in pt {
            match nt.get(g) {
                Some(i) if i == id => r.kept += 1,
                Some(i) => r.problems.push(format!("{reg}: {id} (GUID {g}) is {i} on the new build: a Kiln id never moves (the record must pin it)")),
                None => match by_id.get(id) {
                    Some(g2) if pins.get(&(reg.clone(), (*g2).clone())) == Some(id) => r.guid_changes.push(format!("{reg} {id}: {g} -> {g2}")),
                    Some(g2) => r.problems.push(format!("{reg}: {id} was GUID {g}; the new build gives it to GUID {g2} (record the GUID \
                                                         change with a pin, or retire the id)")),
                    None if removed.contains_key(&(reg.clone(), id.clone())) => r.removed.push(format!("{reg} {id}")),
                    None => r.problems.push(format!("{reg}: {id} (GUID {g}) is gone on the new build and the record does not list it as removed")),
                },
            }
        }
    }
    // the new build's ids: unique, never a removed one
    for (reg, nt) in &now.registries {
        let pt = prev.registries.get(reg).unwrap_or(&empty);
        let prev_ids: BTreeSet<&String> = pt.values().collect();
        let mut seen: BTreeMap<&String, &String> = BTreeMap::new();
        for (g, id) in nt {
            if let Some(other) = seen.insert(id, g) {
                r.problems.push(format!("{reg}: {id} is given to two entries (GUIDs {other} and {g})"));
            }
            if let Some(b) = removed.get(&(reg.clone(), id.clone())) {
                r.problems.push(format!("{reg}: {id} was removed in game build {b} and is never given again; the new build gives it to GUID {g}"));
            }
            if !pt.contains_key(g) && !prev_ids.contains(id) {
                r.added.push(format!("{reg} {id}"));
            }
        }
    }
    // every pin holds
    for ((reg, g), id) in &pins {
        match now.registries.get(reg).and_then(|t| t.get(g)) {
            Some(i) if i == id => {}
            Some(i) => r.problems.push(format!("{reg}: the record pins GUID {g} to {id}, the new build's table gives {i}")),
            None => r.problems.push(format!("{reg}: the record pins GUID {g} ({id}), which the new build does not have")),
        }
    }
    // kiln.content's own table (the lab ran without the candidate record: pinned entries may differ)
    if let Some(nat) = native {
        for (reg, nt) in &now.registries {
            let Some(xt) = nat.registries.get(reg) else {
                r.notes.push(format!("{reg}: kiln.content's table of the new build has no entries for it"));
                continue;
            };
            let diff: Vec<String> = nt
                .iter()
                .filter(|(g, _)| !pins.contains_key(&(reg.clone(), (*g).clone())))
                .filter_map(|(g, id)| xt.get(g).filter(|x| *x != id).map(|x| format!("GUID {g}: {x} / {id}")))
                .collect();
            if let Some(first) = diff.first() {
                r.problems.push(format!("{reg}: kiln.content's table and regmap_check's give {} entr{} different ids (first: {first})",
                                        diff.len(), if diff.len() == 1 { "y" } else { "ies" }));
            }
        }
    }
    r.ok = r.problems.is_empty();
    r
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use serde_json::json;

    pub(crate) const OLD: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    pub(crate) const NEW: &str = "1111111111111111111111111111111111111111111111111111111111111111";

    pub(crate) fn table(sha: &str, gadgets: &[(&str, &str)]) -> IdTable {
        let mut r = Tables::new();
        r.insert("gadgets".into(), gadgets.iter().map(|(g, i)| (g.to_string(), i.to_string())).collect());
        IdTable { side: "server".into(), exe_sha256: sha.into(), decoder: None, registries: r }
    }

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("kilnctl-ids-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn parses_both_table_forms_and_writes_kiln_contents() {
        let native = r##"{"format": 1, "side": "server", "exeSha256": "ab", "decoder": "kiln-model 1",
                         "registries": {"gadgets": {"0A-B": "game:a", "#7": "game:item_00000007"}}}"##;
        let t = IdTable::parse(native, "client", "zz").unwrap();
        assert_eq!((t.side.as_str(), t.exe_sha256.as_str(), t.decoder.as_deref()), ("server", "ab", Some("kiln-model 1")));
        assert_eq!(t.registries["gadgets"]["0a-b"], "game:a", "GUIDs folded to lower case");
        assert_eq!(t.registries["gadgets"]["#7"], "game:item_00000007");
        let bare = IdTable::parse(r#"{"gadgets": {"g1": "game:a"}}"#, "server", NEW).unwrap();
        assert_eq!((bare.side.as_str(), bare.exe_sha256.as_str()), ("server", NEW));
        let back: Value = serde_json::from_str(&bare.to_json()).unwrap();
        assert_eq!(back["format"], 1);
        assert_eq!(back["exeSha256"], NEW);
        assert_eq!(back["registries"]["gadgets"]["g1"], "game:a");
        assert_eq!(IdTable::parse(&bare.to_json(), "x", "y").unwrap(), bare);
        assert!(IdTable::parse("[1]", "server", NEW).is_err());
        assert!(IdTable::parse(r#"{"gadgets": {"g1": 3}}"#, "server", NEW).is_err());
        assert_eq!(file_name(&bare.side, &bare.exe_sha256), format!("ids-server-{NEW}.json"));
    }

    #[test]
    fn the_store_keeps_deploys_and_harvests_tables() {
        let root = scratch("store");
        let store = Store { dir: root.join("store") };
        let t = table(NEW, &[("g1", "game:a")]);
        assert!(store.get("server", NEW).is_none());
        let p = store.put(&t).unwrap();
        assert_eq!(p, root.join(format!("store/server/ids-server-{NEW}.json")));
        assert_eq!(store.get("server", NEW).unwrap().unwrap(), t);
        // into a game's data folder, in kiln.content's form
        let data = root.join("game/kiln/data");
        let d = deploy(&t, &data).unwrap();
        assert_eq!(d, data.join(format!("content/registry/ids-server-{NEW}.json")));
        assert_eq!(IdTable::parse(&std::fs::read_to_string(&d).unwrap(), "?", "?").unwrap(), t);
        assert!(!std::fs::read_dir(d.parent().unwrap()).unwrap().flatten().any(|e| e.file_name().to_string_lossy().contains(".tmp")));
        // harvest: a table the store lacks is kept; one that differs from the store's leaves the store's alone
        let old = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        deploy(&old, &data).unwrap();
        deploy(&table(NEW, &[("g1", "game:other")]), &data).unwrap();
        std::fs::write(data.join("content/registry/notes.json"), "{}").unwrap();
        let h = harvest(&data, &store).unwrap();
        assert_eq!(h, [
            Harvested { side: "server".into(), exe_sha256: NEW.into(), stored: false, differs: true },
            Harvested { side: "server".into(), exe_sha256: OLD.into(), stored: true, differs: false },
        ]);
        assert_eq!(store.get("server", OLD).unwrap().unwrap(), old);
        assert_eq!(store.get("server", NEW).unwrap().unwrap(), t, "the store's table is kept");
        assert!(harvest(&root.join("nowhere"), &store).unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_store_default_is_outside_any_repository() {
        let d = Store::default_dir();
        assert!(d.ends_with("regmap-ids"), "{}", d.display());
    }

    #[test]
    fn ids_never_go_into_a_kiln_checkout() {
        let root = scratch("guard");
        let tree = root.join("kiln");
        std::fs::create_dir_all(tree.join("regmap/common")).unwrap();
        std::fs::create_dir_all(tree.join(".git")).unwrap();
        assert!(guard(&tree.join("out/ids"), &tree).is_err(), "inside the Kiln tree");
        let other = root.join("other-checkout");
        std::fs::create_dir_all(other.join("regmap/common")).unwrap();
        std::fs::write(other.join(".git"), "gitdir: elsewhere").unwrap();
        assert!(guard(&other.join("a/b"), &tree).is_err(), "inside another Kiln checkout (a worktree)");
        assert!(guard(&root.join("state/regmap-ids"), &tree).is_ok());
        let _ = std::fs::remove_dir_all(&root);
    }

    fn aliases(v: Value) -> Aliases {
        Aliases::of_record(&json!({"aliases": v}))
    }

    #[test]
    fn a_stable_build_keeps_every_id() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let now = table(NEW, &[("g1", "game:a"), ("g2", "game:b"), ("g3", "game:c")]);
        let r = check(&prev, &now, &Aliases::default(), Some(&now));
        assert!(r.ok, "{:?}", r.problems);
        assert_eq!(r.kept, 2);
        assert_eq!(r.added, ["gadgets game:c"]);
    }

    #[test]
    fn removals_must_be_recorded_and_never_reused() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let now = table(NEW, &[("g1", "game:a")]);
        let unrecorded = check(&prev, &now, &Aliases::default(), None);
        assert!(!unrecorded.ok && unrecorded.problems[0].contains("game:b"), "{:?}", unrecorded.problems);
        let rec = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:b", "removedIn": "101"}]}));
        let r = check(&prev, &now, &rec, None);
        assert!(r.ok, "{:?}", r.problems);
        assert_eq!(r.removed, ["gadgets game:b"]);
        // an id removed in an earlier build given to a new entry
        let older = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:z", "removedIn": "90"}]}));
        let reuse = check(&prev, &table(NEW, &[("g1", "game:a"), ("g2", "game:b"), ("g9", "game:z")]), &older, None);
        assert!(!reuse.ok && reuse.problems.iter().any(|p| p.contains("game:z") && p.contains("removed")), "{:?}", reuse.problems);
    }

    #[test]
    fn ids_never_move_and_guid_changes_need_a_pin() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let moved = check(&prev, &table(NEW, &[("g1", "game:a_renamed"), ("g2", "game:b")]), &Aliases::default(), None);
        assert!(!moved.ok && moved.problems[0].contains("game:a"), "{:?}", moved.problems);
        // the game gave game:b's entry a new GUID: the id follows only where the record pins it
        let rekeyed = table(NEW, &[("g1", "game:a"), ("g7", "game:b")]);
        let unpinned = check(&prev, &rekeyed, &Aliases::default(), None);
        assert!(!unpinned.ok && unpinned.problems.iter().any(|p| p.contains("game:b")), "{:?}", unpinned.problems);
        let pinned = check(&prev, &rekeyed, &aliases(json!({"renames": [{"registry": "gadgets", "guid": "g7", "id": "game:b"}]})), None);
        assert!(pinned.ok, "{:?}", pinned.problems);
        assert_eq!(pinned.guid_changes, ["gadgets game:b: g2 -> g7"]);
    }

    #[test]
    fn pins_hold_ids_are_unique_and_both_implementations_agree() {
        let prev = table(OLD, &[("g1", "game:a")]);
        let now = table(NEW, &[("g1", "game:a"), ("g2", "game:x~00000002")]);
        let stale = aliases(json!({"suffixes": [{"registry": "gadgets", "guid": "g2", "id": "game:x~0000000f"}]}));
        assert!(!check(&prev, &now, &stale, None).ok, "a pin the table does not hold");
        let gone = aliases(json!({"renames": [{"registry": "gadgets", "guid": "g404", "id": "game:q"}]}));
        assert!(!check(&prev, &now, &gone, None).ok, "a pin of a GUID the build does not have");
        let twice = check(&prev, &table(NEW, &[("g1", "game:a"), ("g2", "game:a")]), &Aliases::default(), None);
        assert!(!twice.ok && twice.problems.iter().any(|p| p.contains("two entries")), "{:?}", twice.problems);
        // kiln.content's own table differs on an entry the record does not pin: the implementations disagree
        let native = table(NEW, &[("g1", "game:a"), ("g2", "game:x")]);
        let r = check(&prev, &now, &Aliases::default(), Some(&native));
        assert!(!r.ok && r.problems.iter().any(|p| p.contains("kiln.content")), "{:?}", r.problems);
        // ... but not on a pinned one (the lab ran without the candidate record)
        let pin = aliases(json!({"suffixes": [{"registry": "gadgets", "guid": "g2", "id": "game:x~00000002"}]}));
        assert!(check(&prev, &now, &pin, Some(&native)).ok);
        // a registry with no entries on the new build: its decode failed
        let mut empty = now.clone();
        empty.registries.clear();
        assert!(!check(&prev, &empty, &Aliases::default(), None).ok);
    }
}
