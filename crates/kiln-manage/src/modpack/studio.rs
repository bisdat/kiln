//! Pack Studio, the creator's side of Crucible (DESIGN.md section 3; docs/kiln/launcher/pack-studio.md): a **pack
//! source** folder from which every version of a pack is built.
//!
//! ~~~text
//! <folder>/                    the launcher keeps one per pack in <data>/studio/<id>/; kilnctl pack works on any folder
//!   kiln-pack.json             the source: name, version, the profile it is made from, each mod's switch and sides,
//!                              the chosen settings keys, the server's game settings, visibility, what was published
//!   credits.json               attribution per mod id ({author, source, match})
//!   layer/                     the pack layer: a data-only Kiln mod (kiln.mod.json, content/*.json)
//!   versions/                  every version built for publishing (<id>-<version>.kilnpack)
//! ~~~
//!
//! The mods, their order and switches and the config folder are the profile's (the creator plays it, changes settings in
//! it and captures them); the build is [pack::build] fed from the source. The test instance is the built pack
//! installed as an instance of its own ([pack::install]), so the creator tries exactly what players get.

use super::merge::{self, OverrideMode};
use super::pack::{self, BuildRequest, BuiltPack, Credit, Dep, Env, Need, OverrideSpec, PartSide};
use crate::manifest::{ModKind, Side};
use crate::paths::DataDirs;
use crate::task::Progress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Where the launcher keeps its pack sources: <data>/studio/<id>/.
pub const DIR: &str = "studio";
pub const SOURCE_FILE: &str = "kiln-pack.json";
pub const CREDITS_FILE: &str = "credits.json";
pub const LAYER_DIR: &str = "layer";
pub const VERSIONS_DIR: &str = "versions";
pub const SOURCE_KIND: &str = "pack-source";
pub const SOURCE_FORMAT: u32 = 1;
/// The layer file "Set in the pack" writes to (the conflict view).
pub const STUDIO_CONTENT: &str = "content/pack.json";

fn err(code: ErrorCode, msg: impl Into<String>) -> Error {
    Error::new(code, msg)
}

/// Link-only (anyone with the link or code) or public (listed on Crucible; needs credits for every mod).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Visibility {
    #[default]
    Link,
    Public,
}

/// The creator's choice for one mod of the profile (absent: required on both sides, or optional-off when the mod is
/// switched off in the profile).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModChoice {
    #[serde(default)]
    pub dep: Dep,
    /// An optional mod's switch until the player sets it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub default: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub env: Option<Env>,
}

/// One version published to Crucible.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublishedVersion {
    pub version: String,
    /// Crucible's version id (pv_...).
    pub id: String,
    pub published: String,
    pub visibility: Visibility,
    /// The .kilnpack kept in versions/ (a server pack is built from it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub file: Option<String>,
    /// Crucible has the version but the publish did not finish (an upload, the credits or the visibility failed): the
    /// next publish of the same version resumes it instead of making another.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub pending: bool,
}

/// Where the pack is on Crucible.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Published {
    /// Crucible's pack id (pck_...).
    pub pack: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub slug: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(default)]
    pub visibility: Visibility,
    #[serde(default)]
    pub versions: Vec<PublishedVersion>,
}

/// kiln-pack.json.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackSource {
    pub format: u32,
    /// "pack-source"
    pub kind: String,
    /// The pack's id (a mod id; its layer is pack.<id>). Fixed once made.
    pub id: String,
    pub name: String,
    /// The next version to build (SemVer).
    pub version: String,
    /// The launcher profile the pack is made from.
    pub profile: String,
    /// One line for Crucible's lists (public packs: at least 10 characters).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub summary: Option<String>,
    /// The pack page's text.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub authors: Vec<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub tags: Vec<String>,
    /// What changed in the next version.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub changes: Option<String>,
    /// By mod id.
    #[serde(default)]
    pub mods: BTreeMap<String, ModChoice>,
    /// The settings keys the pack carries (captured from the profile's config folder, never written by hand).
    #[serde(default)]
    pub overrides: Vec<OverrideSpec>,
    /// {"gameSettings": {...}} for servers (no client world settings).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub server_settings: Option<Value>,
    #[serde(default)]
    pub visibility: Visibility,
    /// Steam builds the creator tested with.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub game_builds: Vec<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub published: Option<Published>,
    /// The profile of the last "Test this pack" instance.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub test_profile: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub test_started: Option<String>,
    pub created: String,
    pub updated: String,
}

/// One mod of the pack as the Studio shows it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StudioMod {
    pub kind: ModKind,
    pub id: String,
    pub name: String,
    pub version: String,
    pub hash: String,
    /// Switched on in the profile.
    pub enabled: bool,
    /// The Library entry's side, when it says.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub side: Option<Side>,
    pub dep: Dep,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub default: Option<bool>,
    /// What each side needs (as the build writes it).
    pub env: Env,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub credit: Option<Credit>,
}

/// A pack source with what it builds from.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Studio {
    pub dir: PathBuf,
    pub source: PackSource,
    pub mods: Vec<StudioMod>,
    pub credits: BTreeMap<String, Credit>,
    /// The profile's name (None: the profile is gone).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile_name: Option<String>,
    /// Files in layer/content.
    pub layer_files: usize,
    /// Changes with anything a build reads (the source, the credits, the layer files, the profile): the UI's analysis
    /// is made again when it differs.
    pub revision: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StudioSummary {
    pub id: String,
    pub name: String,
    pub version: String,
    pub profile: String,
    pub mods: usize,
    pub updated: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub published: Option<Published>,
}

/// <data>/studio
pub fn root(dirs: &DataDirs) -> PathBuf {
    dirs.root.join(DIR)
}

/// <data>/studio/<id> (the id checked).
pub fn dir_of(dirs: &DataDirs, id: &str) -> Result<PathBuf> {
    if !valid_id(id) {
        return Err(Error::invalid(format!("not a pack id: {id}")));
    }
    Ok(root(dirs).join(id))
}

/// A pack id: what a mod id may be, at most 59 characters (pack.<id> is the layer's).
pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 59 && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_' || b == b'.')
        && id.as_bytes()[0].is_ascii_alphanumeric()
}

fn id_from_name(name: &str) -> String {
    let mut s = crate::profile::id_from_name(name).to_ascii_lowercase();
    s.retain(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_' || c == '.');
    let s = s.trim_matches(|c: char| !c.is_ascii_alphanumeric()).to_string();
    let s: String = s.chars().take(59).collect();
    if s.is_empty() { "pack".into() } else { s }
}

pub fn read_source(dir: &Path) -> Result<PackSource> {
    let p = dir.join(SOURCE_FILE);
    let s: PackSource = crate::util::read_json(&p)?.ok_or_else(|| Error::not_found(format!("no {SOURCE_FILE} in {}", dir.display())))?;
    if s.kind != SOURCE_KIND {
        return Err(err(ErrorCode::Schema, format!("{}: not a pack source (kind {})", p.display(), s.kind)));
    }
    if s.format > SOURCE_FORMAT {
        return Err(err(ErrorCode::Unsupported, format!("{}: made by a newer Kiln (format {}): update Kiln", p.display(), s.format)));
    }
    Ok(s)
}

pub fn write_source(dir: &Path, s: &PackSource) -> Result<()> {
    std::fs::create_dir_all(dir).map_err(crate::util::io_at(dir, "creating"))?;
    crate::util::write_json_atomic(&dir.join(SOURCE_FILE), s)
}

pub fn read_credits(dir: &Path) -> Result<BTreeMap<String, Credit>> {
    Ok(crate::util::read_json(&dir.join(CREDITS_FILE))?.unwrap_or_default())
}

fn check_source(s: &PackSource) -> Result<()> {
    if !valid_id(&s.id) {
        return Err(Error::invalid(format!("not a pack id: {}", s.id)));
    }
    let name = s.name.trim();
    if name.is_empty() || name.chars().count() > 128 {
        return Err(Error::invalid("the pack needs a name (at most 128 characters)"));
    }
    crate::semver::SemVersion::parse(&s.version).map_err(|_| Error::invalid(format!("{} is not a version like 1.2.0", s.version)))?;
    for o in &s.overrides {
        if !pack::override_path_ok(&o.path) {
            return Err(Error::invalid(format!("{}: only settings files below the config folder can travel in a pack", o.path)));
        }
    }
    if let Some(v) = &s.server_settings {
        let ok = v.as_object().is_some_and(|m| m.keys().all(|k| k == "gameSettings") && m.get("gameSettings").is_none_or(Value::is_object));
        if !ok {
            return Err(Error::invalid("server settings hold gameSettings only ({\"gameSettings\": {...}})"));
        }
    }
    Ok(())
}

/// The layer's kiln.mod.json, written when the source is made (lint sees a mod; the build sets version and loadAfter).
fn write_layer_manifest(dir: &Path, s: &PackSource) -> Result<()> {
    let layer = dir.join(LAYER_DIR);
    std::fs::create_dir_all(layer.join("content")).map_err(crate::util::io_at(&layer, "creating"))?;
    let m = layer.join(crate::hash::MOD_MANIFEST);
    if m.exists() {
        return Ok(());
    }
    let v = serde_json::json!({
        "id": format!("{}{}", pack::LAYER_PREFIX, s.id),
        "name": format!("{} (pack layer)", s.name.trim()),
        "version": s.version,
        "apiVersion": "0.2",
        "side": "both",
        "clients": "mod",
        "description": "The pack's own data: loads after the pack's mods and combines them.",
        "loadAfter": [],
    });
    let mut text = serde_json::to_string_pretty(&v)?;
    text.push('\n');
    crate::util::write_atomic(&m, text.as_bytes())
}

/// What a new pack starts from.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NewPack {
    pub name: String,
    /// A profile to make the pack from; None: a new empty profile named after the pack.
    #[serde(default)]
    pub from: Option<String>,
    /// The pack's id (default: from the name, made unique).
    #[serde(default)]
    pub id: Option<String>,
}

/// A new pack source in folder dir (which must not hold one), from a profile or with a new empty one.
pub fn init(dirs: &DataDirs, dir: &Path, req: &NewPack) -> Result<Studio> {
    if dir.join(SOURCE_FILE).exists() {
        return Err(err(ErrorCode::Conflict, format!("{} holds a pack already", dir.display())));
    }
    let name = req.name.trim().to_string();
    if name.is_empty() {
        return Err(Error::invalid("the pack needs a name"));
    }
    let id = match &req.id {
        Some(id) => id.clone(),
        None => id_from_name(&name),
    };
    if !valid_id(&id) {
        return Err(Error::invalid(format!("not a pack id: {id}")));
    }
    if name.chars().count() > 128 {
        return Err(Error::invalid("the pack needs a name (at most 128 characters)"));
    }
    let (profile, made) = match &req.from {
        Some(pid) => (crate::profile::get(dirs, pid)?, false),
        None => (crate::profile::create(dirs, &name, None)?, true),
    };
    let r = init_with(dirs, dir, id, name, &profile);
    if r.is_err() && made {
        let _ = crate::profile::delete(dirs, &profile.id);
    }
    r
}

fn init_with(dirs: &DataDirs, dir: &Path, id: String, name: String, profile: &crate::profile::Profile) -> Result<Studio> {
    let now = crate::util::now_rfc3339();
    let s = PackSource {
        format: SOURCE_FORMAT,
        kind: SOURCE_KIND.into(),
        id,
        name,
        version: "1.0.0".into(),
        profile: profile.id.clone(),
        summary: None,
        description: None,
        authors: Vec::new(),
        tags: Vec::new(),
        changes: None,
        mods: BTreeMap::new(),
        overrides: Vec::new(),
        server_settings: None,
        visibility: Visibility::Link,
        game_builds: Vec::new(),
        published: None,
        test_profile: None,
        test_started: None,
        created: now.clone(),
        updated: now,
    };
    check_source(&s)?;
    write_source(dir, &s)?;
    write_layer_manifest(dir, &s)?;
    open(dirs, dir)
}

/// A new pack in the launcher's studio folder (<data>/studio/<id>, the id made unique).
pub fn create(dirs: &DataDirs, req: &NewPack) -> Result<Studio> {
    let base = req.id.clone().unwrap_or_else(|| id_from_name(&req.name));
    if !valid_id(&base) {
        return Err(Error::invalid(format!("not a pack id: {base}")));
    }
    let mut id = base.clone();
    let mut n = 2;
    while root(dirs).join(&id).exists() {
        let suffix = format!("-{n}");
        id = format!("{}{suffix}", &base[..base.len().min(59 - suffix.len())]);
        n += 1;
    }
    init(dirs, &root(dirs).join(&id), &NewPack { id: Some(id), ..req.clone() })
}

/// The source with its profile's mods, the credits and the layer.
pub fn open(dirs: &DataDirs, dir: &Path) -> Result<Studio> {
    let source = read_source(dir)?;
    let credits = read_credits(dir)?;
    let (mods, profile_name, profile_stamp) = match crate::profile::get(dirs, &source.profile) {
        Ok(p) => (mods_of(dirs, &p, &source, &credits)?, Some(p.name.clone()), format!("{}:{}", p.generation, p.updated)),
        Err(e) if e.code == ErrorCode::NotFound => (Vec::new(), None, String::new()),
        Err(e) => return Err(e),
    };
    let files = layer_files(dir)?;
    let mut rev = format!("{}|{}|{}|{}", source.updated, serde_json::to_string(&credits)?, profile_stamp, serde_json::to_string(&source.overrides)?);
    for f in &files {
        let t = std::fs::metadata(dir.join(LAYER_DIR).join(&f.path)).and_then(|m| m.modified()).map(crate::util::unix).unwrap_or(0);
        rev.push_str(&format!("|{}:{}:{t}", f.path, f.size));
    }
    let revision = crate::hash::sha256_hex(rev.as_bytes())[..16].to_string();
    Ok(Studio { dir: dir.to_path_buf(), source, mods, credits, profile_name, layer_files: files.len(), revision })
}

fn mods_of(dirs: &DataDirs, p: &crate::profile::Profile, s: &PackSource, credits: &BTreeMap<String, Credit>) -> Result<Vec<StudioMod>> {
    let libs: BTreeMap<(ModKind, String), crate::library::LibraryEntry> =
        crate::library::list(dirs, None)?.into_iter().map(|e| ((e.kind, e.id.clone()), e)).collect();
    let mut out = Vec::new();
    for kind in ModKind::ALL {
        for m in p.mods.list(kind) {
            let lib = libs.get(&(kind, m.id.clone()));
            let side = lib.and_then(|e| e.versions.iter().find(|v| v.hash == m.hash)).and_then(|v| v.side);
            let choice = s.mods.get(&m.id);
            let (dep, default) = match choice {
                Some(c) if c.dep == Dep::Optional => (Dep::Optional, Some(c.default.unwrap_or(true))),
                Some(_) => (Dep::Required, None),
                None if !m.enabled => (Dep::Optional, Some(false)),
                None => (Dep::Required, None),
            };
            let env = choice.and_then(|c| c.env).unwrap_or(if side == Some(Side::Client) {
                Env { client: Need::Required, server: Need::Unsupported }
            } else if side == Some(Side::Server) {
                Env { client: Need::Unsupported, server: Need::Required }
            } else {
                Env::default()
            });
            out.push(StudioMod {
                kind,
                id: m.id.clone(),
                name: lib.map(|e| e.name.clone()).unwrap_or_else(|| m.id.clone()),
                version: m.version.clone(),
                hash: m.hash.clone(),
                enabled: m.enabled,
                side,
                dep,
                default,
                env,
                credit: credits.get(&m.id).cloned(),
            });
        }
    }
    Ok(out)
}

/// The launcher's pack sources, newest change first.
pub fn list(dirs: &DataDirs) -> Result<Vec<StudioSummary>> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(root(dirs)) else { return Ok(out) };
    for e in rd.flatten() {
        let Some(id) = e.file_name().to_str().map(str::to_string) else { continue };
        if !valid_id(&id) {
            continue;
        }
        let Ok(s) = read_source(&e.path()) else { continue };
        let mods = crate::profile::get(dirs, &s.profile).map(|p| ModKind::ALL.iter().map(|k| p.mods.list(*k).len()).sum()).unwrap_or(0);
        out.push(StudioSummary { id, name: s.name, version: s.version, profile: s.profile, mods, updated: s.updated, published: s.published });
    }
    out.sort_by(|a, b| b.updated.cmp(&a.updated));
    Ok(out)
}

/// Saves what the creator changed. The id, the profile, when it was made, what was published and the test instance
/// stay as stored; mod choices of mods the profile no longer has are dropped.
pub fn save(dirs: &DataDirs, dir: &Path, mut next: PackSource) -> Result<Studio> {
    let cur = read_source(dir)?;
    next.format = SOURCE_FORMAT;
    next.kind = SOURCE_KIND.into();
    next.id = cur.id.clone();
    next.profile = cur.profile.clone();
    next.created = cur.created.clone();
    next.published = cur.published.clone();
    next.test_profile = cur.test_profile.clone();
    next.test_started = cur.test_started.clone();
    next.name = next.name.trim().to_string();
    next.version = next.version.trim().to_string();
    for t in [&mut next.summary, &mut next.description, &mut next.changes] {
        if t.as_deref().is_some_and(|s| s.trim().is_empty()) {
            *t = None;
        }
    }
    if let Ok(p) = crate::profile::get(dirs, &cur.profile) {
        let ids: std::collections::BTreeSet<String> = ModKind::ALL.iter().flat_map(|k| p.mods.list(*k).iter().map(|m| m.id.clone())).collect();
        next.mods.retain(|id, _| ids.contains(id));
    }
    next.overrides.retain(|o| !o.path.is_empty());
    check_source(&next)?;
    next.updated = crate::util::now_rfc3339();
    write_source(dir, &next)?;
    open(dirs, dir)
}

/// The credits (by mod id). A row is kept as given; public publishing checks they are complete.
pub fn set_credits(dirs: &DataDirs, dir: &Path, credits: &BTreeMap<String, Credit>) -> Result<Studio> {
    let s = read_source(dir)?;
    let ids: std::collections::BTreeSet<String> = crate::profile::get(dirs, &s.profile)
        .map(|p| ModKind::ALL.iter().flat_map(|k| p.mods.list(*k).iter().map(|m| m.id.clone())).collect())
        .unwrap_or_default();
    let mut clean = BTreeMap::new();
    for (id, c) in credits {
        if !ids.contains(id) {
            continue;
        }
        let author = c.author.trim().to_string();
        let source = c.source.trim().to_string();
        if author.is_empty() && source.is_empty() {
            continue;
        }
        if !source.is_empty() && !source.starts_with("https://") {
            return Err(Error::invalid(format!("{id}: the source must be an https:// link")));
        }
        clean.insert(id.clone(), Credit { author, source, match_kind: c.match_kind.clone() });
    }
    crate::util::write_json_atomic(&dir.join(CREDITS_FILE), &clean)?;
    open(dirs, dir)
}

/// Is a credit complete for a public pack (an author and an https link)?
pub fn credit_complete(c: Option<&Credit>) -> bool {
    c.is_some_and(|c| !c.author.trim().is_empty() && c.source.starts_with("https://") && c.source.len() > "https://".len())
}

/// What public publishing still needs (empty: ready): credits of every mod, a summary.
pub fn public_needs(st: &Studio) -> Vec<String> {
    let mut out = Vec::new();
    let missing: Vec<&str> = st.mods.iter().filter(|m| !credit_complete(m.credit.as_ref())).map(|m| m.name.as_str()).collect();
    if !missing.is_empty() {
        out.push(format!("credits for {}", missing.join(", ")));
    }
    if st.source.summary.as_deref().map(str::trim).unwrap_or("").chars().count() < 10 {
        out.push("a summary of at least 10 characters".into());
    }
    out
}

/// Removes a launcher pack source: its folder goes to backups/studio/, its test instance is removed. The profile it
/// was made from stays.
pub fn delete(dirs: &DataDirs, dir: &Path, game: Option<&crate::game::GameInstall>) -> Result<()> {
    let s = read_source(dir)?;
    drop_test(dirs, &s, game)?;
    let dest = dirs.backups().join(DIR).join(format!("{}-{}", s.id, crate::util::now_rfc3339().replace([':', '-'], "")));
    if let Some(d) = dest.parent() {
        std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
    }
    std::fs::rename(dir, &dest).map_err(crate::util::io_at(dir, "moving"))
}

// ================================================================================================= building

/// The pack::build request of a source.
pub fn build_request(st: &Studio) -> BuildRequest {
    let s = &st.source;
    let mut optional = BTreeMap::new();
    let mut env = BTreeMap::new();
    for m in &st.mods {
        if m.dep == Dep::Optional {
            optional.insert(m.id.clone(), m.default.unwrap_or(true));
        }
        if m.env != Env::default() {
            env.insert(m.id.clone(), m.env);
        }
    }
    let layer = st.dir.join(LAYER_DIR);
    BuildRequest {
        profile: s.profile.clone(),
        id: Some(s.id.clone()),
        name: Some(s.name.clone()),
        version: s.version.clone(),
        authors: s.authors.clone(),
        description: s.summary.clone().or_else(|| s.description.clone()),
        changes: s.changes.clone(),
        optional,
        env,
        // a credit of a mod the profile no longer has would make the build refuse
        credits: st.credits.iter().filter(|(k, c)| credit_complete(Some(c)) && st.mods.iter().any(|m| &m.id == *k)).map(|(k, c)| (k.clone(), c.clone())).collect(),
        layer: (layer_files(&st.dir).map(|f| !f.is_empty()).unwrap_or(false)).then_some(layer),
        overrides: s.overrides.clone(),
        server_settings: s.server_settings.clone().filter(|v| v.get("gameSettings").and_then(Value::as_object).is_some_and(|m| !m.is_empty())),
        game_builds: s.game_builds.clone(),
    }
}

/// Builds the source's .kilnpack into out_dir (default <folder>/out).
#[cfg(feature = "zip")]
pub fn build(dirs: &DataDirs, dir: &Path, out_dir: Option<&Path>, progress: &mut dyn Progress) -> Result<BuiltPack> {
    build_as(dirs, dir, None, out_dir, progress)
}

/// [build] with another version than the source's (kilnctl pack build DIR --version: the source keeps its own).
#[cfg(feature = "zip")]
pub fn build_as(dirs: &DataDirs, dir: &Path, version: Option<&str>, out_dir: Option<&Path>, progress: &mut dyn Progress) -> Result<BuiltPack> {
    let st = open(dirs, dir)?;
    if st.profile_name.is_none() {
        return Err(Error::not_found(format!("the profile this pack is made from ({}) is gone", st.source.profile)));
    }
    let out = out_dir.map(Path::to_path_buf).unwrap_or_else(|| dir.join("out"));
    let mut req = build_request(&st);
    if let Some(v) = version {
        crate::semver::SemVersion::parse(v).map_err(|_| Error::invalid(format!("{v} is not a version like 1.2.0")))?;
        req.version = v.to_string();
    }
    pack::build(dirs, &req, &out, progress)
}

/// Keeps a built version in versions/ (a server pack is made from it later). Returns its file name.
pub fn keep_version(dir: &Path, built: &BuiltPack) -> Result<String> {
    let name = format!("{}-{}.{}", built.index.id, built.index.version, pack::EXTENSION);
    let dest = dir.join(VERSIONS_DIR).join(&name);
    std::fs::create_dir_all(dir.join(VERSIONS_DIR)).map_err(crate::util::io_at(dir, "creating"))?;
    if built.file != dest {
        std::fs::copy(&built.file, &dest).map_err(crate::util::io_at(&built.file, "copying"))?;
    }
    Ok(name)
}

/// The kept file of a published version (versions/<file>).
pub fn version_file(dir: &Path, version: &str) -> Result<PathBuf> {
    let s = read_source(dir)?;
    let v = s.published.as_ref().and_then(|p| p.versions.iter().rev().find(|v| v.version == version)).and_then(|v| v.file.clone())
        .unwrap_or_else(|| format!("{}-{version}.{}", s.id, pack::EXTENSION));
    if v.contains(['/', '\\']) || v.starts_with('.') {
        return Err(Error::invalid("not a version file"));
    }
    let p = dir.join(VERSIONS_DIR).join(v);
    if !p.is_file() {
        return Err(Error::not_found(format!("version {version} of this pack is not kept on this PC")));
    }
    Ok(p)
}

/// Records where a publish stands before it is finished (the pack on Crucible, the version pending): a failed publish
/// resumes from here instead of making another pack.
pub fn record_pending(dir: &Path, published: Published) -> Result<PackSource> {
    let mut s = read_source(dir)?;
    s.published = Some(published);
    s.updated = crate::util::now_rfc3339();
    write_source(dir, &s)?;
    Ok(s)
}

/// Records a finished publish: the pack on Crucible and the version.
pub fn record_published(dir: &Path, published: Published) -> Result<PackSource> {
    let mut s = read_source(dir)?;
    s.visibility = published.visibility;
    s.published = Some(published);
    // the next version to build: the patch after the one just published
    if let Ok(v) = crate::semver::SemVersion::parse(&s.version) {
        s.version = format!("{}.{}.{}", v.major, v.minor, v.patch + 1);
    }
    s.changes = None;
    s.updated = crate::util::now_rfc3339();
    write_source(dir, &s)?;
    Ok(s)
}

/// The pack analysis of the source as it is now (DESIGN 6.1/6.2; [super::check]): the source built into
/// <folder>/out/analyze, then Crucible's checks over it: format and zip safety, sizes, code scan, resolution, lint of the
/// layer, data conflicts, and the credits prefilled from the identification. The Credits, Load order and Publish tabs
/// read it. Offline: no network, no Kiln's-own list or earlier credits here (Crucible identifies again on upload).
#[cfg(feature = "zip")]
pub fn analyze(dirs: &DataDirs, dir: &Path, progress: &mut dyn Progress) -> Result<super::check::CheckReport> {
    let out = dir.join("out").join("analyze");
    let built = build(dirs, dir, Some(&out), &mut Scaled { inner: progress, from: 0.0, to: 0.6 })?;
    progress.report(Some(0.65), "checking the pack");
    let opts = super::check::CheckOptions {
        work: Some(dir.join("out")),
        lint: crate::lint::LintOptions { no_catalog_hint: Some(crate::lint::LAUNCHER_CATALOG_HINT.into()), ..Default::default() },
        ..Default::default()
    };
    let r = super::check::check(super::check::Input::Pack(&built.file), &opts);
    let _ = std::fs::remove_file(&built.file);
    progress.report(Some(1.0), "checked");
    r
}

// ================================================================================================= settings (3.2)

/// One key of a settings file the creator may carry.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CaptureKey {
    /// a.b (JSON), section.key (INI)
    pub key: String,
    pub value: Value,
    /// The mod's default (from its .schema/ snapshot); None: not known.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub default: Option<Value>,
    /// In the pack now.
    pub chosen: bool,
    /// Looks personal (a hotkey or key binding): left out unless chosen.
    pub personal: bool,
    /// Looks like a secret: can never travel.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub secret: Option<String>,
}

/// A settings file of the profile and what of it could travel.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CaptureFile {
    pub path: String,
    #[serde(rename = "mod", default, skip_serializing_if = "Option::is_none")]
    pub mod_id: Option<String>,
    pub mode: OverrideMode,
    /// Defaults are known (a .schema/ snapshot): keys equal to them are not offered.
    pub defaults: bool,
    /// Keys that differ from the defaults (every key when none are known). Empty for a file that travels whole.
    pub keys: Vec<CaptureKey>,
    /// The whole file is in the pack (no keys chosen: replace files, or "whole file").
    pub whole: bool,
    pub side: PartSide,
    pub size: u64,
    /// Why it cannot travel (too large, does not parse).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub refused: Option<String>,
}

fn personal_key(key: &str) -> bool {
    let last = key.rsplit('.').next().unwrap_or(key).to_ascii_lowercase();
    let squashed: String = last.chars().filter(|c| c.is_ascii_alphanumeric()).collect();
    ["hotkey", "keybind", "keybinding", "binding", "keycode", "shortcut", "button"].iter().any(|w| squashed.contains(w))
        || squashed == "key" || (squashed.ends_with("key") && !squashed.ends_with("monkey") && !squashed.ends_with("turkey") && !squashed.ends_with("whiskey"))
}

fn loosely_equal(a: &Value, b: &Value) -> bool {
    if a == b {
        return true;
    }
    let t = |v: &Value| match v {
        Value::String(s) => s.trim().to_ascii_lowercase(),
        Value::Number(n) => n.as_f64().map(|f| f.to_string()).unwrap_or_else(|| n.to_string()),
        other => other.to_string().to_ascii_lowercase(),
    };
    let (x, y) = (t(a), t(b));
    if x == y {
        return true;
    }
    matches!((x.parse::<f64>(), y.parse::<f64>()), (Ok(p), Ok(q)) if (p - q).abs() < 1e-9)
}

struct File {
    file: String,
    mod_id: Option<String>,
}

/// Settings files below the config folder (the override allowlist: settings extensions, no dot folders, no logs).
fn settings_walk(dir: &Path, rel: &str, depth: usize, out: &mut Vec<String>) {
    if depth > 8 {
        return;
    }
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let Some(name) = e.file_name().to_str().map(str::to_string) else { continue };
        if name.starts_with('.') {
            continue;
        }
        let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
        let Ok(t) = e.file_type() else { continue };
        if t.is_dir() {
            settings_walk(&e.path(), &r, depth + 1, out);
        } else if t.is_file() && pack::override_path_ok(&r) {
            out.push(r);
        }
    }
}

/// The profile's config folder.
pub fn config_dir(dirs: &DataDirs, profile: &str) -> PathBuf {
    dirs.profile(profile).join("config")
}

/// Every settings file of the pack's profile with the keys that differ from the mods' defaults (DESIGN 3.2): what
/// "Capture from my current settings" offers. Secrets are marked and never offered as choosable.
pub fn capture(dirs: &DataDirs, dir: &Path) -> Result<Vec<CaptureFile>> {
    let s = read_source(dir)?;
    let cdir = config_dir(dirs, &s.profile);
    let mut out = Vec::new();
    let mut files = Vec::new();
    settings_walk(&cdir, "", 0, &mut files);
    files.sort();
    for rel in files {
        let snap_mod = crate::config::read_snapshot(&cdir, &rel).ok().flatten().map(|s| s.mod_id);
        let f = File { file: rel, mod_id: snap_mod };
        let path = cdir.join(&f.file);
        let size = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
        if !path.is_file() {
            continue;
        }
        let mode = OverrideMode::for_path(&f.file);
        let spec = s.overrides.iter().find(|o| o.path == f.file);
        let side = spec.map(|o| o.side).unwrap_or_default();
        let mut cf = CaptureFile { path: f.file.clone(), mod_id: f.mod_id.clone(), mode, defaults: false, keys: Vec::new(), whole: spec.is_some_and(|o| o.keys.is_empty()), side, size, refused: None };
        if size > pack::MAX_OVERRIDE {
            cf.refused = Some("larger than 64 KiB".into());
            out.push(cf);
            continue;
        }
        let text = crate::manifest::decode_text(&std::fs::read(&path).map_err(crate::util::io_at(&path, "reading"))?);
        if mode == OverrideMode::Replace {
            let found = pack::secret_findings(&f.file, &text);
            if !found.is_empty() {
                cf.refused = Some(found.join("; "));
            }
            out.push(cf);
            continue;
        }
        let leaves = match merge::leaves(mode, &f.file, &text) {
            Ok(l) => l,
            Err(e) => {
                cf.refused = Some(e.message);
                out.push(cf);
                continue;
            }
        };
        let snap = crate::config::read_snapshot(&cdir, &f.file).ok().flatten();
        let defaults: BTreeMap<String, Value> = snap.as_ref().map(|s| s.entries.iter().map(|e| (e.path.clone(), e.default.clone())).collect()).unwrap_or_default();
        cf.defaults = snap.is_some();
        for (k, v) in leaves {
            let Some(v) = v else { continue };
            let key = merge::key_text(&k);
            let default = defaults.get(&key).cloned();
            if cf.defaults && default.as_ref().is_some_and(|d| loosely_equal(d, &v)) {
                continue;
            }
            let secret = match mode {
                OverrideMode::MergePatch => {
                    let mut obj = v.clone();
                    for seg in k.iter().rev() {
                        let mut m = serde_json::Map::new();
                        m.insert(seg.clone(), obj);
                        obj = Value::Object(m);
                    }
                    pack::secret_value_findings(&f.file, &obj)
                }
                _ => pack::secret_findings(&f.file, &format!("{} = {}\n", k.last().cloned().unwrap_or_default(), crate::config::ini::text_of(&v))),
            };
            let chosen = spec.is_some_and(|o| o.keys.is_empty() || o.keys.contains(&key));
            cf.keys.push(CaptureKey { personal: personal_key(&key), chosen: chosen && secret.is_empty(), key, value: v, default, secret: (!secret.is_empty()).then(|| secret.join("; ")) });
        }
        out.push(cf);
    }
    Ok(out)
}

/// One capture choice: a file and its keys (empty: the whole file).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CaptureChoice {
    pub path: String,
    #[serde(default)]
    pub side: PartSide,
    #[serde(default)]
    pub keys: Vec<String>,
    /// The whole file (replace files always).
    #[serde(default)]
    pub whole: bool,
}

/// Puts the chosen keys into the source. Every choice is checked against the files as they are now (the file is a
/// settings file of the profile, a chosen key exists and holds no secret): nothing is written by hand.
pub fn set_capture(dirs: &DataDirs, dir: &Path, choices: &[CaptureChoice]) -> Result<Studio> {
    let offered = capture(dirs, dir)?;
    let mut overrides = Vec::new();
    for c in choices {
        let f = offered.iter().find(|f| f.path == c.path).ok_or_else(|| Error::not_found(format!("{}: not a settings file of this pack's profile", c.path)))?;
        if let Some(why) = &f.refused {
            return Err(err(ErrorCode::Refused, format!("{}: {why}", c.path)));
        }
        let whole = c.whole || f.mode == OverrideMode::Replace;
        if whole {
            if let Some(k) = f.keys.iter().find(|k| k.secret.is_some()) {
                return Err(err(ErrorCode::Refused, format!("{}: {} may hold private data; choose keys instead of the whole file", c.path, k.key)));
            }
            overrides.push(OverrideSpec { path: c.path.clone(), side: c.side, keys: Vec::new() });
            continue;
        }
        if c.keys.is_empty() {
            continue;
        }
        let mut keys = Vec::new();
        for k in &c.keys {
            let ck = f.keys.iter().find(|x| &x.key == k).ok_or_else(|| Error::not_found(format!("{}: no key {k} that differs from the default", c.path)))?;
            if let Some(why) = &ck.secret {
                return Err(err(ErrorCode::Refused, format!("{}: {k} may hold private data ({why})", c.path)));
            }
            keys.push(k.clone());
        }
        overrides.push(OverrideSpec { path: c.path.clone(), side: c.side, keys });
    }
    let mut s = read_source(dir)?;
    s.overrides = overrides;
    save(dirs, dir, s)
}

// ================================================================================================= layer (3.3)

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LayerFile {
    /// content/<name>.json
    pub path: String,
    pub size: u64,
}

/// A layer content file name: content/<name>.json(c), name of letters, digits, '.', '-', '_'.
pub fn layer_path_ok(path: &str) -> bool {
    let Some(name) = path.strip_prefix("content/") else { return false };
    let lower = name.to_ascii_lowercase();
    (lower.ends_with(".json") || lower.ends_with(".jsonc"))
        && !name.starts_with('.')
        && name.len() <= 96
        && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b'_')
        && !pack::windows_device(name)
}

pub fn layer_files(dir: &Path) -> Result<Vec<LayerFile>> {
    let c = dir.join(LAYER_DIR).join("content");
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(&c) else { return Ok(out) };
    for e in rd.flatten() {
        let Some(name) = e.file_name().to_str().map(str::to_string) else { continue };
        let path = format!("content/{name}");
        if !layer_path_ok(&path) || !e.file_type().map(|t| t.is_file()).unwrap_or(false) {
            continue;
        }
        out.push(LayerFile { path, size: e.metadata().map(|m| m.len()).unwrap_or(0) });
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

fn layer_file(dir: &Path, path: &str) -> Result<PathBuf> {
    if !layer_path_ok(path) {
        return Err(Error::invalid(format!("{path}: layer files are content/<name>.json")));
    }
    Ok(dir.join(LAYER_DIR).join(path))
}

pub fn layer_read(dir: &Path, path: &str) -> Result<String> {
    let p = layer_file(dir, path)?;
    match std::fs::read(&p) {
        Ok(b) => Ok(crate::manifest::decode_text(&b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(Error::not_found(format!("{path}: no such layer file"))),
        Err(e) => Err(crate::util::io_at(&p, "reading")(e)),
    }
}

/// Writes a layer file (any text: the lint says what is wrong with it). At most 4 MiB.
pub fn layer_write(dir: &Path, path: &str, text: &str) -> Result<()> {
    read_source(dir)?;
    let p = layer_file(dir, path)?;
    if text.len() > 4 * 1024 * 1024 {
        return Err(err(ErrorCode::Refused, format!("{path}: larger than 4 MiB")));
    }
    let found = pack::secret_findings(path, text);
    if !found.is_empty() {
        return Err(err(ErrorCode::Refused, format!("{path} may hold private data ({})", found.join("; "))).with_detail(found));
    }
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
    }
    crate::util::write_atomic(&p, text.as_bytes())
}

pub fn layer_delete(dir: &Path, path: &str) -> Result<()> {
    let p = layer_file(dir, path)?;
    match std::fs::remove_file(&p) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(crate::util::io_at(&p, "removing")(e)),
    }
}

/// Sets one field in the pack layer (the conflict view's "Set in the pack"): content/pack.json
/// {section: {selector: {field: value}}}, comments and the rest of the file kept. value null removes the field.
pub fn layer_set(dir: &Path, section: &str, selector: &str, field: &str, value: &Value) -> Result<()> {
    if [section, selector, field].iter().any(|s| s.trim().is_empty()) {
        return Err(Error::invalid("a section, a selector and a field are needed"));
    }
    let text = match layer_read(dir, STUDIO_CONTENT) {
        Ok(t) => t,
        Err(e) if e.code == ErrorCode::NotFound => "{\n  // Set by Pack Studio (Load order: conflicts)\n  \"format\": 1\n}\n".to_string(),
        Err(e) => return Err(e),
    };
    use crate::jsonc::PathSeg;
    let at = [PathSeg::Key(section.into()), PathSeg::Key(selector.into()), PathSeg::Key(field.into())];
    let next = if value.is_null() { crate::jsonc::remove_value(&text, &at)? } else { crate::jsonc::set_value(&text, &at, value)? };
    layer_write(dir, STUDIO_CONTENT, &next)
}

/// kiln lint over the layer, with the pack's profile as the other mods and the game-data catalog when one is found.
pub fn layer_lint(dirs: &DataDirs, dir: &Path, catalog: Option<PathBuf>) -> Result<crate::lint::LintReport> {
    let s = read_source(dir)?;
    let layer = dir.join(LAYER_DIR);
    let opts = crate::lint::LintOptions {
        catalog,
        profile: Some(dirs.profile(&s.profile)).filter(|p| p.is_dir()),
        no_catalog_hint: Some(crate::lint::LAUNCHER_CATALOG_HINT.into()),
        ..Default::default()
    };
    Ok(crate::lint::lint_paths(&[layer], &opts))
}

/// One pick for the data editors: a game entry (the catalog) or one a mod of the pack adds.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PickHit {
    pub section: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_name: Option<String>,
    /// The mod that adds it (None: the game).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from: Option<String>,
    /// One of the game's old, test or unused entries ([`crate::catalog::is_hidden_name`]); listed only when asked.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub hidden: bool,
}

/// The game-data catalog, loaded once per file and change (it is large).
pub fn load_catalog(explicit: Option<&Path>) -> Option<std::sync::Arc<crate::catalog::Catalog>> {
    use std::sync::{Arc, Mutex};
    type Cached = Option<(PathBuf, Option<std::time::SystemTime>, Arc<crate::catalog::Catalog>)>;
    static CACHE: Mutex<Cached> = Mutex::new(None);
    let path = crate::catalog::find(Some("client"), explicit)?;
    let stamp = std::fs::metadata(&path).and_then(|m| m.modified()).ok();
    let mut c = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((p, t, cat)) = c.as_ref() && *p == path && *t == stamp {
        return Some(cat.clone());
    }
    let cat = Arc::new(crate::catalog::Catalog::load(&path).ok()?);
    *c = Some((path, stamp, cat.clone()));
    Some(cat)
}

/// The names the pack's Kiln mods and the layer add ("add": [{name}] in their content packs), by section.
pub fn added_names(dirs: &DataDirs, dir: &Path) -> Result<Vec<PickHit>> {
    let s = read_source(dir)?;
    let mut roots: Vec<(String, PathBuf)> = Vec::new();
    if let Ok(p) = crate::profile::get(dirs, &s.profile) {
        for m in p.mods.list(ModKind::Kiln) {
            let r = crate::library::LibraryRef { kind: ModKind::Kiln, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
            if let Ok(at) = crate::library::path_of(dirs, &r) {
                roots.push((m.id.clone(), at));
            }
        }
    }
    roots.push((format!("{}{}", pack::LAYER_PREFIX, s.id), dir.join(LAYER_DIR)));
    let mut out = Vec::new();
    for (mod_id, root) in roots {
        let Ok(rd) = std::fs::read_dir(root.join("content")) else { continue };
        let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.extension().and_then(|x| x.to_str()).is_some_and(|x| x.eq_ignore_ascii_case("json") || x.eq_ignore_ascii_case("jsonc"))).collect();
        files.sort();
        for f in files {
            let Ok(b) = std::fs::read(&f) else { continue };
            let Ok(tree) = crate::jsonc::parse_tree(&crate::manifest::decode_text(&b)) else { continue };
            let v = tree.to_value();
            let Some(obj) = v.as_object() else { continue };
            for (section, body) in obj {
                for a in body.get("add").and_then(Value::as_array).into_iter().flatten() {
                    if let Some(name) = a.get("name").and_then(Value::as_str) {
                        out.push(PickHit { section: section.clone(), name: name.to_string(), id: None, display_name: a.get("displayName").and_then(Value::as_str).map(str::to_string), from: Some(mod_id.clone()), hidden: false });
                    }
                }
            }
        }
    }
    Ok(out)
}

/// The data editors' picker: entries of a section whose name or display name holds the query, the pack's own first. The
/// game's old, test and unused entries ([`crate::catalog::is_hidden_name`]) only with `hidden` ("Show hidden game entries");
/// an exact name finds one either way (a creator typed or pasted it), marked `hidden`.
pub fn pick(dirs: &DataDirs, dir: &Path, section: &str, query: &str, limit: usize, hidden: bool) -> Result<Vec<PickHit>> {
    let limit = if limit == 0 { 50 } else { limit.min(500) };
    let q = query.trim().to_lowercase();
    let mut out: Vec<PickHit> = added_names(dirs, dir)?
        .into_iter()
        .filter(|h| h.section == section && (q.is_empty() || h.name.to_lowercase().contains(&q) || h.display_name.as_deref().is_some_and(|d| d.to_lowercase().contains(&q))))
        .collect();
    if let Some(cat) = load_catalog(None) {
        // room for the hidden entries the search ranks among the first (63 of 4268 items: a page never needs more)
        let wide = if hidden { limit } else { limit + 200 };
        for h in crate::catalog::search(&cat, query, Some(section), wide) {
            let Some(name) = h.name.clone() else { continue };
            if out.iter().any(|o| o.name == name) {
                continue;
            }
            let is_hidden = crate::catalog::is_hidden_name(&name);
            if is_hidden && !hidden && !name.eq_ignore_ascii_case(query.trim()) {
                continue;
            }
            out.push(PickHit { section: h.section, name, id: h.id, display_name: h.display_name, from: None, hidden: is_hidden });
        }
    }
    out.truncate(limit);
    Ok(out)
}

/// The fields of a catalog section (the typed editors), empty without a catalog.
pub fn fields(section: &str) -> Vec<crate::catalog::FieldInfo> {
    load_catalog(None).and_then(|c| c.fields.get(section).cloned()).unwrap_or_default()
}

// ================================================================================================= load order (3.4)

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OrderRow {
    pub id: String,
    pub name: String,
    /// Loads (in this place), or why not.
    pub loads: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    /// The pack layer (always last).
    pub layer: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StudioOrder {
    /// Kiln mods in load order (later wins for data), the layer last; then the ones that do not load.
    pub kiln: Vec<OrderRow>,
    /// Native mods whose DLLs load late (chain.ini [late]).
    pub late: Vec<String>,
    pub warnings: Vec<String>,
}

/// The resolver's order of the profile's Kiln mods (as a client hosting its own world loads them), the pack layer
/// forced last.
pub fn order(dirs: &DataDirs, dir: &Path) -> Result<StudioOrder> {
    let s = read_source(dir)?;
    let p = crate::profile::get(dirs, &s.profile)?;
    let names: BTreeMap<String, String> = crate::library::list(dirs, Some(ModKind::Kiln))?.into_iter().map(|e| (e.id, e.name)).collect();
    let r = crate::profile::resolve_preview_for(dirs, &p, crate::manifest::RunSide::Client, true)?;
    let name = |id: &str| names.get(id).cloned().unwrap_or_else(|| id.to_string());
    let mut kiln: Vec<OrderRow> = r.kiln.order.iter().map(|id| OrderRow { id: id.clone(), name: name(id), loads: true, reason: None, layer: false }).collect();
    kiln.push(OrderRow { id: format!("{}{}", pack::LAYER_PREFIX, s.id), name: format!("{} (pack layer)", s.name), loads: true, reason: None, layer: true });
    for x in &r.kiln.rejected {
        if r.with_world.contains(&x.id) {
            continue;
        }
        kiln.push(OrderRow { id: x.id.clone(), name: name(&x.id), loads: false, reason: Some(x.reason.clone()), layer: false });
    }
    Ok(StudioOrder { kiln, late: p.late.clone(), warnings: r.warnings })
}

// ================================================================================================= test (3.5)

/// What a test run left in the game's kiln.log.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TestReport {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile: Option<String>,
    /// The log is newer than the test's start (the game ran with the test instance).
    pub ran: bool,
    /// Data conflicts the game logged (Content.PackReport's).
    pub conflicts: Vec<String>,
    /// Errors and warnings the game logged.
    pub problems: Vec<String>,
}

/// Reads the game's kiln.log after a test (conflict lines and errors/warnings, at most 100 each).
pub fn test_report(dir: &Path, game_dir: Option<&Path>) -> Result<TestReport> {
    let s = read_source(dir)?;
    let mut r = TestReport { profile: s.test_profile.clone(), ..Default::default() };
    let (Some(game), Some(started)) = (game_dir, s.test_started.as_deref()) else { return Ok(r) };
    let Some(log) = crate::logs::list(game).ok().and_then(|l| l.into_iter().find(|f| f.name.rsplit('/').next().is_some_and(|n| n.eq_ignore_ascii_case("kiln.log")))) else { return Ok(r) };
    let path = PathBuf::from(&log.path);
    // the same second counts (both are whole seconds)
    let newer = log.modified.as_str() >= started;
    r.ran = newer;
    if !newer {
        return Ok(r);
    }
    let text = crate::manifest::decode_text(&std::fs::read(&path).map_err(crate::util::io_at(&path, "reading"))?);
    // the last boot's lines
    let lines: Vec<&str> = text.lines().collect();
    let from = lines.iter().rposition(|l| crate::logs::parse_line(l).is_some_and(|p| p.source == "kiln" && p.message.starts_with("Kiln ") && p.message.contains(" starting"))).unwrap_or(0);
    for line in &lines[from..] {
        let lower = line.to_ascii_lowercase();
        if lower.contains("conflict:") {
            if r.conflicts.len() < 100 {
                r.conflicts.push(line.trim().to_string());
            }
        } else if let Some(l) = crate::logs::parse_line(line)
            && matches!(l.level, 'E' | 'W')
            && r.problems.len() < 100
        {
            r.problems.push(line.trim().to_string());
        }
    }
    Ok(r)
}

/// Records the test instance and when the test started.
pub fn record_test(dir: &Path, profile: Option<String>) -> Result<PackSource> {
    let mut s = read_source(dir)?;
    s.test_started = profile.as_ref().map(|_| crate::util::now_rfc3339());
    s.test_profile = profile;
    write_source(dir, &s)?;
    Ok(s)
}

/// Removes the last test instance; a game that has it active goes back to the pack's own profile first (an instance
/// that is active cannot be removed).
fn drop_test(dirs: &DataDirs, s: &PackSource, game: Option<&crate::game::GameInstall>) -> Result<()> {
    let Some(old) = &s.test_profile else { return Ok(()) };
    if let Some(g) = game
        && crate::profile::active(dirs, g).ok().flatten().is_some_and(|a| &a.profile_id == old)
    {
        crate::profile::activate(dirs, &s.profile, g)?;
    }
    match pack::uninstall(dirs, old) {
        Ok(_) => Ok(()),
        Err(e) if e.code == ErrorCode::NotFound => Ok(()),
        Err(e) if e.code == ErrorCode::Refused => Err(err(ErrorCode::Refused, format!("the test instance is still the loaded profile of a game: load {} again first ({})", s.name, e.message))),
        Err(e) => Err(e),
    }
}

/// "Test this pack": the source built and installed as an instance of its own ("<name> (test)"), replacing the last
/// test instance; with a game, a save backup and activation (then Play starts it).
#[cfg(feature = "zip")]
pub fn test(dirs: &DataDirs, dir: &Path, game: Option<&crate::game::GameInstall>, progress: &mut dyn Progress) -> Result<pack::InstallResult> {
    let s = read_source(dir)?;
    if s.test_profile.is_some() {
        drop_test(dirs, &s, game)?;
        record_test(dir, None)?;
    }
    let tmp = dir.join("out").join("test");
    let built = build(dirs, dir, Some(&tmp), &mut Scaled { inner: progress, from: 0.0, to: 0.4 })?;
    let req = pack::InstallRequest {
        source: super::ModpackSource::Path { path: built.file.clone(), sha256: Some(built.sha256.clone()) },
        consent: true,
        allow_http: false,
        name: Some(format!("{} (test)", s.name)),
        optional: BTreeMap::new(),
        activate: Some(game.is_some()),
    };
    let r = pack::install(dirs, game, &req, &mut Scaled { inner: progress, from: 0.4, to: 1.0 });
    let _ = std::fs::remove_file(&built.file);
    let r = r?;
    record_test(dir, Some(r.profile.id.clone()))?;
    Ok(r)
}

/// Removes the test instance (a game that has it loaded goes back to the pack's own profile).
pub fn test_end(dirs: &DataDirs, dir: &Path, game: Option<&crate::game::GameInstall>) -> Result<()> {
    let s = read_source(dir)?;
    drop_test(dirs, &s, game)?;
    record_test(dir, None).map(|_| ())
}

struct Scaled<'a> {
    inner: &'a mut dyn Progress,
    from: f32,
    to: f32,
}

impl Progress for Scaled<'_> {
    fn report(&mut self, fraction: Option<f32>, message: &str) {
        self.inner.report(fraction.map(|f| self.from + f.clamp(0.0, 1.0) * (self.to - self.from)), message);
    }
    fn canceled(&self) -> bool {
        self.inner.canceled()
    }
}

