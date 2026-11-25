//! EMF mods' dependencies, checked before Play from their mod.json as kiln-emf checks them at the start
//! (native/plugins/emf/src/mods.cpp: `read_deps`, `parse_range`, `version_problem`, `resolve_order`), so the launcher
//! can say on a mod's row what is missing and how to get it instead of the player finding "needs lua, which is not
//! installed" in kiln/state/emf.json after a start (PepegaJedi's Mage Skills, 0.2.0-dev.30.1).
//!
//! The mods EMF would see: the profile's EMF entries (their Library folders), then, while the profile has EMF drop-ins
//! on, `<game>/emf/mods` (folders with a mod.json that are not Kiln's copy of a profile entry, and loose DLLs, id = file
//! name, version 0.0.0). A second copy of one id is ignored, the first wins (kiln-emf `add_mod`). Not read here: EMF's
//! ini switch (`<configDir>/emf/<id>.ini` `enabled`) and the EMF API version a mod asks for.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::manifest::{ModKind, RunSide};
use crate::paths::DataDirs;
use crate::profile::Profile;

// ------------------------------------------------------------------------------------------------- versions ------

type V3 = [i64; 3];
const TOP: V3 = [i64::MAX, 0, 0];

#[derive(Clone, Copy, Default)]
struct Version {
    v: V3,
    /// how many numbers were written (0..3)
    parts: usize,
    /// an x / * took the place of the next part ("1.2.x", "1.*")
    wild: bool,
}

/// EMF `parse_version`: "1", "1.2" or "1.2.3", numbers only (a wildcard part with `allow_wild`).
fn parse_version(s: &str, allow_wild: bool) -> Option<Version> {
    let mut r = Version::default();
    let mut rest = s;
    loop {
        let (part, more) = match rest.find('.') {
            Some(j) => (&rest[..j], Some(&rest[j + 1..])),
            None => (rest, None),
        };
        if part.is_empty() || r.parts >= 3 { return None; }
        if matches!(part, "x" | "X" | "*") {
            if !allow_wild { return None; }
            r.wild = true;
            break; // whatever follows a wildcard is a wildcard too
        }
        let mut n: i64 = 0;
        for c in part.bytes() {
            if !c.is_ascii_digit() { return None; }
            n = n * 10 + i64::from(c - b'0');
            if n > 1_000_000_000 { return None; }
        }
        r.v[r.parts] = n;
        r.parts += 1;
        match more {
            Some(m) => rest = m,
            None => break,
        }
    }
    if r.parts == 0 && !r.wild { return None; }
    Some(r)
}

/// The first version after the family of v's first `at` parts.
fn bump(v: &Version, at: usize) -> V3 {
    if at == 0 { return TOP; }
    let mut r = [0; 3];
    r[..at.min(3)].copy_from_slice(&v.v[..at.min(3)]);
    r[at - 1] += 1;
    r
}

/// lo <= v < hi (lo_incl / hi_incl adjust)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Interval {
    lo: V3,
    lo_incl: bool,
    hi: V3,
    hi_incl: bool,
}

impl Default for Interval {
    fn default() -> Self { Interval { lo: [0; 3], lo_incl: true, hi: TOP, hi_incl: false } }
}

impl Interval {
    fn contains(&self, v: &V3) -> bool {
        let a = v.cmp(&self.lo);
        let b = v.cmp(&self.hi);
        (a.is_gt() || (a.is_eq() && self.lo_incl)) && (b.is_lt() || (b.is_eq() && self.hi_incl))
    }
    fn intersect(&mut self, o: &Interval) {
        let a = o.lo.cmp(&self.lo);
        if a.is_gt() || (a.is_eq() && !o.lo_incl) {
            self.lo = o.lo;
            self.lo_incl = o.lo_incl;
        }
        let b = o.hi.cmp(&self.hi);
        if b.is_lt() || (b.is_eq() && !o.hi_incl) {
            self.hi = o.hi;
            self.hi_incl = o.hi_incl;
        }
    }
    fn empty(&self) -> bool {
        let c = self.lo.cmp(&self.hi);
        c.is_gt() || (c.is_eq() && !(self.lo_incl && self.hi_incl))
    }
}

/// EMF `comparator`: one of `>= <= > < = ^ ~` (or none) and a version, or a wildcard alone.
fn comparator(t: &str) -> Option<Interval> {
    let mut r = Interval::default();
    if matches!(t, "" | "*" | "x" | "X") { return Some(r); }
    let op = [">=", "<=", ">", "<", "=", "^", "~"].into_iter().find(|o| t.starts_with(o)).unwrap_or("");
    let v = parse_version(&t[op.len()..], true)?;
    let p = v.parts;
    let full = p == 3 && !v.wild;
    if op.is_empty() && (v.wild || v.parts == 0) {
        // x-range: "1.2.x" = 1.2.0 up to below 1.3.0
        r.lo = v.v;
        if p > 0 { r.hi = bump(&v, p); }
    } else if op.is_empty() || op == "^" {
        // a bare version is the same as ^ (EMF)
        r.lo = v.v;
        let at = if v.v[0] > 0 || p == 1 { 1 } else if v.v[1] > 0 || p == 2 { 2 } else { 3 };
        r.hi = bump(&v, at);
    } else if op == "~" {
        r.lo = v.v;
        r.hi = bump(&v, if p >= 2 { 2 } else { 1 });
    } else if op == ">=" {
        r.lo = v.v;
    } else if op == ">" {
        if full {
            r.lo = v.v;
            r.lo_incl = false;
        } else {
            r.lo = bump(&v, p);
        }
    } else if op == "<" {
        r.hi = v.v;
    } else if op == "<=" {
        if full {
            r.hi = v.v;
            r.hi_incl = true;
        } else {
            r.hi = bump(&v, p);
        }
    } else {
        // "="
        r.lo = v.v;
        r.hi = v.v;
        r.hi_incl = true;
    }
    Some(r)
}

fn split_any<'a>(s: &'a str, seps: &'a [char]) -> impl Iterator<Item = &'a str> {
    s.split(seps).filter(|p| !p.is_empty())
}

fn trim(s: &str) -> &str { s.trim_matches(|c| c == ' ' || c == '\t') }

/// A range as EMF reads it (`^1.2`, `~1.2`, `>=1.0 <2`, `1.2.x`, `1.2 - 2`, `||`; empty = any). None for a malformed
/// range or one that matches no version.
fn parse_range(range: &str) -> Option<Vec<Interval>> {
    let range = match trim(range) { "" => "*", r => r };
    let mut out = Vec::new();
    for alt in range.split("||").map(trim) {
        let mut acc = Interval::default();
        if let Some(dash) = alt.find(" - ") {
            let a = parse_version(trim(&alt[..dash]), true)?;
            let b = parse_version(trim(&alt[dash + 3..]), true)?;
            acc.lo = a.v;
            if b.parts == 3 {
                acc.hi = b.v;
                acc.hi_incl = true;
            } else {
                acc.hi = bump(&b, b.parts);
            }
        } else {
            // ">= 1.0" with a space after the operator is accepted too
            let mut joined = String::new();
            for t in split_any(alt, &[',', ' ']) {
                if joined.ends_with(['=', '>', '<', '^', '~']) {
                    joined.push_str(t);
                } else {
                    if !joined.is_empty() { joined.push(' '); }
                    joined.push_str(t);
                }
            }
            for t in split_any(&joined, &[' ']) {
                acc.intersect(&comparator(t)?);
            }
        }
        if !acc.empty() { out.push(acc); }
    }
    (!out.is_empty()).then_some(out)
}

/// Does `version` (EMF's "1", "1.2" or "1.2.3") fall in EMF's `range`? None: either cannot be read as EMF reads it.
pub fn satisfies(range: &str, version: &str) -> Option<bool> {
    let r = parse_range(range)?;
    let v = parse_version(version, false)?;
    Some(r.iter().any(|i| i.contains(&v.v)))
}

fn vtext(v: &V3) -> String {
    if v[0] == i64::MAX { "any".into() } else { format!("{}.{}.{}", v[0], v[1], v[2]) }
}

/// EMF `version_problem`: None when `found` is in range, else EMF's words and whether it is too old.
/// "needs lua ^1.3 (1.3.0 or newer, below 2.0.0), found 1.2.0: too old (update lua)"
fn version_problem(id: &str, range: &str, found: &str) -> Option<(String, bool)> {
    let Some(r) = parse_range(range) else {
        return Some((format!("its range for {id} (\"{range}\") matches no version"), false));
    };
    let Some(v) = parse_version(found, false) else { return Some((format!("{id} has no readable version"), false)) };
    if r.iter().any(|i| i.contains(&v.v)) { return None; }
    let first = r[0];
    let c = v.v.cmp(&first.lo);
    let too_old = c.is_lt() || (c.is_eq() && !first.lo_incl);
    let mut what = format!("needs {id} {range} ({}{}", vtext(&first.lo), if first.lo_incl { " or newer" } else { " exclusive" });
    if first.hi[0] != i64::MAX {
        what += if first.hi_incl { ", up to " } else { ", below " };
        what += &vtext(&first.hi);
    }
    what += &format!("), found {}", vtext(&v.v));
    what += &if too_old { format!(": too old (update {id})") } else { ": too new (look for an update of this mod)".to_string() };
    Some((what, too_old))
}

// ------------------------------------------------------------------------------------------------ manifests ------

/// One entry of a mod.json's `depends` / `dependencies`, as EMF reads it (the id in lower case, the range as written,
/// empty for any version). `url`: an https link the entry gives for where to get the mod (EMF ignores it).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Dep {
    pub id: String,
    pub range: String,
    pub optional: bool,
    pub url: Option<String>,
}

/// An EMF mod as the dependency check sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EmfMod {
    /// The profile entry's id (the launcher's row); None for a mod in emf/mods, which is not a profile entry.
    pub entry: Option<String>,
    pub id: String,
    pub name: String,
    pub version: String,
    /// Switched on (a profile entry's switch; a drop-in counts as on).
    pub enabled: bool,
    /// mod.json `side` in lower case ("client", "server", "both" or empty).
    pub side: String,
    pub deps: Vec<Dep>,
    /// Why EMF refuses the mod whatever its dependencies (a broken mod.json): it does not load, and the mods that need
    /// it say so.
    pub broken: Option<String>,
}

/// EMF's mod ids: 1 to 48 of a-z 0-9 _ . - (`valid_mod_id`).
fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 48 && id.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, b'_' | b'.' | b'-'))
}

fn web_link(v: Option<&serde_json::Value>) -> Option<String> {
    let s = v?.as_str()?.trim();
    // the launcher opens https links only (app_open_url)
    (s.starts_with("https://") && s.len() <= 512 && !s.contains(char::is_whitespace)).then(|| s.to_string())
}

/// EMF `read_deps`: an object (id -> range or {version, optional}) or a list (ids, or objects with an `id`).
fn read_deps(j: &serde_json::Value) -> Result<Vec<Dep>, String> {
    let add = |out: &mut Vec<Dep>, id: &str, v: &serde_json::Value| -> Result<(), String> {
        let mut d = Dep { id: id.to_lowercase(), range: String::new(), optional: false, url: None };
        match v {
            serde_json::Value::String(s) => d.range = s.clone(),
            serde_json::Value::Object(o) => {
                if let Some(s) = o.get("version").and_then(|x| x.as_str()) { d.range = s.to_string(); }
                if let Some(b) = o.get("optional").and_then(|x| x.as_bool()) { d.optional = b; }
                d.url = web_link(o.get("url"));
            }
            serde_json::Value::Null => {}
            _ => return Err(format!("\"depends\": the entry for {id} is neither a range nor an object")),
        }
        if parse_range(&d.range).is_none() {
            return Err(format!("\"depends\": the range for {id} (\"{}\") matches no version", d.range));
        }
        out.push(d);
        Ok(())
    };
    let mut out = Vec::new();
    match j {
        // nlohmann's objects are ordered by key, as written (upper case before lower case): EMF checks them so
        serde_json::Value::Object(o) => {
            let mut keys: Vec<&String> = o.keys().collect();
            keys.sort();
            for k in keys { add(&mut out, k, &o[k.as_str()])?; }
        }
        serde_json::Value::Array(a) => for e in a {
            match e {
                serde_json::Value::String(s) => add(&mut out, s, &serde_json::Value::Null)?,
                serde_json::Value::Object(o) if o.get("id").is_some_and(|i| i.is_string()) => add(&mut out, o["id"].as_str().unwrap_or_default(), e)?,
                _ => return Err("\"depends\": a list entry has no \"id\"".into()),
            }
        },
        _ => return Err("\"depends\" is neither an object nor a list".into()),
    }
    Ok(out)
}

/// The mod in `folder` (its mod.json) as EMF reads it: a mod.json EMF refuses gives a mod with `broken` set (the id
/// from the manifest, else the folder's name). None: no mod.json.
pub fn read_folder(folder: &Path, entry: Option<&str>, enabled: bool) -> Option<EmfMod> {
    let file = folder.join("mod.json");
    if !file.is_file() { return None; }
    let fallback = || entry.map(str::to_lowercase).unwrap_or_else(|| folder.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default());
    let mut m = EmfMod { entry: entry.map(str::to_string), id: String::new(), name: String::new(), version: String::new(), enabled,
        side: String::new(), deps: Vec::new(), broken: None };
    let broken = |mut m: EmfMod, why: String| {
        if m.id.is_empty() { m.id = fallback(); }
        if m.name.is_empty() { m.name = m.id.clone(); }
        m.broken = Some(why);
        Some(m)
    };
    let Ok(text) = crate::manifest::read_text(&file) else { return broken(m, "mod.json cannot be read".into()) };
    let Ok(j) = crate::jsonc::parse(&text) else { return broken(m, "mod.json is not valid JSON".into()) };
    let Some(o) = j.as_object() else { return broken(m, "mod.json is not an object".into()) };
    let s = |k: &str| o.get(k).and_then(|v| v.as_str()).unwrap_or_default().to_string();
    m.id = s("id");
    if !valid_id(&m.id) {
        let why = if m.id.is_empty() { "mod.json has no \"id\"".to_string() } else { format!("\"id\" \"{}\" is not 1-48 of a-z 0-9 _ . -", m.id) };
        m.id = String::new();
        return broken(m, why);
    }
    m.version = s("version");
    m.name = match s("name") { n if n.trim().is_empty() => m.id.clone(), n => n };
    m.side = s("side").to_lowercase();
    if parse_version(&m.version, false).is_none() { return broken(m, "\"version\" must be \"1\", \"1.2\" or \"1.2.3\"".into()); }
    if o.get("emf").is_some_and(|e| !e.is_i64() && !e.is_u64()) {
        return broken(m, "\"emf\" must be a whole number (the lowest EMF API version the mod needs)".into());
    }
    if o.contains_key("depends") && o.contains_key("dependencies") {
        return broken(m, "mod.json has both \"depends\" and \"dependencies\"".into());
    }
    for k in ["depends", "dependencies"] {
        if let Some(d) = o.get(k) {
            match read_deps(d) {
                Ok(deps) => m.deps = deps,
                Err(why) => return broken(m, why),
            }
        }
    }
    Some(m)
}

/// The mods of `<game>/emf/mods` EMF would read beside the profile's (kiln-emf `scan_legacy`, in name order): folders
/// with a mod.json that are not Kiln's copy of an EMF mod (activation's marker: the profile entry is that mod), and
/// loose DLLs (id: the file name in lower case, version 0.0.0).
pub fn dropins(dir: &Path) -> Vec<EmfMod> {
    dropin_candidates(dir).into_iter().map(|(m, _)| m).collect()
}

fn dropin_candidates(dir: &Path) -> Vec<(EmfMod, PathBuf)> {
    let mut entries: Vec<PathBuf> = std::fs::read_dir(dir).into_iter().flatten().flatten().take(4096).map(|e| e.path()).collect();
    entries.sort();
    let mut out = Vec::new();
    for p in entries {
        if p.is_dir() {
            if crate::engine_mods::marker(&p).is_some_and(|k| k.kind == ModKind::Emf) { continue; }
            if let Some(m) = read_folder(&p, None, true) { out.push((m, p)); }
        } else if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("dll")) {
            let stem = p.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
            let id = stem.to_lowercase();
            let broken = (!valid_id(&id)).then(|| "the file name is not a valid mod id (1-48 of a-z 0-9 _ . -)".to_string());
            out.push((EmfMod { entry: None, id, name: stem, version: "0.0.0".into(), enabled: true, side: String::new(), deps: Vec::new(), broken }, p));
        }
    }
    out
}

// ------------------------------------------------------------------------------------------------ the check ------

/// EMF's own mods in its release zip (`EMF-<version>.zip`, `game/emf/mods/<id>/`) that other mods depend on: its
/// `lua` mod, which runs every Lua mod (crate::found's `EMF_LUA_HOST`), and the nine Lua mods of its download
/// (crate::library's `emf_lua_manifest`, the wiki's EMF mods page), with the names players know them by.
pub const EMF_RELEASE_MODS: [(&str, &str); 10] = [
    ("lua", "EMF's lua mod"), ("beastmaster", "Beastmaster"), ("bounty", "Bounty Board"), ("champion", "Wandering Champion"),
    ("grave", "Grave Keeper"), ("handover", "Hand-over"), ("kill_counter", "Kill Counter"), ("life_skills", "Life Skills"),
    ("minimap", "Minimap"), ("necromancer", "Necromancer"),
];

/// EMF's own page, where its release zip is: the link Kiln's docs give for EMF (CREDITS.md, docs/kiln/universal-loader.md,
/// the wiki's dinput8-chain page).
pub const EMF_PAGE: &str = "https://github.com/matenm/emf-enshrouded";

/// Why a dependency keeps a mod from loading.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum DepState {
    /// No mod of that id among the EMF mods.
    Missing,
    /// It is there, switched off.
    Off,
    /// It is there and will not load itself (its own dependency, a broken mod.json, a client mod on a server).
    NotLoading,
    /// Its version is below the range.
    TooOld,
    /// Its version is above the range.
    TooNew,
}

/// One EMF mod of the profile that will not load because of a dependency, with what the launcher says about it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmfDepProblem {
    /// The profile entry's id (the row).
    pub id: String,
    /// The dependency's id, as EMF reads it.
    pub dep: String,
    /// Its name: from its mod.json when it is there, else EMF's name for one of its own mods ([`EMF_RELEASE_MODS`]).
    pub dep_name: Option<String>,
    /// The range as the mod.json writes it ("" for any version).
    pub range: String,
    pub state: DepState,
    /// The version there (Off, NotLoading, TooOld, TooNew).
    pub found: Option<String>,
    /// One of EMF's own mods, which come with EMF's release zip ([`EMF_RELEASE_MODS`]).
    pub with_emf: bool,
    /// Where to get it: the dependency entry's own link, else [`EMF_PAGE`] for one of EMF's mods.
    pub url: Option<String>,
    /// kiln-emf's words for it at the start (kiln/state/emf.json `reason`).
    pub reason: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum St { Pending, Disabled, Rejected }

/// kiln-emf `resolve_order`'s decisions over `mods` (the profile's first, then the drop-ins; a later copy of an id is
/// ignored): the profile entries a dependency keeps from loading, the first such dependency each, as EMF reports it.
/// A required dependency that is missing, switched off or not loading, or any dependency (optional too) whose version
/// is out of range rejects the mod; that rejection reaches the mods that need it in turn.
pub fn check(mods: &[EmfMod], side: RunSide) -> Vec<EmfDepProblem> {
    let mut list: Vec<&EmfMod> = Vec::new();
    for m in mods {
        if !list.iter().any(|o| o.id == m.id) { list.push(m); }
    }
    let mut st: Vec<St> = list.iter().map(|m| {
        if m.broken.is_some() { St::Rejected }
        else if !m.enabled { St::Disabled }
        else if side == RunSide::Server && m.side == "client" { St::Rejected }
        else { St::Pending }
    }).collect();
    let mut why: Vec<Option<String>> = list.iter().map(|m| m.broken.clone()
        .or_else(|| (side == RunSide::Server && m.side == "client").then(|| "a client mod (side \"client\"): it does nothing on a dedicated server".into()))).collect();
    let mut found: Vec<Option<EmfDepProblem>> = vec![None; list.len()];
    let index = |id: &str| list.iter().position(|m| m.id == id);
    let mut changed = true;
    while changed {
        changed = false;
        for i in 0..list.len() {
            if st[i] != St::Pending { continue; }
            for d in &list[i].deps {
                let at = index(&d.id);
                let release = EMF_RELEASE_MODS.iter().find(|(id, _)| *id == d.id);
                let mut p = EmfDepProblem {
                    id: list[i].entry.clone().unwrap_or_else(|| list[i].id.clone()), dep: d.id.clone(),
                    dep_name: at.map(|j| list[j].name.clone()).or_else(|| release.map(|(_, n)| n.to_string())),
                    range: d.range.clone(), state: DepState::Missing, found: at.map(|j| list[j].version.clone()),
                    // EMF's own mods come from EMF's page whatever link a mod.json gives for them
                    with_emf: release.is_some(), url: if release.is_some() { Some(EMF_PAGE.to_string()) } else { d.url.clone() }, reason: String::new(),
                };
                match at {
                    None if d.optional => continue,
                    None => p.reason = format!("needs {}, which is not installed", d.id),
                    Some(j) if st[j] != St::Pending => {
                        if d.optional { continue; }
                        if st[j] == St::Disabled {
                            p.state = DepState::Off;
                            p.reason = format!("needs {}, which is switched off", d.id);
                        } else {
                            p.state = DepState::NotLoading;
                            p.reason = format!("needs {}, which is not loading ({})", d.id, why[j].clone().unwrap_or_default());
                        }
                    }
                    Some(j) => match version_problem(&d.id, &d.range, &list[j].version) {
                        None => continue,
                        Some((text, old)) => {
                            p.state = if old { DepState::TooOld } else { DepState::TooNew };
                            p.reason = text;
                        }
                    },
                }
                st[i] = St::Rejected;
                why[i] = Some(p.reason.clone());
                found[i] = Some(p);
                changed = true;
                break;
            }
        }
    }
    list.iter().zip(found).filter(|(m, _)| m.entry.is_some()).filter_map(|(_, p)| p).collect()
}

/// Is EMF's own loader installed for this game ([`crate::own_loaders::find`]: next to the game, in chain.ini or EMF's
/// dinput8-chain.ini)? Then kiln-emf steps aside at the start (native/plugins/emf/src/plugin.cpp `emf_in_process`) and
/// the real EMF loads whatever is in `<game>/emf/mods`, switched by its own `emf/config/<id>.ini`, which Kiln does not
/// read. A loader found on a path Wine may not load without a DLL override counts as well.
pub fn own_loader(game: &Path) -> bool {
    crate::own_loaders::find(game, None).iter().any(|l| l.kind == ModKind::Emf)
}

/// The runtime's private Lua host, not a Library/profile mod. Read the installed manifest rather than assuming a
/// launcher version installed it. Only the fixed DLL at this path can be the bundled host (kiln-emf's fallback rule).
fn bundled_lua(game: &Path) -> Option<EmfMod> {
    let folder = game.join("kiln/plugins/emf-lua");
    if !folder.join("lua.dll").is_file() { return None; }
    let m = read_folder(&folder, None, true)?;
    if m.id != "lua" || m.broken.is_some() { return None; }
    let text = crate::manifest::read_text(&folder.join("mod.json")).ok()?;
    let j = crate::jsonc::parse(&text).ok()?;
    let s = |k: &str| j.get(k).and_then(|v| v.as_str()).unwrap_or_default();
    if !s("main").is_empty() || !matches!(s("dll"), "" | "lua.dll")
        || j.get("settings").is_some_and(|v| !v.is_null() && !v.is_array())
    {
        return None;
    }
    Some(m)
}

/// kiln-emf's demand rule is the main file's suffix, even if that file is missing (the host then reports that error).
fn lua_script(folder: &Path) -> bool {
    let Ok(text) = crate::manifest::read_text(&folder.join("mod.json")) else { return false };
    let Ok(j) = crate::jsonc::parse(&text) else { return false };
    j.get("main").and_then(|v| v.as_str()).is_some_and(|s| s.len() > 4 && s.to_ascii_lowercase().ends_with(".lua"))
}

/// The dependency candidates in runtime order, including its on-demand private Lua host. This is a read-only preview:
/// it never adds a profile/Library entry or asks activation to place the host in emf/mods.
pub(crate) fn preview_mods(dirs: &DataDirs, profile: &Profile, game: Option<&Path>) -> Vec<EmfMod> {
    let mut mods = Vec::new();
    for m in &profile.mods.emf {
        let r = crate::library::LibraryRef { kind: ModKind::Emf, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
        let Ok(folder) = crate::library::path_of(dirs, &r) else { continue };
        // a DLL mod without a mod.json of its own (never left so by an import): EMF takes it as it is
        let entry = read_folder(&folder, Some(&m.id), m.enabled).unwrap_or_else(|| EmfMod {
            entry: Some(m.id.clone()), id: m.id.to_lowercase(), name: m.id.clone(), version: m.version.clone(), enabled: m.enabled,
            side: String::new(), deps: Vec::new(), broken: None,
        });
        mods.push((entry, folder));
    }
    if let Some(game) = game.filter(|_| profile.dropin_kinds().contains(&ModKind::Emf)) {
        mods.extend(dropin_candidates(&game.join(crate::paths::game::DROPIN_EMF)));
    }
    // A disabled or rejected explicit lua still wins, as does a profile row named lua with a mismatched manifest.
    let explicit = profile.mods.emf.iter().any(|m| m.id.eq_ignore_ascii_case("lua")) || mods.iter().any(|(m, _)| m.id == "lua");
    let wants_lua = !explicit && mods.iter().enumerate().any(|(i, (m, folder))| m.enabled && m.broken.is_none()
        && !mods[..i].iter().any(|(prior, _)| prior.id == m.id) && lua_script(folder));
    let mut out: Vec<EmfMod> = mods.into_iter().map(|(m, _)| m).collect();
    if let Some(host) = game.filter(|g| wants_lua && !own_loader(g)).and_then(bundled_lua) {
        out.push(host);
    }
    out
}

/// [`check`] over a profile's EMF entries, enabled drop-ins and the installed bundle when an enabled Lua script needs
/// it: the enabled entries a dependency keeps from loading. Nothing while EMF's own loader is installed in `game`
/// ([`own_loader`]): EMF decides then, by its own switches, so any word here could be wrong.
pub fn profile_problems(dirs: &DataDirs, profile: &Profile, side: RunSide, game: Option<&Path>) -> Vec<EmfDepProblem> {
    if profile.mods.emf.iter().all(|m| !m.enabled) || game.is_some_and(own_loader) {
        return Vec::new();
    }
    let mut out = check(&preview_mods(dirs, profile, game), side);
    out.retain(|p| profile.mods.emf.iter().any(|m| m.id == p.id && m.enabled));
    out
}

#[cfg(test)]
#[path = "emf_deps_tests.rs"]
mod tests;
