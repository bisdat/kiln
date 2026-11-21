//! The player layer of a Crucible instance: `local.<profile id>`, a data-only Kiln mod that loads after the pack layer
//! and holds the player's own data edits (docs/kiln/launcher/crucible.md "The player layer"). The working copy lives in
//! `profiles/<pid>/pack/local/` (kiln.mod.json + content/*.json); [`save`] puts it into the Library (an immutable entry,
//! as every mod a profile uses) and makes it the profile's last Kiln mod. Updates keep it: the pack layer is replaced,
//! this one stays and still wins; the fields both set are listed ([`overlaps`]) and the player may drop theirs.
//!
//! A data field is `[section, target, field]`: `["items", "Material_T1_Wood_Logs", "maxStackSize"]`; a target with
//! conditions carries them (`* @where {...}`); a new definition (`"add"` lists) is `[section, "add", name]`; a value
//! straight under a section (`options.stackGuard`) is `[section, key]`. File names do not matter: a field moved to
//! another file is the same field.

use super::pack::{INSTANCE_DIR, PLAYER_LAYER_PREFIX};
use crate::jsonc::{self, PathSeg};
use crate::library::LibraryRef;
use crate::manifest::ModKind;
use crate::paths::DataDirs;
use crate::profile::{Profile, ProfileMod};
use crate::task::NoProgress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const LOCAL_DIR: &str = "local";
pub const CONTENT_DIR: &str = "content";
/// A player layer file: 1 MiB at most (data files are small; the editors write them).
pub const MAX_LOCAL_FILE: usize = 1024 * 1024;

/// `local.<pid>`
pub fn local_id(pid: &str) -> String {
    format!("{PLAYER_LAYER_PREFIX}{pid}")
}

/// `profiles/<pid>/pack/local/`: the working copy of the player layer.
pub fn local_dir(dirs: &DataDirs, pid: &str) -> PathBuf {
    dirs.profile(pid).join(INSTANCE_DIR).join(LOCAL_DIR)
}

/// A file of the player layer: `content/<name>.json`, one level, no dot names.
pub fn local_path_ok(rel: &str) -> bool {
    let Some(name) = rel.strip_prefix("content/") else { return false };
    !name.is_empty()
        && !name.contains(['/', '\\', ':'])
        && !name.starts_with('.')
        && name.to_ascii_lowercase().ends_with(".json")
        && name.len() <= 128
        && !super::pack::windows_device(name)
}

fn manifest_text(pid: &str, load_after: &[String]) -> Result<String> {
    let m = serde_json::json!({
        "id": local_id(pid),
        "name": "Your changes",
        "version": "1.0.0",
        "apiVersion": "0.2",
        "side": "both",
        "clients": "none",
        "description": "Your own data changes to this pack: they load after the pack and win over it.",
        "loadAfter": load_after,
    });
    let mut s = serde_json::to_string_pretty(&m)?;
    s.push('\n');
    Ok(s)
}

/// The working copy, made when missing (kiln.mod.json + content/).
pub fn ensure(dirs: &DataDirs, pid: &str) -> Result<PathBuf> {
    crate::profile::get(dirs, pid)?;
    let d = local_dir(dirs, pid);
    std::fs::create_dir_all(d.join(CONTENT_DIR)).map_err(crate::util::io_at(&d, "creating"))?;
    let m = d.join(crate::hash::MOD_MANIFEST);
    if !m.is_file() {
        crate::util::write_atomic(&m, manifest_text(pid, &[])?.as_bytes())?;
    }
    Ok(d)
}

/// One file of the player layer.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LocalFile {
    /// `content/<name>.json`
    pub path: String,
    pub text: String,
}

/// The player layer's data files (none when it was never made).
pub fn files(dirs: &DataDirs, pid: &str) -> Result<Vec<LocalFile>> {
    crate::profile::get(dirs, pid)?;
    content_files(&local_dir(dirs, pid))
}

/// `content/*.json` of a mod folder, by name.
pub fn content_files(mod_dir: &Path) -> Result<Vec<LocalFile>> {
    let mut out = Vec::new();
    let d = mod_dir.join(CONTENT_DIR);
    let Ok(rd) = std::fs::read_dir(&d) else { return Ok(out) };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let rel = format!("{CONTENT_DIR}/{name}");
        if !local_path_ok(&rel) || !e.file_type().is_ok_and(|t| t.is_file()) {
            continue;
        }
        let bytes = std::fs::read(e.path()).map_err(crate::util::io_at(&e.path(), "reading"))?;
        out.push(LocalFile { path: rel, text: crate::manifest::decode_text(&bytes) });
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

/// Writes one file of the working copy (`text` None: removes it). The text must be a data file (a JSON object).
/// [`save`] makes the change count.
pub fn write(dirs: &DataDirs, pid: &str, rel: &str, text: Option<&str>) -> Result<()> {
    if !local_path_ok(rel) {
        return Err(Error::invalid(format!("{rel}: the player layer holds content/<name>.json files only")));
    }
    let d = ensure(dirs, pid)?;
    let p = d.join(rel);
    match text {
        Some(t) => {
            if t.len() > MAX_LOCAL_FILE {
                return Err(Error::invalid(format!("{rel}: larger than 1 MiB")));
            }
            let v = jsonc::parse(t).map_err(|e| Error::new(ErrorCode::Parse, format!("{rel}: {}", e.message)))?;
            if !v.is_object() {
                return Err(Error::new(ErrorCode::Schema, format!("{rel}: a data file is one {{ ... }} object")));
            }
            crate::util::write_atomic(&p, t.as_bytes())
        }
        None => match std::fs::remove_file(&p) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(crate::util::io_at(&p, "removing")(e)),
        },
    }
}

/// The Kiln mods the player layer loads after: every other Kiln mod of the profile.
fn after_ids(p: &Profile) -> Vec<String> {
    let me = local_id(&p.id);
    p.mods.kiln.iter().filter(|m| m.id != me).map(|m| m.id.clone()).collect()
}

/// Puts the working copy into the Library and makes it the profile's last Kiln mod (on). A working copy without data
/// files takes the player layer out of the profile. Returns the profile as saved.
pub fn save(dirs: &DataDirs, pid: &str) -> Result<Profile> {
    let mut p = crate::profile::get(dirs, pid)?;
    place(dirs, &mut p)?;
    crate::profile::save(dirs, &p)
}

/// [`save`] on a profile in hand (not saved): its manifest names `p`'s Kiln mods, the entry goes last.
pub(crate) fn place(dirs: &DataDirs, p: &mut Profile) -> Result<Option<LibraryRef>> {
    let me = local_id(&p.id);
    let d = local_dir(dirs, &p.id);
    let had = p.mods.kiln.iter().find(|m| m.id == me).cloned();
    if let Some(e) = &had
        && !d.join(crate::hash::MOD_MANIFEST).is_file()
    {
        // the profile names a player layer but its working copy is gone (a copied or restored profile, a folder
        // removed by hand; files the player removed leave the folder and its manifest): the Library entry is the
        // player's data, so the working copy comes back from it
        let r = LibraryRef { kind: ModKind::Kiln, id: e.id.clone(), version: e.version.clone(), hash: e.hash.clone() };
        if let Ok(at) = crate::library::path_of(dirs, &r)
            && at.is_dir()
        {
            let tmp = crate::util::with_suffix(&d, ".tmp");
            let _ = std::fs::remove_dir_all(&tmp);
            crate::util::copy_tree(&at, &tmp)?;
            super::pack::swap_in(&tmp, &d)?;
        }
    }
    p.mods.kiln.retain(|m| m.id != me);
    if content_files(&d)?.is_empty() {
        return Ok(None);
    }
    crate::util::write_atomic(&d.join(crate::hash::MOD_MANIFEST), manifest_text(&p.id, &after_ids(p))?.as_bytes())?;
    let report = crate::library::import(dirs, std::slice::from_ref(&d), &mut NoProgress)?;
    let r = report.imported.iter().chain(&report.existing).find(|r| r.kind == ModKind::Kiln && r.id == me).cloned()
        .ok_or_else(|| Error::new(ErrorCode::Internal, format!("the library did not take the player layer {me}"))
            .with_detail(serde_json::to_value(&report.skipped).unwrap_or_default()))?;
    p.mods.kiln.push(ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: had.map(|e| e.enabled).unwrap_or(true) });
    Ok(Some(r))
}

// ================================================================================================= data fields

/// A field of a data file and where it is.
#[derive(Debug, Clone, PartialEq)]
pub struct FieldAt {
    pub file: String,
    /// The JSON path in the file (section, target as written, field) or (section, key); an "add" entry: (section, "add").
    pub path: Vec<String>,
    pub value: Value,
}

fn target_of(sel: &str, body: &Value) -> String {
    match body.get("@where") {
        Some(w) => format!("{sel} @where {w}"),
        None => sel.to_string(),
    }
}

/// Every field the data files set, by `[section, target, field]` (a later file wins, as data packs apply).
pub fn fields(files: &[LocalFile]) -> BTreeMap<Vec<String>, FieldAt> {
    let mut out = BTreeMap::new();
    for f in files {
        let Ok(v) = jsonc::parse(&f.text) else { continue };
        let Some(sections) = v.as_object() else { continue };
        for (section, sv) in sections {
            if section == "format" {
                continue;
            }
            let Some(entries) = sv.as_object() else { continue };
            for (sel, body) in entries {
                if sel == "add" {
                    for d in body.as_array().into_iter().flatten() {
                        if let Some(name) = d.get("name").and_then(Value::as_str) {
                            out.insert(vec![section.clone(), "add".into(), name.to_string()],
                                FieldAt { file: f.path.clone(), path: vec![section.clone(), "add".into()], value: d.clone() });
                        }
                    }
                    continue;
                }
                match body.as_object() {
                    Some(fs) => {
                        let target = target_of(sel, body);
                        for (field, fv) in fs {
                            if field.starts_with('@') {
                                continue;
                            }
                            out.insert(vec![section.clone(), target.clone(), field.clone()],
                                FieldAt { file: f.path.clone(), path: vec![section.clone(), sel.clone(), field.clone()], value: fv.clone() });
                        }
                    }
                    None => {
                        out.insert(vec![section.clone(), sel.clone()], FieldAt { file: f.path.clone(), path: vec![section.clone(), sel.clone()], value: body.clone() });
                    }
                }
            }
        }
    }
    out
}

/// A field both the pack layer and the player layer set: the player's value still wins (it loads later).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DataOverlap {
    /// The field's id for [`drop`] and the update's `drop` list (a JSON array as text).
    pub key: String,
    pub section: String,
    /// The item, recipe, table... (with its `@where` when it has one); `add` for a new definition.
    pub target: String,
    /// The field; for a new definition its name; empty for a value straight under the section.
    pub field: String,
    /// The pack layer's value (the new version's in an update plan).
    pub pack: Value,
    /// The installed version's value (None: it did not set the field).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base: Option<Value>,
    pub mine: Value,
    /// The player layer's file holding it.
    pub file: String,
    /// The pack changed the field since the version installed (or sets it for the first time).
    pub changed: bool,
}

pub fn field_key(k: &[String]) -> String {
    serde_json::to_string(k).unwrap_or_default()
}

/// The fields the pack layer (`theirs`; `base` = the installed version's) and the player layer both set.
pub fn overlaps(base: &[LocalFile], theirs: &[LocalFile], mine: &[LocalFile]) -> Vec<DataOverlap> {
    let (b, t, m) = (fields(base), fields(theirs), fields(mine));
    let mut out = Vec::new();
    for (k, mv) in &m {
        let Some(tv) = t.get(k) else { continue };
        let bv = b.get(k).map(|x| x.value.clone());
        out.push(DataOverlap {
            key: field_key(k),
            section: k[0].clone(),
            target: k.get(1).cloned().unwrap_or_default(),
            field: k.get(2).cloned().unwrap_or_default(),
            pack: tv.value.clone(),
            changed: bv.as_ref() != Some(&tv.value),
            base: bv,
            mine: mv.value.clone(),
            file: mv.file.clone(),
        });
    }
    out
}

/// Takes fields out of the working copy (`keys` as [`DataOverlap::key`]): each from its file, an emptied target and
/// section with it. Returns how many went. [`save`] makes it count.
pub fn drop(dirs: &DataDirs, pid: &str, keys: &[String]) -> Result<usize> {
    if keys.is_empty() {
        return Ok(0);
    }
    let d = local_dir(dirs, pid);
    let mut list = content_files(&d)?;
    let before = list.clone();
    let mut gone = 0usize;
    for key in keys {
        let k: Vec<String> = serde_json::from_str(key).map_err(|_| Error::invalid(format!("not a data field: {key}")))?;
        // the last file setting it is the one that counts; take it out of every file so none shows through
        let mut hit = false;
        for f in list.iter_mut() {
            let here = fields(std::slice::from_ref(f));
            let Some(at) = here.get(&k) else { continue };
            f.text = remove_field(&f.text, &k, at)?;
            hit = true;
        }
        if hit {
            gone += 1;
        }
    }
    // all or nothing: the changed files go into a copy that replaces the working copy in one move
    if list != before {
        let tmp = crate::util::with_suffix(&d, ".tmp");
        let _ = std::fs::remove_dir_all(&tmp);
        crate::util::copy_tree(&d, &tmp)?;
        for f in &list {
            crate::util::write_atomic(&tmp.join(&f.path), f.text.as_bytes())?;
        }
        super::pack::swap_in(&tmp, &d)?;
    }
    Ok(gone)
}

fn remove_field(text: &str, k: &[String], at: &FieldAt) -> Result<String> {
    let seg = |p: &[String]| -> Vec<PathSeg> { p.iter().map(|s| PathSeg::Key(s.clone())).collect() };
    let v = jsonc::parse(text)?;
    if at.path.len() == 2 && at.path[1] == "add" {
        // an "add" entry: the list without it
        let list: Vec<Value> = v.pointer(&format!("/{}/add", esc(&at.path[0]))).and_then(Value::as_array).cloned().unwrap_or_default()
            .into_iter().filter(|d| d.get("name").and_then(Value::as_str) != k.get(2).map(String::as_str)).collect();
        let t = if list.is_empty() { jsonc::remove_value(text, &seg(&at.path))? } else { jsonc::set_value(text, &seg(&at.path), &Value::Array(list))? };
        return prune(&t, &at.path[..1]);
    }
    let t = jsonc::remove_value(text, &seg(&at.path))?;
    let t = if at.path.len() == 3 { prune(&t, &at.path[..2])? } else { t };
    prune(&t, &at.path[..1])
}

fn esc(s: &str) -> String {
    s.replace('~', "~0").replace('/', "~1")
}

/// Removes the object at `path` when nothing is left in it but conditions (`@where`, `@refs`).
fn prune(text: &str, path: &[String]) -> Result<String> {
    let v = jsonc::parse(text)?;
    let ptr: String = path.iter().map(|p| format!("/{}", esc(p))).collect();
    let empty = v.pointer(&ptr).and_then(Value::as_object).is_some_and(|o| o.keys().all(|k| k.starts_with('@')));
    if empty {
        jsonc::remove_value(text, &path.iter().map(|s| PathSeg::Key(s.clone())).collect::<Vec<_>>())
    } else {
        Ok(text.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn f(path: &str, text: &str) -> LocalFile {
        LocalFile { path: path.into(), text: text.into() }
    }

    #[test]
    fn fields_by_target_whatever_the_file() {
        let a = f("content/a.json", r#"{ "format": 1, "items": { "Logs": { "maxStackSize": 500, "rarity": "Epic" },
            "*": { "@where": { "category": "Materials" }, "maxStackSize": { "mul": 2 } }, "add": [ { "name": "Lantern", "base": "Resin" } ] },
            "options": { "stackGuard": true } }"#);
        let fs = fields(std::slice::from_ref(&a));
        let keys: Vec<String> = fs.keys().map(|k| k.join("|")).collect();
        assert_eq!(keys, [
            r#"items|* @where {"category":"Materials"}|maxStackSize"#,
            "items|Logs|maxStackSize",
            "items|Logs|rarity",
            "items|add|Lantern",
            "options|stackGuard",
        ]);
        // the same field in a file of another name is the same field
        let b = f("content/renamed.json", r#"{ "items": { "Logs": { "maxStackSize": 800 } } }"#);
        let mine = f("content/mine.json", r#"{ "items": { "Logs": { "maxStackSize": 999, "rarity": "Rare" }, "Other": { "x": 1 } } }"#);
        let o = overlaps(std::slice::from_ref(&a), std::slice::from_ref(&b), std::slice::from_ref(&mine));
        assert_eq!(o.len(), 1);
        assert_eq!((o[0].target.as_str(), o[0].field.as_str(), o[0].pack.clone(), o[0].base.clone(), o[0].mine.clone(), o[0].changed),
            ("Logs", "maxStackSize", json!(800), Some(json!(500)), json!(999), true));
        // unchanged by the pack: still listed, not "changed"
        let o = overlaps(std::slice::from_ref(&b), std::slice::from_ref(&b), std::slice::from_ref(&mine));
        assert!(!o[0].changed);
    }

    #[test]
    fn removing_a_field_prunes_what_it_leaves_empty() {
        let text = r#"{
  // mine
  "format": 1,
  "items": { "Logs": { "maxStackSize": 999 }, "Other": { "x": 1 } },
  "recipes": { "*": { "@where": { "output": "Charcoal" }, "craftingDuration": 30 } },
  "options": { "stackGuard": true }
}"#;
        let lf = f("content/m.json", text);
        let fs = fields(std::slice::from_ref(&lf));
        let k = |s: &[&str]| -> Vec<String> { s.iter().map(|x| x.to_string()).collect() };
        let t = remove_field(text, &k(&["items", "Logs", "maxStackSize"]), &fs[&k(&["items", "Logs", "maxStackSize"])]).unwrap();
        assert!(t.contains("// mine"), "{t}");
        assert_eq!(jsonc::parse(&t).unwrap()["items"], json!({ "Other": { "x": 1 } }));
        let rk = k(&["recipes", r#"* @where {"output":"Charcoal"}"#, "craftingDuration"]);
        let t = remove_field(&t, &rk, &fs[&rk]).unwrap();
        assert!(jsonc::parse(&t).unwrap().get("recipes").is_none(), "{t}");
        let ok = k(&["options", "stackGuard"]);
        let t = remove_field(&t, &ok, &fs[&ok]).unwrap();
        assert!(jsonc::parse(&t).unwrap().get("options").is_none(), "{t}");
    }

    #[test]
    fn layer_paths() {
        assert!(local_path_ok("content/mine.json"));
        for bad in ["content/../x.json", "content/a/b.json", "content/.x.json", "kiln.mod.json", "content/x.txt", "content/nul.json", "content/"] {
            assert!(!local_path_ok(bad), "{bad}");
        }
        assert_eq!(local_id("ember-run"), "local.ember-run");
    }
}
