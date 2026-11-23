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
        Self::from_value(&serde_json::from_str(text).context("not JSON")?, side, exe_sha256)
    }

    pub fn from_value(v: &Value, side: &str, exe_sha256: &str) -> Result<IdTable> {
        let obj = v.as_object().context("an id table is a JSON object")?;
        let native = obj.contains_key("format") && obj.get("registries").is_some_and(Value::is_object);
        let text_of = |k: &str, d: &str| obj.get(k).and_then(Value::as_str).unwrap_or(d).to_string();
        let (side, exe_sha256, decoder, regs) = if native {
            (text_of("side", side), text_of("exeSha256", exe_sha256), obj.get("decoder").and_then(Value::as_str).map(str::to_string), &obj["registries"])
        } else {
            (side.to_string(), exe_sha256.to_string(), None, v)
        };
        // a table names its build: it becomes a file name in the maintainer folder
        if native && (!valid_side(&side) || !valid_sha(&exe_sha256)) {
            bail!("the table's side {side:?} or exeSha256 {exe_sha256:?} is not a side and an exe's SHA-256");
        }
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

pub fn valid_side(s: &str) -> bool {
    s == "server" || s == "client"
}

/// 64 lower-case hex digits.
pub fn valid_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
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
    /// Keeps a proven table; None when the store already holds one of this build (never overwritten).
    pub fn put(&self, t: &IdTable) -> Result<Option<PathBuf>> {
        if !valid_side(&t.side) || !valid_sha(&t.exe_sha256) {
            bail!("an id table of side {:?} and exe {:?} is not kept", t.side, t.exe_sha256);
        }
        let p = self.path(&t.side, &t.exe_sha256);
        if p.exists() {
            return Ok(None);
        }
        write_whole(&p, &t.to_json())?;
        Ok(Some(p))
    }
    /// Where a table harvested from a data folder is kept (apart from the proven ones).
    pub fn harvested_path(&self, side: &str, exe_sha256: &str) -> PathBuf {
        self.dir.join(side).join("harvested").join(file_name(side, exe_sha256))
    }
    /// The proven table of a build, else the harvested one.
    pub fn get_any(&self, side: &str, exe_sha256: &str) -> Option<Result<IdTable>> {
        self.get(side, exe_sha256).or_else(|| {
            let p = self.harvested_path(side, exe_sha256);
            p.is_file().then(|| read_table(&p, side, exe_sha256))
        })
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
/// What [`harvest`] found: the tables kept, and the files it could not take (named, not fatal).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Harvest {
    pub tables: Vec<Harvested>,
    pub problems: Vec<String>,
}

pub fn harvest(data_dir: &Path, store: &Store) -> Result<Harvest> {
    let dir = data_dir.join("content/registry");
    let Ok(rd) = std::fs::read_dir(&dir) else { return Ok(Harvest::default()) };
    let mut names: Vec<String> = rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    names.sort();
    let (mut out, mut problems) = (Vec::new(), Vec::new());
    for n in names {
        let Some((side, sha)) = n.strip_prefix("ids-").and_then(|x| x.strip_suffix(".json")).and_then(|x| x.split_once('-')) else { continue };
        let p = dir.join(&n);
        if !valid_side(side) || !valid_sha(sha) {
            problems.push(format!("{}: not the name of a build's id table (ids-<server|client>-<exe sha256>.json); not taken", p.display()));
            continue;
        }
        let t = match read_table(&p, side, sha) {
            Ok(t) if t.side == side && t.exe_sha256 == sha => t,
            Ok(t) => {
                problems.push(format!("{}: the table inside is of {} {}, not of the build its name says; not taken", p.display(), t.side, t.exe_sha256));
                continue;
            }
            Err(e) => {
                problems.push(format!("{e:#}; not taken"));
                continue;
            }
        };
        let (stored, differs) = match store.get_any(side, sha) {
            None => {
                write_whole(&store.harvested_path(side, sha), &t.to_json())?;
                (true, false)
            }
            Some(Ok(have)) => (false, have.registries != t.registries),
            Some(Err(_)) => (false, true),
        };
        out.push(Harvested { side: side.into(), exe_sha256: sha.into(), stored, differs });
    }
    Ok(Harvest { tables: out, problems })
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

/// The new build's table to check and keep: regmap_check.py's (its pins applied) with kiln.content's entries it does not
/// cover (entries without a GUID, such as the loot registry's, which kiln.content keys `#<id>`; registries regmap_check.py
/// does not name), so the kept table is as complete as the one kiln.content writes for itself.
pub fn merged(computed: &IdTable, native: Option<&IdTable>) -> IdTable {
    let mut t = computed.clone();
    if let Some(n) = native {
        if t.decoder.is_none() {
            t.decoder = n.decoder.clone();
        }
        for (reg, nt) in &n.registries {
            let uncovered = t.registries.get(reg).is_none_or(|e| e.is_empty());
            let e = t.registries.entry(reg.clone()).or_default();
            // a GUID entry regmap_entries.py missed in a registry it covers stays missing (the agreement check sees it)
            for (k, id) in nt.iter().filter(|(k, _)| uncovered || k.starts_with('#')) {
                e.entry(k.clone()).or_insert_with(|| id.clone());
            }
        }
    }
    t
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
pub fn check(prev: &IdTable, now: &IdTable, aliases: &Aliases, previous: &Aliases, native: Option<&IdTable>) -> AliasReport {
    use std::collections::BTreeSet;
    let mut r = AliasReport::default();
    let pins: BTreeMap<(String, String), String> =
        aliases.renames.iter().chain(&aliases.suffixes).map(|p| ((p.registry.clone(), p.guid.to_ascii_lowercase()), p.id.clone())).collect();
    let removed: BTreeMap<(String, String), String> =
        aliases.removals.iter().map(|x| ((x.registry.clone(), x.id.clone()), x.removed_in.clone())).collect();
    // the previous record's history: an id table holds no removed ids, so the records carry them
    let earlier: BTreeMap<(String, String), String> =
        previous.removals.iter().map(|x| ((x.registry.clone(), x.id.clone()), x.removed_in.clone())).collect();
    for ((reg, id), b) in &earlier {
        if !removed.contains_key(&(reg.clone(), id.clone())) {
            r.problems.push(format!("{reg}: the candidate drops the removal of {id} (removed in game build {b}): a record carries every earlier removal"));
        }
    }
    for p in previous.renames.iter().chain(&previous.suffixes) {
        let kept = pins.get(&(p.registry.clone(), p.guid.to_ascii_lowercase())) == Some(&p.id);
        if !kept && !removed.contains_key(&(p.registry.clone(), p.id.clone())) {
            r.problems.push(format!("{}: the previous record pins GUID {} to {}; the candidate neither keeps the pin nor lists {} as removed",
                                    p.registry, p.guid, p.id, p.id));
        }
    }
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
            if let Some(b) = removed.get(&(reg.clone(), id.clone())).or_else(|| earlier.get(&(reg.clone(), id.clone()))) {
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
        let native = r##"{"format": 1, "side": "server", "exeSha256": "abababababababababababababababababababababababababababababababab", "decoder": "kiln-model 1",
                         "registries": {"gadgets": {"0A-B": "game:a", "#7": "game:item_00000007"}}}"##;
        let t = IdTable::parse(native, "client", "zz").unwrap();
        assert_eq!((t.side.as_str(), t.exe_sha256.as_str(), t.decoder.as_deref()), ("server", "ab".repeat(32).as_str(), Some("kiln-model 1")));
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
        assert_eq!(p, Some(root.join(format!("store/server/ids-server-{NEW}.json"))));
        assert_eq!(store.get("server", NEW).unwrap().unwrap(), t);
        assert_eq!(store.put(&table(NEW, &[("g1", "game:z")])).unwrap(), None, "a kept table is never overwritten");
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
        let h = harvest(&data, &store).unwrap().tables;
        assert_eq!(h, [
            Harvested { side: "server".into(), exe_sha256: NEW.into(), stored: false, differs: true },
            Harvested { side: "server".into(), exe_sha256: OLD.into(), stored: true, differs: false },
        ]);
        assert!(store.get("server", OLD).is_none(), "harvested tables are kept apart from proven ones");
        assert!(store.harvested_path("server", OLD).is_file());
        assert_eq!(store.get_any("server", OLD).unwrap().unwrap(), old);
        assert_eq!(store.get("server", NEW).unwrap().unwrap(), t, "the store's table is kept");
        assert!(harvest(&root.join("nowhere"), &store).unwrap().tables.is_empty());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A data folder is written by the game (and mods under Wine): names inside a table never choose where kilnctl writes.
    #[test]
    fn harvest_takes_only_tables_whose_names_match_and_names_the_rest() {
        let root = scratch("hostile");
        let store = Store { dir: root.join("store") };
        let reg = root.join("data/content/registry");
        std::fs::create_dir_all(&reg).unwrap();
        let w = |n: &str, v: Value| std::fs::write(reg.join(n), v.to_string()).unwrap();
        w(&format!("ids-server-{NEW}.json"), json!({"format": 1, "side": "../../x", "exeSha256": NEW, "registries": {}}));
        w(&format!("ids-server-{OLD}.json"), json!({"format": 1, "side": "client", "exeSha256": OLD, "registries": {}}));
        w("ids-server-nothex.json", json!({"format": 1, "side": "server", "exeSha256": "nothex", "registries": {}}));
        std::fs::write(reg.join(format!("ids-client-{NEW}.json")), "{ truncated").unwrap();
        let h = harvest(&root.join("data"), &store).unwrap();
        assert!(h.tables.is_empty(), "{:?}", h.tables);
        assert_eq!(h.problems.len(), 4, "{:?}", h.problems);
        assert!(!root.join("x").exists() && !root.join("store/x").exists());
        assert!(IdTable::parse(&json!({"format": 1, "side": "../../x", "exeSha256": NEW, "registries": {}}).to_string(), "server", NEW).is_err());
        assert!(IdTable::parse(&json!({"format": 1, "side": "server", "exeSha256": "ab", "registries": {}}).to_string(), "server", NEW).is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn merged_fills_only_entries_without_a_guid_and_uncovered_registries() {
        let computed = table(NEW, &[("g1", "game:a")]);
        let mut native = table(NEW, &[("g1", "game:a"), ("g2", "game:b"), ("#9", "game:n")]);
        native.registries.insert("cats".into(), BTreeMap::from([("c1".to_string(), "game:wood".to_string())]));
        let m = merged(&computed, Some(&native));
        assert!(!m.registries["gadgets"].contains_key("g2"), "an entry regmap_entries.py missed stays missing: {m:?}");
        assert_eq!(m.registries["gadgets"]["#9"], "game:n");
        assert_eq!(m.registries["cats"]["c1"], "game:wood");
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
    fn kiln_contents_entries_without_a_guid_complete_the_computed_table() {
        // the shape of a real build: regmap_check.py's table has no loot entries (they have no GUID), kiln.content keys
        // them by number and has a registry regmap_check.py does not name
        let mut native = table(NEW, &[("g1", "game:a"), ("g2", "game:b")]);
        native.registries.insert("loot".into(), BTreeMap::from([("#7".to_string(), "game:a".to_string())]));
        native.registries.insert("cats".into(), BTreeMap::from([("c1".to_string(), "game:wood".to_string())]));
        native.decoder = Some("kiln-model 1".into());
        let mut computed = table(NEW, &[("g1", "game:a"), ("g2", "game:b")]);
        computed.registries.insert("loot".into(), BTreeMap::new());
        let mut prev = native.clone();
        prev.exe_sha256 = OLD.into();
        // without kiln.content's entries the previous build's loot ids would look lost
        assert!(!check(&prev, &computed, &Aliases::default(), &Aliases::default(), Some(&native)).ok);
        let m = merged(&computed, Some(&native));
        assert_eq!(m.registries["loot"]["#7"], "game:a");
        assert_eq!(m.registries["cats"]["c1"], "game:wood");
        assert_eq!(m.decoder.as_deref(), Some("kiln-model 1"));
        let r = check(&prev, &m, &Aliases::default(), &Aliases::default(), Some(&native));
        assert!(r.ok, "{:?}", r.problems);
        assert_eq!(r.kept, 4);
        // regmap_check.py's ids win where both have the entry (its pins hold)
        let mut pinned = computed.clone();
        pinned.registries.get_mut("gadgets").unwrap().insert("g2".into(), "game:b_kept".into());
        assert_eq!(merged(&pinned, Some(&native)).registries["gadgets"]["g2"], "game:b_kept");
        assert_eq!(merged(&computed, None), computed);
    }

    #[test]
    fn a_stable_build_keeps_every_id() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let now = table(NEW, &[("g1", "game:a"), ("g2", "game:b"), ("g3", "game:c")]);
        let r = check(&prev, &now, &Aliases::default(), &Aliases::default(), Some(&now));
        assert!(r.ok, "{:?}", r.problems);
        assert_eq!(r.kept, 2);
        assert_eq!(r.added, ["gadgets game:c"]);
    }

    #[test]
    fn removals_must_be_recorded_and_never_reused() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let now = table(NEW, &[("g1", "game:a")]);
        let unrecorded = check(&prev, &now, &Aliases::default(), &Aliases::default(), None);
        assert!(!unrecorded.ok && unrecorded.problems[0].contains("game:b"), "{:?}", unrecorded.problems);
        let rec = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:b", "removedIn": "101"}]}));
        let r = check(&prev, &now, &rec, &Aliases::default(), None);
        assert!(r.ok, "{:?}", r.problems);
        assert_eq!(r.removed, ["gadgets game:b"]);
        // an id removed in an earlier build given to a new entry
        let older = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:z", "removedIn": "90"}]}));
        let reuse = check(&prev, &table(NEW, &[("g1", "game:a"), ("g2", "game:b"), ("g9", "game:z")]), &older, &Aliases::default(), None);
        assert!(!reuse.ok && reuse.problems.iter().any(|p| p.contains("game:z") && p.contains("removed")), "{:?}", reuse.problems);
    }

    /// The removal history lives in the records (an id table does not hold removed ids): every earlier removal is carried
    /// by the candidate and never reused, and every earlier pin is kept or its id retired.
    #[test]
    fn the_removal_history_carries_through_the_previous_record() {
        let prev = table(OLD, &[("g1", "game:a")]);
        let earlier = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:z", "removedIn": "100"}],
                                     "renames": [{"registry": "gadgets", "guid": "g1", "id": "game:a"}]}));
        let keeps = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:z", "removedIn": "100"}],
                                   "renames": [{"registry": "gadgets", "guid": "g1", "id": "game:a"}]}));
        let now = table(NEW, &[("g1", "game:a")]);
        assert!(check(&prev, &now, &keeps, &earlier, None).ok, "the candidate carries the history");
        // the candidate dropped game:z's removal (the previous record was not where the record step looked)
        let drops = aliases(json!({"renames": [{"registry": "gadgets", "guid": "g1", "id": "game:a"}]}));
        let r = check(&prev, &now, &drops, &earlier, None);
        assert!(!r.ok && r.problems.iter().any(|p| p.contains("game:z") && p.contains("drops")), "{:?}", r.problems);
        // ... and a new entry got the retired id: blocked even though the candidate does not list the removal
        let reused = table(NEW, &[("g1", "game:a"), ("g9", "game:z")]);
        let r = check(&prev, &reused, &drops, &earlier, None);
        assert!(r.problems.iter().any(|p| p.contains("game:z") && p.contains("never given again")), "{:?}", r.problems);
        // an earlier pin neither kept nor retired
        let unpinned = aliases(json!({"removals": [{"registry": "gadgets", "id": "game:z", "removedIn": "100"}]}));
        let r = check(&prev, &now, &unpinned, &earlier, None);
        assert!(!r.ok && r.problems.iter().any(|p| p.contains("g1") && p.contains("previous record pins")), "{:?}", r.problems);
    }

    #[test]
    fn ids_never_move_and_guid_changes_need_a_pin() {
        let prev = table(OLD, &[("g1", "game:a"), ("g2", "game:b")]);
        let moved = check(&prev, &table(NEW, &[("g1", "game:a_renamed"), ("g2", "game:b")]), &Aliases::default(), &Aliases::default(), None);
        assert!(!moved.ok && moved.problems[0].contains("game:a"), "{:?}", moved.problems);
        // the game gave game:b's entry a new GUID: the id follows only where the record pins it
        let rekeyed = table(NEW, &[("g1", "game:a"), ("g7", "game:b")]);
        let unpinned = check(&prev, &rekeyed, &Aliases::default(), &Aliases::default(), None);
        assert!(!unpinned.ok && unpinned.problems.iter().any(|p| p.contains("game:b")), "{:?}", unpinned.problems);
        let pinned = check(&prev, &rekeyed, &aliases(json!({"renames": [{"registry": "gadgets", "guid": "g7", "id": "game:b"}]})), &Aliases::default(), None);
        assert!(pinned.ok, "{:?}", pinned.problems);
        assert_eq!(pinned.guid_changes, ["gadgets game:b: g2 -> g7"]);
    }

    #[test]
    fn pins_hold_ids_are_unique_and_both_implementations_agree() {
        let prev = table(OLD, &[("g1", "game:a")]);
        let now = table(NEW, &[("g1", "game:a"), ("g2", "game:x~00000002")]);
        let stale = aliases(json!({"suffixes": [{"registry": "gadgets", "guid": "g2", "id": "game:x~0000000f"}]}));
        assert!(!check(&prev, &now, &stale, &Aliases::default(), None).ok, "a pin the table does not hold");
        let gone = aliases(json!({"renames": [{"registry": "gadgets", "guid": "g404", "id": "game:q"}]}));
        assert!(!check(&prev, &now, &gone, &Aliases::default(), None).ok, "a pin of a GUID the build does not have");
        let twice = check(&prev, &table(NEW, &[("g1", "game:a"), ("g2", "game:a")]), &Aliases::default(), &Aliases::default(), None);
        assert!(!twice.ok && twice.problems.iter().any(|p| p.contains("two entries")), "{:?}", twice.problems);
        // kiln.content's own table differs on an entry the record does not pin: the implementations disagree
        let native = table(NEW, &[("g1", "game:a"), ("g2", "game:x")]);
        let r = check(&prev, &now, &Aliases::default(), &Aliases::default(), Some(&native));
        assert!(!r.ok && r.problems.iter().any(|p| p.contains("kiln.content")), "{:?}", r.problems);
        // ... but not on a pinned one (the lab ran without the candidate record)
        let pin = aliases(json!({"suffixes": [{"registry": "gadgets", "guid": "g2", "id": "game:x~00000002"}]}));
        assert!(check(&prev, &now, &pin, &Aliases::default(), Some(&native)).ok);
        // a registry with no entries on the new build: its decode failed
        let mut empty = now.clone();
        empty.registries.clear();
        assert!(!check(&prev, &empty, &Aliases::default(), &Aliases::default(), None).ok);
    }
}
