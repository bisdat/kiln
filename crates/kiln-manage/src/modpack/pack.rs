//! Crucible packs: format 2 of `kiln-modpack.json` (a `.kilnpack` zip) and the instances they install as.
//! Docs: docs/kiln/launcher/crucible.md; schema: schema/kiln-modpack.schema.json (format 2).
//!
//! A pack is a profile made shareable: every mod it needs (pinned by kdh1, the files inside), plus the pack's own layer:
//! a data-only Kiln mod that loads after its mods, config overrides (merge patches, INI keys, whole files), the switches
//! of optional mods, client/server parts and the server's game settings. Installing makes an **instance**: a profile of
//! its own whose mods are Library entries, with three layers: the pack's mods < the pack layer < the player's own changes
//! (the live config folder and any mod the player adds, such as a `local.<profile>` data mod). The pack's version as
//! installed is kept in `profiles/<pid>/pack/base/`, so an update is a three-way merge of each override (`super::merge`).
//!
//! Zip layout (entries sorted bytewise, one fixed time, mode 0644; same input, same bytes):
//! ```text
//! kiln-modpack.json                       the index (format 2, kind "pack")
//! mods/<kind>/<id>/...                    every mod; plugins as mods/plugin/<file>.dll
//! layer/                                  the pack layer: kiln.mod.json (id pack.<pack id>, no entryAssembly) + content/
//! overrides/<path>                        config overrides for both sides (path relative to the profile's configDir)
//! client-overrides/<path>, server-overrides/<path>
//! ```

use super::merge::{self, Key, KeyConflict, Leaves, OverrideMode, Pick};
use super::{ModpackSource, Scaled, err, mod_id_ok, records, valid_rel};
use crate::game::GameInstall;
use crate::library::LibraryRef;
use crate::manifest::{ModKind, Side};
use crate::paths::DataDirs;
use crate::profile::{Profile, ProfileMod};
use crate::task::Progress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

pub const FORMAT: u32 = 2;
pub const KIND: &str = "pack";
/// File extension of a pack (MIME application/vnd.kiln.pack+zip).
pub const EXTENSION: &str = "kilnpack";
pub const MODS_DIR: &str = "mods";
pub const LAYER_PATH: &str = "layer";
/// The pack layer's mod id is this + the pack id.
pub const LAYER_PREFIX: &str = "pack.";
/// The player's own data mod in an instance is this + the profile id (made by the launcher's editors; kept by updates).
pub const PLAYER_LAYER_PREFIX: &str = "local.";
/// `profiles/<pid>/pack/`: instance.json, base/ (the installed version's index + overrides), undo/.
pub const INSTANCE_DIR: &str = "pack";
pub const INSTANCE_FILE: &str = "instance.json";

pub const MAX_FILE: u64 = 512 * 1024 * 1024;
pub const MAX_EXPANDED: u64 = 4 * 1024 * 1024 * 1024;
pub const MAX_ENTRIES: usize = 50_000;
pub const MAX_OVERRIDE: u64 = 64 * 1024;
pub const MAX_OVERRIDES: u64 = 1024 * 1024;

fn yes() -> bool {
    true
}

// ================================================================================================= the index

/// Required mods are always on; optional ones are on or off as the player chooses (`default` until they do).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Dep {
    #[default]
    Required,
    Optional,
}

/// What a side needs of a mod (Modrinth's env idea).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Need {
    #[default]
    Required,
    Optional,
    Unsupported,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Env {
    #[serde(default)]
    pub client: Need,
    #[serde(default)]
    pub server: Need,
}

/// Which part of an install an override belongs to.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PartSide {
    #[default]
    Both,
    Client,
    Server,
}

impl PartSide {
    /// The zip folder of this side's overrides.
    pub fn dir(self) -> &'static str {
        match self {
            PartSide::Both => "overrides",
            PartSide::Client => "client-overrides",
            PartSide::Server => "server-overrides",
        }
    }
    fn for_client(self) -> bool {
        self != PartSide::Server
    }
    fn for_server(self) -> bool {
        self != PartSide::Client
    }
}

/// Where Crucible stores a file (the archive of a mod or the layer, content-addressed). Not in a locally built pack.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StoredFile {
    pub sha256: String,
    pub size: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
}

/// Attribution of a mod (required for public packs on Crucible).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Credit {
    pub author: String,
    /// https link to where the mod comes from.
    pub source: String,
    /// How the credit was found: kiln | crucible | github | nexus | manifest | manual.
    #[serde(default, rename = "match", skip_serializing_if = "Option::is_none")]
    pub match_kind: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackMod {
    pub kind: ModKind,
    pub id: String,
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Inside the zip: mods/<kind>/<id>, or mods/plugin/<file>.dll.
    pub path: String,
    /// kdh1 (a Kiln mod's identity); sha256 for a plugin DLL.
    pub hash: String,
    #[serde(default)]
    pub dep: Dep,
    /// An optional mod's switch until the player sets it (default on).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub default: Option<bool>,
    /// Absent: required on both sides.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub env: Option<Env>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub file: Option<StoredFile>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub credit: Option<Credit>,
}

impl PackMod {
    pub fn env(&self) -> Env {
        self.env.unwrap_or_default()
    }
    /// On unless the player (or the pack's default) switched it off.
    pub fn on(&self, choices: &BTreeMap<String, bool>) -> bool {
        match self.dep {
            Dep::Required => true,
            Dep::Optional => choices.get(&self.id).copied().unwrap_or(self.default.unwrap_or(true)),
        }
    }
    fn library_ref(&self) -> LibraryRef {
        LibraryRef { kind: self.kind, id: self.id.clone(), version: self.version.clone(), hash: self.hash.clone() }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackLayer {
    /// pack.<pack id>
    pub id: String,
    /// The pack's version.
    pub version: String,
    /// "layer"
    pub path: String,
    pub hash: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub file: Option<StoredFile>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackOrder {
    /// The Kiln load order the creator had (the layer last). The runtime orders by manifests; the layer's loadAfter
    /// names every Kiln mod of the pack, so it loads after them.
    #[serde(default)]
    pub kiln: Vec<String>,
    /// Native mod ids whose DLLs load late (chain.ini [late]), as the profile's `late`.
    #[serde(default)]
    pub late: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackOverride {
    /// Relative to the profile's configDir (the server's kiln/config).
    pub path: String,
    pub mode: OverrideMode,
    #[serde(default)]
    pub side: PartSide,
    /// Of the file in the zip.
    pub sha256: String,
    /// Of the file in the zip, in bytes (Crucible plans an upload by sha256 and size; checked when present).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub size: Option<u64>,
}

impl PackOverride {
    pub fn zip_path(&self) -> String {
        format!("{}/{}", self.side.dir(), self.path)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackServer {
    /// Merged into the server's enshrouded_server.json (`{gameSettings: {...}}`, kiln.settings' rules).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub settings: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notes: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackGame {
    /// Steam build ids the pack was made for.
    #[serde(default)]
    pub builds: Vec<u64>,
}

/// Where Crucible has the pack (filled in by Crucible's signed index; absent in a locally built pack).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CrucibleRef {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pack: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub slug: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<String>,
}

/// `kiln-modpack.json` format 2 (schema/kiln-modpack.schema.json).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackIndex {
    pub format: u32,
    /// "pack"
    pub kind: String,
    /// The pack's id (a mod id; at most 59 characters, the layer is pack.<id>).
    pub id: String,
    pub name: String,
    /// SemVer.
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub created: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub authors: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game: Option<PackGame>,
    /// Kiln versions the pack needs.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kiln: Option<String>,
    /// Kiln API versions the pack needs.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub api: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub crucible: Option<CrucibleRef>,
    pub mods: Vec<PackMod>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layer: Option<PackLayer>,
    #[serde(default)]
    pub order: PackOrder,
    /// Drop-in folders the instance loads (default none: an instance has only its pack's mods).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub dropins: Vec<ModKind>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub overrides: Vec<PackOverride>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub server: Option<PackServer>,
    /// What changed in this version (the creator's words).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub changes: Option<String>,
}

impl PackIndex {
    pub fn layer_id(&self) -> String {
        format!("{LAYER_PREFIX}{}", self.id)
    }
    fn find(&self, kind: ModKind, id: &str) -> Option<&PackMod> {
        self.mods.iter().find(|m| m.kind == kind && m.id == id)
    }
}

/// The zip folder of a mod.
pub fn mod_path(kind: ModKind, id: &str) -> String {
    format!("{MODS_DIR}/{}/{id}", kind.as_str())
}

/// May a pack carry an override for this configDir-relative path? A settings file's extension, not under .schema/, not
/// native.json (switches travel in the index), no dot folder.
pub fn override_path_ok(path: &str) -> bool {
    valid_rel(path)
        && !path.contains('\\')
        && crate::settings_files::is_settings_extension(path)
        && !crate::settings_files::is_log(path)
        && !path.split('/').any(|p| p.starts_with('.') || windows_device(p))
        && !path.eq_ignore_ascii_case("native.json")
}

/// The name two zip entries share when Windows (or macOS) would make them one file: NFC, Unicode lower case. Two
/// entries with one folded name are refused, so an unpack never writes one file over another.
pub fn fold_name(rel: &str) -> String {
    unicode_normalization::UnicodeNormalization::nfc(rel).collect::<String>().to_lowercase()
}

/// A Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an extension, as Windows 10 reads them): no
/// file of that name can be made there.
pub fn windows_device(part: &str) -> bool {
    let stem = part.split('.').next().unwrap_or_default().trim_end_matches(' ').to_ascii_uppercase();
    let numbered = |p: &str| stem.strip_prefix(p).is_some_and(|n| matches!(n, "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "\u{b9}" | "\u{b2}" | "\u{b3}"));
    matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$") || numbered("COM") || numbered("LPT")
}

fn semver_ok(v: &str) -> bool {
    crate::semver::SemVersion::parse(v).is_ok()
}

fn sha256_ok(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}

/// The index's own rules (before any file is looked at): every problem, as a Schema error.
pub fn check_index(ix: &PackIndex) -> Result<()> {
    let mut problems = Vec::new();
    if ix.format != FORMAT {
        problems.push(format!("format {} (this is format {FORMAT})", ix.format));
    }
    if ix.kind != KIND {
        problems.push(format!("kind {:?} (must be pack)", ix.kind));
    }
    if !mod_id_ok(&ix.id) || ix.id.len() > 64 - LAYER_PREFIX.len() {
        problems.push(format!("id {:?}: a mod id of at most {} characters", ix.id, 64 - LAYER_PREFIX.len()));
    }
    if ix.name.trim().is_empty() || ix.name.chars().count() > 128 {
        problems.push("name: 1 to 128 characters".into());
    }
    if !semver_ok(&ix.version) {
        problems.push(format!("version {:?} is not SemVer", ix.version));
    }
    let mut seen = BTreeSet::new();
    for m in &ix.mods {
        let path_ok = if m.kind == ModKind::Plugin {
            m.path.strip_prefix("mods/plugin/").is_some_and(|f| !f.contains('/') && f.to_ascii_lowercase().ends_with(".dll")) && valid_rel(&m.path)
        } else {
            m.path == mod_path(m.kind, &m.id)
        };
        if !path_ok {
            problems.push(format!("mod {}: path {:?} (must be {})", m.id, m.path, mod_path(m.kind, &m.id)));
        }
        if !mod_id_ok(&m.id) || m.id.starts_with(LAYER_PREFIX) {
            problems.push(format!("mod id {:?} is not valid", m.id));
        }
        if !semver_ok(&m.version) {
            problems.push(format!("mod {}: version {:?} is not SemVer", m.id, m.version));
        }
        let hash_ok = if m.kind == ModKind::Plugin { sha256_ok(&m.hash) } else { crate::hash::hash8(&m.hash).is_some() && m.hash.starts_with("kdh1:") };
        if !hash_ok {
            problems.push(format!("mod {}: hash {:?}", m.id, m.hash));
        }
        if m.dep == Dep::Required && m.default.is_some() {
            problems.push(format!("mod {}: default is for optional mods", m.id));
        }
        if !seen.insert((m.kind, m.id.clone())) {
            problems.push(format!("mod {} is listed twice", m.id));
        }
        if let Some(f) = &m.file && !sha256_ok(&f.sha256) {
            problems.push(format!("mod {}: file.sha256", m.id));
        }
        if let Some(c) = &m.credit && (c.author.trim().is_empty() || !c.source.to_ascii_lowercase().starts_with("https://")) {
            problems.push(format!("mod {}: a credit needs an author and an https source", m.id));
        }
    }
    if let Some(l) = &ix.layer {
        if l.id != ix.layer_id() || l.path != LAYER_PATH || l.version != ix.version || !l.hash.starts_with("kdh1:") || crate::hash::hash8(&l.hash).is_none() {
            problems.push(format!("layer: id {}, path layer, the pack's version and a kdh1", ix.layer_id()));
        }
    }
    for id in &ix.order.kiln {
        if ix.find(ModKind::Kiln, id).is_none() && ix.layer.as_ref().is_none_or(|l| &l.id != id) {
            problems.push(format!("order.kiln names {id}, which the pack does not carry"));
        }
    }
    for id in &ix.order.late {
        if ix.find(ModKind::Native, id).is_none() {
            problems.push(format!("order.late names {id}, not a native mod of the pack"));
        }
    }
    let mut paths = BTreeSet::new();
    for o in &ix.overrides {
        if !override_path_ok(&o.path) {
            problems.push(format!("override {:?}: a settings file below the config folder (not .schema/, not native.json)", o.path));
        }
        if !paths.insert((o.side, fold_name(&o.path))) {
            problems.push(format!("override {} is listed twice", o.path));
        }
        if !sha256_ok(&o.sha256) {
            problems.push(format!("override {}: sha256", o.path));
        }
        if o.size.is_some_and(|n| n > MAX_OVERRIDE) {
            problems.push(format!("override {}: size over 64 KiB", o.path));
        }
        let fits = match o.mode {
            OverrideMode::MergePatch => o.path.to_ascii_lowercase().ends_with(".json"),
            OverrideMode::Ini => o.path.to_ascii_lowercase().ends_with(".ini"),
            OverrideMode::Replace => true,
        };
        if !fits {
            problems.push(format!("override {}: mode {:?} does not fit the file", o.path, o.mode));
        }
    }
    if let Some(s) = ix.server.as_ref().and_then(|s| s.settings.as_ref()) {
        // only the game's rules travel: a server's name, ports, user groups (their passwords: admin rights) and bans
        // are the server owner's, and a pack would set them on every server it is put on
        let only_game = s.as_object().is_some_and(|o| o.keys().all(|k| k == "gameSettings") && o.get("gameSettings").is_none_or(Value::is_object));
        if !only_game {
            problems.push("server.settings holds gameSettings only (an object; no name, ports, user groups, passwords or bans)".into());
        }
    }
    if problems.is_empty() {
        Ok(())
    } else {
        Err(Error::new(ErrorCode::Schema, format!("{}: {}", super::MANIFEST, problems.join("; "))).with_detail(problems))
    }
}

// ================================================================================================= reading a pack

/// A verified pack: its index, its overrides' bytes, and the zip's sha256 and size.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReadPack {
    pub index: PackIndex,
    pub overrides: BTreeMap<(PartSide, String), Vec<u8>>,
    pub sha256: String,
    pub size: u64,
    /// Ids of the mods (and the layer) with code files in them (.dll, .exe, .so, .lua, scripts).
    pub code: BTreeSet<String>,
}

/// A file of code by its name: what the install warning counts.
fn is_code(rel: &str) -> bool {
    let l = rel.to_ascii_lowercase();
    [".dll", ".exe", ".so", ".dylib", ".lua", ".js", ".py", ".ps1", ".bat", ".cmd", ".sh", ".asi"].iter().any(|x| l.ends_with(x))
}

impl ReadPack {
    fn override_text(&self, side: PartSide, path: &str) -> Option<String> {
        self.overrides.get(&(side, path.to_string())).map(|b| crate::manifest::decode_text(b))
    }
}

/// The format of the kiln-modpack.json at a zip's root (1: a server modpack, 2: a Crucible pack).
#[cfg(feature = "zip")]
pub fn peek_format(zip_file: &Path) -> Result<u32> {
    use std::io::Read;
    let f = std::fs::File::open(zip_file).map_err(crate::util::io_at(zip_file, "opening"))?;
    let mut a = zip::ZipArchive::new(f).map_err(|e| err(ErrorCode::Parse, format!("{}: {e}", zip_file.display())))?;
    let e = a.by_name(super::MANIFEST).map_err(|_| err(ErrorCode::Schema, format!("{}: no {}", zip_file.display(), super::MANIFEST)))?;
    let mut b = Vec::new();
    e.take(4 << 20).read_to_end(&mut b)?;
    let v: Value = serde_json::from_slice(&b).map_err(|e| err(ErrorCode::Schema, format!("{}: {e}", super::MANIFEST)))?;
    v.get("format").and_then(Value::as_u64).map(|f| f as u32).ok_or_else(|| err(ErrorCode::Schema, format!("{}: no format", super::MANIFEST)))
}

/// Checks a pack and (with `extract_to`) unpacks its mods and layer there, in the zip's layout: the zip's sha256 when
/// given, the index's rules, every entry inside a listed mod, the layer or a listed override (no symlinks, no absolute
/// or `..` paths, no two entries differing in case only), sizes (512 MiB a file, 4 GiB in all, 64 KiB an override, 1 MiB
/// of overrides), each mod's and the layer's kdh1 from the entries, each override's sha256. A Kiln mod carrying cook
/// outputs (assets/cooked/) is refused: they are made on each machine.
#[cfg(feature = "zip")]
pub fn read(zip_file: &Path, sha256: Option<&str>, extract_to: Option<&Path>) -> Result<ReadPack> {
    use std::io::{Read, Write};
    let (have_sha, size) = crate::hash::sha256_file(zip_file).map_err(|e| e.with_detail(zip_file.display().to_string()))?;
    if let Some(want) = sha256 && !have_sha.eq_ignore_ascii_case(want) {
        return Err(err(ErrorCode::HashMismatch, format!("{}: sha256 {have_sha}, expected {want}", zip_file.display())));
    }
    let f = std::fs::File::open(zip_file).map_err(crate::util::io_at(zip_file, "opening"))?;
    let zerr = |e: zip::result::ZipError| err(ErrorCode::Parse, format!("{}: {e}", zip_file.display()));
    let mut a = zip::ZipArchive::new(f).map_err(zerr)?;
    if a.len() > MAX_ENTRIES {
        return Err(err(ErrorCode::Refused, format!("{}: more than {MAX_ENTRIES} entries", zip_file.display())));
    }
    let index: PackIndex = {
        let e = a.by_name(super::MANIFEST).map_err(|_| err(ErrorCode::Schema, format!("{}: no {}", zip_file.display(), super::MANIFEST)))?;
        let mut b = Vec::new();
        e.take(4 << 20).read_to_end(&mut b)?;
        let v: Value = serde_json::from_slice(&b).map_err(|e| err(ErrorCode::Schema, format!("{}: {e}", super::MANIFEST)))?;
        match v.get("format").and_then(Value::as_u64) {
            Some(2) => {}
            Some(1) => return Err(err(ErrorCode::Unsupported, "this is a server modpack (format 1), not a Crucible pack: install it from the server's offer")),
            Some(n) => return Err(err(ErrorCode::Unsupported, format!("{} format {n} is newer than this launcher: update Kiln", super::MANIFEST))),
            None => return Err(err(ErrorCode::Schema, format!("{}: no format", super::MANIFEST))),
        }
        serde_json::from_value(v).map_err(|e| err(ErrorCode::Schema, format!("{}: {e}", super::MANIFEST)))?
    };
    check_index(&index)?;
    if let Some(dir) = extract_to {
        crate::engine_mods::refuse_symlink_path(dir, dir)?;
        std::fs::create_dir_all(dir).map_err(crate::util::io_at(dir, "creating"))?;
    }
    // owner of each entry: a mod (by index), the layer, an override
    let listed: BTreeMap<String, (PartSide, &PackOverride)> = index.overrides.iter().map(|o| (o.zip_path(), (o.side, o))).collect();
    let mut recs: BTreeMap<usize, Vec<crate::hash::Record>> = BTreeMap::new();
    let mut layer_recs: Vec<crate::hash::Record> = Vec::new();
    let mut overrides = BTreeMap::new();
    let mut cooked: BTreeMap<usize, String> = BTreeMap::new();
    let mut code = BTreeSet::new();
    let mut paths = BTreeSet::new();
    let (mut expanded, mut override_bytes) = (0u64, 0u64);
    for i in 0..a.len() {
        let mut e = a.by_index(i).map_err(zerr)?;
        let name = e.name().map_err(zerr)?.to_string();
        let rel = name.trim_end_matches('/');
        if !valid_rel(rel) || e.enclosed_name().is_none() || !paths.insert(fold_name(rel)) {
            return Err(err(ErrorCode::Refused, format!("{}: entry {name:?} has an unsafe path", zip_file.display())));
        }
        // kdh1 skips dot entries, so no hash would cover one; a Windows device name is no file there
        if rel.split('/').any(|p| p.starts_with('.') || windows_device(p)) {
            return Err(err(ErrorCode::Refused, format!("{}: entry {name:?}: a dot name (no hash covers it) or a Windows device name", zip_file.display())));
        }
        if e.is_symlink() || e.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
            return Err(err(ErrorCode::Refused, format!("{}: entry {name} is a symlink", zip_file.display())));
        }
        if e.is_dir() || name == super::MANIFEST {
            continue;
        }
        if e.size() > MAX_FILE {
            return Err(err(ErrorCode::Refused, format!("{}: entry {name} is larger than 512 MiB", zip_file.display())));
        }
        if let Some((side, o)) = listed.get(&name) {
            let mut b = Vec::new();
            (&mut e).take(MAX_OVERRIDE + 1).read_to_end(&mut b)?;
            override_bytes += b.len() as u64;
            if b.len() as u64 > MAX_OVERRIDE || override_bytes > MAX_OVERRIDES {
                return Err(err(ErrorCode::Refused, format!("{}: override {} is too large (64 KiB a file, 1 MiB in all)", zip_file.display(), o.path)));
            }
            if crate::hash::sha256_hex(&b) != o.sha256 {
                return Err(err(ErrorCode::HashMismatch, format!("override {} does not match its sha256", o.path)));
            }
            if let Some(n) = o.size && n != b.len() as u64 {
                return Err(err(ErrorCode::HashMismatch, format!("override {} is {} bytes, its index says {n}", o.path, b.len())));
            }
            overrides.insert((*side, o.path.clone()), b);
            continue;
        }
        let (owner, inner) = if let Some(r) = name.strip_prefix("layer/").filter(|_| index.layer.is_some()) {
            (None, r.to_string())
        } else {
            let found = index.mods.iter().enumerate().find_map(|(k, m)| {
                if m.kind == ModKind::Plugin {
                    (name == m.path).then(|| (k, m.path.rsplit('/').next().unwrap_or_default().to_string()))
                } else {
                    name.strip_prefix(&format!("{}/", m.path)).map(|r| (k, r.to_string()))
                }
            });
            let Some((k, r)) = found else {
                return Err(err(ErrorCode::Refused, format!("{}: entry {name} belongs to no listed mod, the layer or an override", zip_file.display())));
            };
            (Some(k), r)
        };
        let mut out = match extract_to {
            Some(dir) => {
                let to = dir.join(rel);
                if let Some(p) = to.parent() {
                    std::fs::create_dir_all(p).map_err(crate::util::io_at(p, "creating"))?;
                }
                Some((std::fs::File::create(&to).map_err(crate::util::io_at(&to, "writing"))?, to))
            }
            None => None,
        };
        let mut h = <sha2::Sha256 as sha2::Digest>::new();
        let mut buf = vec![0u8; 256 * 1024];
        let mut n_size = 0u64;
        loop {
            let n = e.read(&mut buf)?;
            if n == 0 {
                break;
            }
            n_size += n as u64;
            expanded += n as u64;
            if n_size > MAX_FILE || expanded > MAX_EXPANDED {
                return Err(err(ErrorCode::Refused, format!("{}: the pack unpacks to more than its limits (512 MiB a file, 4 GiB in all)", zip_file.display())));
            }
            sha2::Digest::update(&mut h, &buf[..n]);
            if let Some((f, to)) = out.as_mut() {
                f.write_all(&buf[..n]).map_err(crate::util::io_at(to, "writing"))?;
            }
        }
        let rec = crate::hash::Record { path: inner.clone(), size: n_size, sha256: crate::hash::hex(&sha2::Digest::finalize(h)) };
        if is_code(&inner) {
            code.insert(match owner { Some(k) => index.mods[k].id.clone(), None => index.layer_id() });
        }
        match owner {
            None => layer_recs.push(rec),
            Some(k) => {
                if index.mods[k].kind == ModKind::Kiln && crate::hash::key_of(&inner).is_some_and(|key| crate::hash::is_cooked_key(&key)) {
                    cooked.entry(k).or_insert(name.clone());
                }
                recs.entry(k).or_default().push(rec);
            }
        }
    }
    for (k, m) in index.mods.iter().enumerate() {
        let rs = recs.remove(&k).unwrap_or_default();
        if rs.is_empty() {
            return Err(err(ErrorCode::Refused, format!("mod {} has no files in the pack", m.id)));
        }
        if let Some(entry) = cooked.get(&k) && rs.iter().any(|r| crate::hash::key_of(&r.path).as_deref() == Some(crate::hash::MOD_MANIFEST)) {
            return Err(err(ErrorCode::Refused, format!("the pack carries cooked files of {} ({entry}): they are made on each machine from its own game", m.id)));
        }
        let have = if m.kind == ModKind::Plugin { rs[0].sha256.clone() } else { crate::hash::of_mod_records(m.kind, rs)? };
        if have != m.hash {
            return Err(err(ErrorCode::HashMismatch, format!("mod {} in the pack hashes to {have}, its index says {}", m.id, m.hash)));
        }
    }
    if let Some(l) = &index.layer {
        if layer_recs.is_empty() {
            return Err(err(ErrorCode::Refused, "the pack layer has no files"));
        }
        let have = crate::hash::of_mod_records(ModKind::Kiln, layer_recs)?;
        if have != l.hash {
            return Err(err(ErrorCode::HashMismatch, format!("the pack layer hashes to {have}, its index says {}", l.hash)));
        }
    }
    for o in &index.overrides {
        if !overrides.contains_key(&(o.side, o.path.clone())) {
            return Err(err(ErrorCode::Refused, format!("override {} is listed but not in the pack", o.path)));
        }
        if o.mode != OverrideMode::Replace {
            let text = crate::manifest::decode_text(&overrides[&(o.side, o.path.clone())]);
            merge::leaves(o.mode, &o.path, &text)?;
        }
    }
    Ok(ReadPack { index, overrides, sha256: have_sha, size, code })
}

#[cfg(not(feature = "zip"))]
pub fn read(zip_file: &Path, sha256: Option<&str>, extract_to: Option<&Path>) -> Result<ReadPack> {
    let _ = (zip_file, sha256, extract_to);
    Err(err(ErrorCode::Unsupported, "kiln-manage was built without the zip feature"))
}

/// `read` without unpacking: the checks only.
pub fn verify(zip_file: &Path, sha256: Option<&str>) -> Result<PackIndex> {
    read(zip_file, sha256, None).map(|r| r.index)
}

// ================================================================================================= building a pack

/// An override to capture from the profile's config folder.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OverrideSpec {
    /// Relative to the profile's configDir.
    pub path: String,
    #[serde(default)]
    pub side: PartSide,
    /// Only these keys (dotted `a.b`; INI `section.key`); empty: the whole file.
    #[serde(default)]
    pub keys: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BuildRequest {
    /// The profile to make the pack from.
    pub profile: String,
    /// The pack's id (default: from the name).
    #[serde(default)]
    pub id: Option<String>,
    /// Default: the profile's name.
    #[serde(default)]
    pub name: Option<String>,
    /// SemVer.
    pub version: String,
    #[serde(default)]
    pub authors: Vec<String>,
    #[serde(default)]
    pub description: Option<String>,
    #[serde(default)]
    pub changes: Option<String>,
    /// Mod id -> its default switch: these mods are optional. A mod switched off in the profile is optional, off.
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    /// Mod id -> what each side needs (default: required on both; a client-only mod: unsupported on servers).
    #[serde(default)]
    pub env: BTreeMap<String, Env>,
    #[serde(default)]
    pub credits: BTreeMap<String, Credit>,
    /// A folder with the pack layer's content (content/*.json, assets/...); its kiln.mod.json is written when absent.
    #[serde(default)]
    pub layer: Option<PathBuf>,
    #[serde(default)]
    pub overrides: Vec<OverrideSpec>,
    /// `{gameSettings: {...}}` for the server part.
    #[serde(default)]
    pub server_settings: Option<Value>,
    #[serde(default)]
    pub game_builds: Vec<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BuiltPack {
    pub file: PathBuf,
    pub sha256: String,
    pub size: u64,
    pub index: PackIndex,
    /// What the build changed or left out (a home path replaced, a layer manifest written).
    pub notes: Vec<String>,
}

/// Key names that hold secrets: the name squashed (letters and digits, lower case) contains one of these...
const SECRET_PARTS: [&str; 14] = ["token", "secret", "password", "passwd", "passphrase", "apikey", "webhook", "bearer", "privatekey", "accesskey",
    "credential", "authorization", "cookie", "sessionid"];
/// ...or one of its words (split at separators and camelCase) is one of these.
const SECRET_WORDS: [&str; 3] = ["pwd", "pass", "auth"];

fn key_words(key: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut prev_lower = false;
    for c in key.chars() {
        if !c.is_alphanumeric() {
            out.push(std::mem::take(&mut cur));
            prev_lower = false;
            continue;
        }
        if c.is_uppercase() && prev_lower {
            out.push(std::mem::take(&mut cur));
        }
        prev_lower = c.is_lowercase() || c.is_ascii_digit();
        cur.extend(c.to_lowercase());
    }
    out.push(cur);
    out.retain(|w| !w.is_empty());
    out
}

/// The secret word a key name holds, if any.
fn secret_key(key: &str) -> Option<&'static str> {
    let squashed: String = key.chars().filter(|c| c.is_alphanumeric()).flat_map(char::to_lowercase).collect();
    SECRET_PARTS.iter().find(|w| squashed.contains(*w)).copied()
        .or_else(|| key_words(key).iter().find_map(|w| SECRET_WORDS.iter().find(|s| *s == w).copied()))
}

/// A value that cannot be a secret: empty, a number, a switch, null, an opening bracket.
fn harmless(v: &str) -> bool {
    v.is_empty() || v.parse::<f64>().is_ok() || matches!(v.to_ascii_lowercase().as_str(), "true" | "false" | "null" | "{" | "[" | "yes" | "no" | "on" | "off")
}

/// Text that is a secret whatever key holds it: a Discord webhook URL or bot token, a Steam id, a GitHub or Slack
/// token, a JWT, a bearer token, a private key.
fn secret_shape(s: &str) -> Option<&'static str> {
    let lower = s.to_ascii_lowercase();
    if (lower.contains("discord.com/api/") || lower.contains("discordapp.com/api/")) && lower.contains("/webhooks/") {
        return Some("a Discord webhook URL");
    }
    let chars: Vec<char> = s.chars().collect();
    if chars.windows(17).any(|w| w.iter().all(char::is_ascii_digit) && w[..7].iter().collect::<String>() == "7656119") {
        return Some("a Steam id");
    }
    if let Some(i) = lower.find("steam_") {
        let parts: Vec<&str> = lower[i + 6..].splitn(3, ':').collect();
        if parts.len() == 3 && parts.iter().all(|p| p.bytes().next().is_some_and(|b| b.is_ascii_digit())) {
            return Some("a Steam id");
        }
    }
    if lower.contains("[u:1:") {
        return Some("a Steam id");
    }
    if lower.contains("-----begin") && lower.contains("private key") {
        return Some("a private key");
    }
    if let Some(i) = lower.find("bearer ") && lower[i + 7..].split_whitespace().next().is_some_and(|t| t.len() >= 16) {
        return Some("a bearer token");
    }
    let b64 = |c: char| c.is_ascii_alphanumeric() || c == '-' || c == '_';
    for word in s.split(|c: char| !(b64(c) || c == '.')) {
        let parts: Vec<&str> = word.split('.').collect();
        if parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.chars().all(b64)) {
            if parts[0].len() >= 23 && parts[1].len() >= 6 && parts[2].len() >= 27 {
                return Some("a Discord bot token");
            }
            if parts[0].starts_with("eyJ") && parts.iter().all(|p| p.len() >= 10) {
                return Some("a JWT");
            }
        }
        let w = word.to_ascii_lowercase();
        if ["ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "xoxb-", "xoxp-", "glpat-"].iter().any(|p| w.starts_with(p)) && w.len() >= 24 {
            return Some("an API token");
        }
    }
    None
}

fn secret_walk(file: &str, v: &Value, path: &mut Vec<String>, out: &mut Vec<String>) {
    let at = |path: &[String]| if path.is_empty() { "(top)".to_string() } else { path.join(".") };
    match v {
        Value::Object(m) => {
            for (k, x) in m {
                path.push(k.clone());
                secret_walk(file, x, path, out);
                path.pop();
            }
        }
        Value::Array(a) => {
            for x in a {
                secret_walk(file, x, path, out);
            }
        }
        Value::String(s) => {
            if let Some(what) = secret_shape(s) {
                out.push(format!("{file} {}: {what}", at(path)));
            } else if let Some(word) = path.last().and_then(|k| secret_key(k)) && !harmless(s.trim()) {
                out.push(format!("{file} {}: looks like a secret ({word})", at(path)));
            }
        }
        Value::Number(n) => {
            if let Some(what) = secret_shape(&n.to_string()) {
                out.push(format!("{file} {}: {what}", at(path)));
            }
        }
        _ => {}
    }
}

/// What in a JSON value looks like a secret (the rules of secret_findings).
pub fn secret_value_findings(file: &str, v: &Value) -> Vec<String> {
    let mut out = Vec::new();
    secret_walk(file, v, &mut Vec::new(), &mut out);
    out
}

/// What in a settings file looks like a secret: a key whose name says token, secret, password, pass, pwd, auth, api
/// key, webhook, bearer, private key, credential, cookie... holding text (numbers, switches and empty values are
/// fine), or text shaped like a secret under any key (Discord webhook URLs and bot tokens, Steam ids, GitHub tokens,
/// JWTs, bearer tokens, private keys). JSON files are read as JSON (by key, whatever the layout), other files by
/// line ("key = value", "key: value"). Words in plain text ("the secret passage") are no finding. A pack never
/// carries what this finds: the creator leaves the key out.
pub fn secret_findings(file: &str, text: &str) -> Vec<String> {
    if file.to_ascii_lowercase().ends_with(".json") && let Ok(tree) = crate::jsonc::parse_tree(text) {
        return secret_value_findings(file, &tree.to_value());
    }
    let mut out = Vec::new();
    for (n, line) in text.lines().enumerate() {
        if let Some(what) = secret_shape(line) {
            out.push(format!("{file} line {}: {what}", n + 1));
            continue;
        }
        let t = line.trim();
        if t.starts_with(['[', '#', ';']) || t.starts_with("//") {
            continue;
        }
        if let Some((k, v)) = t.split_once(['=', ':']) {
            let key = k.trim().trim_matches(['"', '\'']);
            let value = v.trim().trim_end_matches(',').trim().trim_matches(['"', '\'']).trim();
            if let Some(word) = secret_key(key) && !harmless(value) {
                out.push(format!("{file} line {}: looks like a secret ({word})", n + 1));
            }
        }
    }
    out
}

fn home_dirs() -> Vec<String> {
    let mut out = Vec::new();
    for var in ["HOME", "USERPROFILE"] {
        if let Ok(h) = std::env::var(var) {
            let h = h.trim_end_matches(['/', '\\']).to_string();
            if h.len() > 3 {
                out.push(h.replace('\\', "\\\\"));
                out.push(h);
            }
        }
    }
    out
}

/// The text a captured override carries.
fn capture(config_dir: &Path, spec: &OverrideSpec, notes: &mut Vec<String>) -> Result<(OverrideMode, Vec<u8>)> {
    if !override_path_ok(&spec.path) {
        return Err(Error::invalid(format!("{}: only settings files below the config folder can be overrides (not .schema/, not native.json)", spec.path)));
    }
    let p = config_dir.join(&spec.path);
    let md = std::fs::symlink_metadata(&p).map_err(|e| Error::from(e).with_detail(p.display().to_string()))?;
    if !md.is_file() {
        return Err(err(ErrorCode::Refused, format!("{}: not a regular file", spec.path)));
    }
    if md.len() > MAX_OVERRIDE {
        return Err(err(ErrorCode::Refused, format!("{}: larger than 64 KiB", spec.path)));
    }
    let text = crate::manifest::decode_text(&std::fs::read(&p).map_err(crate::util::io_at(&p, "reading"))?);
    let mode = OverrideMode::for_path(&spec.path);
    let mut out = match mode {
        OverrideMode::MergePatch => {
            let v = crate::jsonc::parse_tree(&text).map_err(|e| err(ErrorCode::Parse, format!("{}: {e}", spec.path)))?.to_value();
            if !v.is_object() {
                return Err(err(ErrorCode::Schema, format!("{}: the file must hold one {{ ... }} object", spec.path)));
            }
            let patch = if spec.keys.is_empty() {
                v
            } else {
                let mut patch = Value::Object(Default::default());
                for k in &spec.keys {
                    let segs: Vec<&str> = k.split('.').filter(|s| !s.is_empty()).collect();
                    let mut at = &v;
                    for s in &segs {
                        at = at.get(*s).ok_or_else(|| Error::not_found(format!("{}: no key {k}", spec.path)))?;
                    }
                    let mut dst = &mut patch;
                    for (i, s) in segs.iter().enumerate() {
                        let obj = dst.as_object_mut().expect("object");
                        if i + 1 == segs.len() {
                            obj.insert(s.to_string(), at.clone());
                            break;
                        }
                        dst = obj.entry(s.to_string()).or_insert_with(|| Value::Object(Default::default()));
                        if !dst.is_object() {
                            *dst = Value::Object(Default::default());
                        }
                    }
                }
                patch
            };
            let mut s = serde_json::to_string_pretty(&patch)?;
            s.push('\n');
            s
        }
        OverrideMode::Ini => {
            if spec.keys.is_empty() {
                text
            } else {
                let all = merge::leaves(OverrideMode::Ini, &spec.path, &text)?;
                let mut top = String::new();
                let mut sections: BTreeMap<String, String> = BTreeMap::new();
                for k in &spec.keys {
                    let key: Key = match k.split_once('.') {
                        Some((s, key)) if all.contains_key(&vec![s.to_string(), key.to_string()]) => vec![s.to_string(), key.to_string()],
                        _ => vec![k.clone()],
                    };
                    let Some(Some(v)) = all.get(&key) else { return Err(Error::not_found(format!("{}: no key {k}", spec.path))) };
                    let line = format!("{} = {}\n", key.last().expect("key"), crate::config::ini::text_of(v));
                    if key.len() == 2 { sections.entry(key[0].clone()).or_default().push_str(&line) } else { top.push_str(&line) }
                }
                for (s, lines) in sections {
                    top.push_str(&format!("[{s}]\n{lines}"));
                }
                top
            }
        }
        OverrideMode::Replace => {
            if !spec.keys.is_empty() {
                return Err(Error::invalid(format!("{}: keys can be chosen in .json and .ini files only", spec.path)));
            }
            text
        }
    };
    for h in home_dirs() {
        if out.contains(&h) {
            out = out.replace(&h, "~");
            notes.push(format!("{}: your home folder's path was replaced by ~", spec.path));
        }
    }
    let found = secret_findings(&spec.path, &out);
    if !found.is_empty() {
        return Err(err(ErrorCode::Refused, format!("{} may hold private data ({}); leave those keys out", spec.path, found.join("; "))).with_detail(found));
    }
    Ok((mode, out.into_bytes()))
}

/// The pack layer's folder in `stage`: the creator's content plus a kiln.mod.json (id pack.<id>, the pack's version, no
/// entryAssembly, loadAfter every Kiln mod of the pack).
fn stage_layer(src: &Path, stage: &Path, ix: &PackIndex, kiln_ids: &[String], notes: &mut Vec<String>) -> Result<()> {
    crate::util::copy_tree(src, stage)?;
    let mpath = stage.join(crate::hash::MOD_MANIFEST);
    let ids: Vec<Value> = kiln_ids.iter().map(|s| Value::String(s.clone())).collect();
    if mpath.exists() {
        let m = crate::manifest::read_kiln(stage)?;
        if !m.entry_assembly.is_empty() {
            return Err(err(ErrorCode::Refused, "the pack layer is data only: its kiln.mod.json must not name an entryAssembly"));
        }
        if m.id != ix.layer_id() {
            return Err(err(ErrorCode::Refused, format!("the pack layer's id must be {} (it is {})", ix.layer_id(), m.id)));
        }
        let mut after: Vec<Value> = m.load_after.iter().map(|s| Value::String(s.clone())).collect();
        for v in &ids {
            if !after.contains(v) {
                after.push(v.clone());
            }
        }
        let text = crate::manifest::decode_text(&std::fs::read(&mpath)?);
        let text = crate::jsonc::set_value(&text, &[crate::jsonc::PathSeg::Key("version".into())], &Value::String(ix.version.clone()))?;
        let text = crate::jsonc::set_value(&text, &[crate::jsonc::PathSeg::Key("loadAfter".into())], &Value::Array(after))?;
        std::fs::write(&mpath, text)?;
    } else {
        let m = serde_json::json!({
            "id": ix.layer_id(),
            "name": format!("{} (pack layer)", ix.name),
            "version": ix.version,
            "apiVersion": "0.2",
            "side": "both",
            "clients": "mod",
            "description": "The pack's own data: loads after the pack's mods and combines them.",
            "loadAfter": ids,
        });
        let mut s = serde_json::to_string_pretty(&m)?;
        s.push('\n');
        std::fs::write(&mpath, s)?;
        notes.push(format!("the pack layer's kiln.mod.json was written (id {})", ix.layer_id()));
    }
    crate::manifest::read_kiln(stage).map(|_| ())
}

enum Src {
    Disk(PathBuf),
    Mem(Vec<u8>),
}

/// A pack from a profile: its mods as the Library holds them (each checked against its kdh1), the layer, the chosen
/// overrides (allowlist, size limits, secret scan, home path replaced), the server settings. Deterministic: the same
/// input gives the same bytes. Writes `<out_dir>/<id>-<version>.kilnpack` and checks it with [`read`].
#[cfg(feature = "zip")]
pub fn build(dirs: &DataDirs, req: &BuildRequest, out_dir: &Path, progress: &mut dyn Progress) -> Result<BuiltPack> {
    use std::io::Write;
    let profile = crate::profile::get(dirs, &req.profile)?;
    let name = req.name.clone().unwrap_or_else(|| profile.name.clone()).trim().to_string();
    let id = req.id.clone().unwrap_or_else(|| crate::profile::id_from_name(&name));
    let mut notes = Vec::new();
    let libs: BTreeMap<(ModKind, String), crate::library::LibraryEntry> =
        crate::library::list(dirs, None)?.into_iter().map(|e| ((e.kind, e.id.clone()), e)).collect();
    let mut entries: Vec<(String, Src)> = Vec::new();
    let mut mods = Vec::new();
    let mut newest = 315_532_800i64;
    let mut stamp = |p: &Path| {
        if let Ok(t) = std::fs::metadata(p).and_then(|m| m.modified()) {
            newest = newest.max(crate::util::unix(t));
        }
    };
    let total: usize = ModKind::ALL.iter().map(|k| profile.mods.list(*k).len()).sum::<usize>().max(1);
    let mut done = 0usize;
    for kind in ModKind::ALL {
        for m in profile.mods.list(kind) {
            if progress.canceled() {
                return Err(err(ErrorCode::Canceled, "canceled"));
            }
            progress.report(Some(done as f32 / total as f32 * 0.5), &format!("hashing {}", m.id));
            done += 1;
            let r = LibraryRef { kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
            let at = crate::library::path_of(dirs, &r)?;
            let (path, hash) = if kind == ModKind::Plugin {
                let file = at.file_name().and_then(|n| n.to_str()).ok_or_else(|| Error::invalid(format!("plugin {}: no file name", m.id)))?.to_string();
                let zp = format!("{MODS_DIR}/plugin/{file}");
                stamp(&at);
                entries.push((zp.clone(), Src::Disk(at.clone())));
                (zp, crate::hash::sha256_file(&at)?.0)
            } else {
                let files = super::mod_files(kind, &at)?;
                let hash = crate::hash::of_mod_records(kind, records(&files)?)?;
                let zp = mod_path(kind, &m.id);
                for (rel, p) in files {
                    stamp(&p);
                    entries.push((format!("{zp}/{rel}"), Src::Disk(p)));
                }
                (zp, hash)
            };
            if hash != m.hash {
                return Err(err(ErrorCode::Refused, format!(
                    "{} {} changed in its Library folder since it was imported ({hash}, the profile names {}): take it over first (Library, or kilnctl library take-over)",
                    kind.as_str(), m.id, m.hash)));
            }
            let lib = libs.get(&(kind, m.id.clone()));
            let side = lib.and_then(|e| e.versions.iter().find(|v| v.hash == m.hash)).and_then(|v| v.side);
            let (dep, default) = match req.optional.get(&m.id) {
                Some(on) => (Dep::Optional, Some(*on)),
                None if !m.enabled => (Dep::Optional, Some(false)),
                None => (Dep::Required, None),
            };
            let env = req.env.get(&m.id).copied().or_else(|| (side == Some(Side::Client)).then_some(Env { client: Need::Required, server: Need::Unsupported }));
            mods.push(PackMod {
                kind,
                id: m.id.clone(),
                version: m.version.clone(),
                name: lib.map(|e| e.name.clone()).filter(|n| !n.is_empty() && *n != m.id),
                path,
                hash,
                dep,
                default,
                env,
                file: None,
                credit: req.credits.get(&m.id).cloned(),
            });
        }
    }
    for id in req.optional.keys().chain(req.env.keys()).chain(req.credits.keys()) {
        if !mods.iter().any(|m| &m.id == id) {
            return Err(Error::invalid(format!("{id} is not a mod of profile {}", profile.id)));
        }
    }
    if let Some(s) = &req.server_settings {
        let found = secret_value_findings("server settings", s);
        if !found.is_empty() {
            return Err(err(ErrorCode::Refused, format!("the server settings may hold private data ({}); leave those keys out", found.join("; "))).with_detail(found));
        }
    }
    let kiln_ids: Vec<String> = mods.iter().filter(|m| m.kind == ModKind::Kiln).map(|m| m.id.clone()).collect();
    let mut index = PackIndex {
        format: FORMAT,
        kind: KIND.into(),
        id,
        name,
        version: req.version.trim().to_string(),
        created: None,
        authors: req.authors.clone(),
        description: req.description.clone().filter(|s| !s.trim().is_empty()),
        game: (!req.game_builds.is_empty()).then(|| PackGame { builds: req.game_builds.clone() }),
        kiln: Some(format!(">={}", crate::VERSION)),
        api: Some(format!(">={}", crate::profile::KILN_API)),
        crucible: None,
        mods,
        layer: None,
        order: PackOrder { kiln: kiln_ids.clone(), late: profile.late.clone() },
        dropins: Vec::new(),
        overrides: Vec::new(),
        server: req.server_settings.clone().map(|s| PackServer { settings: Some(s), notes: None }),
        changes: req.changes.clone().filter(|s| !s.trim().is_empty()),
    };
    // the layer, staged with its manifest
    std::fs::create_dir_all(out_dir).map_err(crate::util::io_at(out_dir, "creating"))?;
    let stage = crate::library::TempDir::new(out_dir, "kilnpack-layer")?;
    if let Some(src) = &req.layer {
        if !semver_ok(&index.version) || !mod_id_ok(&index.id) {
            check_index(&index)?;
        }
        let dir = stage.path().join(LAYER_PATH);
        stage_layer(src, &dir, &index, &kiln_ids, &mut notes)?;
        let files = super::mod_files(ModKind::Kiln, &dir)?;
        // the layer's settings-like files get the overrides' secret scan
        let mut found = Vec::new();
        for (rel, p) in &files {
            if crate::settings_files::is_settings_extension(rel) && std::fs::metadata(p).is_ok_and(|m| m.len() <= 4 << 20) {
                found.extend(secret_findings(&format!("layer/{rel}"), &crate::manifest::decode_text(&std::fs::read(p)?)));
            }
        }
        if !found.is_empty() {
            return Err(err(ErrorCode::Refused, format!("the pack layer may hold private data ({}); take it out", found.join("; "))).with_detail(found));
        }
        let hash = crate::hash::of_mod_records(ModKind::Kiln, records(&files)?)?;
        for (rel, p) in files {
            entries.push((format!("{LAYER_PATH}/{rel}"), Src::Disk(p)));
        }
        index.order.kiln.push(index.layer_id());
        index.layer = Some(PackLayer { id: index.layer_id(), version: index.version.clone(), path: LAYER_PATH.into(), hash, file: None });
    }
    // overrides
    let config_dir = dirs.profile(&profile.id).join("config");
    let mut total_override = 0u64;
    for spec in &req.overrides {
        let (mode, bytes) = capture(&config_dir, spec, &mut notes)?;
        total_override += bytes.len() as u64;
        if bytes.len() as u64 > MAX_OVERRIDE || total_override > MAX_OVERRIDES {
            return Err(err(ErrorCode::Refused, "overrides: 64 KiB a file, 1 MiB in all"));
        }
        let o = PackOverride { path: spec.path.clone(), mode, side: spec.side, sha256: crate::hash::sha256_hex(&bytes), size: Some(bytes.len() as u64) };
        entries.push((o.zip_path(), Src::Mem(bytes)));
        index.overrides.push(o);
    }
    index.created = Some(crate::util::rfc3339(newest));
    check_index(&index)?;
    let mut index_bytes = serde_json::to_vec_pretty(&index)?;
    index_bytes.push(b'\n');
    let file = out_dir.join(format!("{}-{}.{EXTENSION}", index.id, index.version));
    let tmp = crate::util::with_suffix(&file, ".tmp");
    let time = {
        let (y, mo, d, h, mi, s) = crate::util::utc_secs(newest);
        zip::DateTime::from_date_and_time(y.min(2107) as u16, mo as u8, d as u8, h as u8, mi as u8, s as u8).unwrap_or_else(|_| zip::DateTime::default_for_write())
    };
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated)
        .compression_level(Some(6))
        .last_modified_time(time)
        .unix_permissions(0o644)
        .system(zip::System::Unix)
        .large_file(false);
    entries.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    {
        let f = std::fs::File::create(&tmp).map_err(crate::util::io_at(&tmp, "writing"))?;
        let mut w = zip::ZipWriter::new(std::io::BufWriter::new(f));
        let zerr = |e: zip::result::ZipError| err(ErrorCode::Io, format!("writing {}: {e}", tmp.display()));
        w.start_file(super::MANIFEST, opts).map_err(zerr)?;
        w.write_all(&index_bytes)?;
        let n = entries.len().max(1);
        for (i, (name, src)) in entries.iter().enumerate() {
            if progress.canceled() {
                drop(w);
                let _ = std::fs::remove_file(&tmp);
                return Err(err(ErrorCode::Canceled, "canceled"));
            }
            let data = match src {
                Src::Disk(p) => std::fs::read(p).map_err(crate::util::io_at(p, "reading"))?,
                Src::Mem(b) => b.clone(),
            };
            let o = if data.len() as u64 >= 0xFFFF_0000 { opts.large_file(true) } else { opts };
            w.start_file(name.as_str(), o).map_err(zerr)?;
            w.write_all(&data)?;
            if i % 16 == 0 {
                progress.report(Some(0.5 + i as f32 / n as f32 * 0.45), &format!("packing {name}"));
            }
        }
        let mut inner = w.finish().map_err(zerr)?;
        inner.flush()?;
        inner.into_inner().map_err(|e| Error::from(e.into_error()))?.sync_all()?;
    }
    drop(stage);
    crate::util::rename_replace(&tmp, &file)?;
    progress.report(Some(0.96), "checking the pack");
    let checked = read(&file, None, None)?;
    progress.report(Some(1.0), &format!("built {} ({} mods, {} bytes)", file.display(), checked.index.mods.len(), checked.size));
    Ok(BuiltPack { file, sha256: checked.sha256, size: checked.size, index: checked.index, notes })
}

#[cfg(not(feature = "zip"))]
pub fn build(dirs: &DataDirs, req: &BuildRequest, out_dir: &Path, progress: &mut dyn Progress) -> Result<BuiltPack> {
    let _ = (dirs, req, out_dir, progress);
    Err(err(ErrorCode::Unsupported, "kiln-manage was built without the zip feature"))
}

// ================================================================================================= preview

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackPreview {
    pub index: PackIndex,
    pub sha256: String,
    pub size: u64,
    /// Mods (client side) and the layer the Library lacks: what an install imports.
    pub missing: Vec<LibraryRef>,
    pub present: Vec<LibraryRef>,
    /// Mods with code (native, plugin, a Kiln mod with an entryAssembly, any other engine's mod): the warning says the
    /// pack runs code from other people either way.
    pub code: Vec<String>,
    /// Optional mods with their default switch.
    pub optional: BTreeMap<String, bool>,
    /// The instance this pack would update (same pack id), if one is installed.
    pub instance: Option<String>,
}

/// What the install preview shows (nothing is imported).
pub fn inspect(dirs: &DataDirs, zip_file: &Path, sha256: Option<&str>) -> Result<PackPreview> {
    let r = read(zip_file, sha256, None)?;
    let mut missing = Vec::new();
    let mut present = Vec::new();
    let have: BTreeSet<(ModKind, String, String)> = crate::library::list(dirs, None)?.into_iter()
        .flat_map(|e| e.versions.into_iter().map(move |v| (e.kind, e.id.clone(), v.hash))).collect();
    let layer = r.index.layer.as_ref().map(|l| LibraryRef { kind: ModKind::Kiln, id: l.id.clone(), version: l.version.clone(), hash: l.hash.clone() });
    for lr in r.index.mods.iter().filter(|m| m.env().client != Need::Unsupported).map(PackMod::library_ref).chain(layer) {
        if have.contains(&(lr.kind, lr.id.clone(), lr.hash.clone())) { present.push(lr) } else { missing.push(lr) }
    }
    let code = r.code.iter().cloned().collect::<Vec<_>>();
    let optional = r.index.mods.iter().filter(|m| m.dep == Dep::Optional).map(|m| (m.id.clone(), m.default.unwrap_or(true))).collect();
    let instance = instances(dirs)?.into_iter().find(|(_, i)| i.pack == r.index.id).map(|(p, _)| p);
    Ok(PackPreview { index: r.index, sha256: r.sha256, size: r.size, missing, present, code, optional, instance })
}

// ================================================================================================= instances

/// `profiles/<pid>/pack/instance.json`: which pack a profile is an instance of.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Instance {
    pub format: u32,
    /// The pack's id.
    pub pack: String,
    pub name: String,
    pub version: String,
    /// Of the pack file installed.
    pub sha256: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub crucible: Option<CrucibleRef>,
    /// The player's switches of optional mods.
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    pub installed: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub updated: Option<String>,
    /// The version before the last update (Undo goes back to it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub previous: Option<String>,
}

fn pack_dir(dirs: &DataDirs, pid: &str) -> PathBuf {
    dirs.profile(pid).join(INSTANCE_DIR)
}

/// The instance record of a profile (None: the profile is no pack's instance).
pub fn instance(dirs: &DataDirs, pid: &str) -> Result<Option<Instance>> {
    crate::profile::get(dirs, pid)?;
    crate::util::read_json(&pack_dir(dirs, pid).join(INSTANCE_FILE))
}

/// Every instance: (profile id, record).
pub fn instances(dirs: &DataDirs) -> Result<Vec<(String, Instance)>> {
    let mut out = Vec::new();
    for p in crate::profile::list(dirs)? {
        if let Ok(Some(i)) = crate::util::read_json::<Instance>(&pack_dir(dirs, &p.id).join(INSTANCE_FILE)) {
            out.push((p.id, i));
        }
    }
    Ok(out)
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallRequest {
    pub source: ModpackSource,
    /// Must be true: the player agreed (the warning: the pack runs code from other people).
    pub consent: bool,
    #[serde(default)]
    pub allow_http: bool,
    /// The profile's name (default: the pack's).
    #[serde(default)]
    pub name: Option<String>,
    /// The player's switches of optional mods (default: each mod's default).
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    /// Back up the saves and activate the instance for the game (default true when a game is given).
    #[serde(default)]
    pub activate: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallResult {
    pub profile: Profile,
    pub instance: Instance,
    pub imported: Vec<LibraryRef>,
    /// Config files the pack's overrides wrote.
    pub overrides: Vec<String>,
    pub backup: Option<crate::saves::SaveBackup>,
    pub activation: Option<crate::profile::Activation>,
}

/// The pack file of a source: a download (cached by sha256) or a local file.
fn fetch(dirs: &DataDirs, source: &ModpackSource, allow_http: bool, progress: &mut dyn Progress) -> Result<(PathBuf, Option<String>, Option<String>)> {
    match source {
        ModpackSource::Url { url, sha256 } => {
            let dest = dirs.root.join("modpacks").join("downloads").join(format!("{}.{EXTENSION}", sha256.to_ascii_lowercase()));
            let cached = dest.exists() && crate::hash::sha256_file(&dest).map(|(h, _)| h.eq_ignore_ascii_case(sha256)).unwrap_or(false);
            if !cached {
                super::download(url, sha256, &dest, allow_http, &mut Scaled { inner: progress, from: 0.0, to: 0.5 })?;
            }
            Ok((dest, Some(sha256.clone()), Some(url.clone())))
        }
        ModpackSource::Path { path, sha256 } => Ok((path.clone(), sha256.clone(), None)),
    }
}

/// Unpacks a pack into a staging folder and imports the mods (those `side` uses) and the layer into the Library.
fn import_pack(dirs: &DataDirs, zip_file: &Path, sha256: Option<&str>, use_mod: &dyn Fn(&PackMod) -> bool, progress: &mut dyn Progress)
    -> Result<(ReadPack, Vec<LibraryRef>)> {
    let staging = dirs.root.join("modpacks").join("staging").join(crate::hash::hex(&crate::util::random_bytes::<6>()?));
    let result = (|| {
        let r = read(zip_file, sha256, Some(&staging))?;
        let mut paths = Vec::new();
        let mut want: Vec<LibraryRef> = Vec::new();
        for m in r.index.mods.iter().filter(|m| use_mod(m)) {
            paths.push(staging.join(&m.path));
            want.push(m.library_ref());
        }
        if let Some(l) = &r.index.layer {
            paths.push(staging.join(&l.path));
            want.push(LibraryRef { kind: ModKind::Kiln, id: l.id.clone(), version: l.version.clone(), hash: l.hash.clone() });
        }
        progress.report(Some(0.6), "importing into the library");
        let report = crate::library::import(dirs, &paths, &mut Scaled { inner: progress, from: 0.6, to: 0.8 })?;
        let refs: Vec<&LibraryRef> = report.imported.iter().chain(&report.existing).collect();
        for w in &want {
            if !refs.iter().any(|r| r.kind == w.kind && r.id == w.id && r.hash == w.hash) {
                return Err(err(ErrorCode::Internal, format!("the library did not take {} {} ({})", w.kind.as_str(), w.id, w.hash))
                    .with_detail(serde_json::to_value(&report.skipped).unwrap_or_default()));
            }
        }
        Ok((r, report.imported))
    })();
    let _ = std::fs::remove_dir_all(&staging);
    result
}

/// The profile's mod lists for a pack: its client mods (optional ones as the player chose) in the pack's order, the
/// layer last among the pack's Kiln mods.
fn pack_mods(ix: &PackIndex, choices: &BTreeMap<String, bool>) -> crate::profile::ProfileMods {
    let mut mods = crate::profile::ProfileMods::default();
    let rank = |id: &str| ix.order.kiln.iter().position(|x| x == id).unwrap_or(usize::MAX);
    let mut list: Vec<&PackMod> = ix.mods.iter().filter(|m| m.env().client != Need::Unsupported).collect();
    list.sort_by_key(|m| if m.kind == ModKind::Kiln { rank(&m.id) } else { 0 });
    for m in list {
        mods.list_mut(m.kind).push(ProfileMod { id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone(), enabled: m.on(choices) });
    }
    if let Some(l) = &ix.layer {
        mods.kiln.push(ProfileMod { id: l.id.clone(), version: l.version.clone(), hash: l.hash.clone(), enabled: true });
    }
    mods
}

/// Writes the overrides of a side into a config folder (each a fresh merge onto what is there): the files written.
fn apply_overrides(config_dir: &Path, r: &ReadPack, side_ok: &dyn Fn(PartSide) -> bool) -> Result<Vec<String>> {
    let mut written = Vec::new();
    // the both-sides override first, the side's own on top of it, whatever order the index lists them in
    let mut list: Vec<&PackOverride> = r.index.overrides.iter().filter(|o| side_ok(o.side)).collect();
    list.sort_by_key(|o| o.side != PartSide::Both);
    for o in list {
        let text = r.override_text(o.side, &o.path).unwrap_or_default();
        let leaves = merge::leaves(o.mode, &o.path, &text)?;
        let target = config_dir.join(&o.path);
        let live = std::fs::read(&target).ok().map(|b| crate::manifest::decode_text(&b));
        let edits: Vec<(Key, Option<Value>)> = leaves.into_iter().collect();
        let out = merge::apply(o.mode, &o.path, live.as_deref(), &edits)?;
        if let Some(p) = target.parent() {
            std::fs::create_dir_all(p).map_err(crate::util::io_at(p, "creating"))?;
        }
        crate::util::write_atomic(&target, out.as_bytes())?;
        written.push(o.path.clone());
    }
    Ok(written)
}

/// `profiles/<pid>/pack/base/`: the index and the overrides of the version installed (the merge base of the next update).
fn write_base(dirs: &DataDirs, pid: &str, r: &ReadPack) -> Result<()> {
    let base = pack_dir(dirs, pid).join("base");
    let tmp = pack_dir(dirs, pid).join("base.tmp");
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(crate::util::io_at(&tmp, "creating"))?;
    crate::util::write_json_atomic(&tmp.join(super::MANIFEST), &r.index)?;
    for ((side, path), bytes) in &r.overrides {
        let p = tmp.join(side.dir()).join(path);
        if let Some(d) = p.parent() {
            std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
        }
        std::fs::write(&p, bytes).map_err(crate::util::io_at(&p, "writing"))?;
    }
    swap_in(&tmp, &base)
}

/// Moves a staged folder into place: the old one moves aside to <dest>.old first and comes back if the new one cannot
/// take its place, so there is always one (read_base falls back to base.old).
fn swap_in(staged: &Path, dest: &Path) -> Result<()> {
    let old = crate::util::with_suffix(dest, ".old");
    let _ = std::fs::remove_dir_all(&old);
    if dest.exists() {
        std::fs::rename(dest, &old).map_err(crate::util::io_at(dest, "moving"))?;
    }
    if let Err(e) = std::fs::rename(staged, dest) {
        let _ = std::fs::rename(&old, dest);
        return Err(crate::util::io_at(dest, "moving")(e));
    }
    let _ = std::fs::remove_dir_all(&old);
    Ok(())
}

/// The installed version (index + overrides) of an instance.
fn read_base(dirs: &DataDirs, pid: &str) -> Result<(PackIndex, BTreeMap<(PartSide, String), Vec<u8>>)> {
    let mut base = pack_dir(dirs, pid).join("base");
    // a write_base cut off between its two renames left the installed version in base.old
    if !base.exists() && pack_dir(dirs, pid).join("base.old").join(super::MANIFEST).is_file() {
        base = pack_dir(dirs, pid).join("base.old");
    }
    let ix: PackIndex = crate::util::read_json(&base.join(super::MANIFEST))?
        .ok_or_else(|| Error::not_found(format!("profile {pid} has no pack base (pack/base/{})", super::MANIFEST)))?;
    let mut ov = BTreeMap::new();
    for o in &ix.overrides {
        let p = base.join(o.side.dir()).join(&o.path);
        ov.insert((o.side, o.path.clone()), std::fs::read(&p).map_err(crate::util::io_at(&p, "reading"))?);
    }
    Ok((ix, ov))
}

/// Installs a pack as an instance: consent -> the file (download or local) -> every check of [`read`] -> the client
/// mods and the layer into the Library -> a new profile with exactly them (optional mods as the player chose, no
/// drop-in folders) -> the pack's overrides into its config folder -> the base copy and instance.json -> with a game
/// (unless `activate: false`): a save backup and activation. Nothing is fetched without `consent: true`.
pub fn install(dirs: &DataDirs, game: Option<&GameInstall>, req: &InstallRequest, progress: &mut dyn Progress) -> Result<InstallResult> {
    if !req.consent {
        return Err(err(ErrorCode::ConsentRequired, "a pack runs code from other people on this PC: the player must agree first"));
    }
    let (zip_file, sha, url) = fetch(dirs, &req.source, req.allow_http, progress)?;
    progress.report(Some(0.52), "checking the pack");
    let (r, imported) = import_pack(dirs, &zip_file, sha.as_deref(), &|m| m.env().client != Need::Unsupported, progress)?;
    for id in req.optional.keys() {
        if !r.index.mods.iter().any(|m| &m.id == id && m.dep == Dep::Optional) {
            return Err(Error::invalid(format!("{id} is not an optional mod of this pack")));
        }
    }
    progress.report(Some(0.82), "making the instance");
    let name = req.name.clone().unwrap_or_else(|| r.index.name.clone());
    let mut profile = crate::profile::create(dirs, &name, None)?;
    let pid = profile.id.clone();
    let made = (|| -> Result<(Profile, Instance, Vec<String>)> {
        profile.mods = pack_mods(&r.index, &req.optional);
        profile.late = r.index.order.late.clone();
        profile.dropins = r.index.dropins.clone();
        profile.dropins_engines = true;
        profile.notes = format!("Crucible pack {} {}", r.index.name, r.index.version);
        let profile = crate::profile::save(dirs, &profile)?;
        let written = apply_overrides(&dirs.profile(&pid).join("config"), &r, &|s| s.for_client())?;
        write_base(dirs, &pid, &r)?;
        let inst = Instance {
            format: 1,
            pack: r.index.id.clone(),
            name: r.index.name.clone(),
            version: r.index.version.clone(),
            sha256: r.sha256.clone(),
            url: url.clone(),
            crucible: r.index.crucible.clone(),
            optional: r.index.mods.iter().filter(|m| m.dep == Dep::Optional).map(|m| (m.id.clone(), m.on(&req.optional))).collect(),
            installed: crate::util::now_rfc3339(),
            updated: None,
            previous: None,
        };
        crate::util::write_json_atomic(&pack_dir(dirs, &pid).join(INSTANCE_FILE), &inst)?;
        Ok((profile, inst, written))
    })();
    let (profile, instance, overrides) = match made {
        Ok(x) => x,
        Err(e) => {
            let _ = crate::profile::delete(dirs, &pid);
            return Err(e);
        }
    };
    let (mut backup, mut activation) = (None, None);
    if let Some(game) = game && req.activate != Some(false) {
        progress.report(Some(0.9), "backing up saves");
        backup = Some(crate::saves::backup(dirs, game, crate::saves::BackupReason::Modpack, Some(&format!("before pack {} {}", r.index.name, r.index.version)))?);
        progress.report(Some(0.95), "activating the instance");
        activation = Some(crate::profile::activate(dirs, &pid, game)?);
    }
    progress.report(Some(1.0), &format!("pack {} {} installed as profile {}", r.index.name, r.index.version, profile.name));
    Ok(InstallResult { profile, instance, imported, overrides, backup, activation })
}

// ================================================================================================= updates

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanMod {
    pub kind: ModKind,
    pub id: String,
    pub version: String,
    pub enabled: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanUpdate {
    pub kind: ModKind,
    pub id: String,
    pub from: String,
    pub to: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModChanges {
    pub added: Vec<PlanMod>,
    pub updated: Vec<PlanUpdate>,
    /// Leave the instance; they stay in the Library.
    pub removed: Vec<PlanMod>,
    /// Mods the player added to the instance: kept, after the pack layer.
    pub yours: Vec<PlanMod>,
    /// Player switches kept, in words.
    pub kept_switches: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LayerChange {
    pub from: Option<String>,
    pub to: Option<String>,
    pub changed: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileMerge {
    pub path: String,
    pub side: PartSide,
    pub mode: OverrideMode,
    /// Keys the pack changed and the player had not: updated.
    pub taken: Vec<String>,
    /// Keys the player changed: kept.
    pub kept: Vec<String>,
    /// Keys both changed differently: the player picks (`resolve`).
    pub conflicts: Vec<KeyConflict>,
}

/// What an update does (the "See changes" view), before anything is written.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdatePlan {
    pub profile: String,
    pub pack: String,
    pub from: String,
    pub to: String,
    pub changes: Option<String>,
    pub mods: ModChanges,
    pub layer: LayerChange,
    pub files: Vec<FileMerge>,
    /// How many conflicts need a choice.
    pub conflicts: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateRequest {
    pub profile: String,
    pub source: ModpackSource,
    pub consent: bool,
    #[serde(default)]
    pub allow_http: bool,
    /// Switches for optional mods new in this version (default: their default).
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    /// `<path>#<key>` -> pack | mine, per conflict (key as in the plan: `a.b`, `section.key`, `(whole file)`).
    #[serde(default)]
    pub resolve: BTreeMap<String, Pick>,
    /// Every conflict without its own choice.
    #[serde(default)]
    pub all: Option<Pick>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateResult {
    pub plan: UpdatePlan,
    pub profile: Profile,
    pub instance: Instance,
    pub imported: Vec<LibraryRef>,
}

struct Computed {
    plan: UpdatePlan,
    mods: crate::profile::ProfileMods,
    /// (path, mode, take edits, conflicts) per client-side file.
    files: Vec<(String, OverrideMode, Vec<(Key, Option<Value>)>, Vec<(Key, KeyConflict)>)>,
}

fn side_leaves(ix: &PackIndex, ov: &BTreeMap<(PartSide, String), Vec<u8>>, path: &str) -> Result<Option<(OverrideMode, Leaves)>> {
    // a client instance: the both-sides override, then the client one on top
    let mut out: Option<(OverrideMode, Leaves)> = None;
    for side in [PartSide::Both, PartSide::Client] {
        if let Some(o) = ix.overrides.iter().find(|o| o.side == side && o.path == path) {
            let text = ov.get(&(side, path.to_string())).map(|b| crate::manifest::decode_text(b)).unwrap_or_default();
            let l = merge::leaves(o.mode, path, &text)?;
            match out.as_mut() {
                Some((_, acc)) => acc.extend(l),
                None => out = Some((o.mode, l)),
            }
        }
    }
    Ok(out)
}

fn compute(dirs: &DataDirs, profile: &Profile, inst: &Instance, new: &ReadPack, new_choices: &BTreeMap<String, bool>) -> Result<Computed> {
    let (base, base_ov) = read_base(dirs, &profile.id)?;
    let theirs = &new.index;
    let mut changes = ModChanges::default();
    // mods
    let in_base = |k: ModKind, id: &str| base.mods.iter().any(|m| m.kind == k && m.id == id && m.env().client != Need::Unsupported);
    let layer_ids: BTreeSet<String> = [base.layer.as_ref().map(|l| l.id.clone()), theirs.layer.as_ref().map(|l| l.id.clone())].into_iter().flatten().collect();
    let mut choices = inst.optional.clone();
    for (k, v) in new_choices {
        choices.insert(k.clone(), *v);
    }
    let mut mods = pack_mods(theirs, &choices);
    for kind in ModKind::ALL {
        let mine = profile.mods.list(kind);
        let list = mods.list_mut(kind);
        // the player's switches on mods the pack had before
        for e in list.iter_mut() {
            if let Some(m) = mine.iter().find(|m| m.id == e.id) && in_base(kind, &e.id) && m.enabled != e.enabled && !layer_ids.contains(&e.id) {
                changes.kept_switches.push(format!("{}: {} (your switch)", e.id, if m.enabled { "on" } else { "off" }));
                e.enabled = m.enabled;
            }
            match mine.iter().find(|m| m.id == e.id) {
                Some(m) if m.version != e.version || m.hash != e.hash => {
                    if !layer_ids.contains(&e.id) {
                        changes.updated.push(PlanUpdate { kind, id: e.id.clone(), from: m.version.clone(), to: e.version.clone() });
                    }
                }
                Some(_) => {}
                None if !layer_ids.contains(&e.id) => changes.added.push(PlanMod { kind, id: e.id.clone(), version: e.version.clone(), enabled: e.enabled }),
                None => {}
            }
        }
        for m in mine {
            if list.iter().any(|e| e.id == m.id) {
                continue;
            }
            if in_base(kind, &m.id) || layer_ids.contains(&m.id) {
                if !layer_ids.contains(&m.id) {
                    changes.removed.push(PlanMod { kind, id: m.id.clone(), version: m.version.clone(), enabled: m.enabled });
                }
            } else {
                // the player's own: kept, after the pack's
                changes.yours.push(PlanMod { kind, id: m.id.clone(), version: m.version.clone(), enabled: m.enabled });
                list.push(m.clone());
            }
        }
    }
    // config files (client side), base U theirs
    let config_dir = dirs.profile(&profile.id).join("config");
    let paths: BTreeSet<String> = base.overrides.iter().chain(&theirs.overrides).filter(|o| o.side.for_client()).map(|o| o.path.clone()).collect();
    let mut files = Vec::new();
    let mut merges = Vec::new();
    let mut conflicts = 0usize;
    for path in paths {
        let b = side_leaves(&base, &base_ov, &path)?;
        let t = side_leaves(theirs, &new.overrides, &path)?;
        let mode = t.as_ref().or(b.as_ref()).map(|x| x.0).unwrap_or(OverrideMode::for_path(&path));
        let live = std::fs::read(config_dir.join(&path)).ok().map(|b| crate::manifest::decode_text(&b));
        let mine = |k: &[String]| merge::value_at(mode, live.as_deref(), k);
        let m = merge::three_way(b.as_ref().map(|x| &x.1), t.as_ref().map(|x| &x.1), &mine);
        conflicts += m.conflicts.len();
        let fm = FileMerge {
            path: path.clone(),
            side: PartSide::Client,
            mode,
            taken: m.taken.clone(),
            kept: m.kept.clone(),
            conflicts: m.conflicts.iter().map(|(_, c)| c.clone()).collect(),
        };
        if !(fm.taken.is_empty() && fm.kept.is_empty() && fm.conflicts.is_empty()) {
            merges.push(fm);
        }
        files.push((path, mode, m.take, m.conflicts));
    }
    let layer = LayerChange {
        from: base.layer.as_ref().map(|l| l.hash.clone()),
        to: theirs.layer.as_ref().map(|l| l.hash.clone()),
        changed: base.layer.as_ref().map(|l| &l.hash) != theirs.layer.as_ref().map(|l| &l.hash),
    };
    let plan = UpdatePlan {
        profile: profile.id.clone(),
        pack: theirs.id.clone(),
        from: inst.version.clone(),
        to: theirs.version.clone(),
        changes: theirs.changes.clone(),
        mods: changes,
        layer,
        files: merges,
        conflicts,
    };
    Ok(Computed { plan, mods, files })
}

fn same_pack(inst: &Instance, ix: &PackIndex) -> Result<()> {
    if inst.pack != ix.id {
        return Err(err(ErrorCode::Refused, format!("this is pack {}, the instance is of pack {}", ix.id, inst.pack)));
    }
    Ok(())
}

/// What updating an instance to a pack file would do (nothing is written or imported).
pub fn plan_update(dirs: &DataDirs, pid: &str, zip_file: &Path, sha256: Option<&str>, optional: &BTreeMap<String, bool>) -> Result<UpdatePlan> {
    let profile = crate::profile::get(dirs, pid)?;
    let inst = instance(dirs, pid)?.ok_or_else(|| Error::not_found(format!("profile {pid} is not a Crucible instance")))?;
    let r = read(zip_file, sha256, None)?;
    same_pack(&inst, &r.index)?;
    Ok(compute(dirs, &profile, &inst, &r, optional)?.plan)
}

fn conflict_key(path: &str, key: &str) -> String {
    format!("{path}#{key}")
}

/// Updates an instance: the plan; refused (`conflict`, detail = the plan) while a conflict has no choice; then the new mods
/// and layer into the Library, an undo snapshot (`pack/undo/`: meta.json, config/, instance.json, base/), the profile's
/// mods (the pack's, the player's switches and own mods kept), each config file merged key by key, the new base.
pub fn update(dirs: &DataDirs, req: &UpdateRequest, progress: &mut dyn Progress) -> Result<UpdateResult> {
    if !req.consent {
        return Err(err(ErrorCode::ConsentRequired, "a pack runs code from other people on this PC: the player must agree first"));
    }
    let profile = crate::profile::get(dirs, &req.profile)?;
    let inst = instance(dirs, &req.profile)?.ok_or_else(|| Error::not_found(format!("profile {} is not a Crucible instance", req.profile)))?;
    let (zip_file, sha, url) = fetch(dirs, &req.source, req.allow_http, progress)?;
    progress.report(Some(0.5), "checking the pack");
    let r = read(&zip_file, sha.as_deref(), None)?;
    same_pack(&inst, &r.index)?;
    let c = compute(dirs, &profile, &inst, &r, &req.optional)?;
    // every conflict needs a choice before anything changes
    let mut unresolved = Vec::new();
    for (path, _, _, conflicts) in &c.files {
        for (_, k) in conflicts {
            if req.resolve.get(&conflict_key(path, &k.key)).or(req.all.as_ref()).is_none() {
                unresolved.push(conflict_key(path, &k.key));
            }
        }
    }
    if !unresolved.is_empty() {
        return Err(err(ErrorCode::Conflict, format!("{} setting(s) changed by both the pack and you need a choice: {}", unresolved.len(), unresolved.join(", ")))
            .with_detail(serde_json::to_value(&c.plan).unwrap_or_default()));
    }
    // every merge first: a file that cannot be merged (a live file that does not parse) stops the update here, before
    // anything is imported or written
    let config_dir = dirs.profile(&profile.id).join("config");
    let mut writes: Vec<(PathBuf, String)> = Vec::new();
    for (path, mode, take, conflicts) in &c.files {
        let mut edits = take.clone();
        for (key, k) in conflicts {
            let pick = req.resolve.get(&conflict_key(path, &k.key)).or(req.all.as_ref()).copied().unwrap_or(Pick::Mine);
            if pick == Pick::Pack {
                edits.push((key.clone(), k.pack.clone()));
            }
        }
        if edits.is_empty() {
            continue;
        }
        let target = config_dir.join(path);
        let live = std::fs::read(&target).ok().map(|b| crate::manifest::decode_text(&b));
        writes.push((target, merge::apply(*mode, path, live.as_deref(), &edits)?));
    }
    let (_, imported) = import_pack(dirs, &zip_file, sha.as_deref(), &|m| m.env().client != Need::Unsupported, progress)?;
    progress.report(Some(0.85), "keeping an undo copy");
    // the instance as it is now, staged: it becomes the undo copy once the update stands, and puts the instance back if
    // a step fails (a profile changed meanwhile, a file that cannot be written), so a failed update changes nothing
    let pd = pack_dir(dirs, &profile.id);
    let staged = pd.join("undo.new");
    snapshot_undo(dirs, &profile.id, &staged)?;
    let (mut mods_saved, mut config_written) = (false, false);
    let applied = (|| -> Result<(Profile, Instance)> {
        let mut p = profile.clone();
        p.mods = c.mods.clone();
        p.late = r.index.order.late.iter().cloned().chain(profile.late.iter().filter(|id| !r.index.order.late.contains(id)).cloned()).collect();
        let saved = crate::profile::save(dirs, &p)?;
        mods_saved = true;
        for (target, out) in &writes {
            if let Some(d) = target.parent() {
                std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
            }
            config_written = true;
            crate::util::write_atomic(target, out.as_bytes())?;
        }
        write_base(dirs, &profile.id, &r)?;
        let mut optional = inst.optional.clone();
        for m in r.index.mods.iter().filter(|m| m.dep == Dep::Optional) {
            let on = saved.mods.list(m.kind).iter().find(|e| e.id == m.id).map(|e| e.enabled).unwrap_or(m.default.unwrap_or(true));
            optional.insert(m.id.clone(), on);
        }
        optional.retain(|id, _| r.index.mods.iter().any(|m| &m.id == id && m.dep == Dep::Optional));
        let new_inst = Instance {
            version: r.index.version.clone(),
            name: r.index.name.clone(),
            sha256: r.sha256.clone(),
            url: url.clone().or(inst.url.clone()),
            crucible: r.index.crucible.clone().or(inst.crucible.clone()),
            optional,
            updated: Some(crate::util::now_rfc3339()),
            previous: Some(inst.version.clone()),
            ..inst.clone()
        };
        crate::util::write_json_atomic(&pd.join(INSTANCE_FILE), &new_inst)?;
        Ok((saved, new_inst))
    })();
    let (saved, new_inst) = match applied {
        Ok(x) => x,
        Err(e) => {
            if let Err(back) = restore_from(dirs, &profile.id, &staged, mods_saved, config_written) {
                // the copy stays as the undo copy: pack undo tries again
                let _ = std::fs::remove_dir_all(pd.join("undo"));
                let _ = std::fs::rename(&staged, pd.join("undo"));
                return Err(Error::new(e.code, format!("{} (putting the instance back failed too: {}; Undo tries again)", e.message, back.message)));
            }
            let _ = std::fs::remove_dir_all(&staged);
            return Err(e);
        }
    };
    // the update stands: its copy replaces the last undo copy
    let undo = pd.join("undo");
    let _ = std::fs::remove_dir_all(&undo);
    if let Err(e) = std::fs::rename(&staged, &undo) {
        progress.report(None, &format!("updated, but the undo copy could not be kept: {e}"));
    }
    progress.report(Some(1.0), &format!("pack {} updated {} -> {}", r.index.name, c.plan.from, c.plan.to));
    Ok(UpdateResult { plan: c.plan, profile: saved, instance: new_inst, imported })
}

/// A copy of the instance as it is (profile meta.json, instance.json, config/, base/) in dest.
fn snapshot_undo(dirs: &DataDirs, pid: &str, dest: &Path) -> Result<()> {
    let pd = pack_dir(dirs, pid);
    let _ = std::fs::remove_dir_all(dest);
    std::fs::create_dir_all(dest).map_err(crate::util::io_at(dest, "creating"))?;
    std::fs::copy(dirs.profile(pid).join(crate::profile::META), dest.join(crate::profile::META)).map_err(crate::util::io_at(dest, "copying into"))?;
    std::fs::copy(pd.join(INSTANCE_FILE), dest.join(INSTANCE_FILE)).map_err(crate::util::io_at(dest, "copying into"))?;
    let config = dirs.profile(pid).join("config");
    if config.is_dir() {
        crate::util::copy_tree(&config, &dest.join("config"))?;
    }
    crate::util::copy_tree(&pd.join("base"), &dest.join("base"))?;
    Ok(())
}

/// Puts an instance back as a copy holds it: its mods and late DLLs (with mods), its config folder (with config; the
/// live one comes back if the copy cannot be laid down), the base and instance.json.
fn restore_from(dirs: &DataDirs, pid: &str, snap: &Path, mods: bool, config: bool) -> Result<Profile> {
    let pd = pack_dir(dirs, pid);
    let mut p = crate::profile::get(dirs, pid)?;
    if mods {
        let before: Profile = crate::util::read_json(&snap.join(crate::profile::META))?
            .ok_or_else(|| Error::not_found(format!("{}: no profile copy", snap.display())))?;
        p.mods = before.mods;
        p.late = before.late;
        p = crate::profile::save(dirs, &p)?;
    }
    if config {
        let live = dirs.profile(pid).join("config");
        let old = dirs.profile(pid).join("config.undo-old");
        let _ = std::fs::remove_dir_all(&old);
        if live.exists() {
            std::fs::rename(&live, &old).map_err(crate::util::io_at(&live, "moving"))?;
        }
        let laid = if snap.join("config").is_dir() {
            crate::util::copy_tree(&snap.join("config"), &live).map(|_| ())
        } else {
            std::fs::create_dir_all(&live).map_err(crate::util::io_at(&live, "creating"))
        };
        if let Err(e) = laid {
            let _ = std::fs::remove_dir_all(&live);
            let _ = std::fs::rename(&old, &live);
            return Err(e);
        }
        let _ = std::fs::remove_dir_all(&old);
    }
    let tmp = pd.join("base.tmp");
    let _ = std::fs::remove_dir_all(&tmp);
    crate::util::copy_tree(&snap.join("base"), &tmp)?;
    swap_in(&tmp, &pd.join("base"))?;
    crate::util::write_atomic(&pd.join(INSTANCE_FILE), &std::fs::read(snap.join(INSTANCE_FILE)).map_err(crate::util::io_at(snap, "reading"))?)?;
    Ok(p)
}

/// Takes the last update back: the mods, the config folder, the base and instance.json as they were before it.
pub fn undo(dirs: &DataDirs, pid: &str) -> Result<Profile> {
    let undo = pack_dir(dirs, pid).join("undo");
    if !undo.join(crate::profile::META).is_file() {
        crate::profile::get(dirs, pid)?;
        return Err(Error::not_found(format!("profile {pid} has no update to undo")));
    }
    let p = restore_from(dirs, pid, &undo, true, true)?;
    let _ = std::fs::remove_dir_all(&undo);
    Ok(p)
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UninstallResult {
    pub profile: String,
    /// The pack's Library entries no profile uses any more (the Library's clean-up can remove them).
    pub unused: Vec<LibraryRef>,
}

/// Removes an instance (its profile goes to backups/profiles/, as a deleted profile does). Refused while a game has it
/// active. The Library keeps the mods; `unused` lists the pack's entries nothing uses now.
pub fn uninstall(dirs: &DataDirs, pid: &str) -> Result<UninstallResult> {
    let inst = instance(dirs, pid)?.ok_or_else(|| Error::not_found(format!("profile {pid} is not a Crucible instance")))?;
    if crate::profile::list(dirs)?.iter().any(|p| p.id == pid && !p.active.is_empty()) {
        return Err(err(ErrorCode::Refused, format!("{} is the active profile of a game: switch to another profile first", inst.name)));
    }
    let (base, _) = read_base(dirs, pid).unwrap_or_else(|_| (PackIndex {
        format: FORMAT, kind: KIND.into(), id: inst.pack.clone(), name: inst.name.clone(), version: inst.version.clone(), created: None,
        authors: Vec::new(), description: None, game: None, kiln: None, api: None, crucible: None, mods: Vec::new(), layer: None,
        order: PackOrder::default(), dropins: Vec::new(), overrides: Vec::new(), server: None, changes: None,
    }, BTreeMap::new()));
    crate::profile::delete(dirs, pid)?;
    let ours: BTreeSet<(ModKind, String)> = base.mods.iter().map(|m| (m.kind, m.id.clone()))
        .chain(base.layer.iter().map(|l| (ModKind::Kiln, l.id.clone()))).collect();
    let unused = crate::library::unused(dirs)?.into_iter().filter(|r| ours.contains(&(r.kind, r.id.clone()))).collect();
    Ok(UninstallResult { profile: pid.to_string(), unused })
}

// ================================================================================================= two packs

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackDiff {
    pub from: String,
    pub to: String,
    pub added: Vec<PlanMod>,
    pub updated: Vec<PlanUpdate>,
    pub removed: Vec<PlanMod>,
    pub layer_changed: bool,
    /// `<side dir>/<path>`
    pub overrides_added: Vec<String>,
    pub overrides_changed: Vec<String>,
    pub overrides_removed: Vec<String>,
    pub server_settings_changed: bool,
}

/// What changed between two versions of a pack (the automatic change list).
pub fn diff(old: &PackIndex, new: &PackIndex) -> PackDiff {
    let mut d = PackDiff { from: old.version.clone(), to: new.version.clone(), ..Default::default() };
    let pm = |m: &PackMod| PlanMod { kind: m.kind, id: m.id.clone(), version: m.version.clone(), enabled: m.dep == Dep::Required || m.default.unwrap_or(true) };
    for m in &new.mods {
        match old.find(m.kind, &m.id) {
            None => d.added.push(pm(m)),
            Some(o) if o.hash != m.hash => d.updated.push(PlanUpdate { kind: m.kind, id: m.id.clone(), from: o.version.clone(), to: m.version.clone() }),
            Some(_) => {}
        }
    }
    for m in &old.mods {
        if new.find(m.kind, &m.id).is_none() {
            d.removed.push(pm(m));
        }
    }
    d.layer_changed = old.layer.as_ref().map(|l| &l.hash) != new.layer.as_ref().map(|l| &l.hash);
    for o in &new.overrides {
        match old.overrides.iter().find(|x| x.side == o.side && x.path == o.path) {
            None => d.overrides_added.push(o.zip_path()),
            Some(x) if x.sha256 != o.sha256 || x.mode != o.mode => d.overrides_changed.push(o.zip_path()),
            Some(_) => {}
        }
    }
    for o in &old.overrides {
        if !new.overrides.iter().any(|x| x.side == o.side && x.path == o.path) {
            d.overrides_removed.push(o.zip_path());
        }
    }
    d.server_settings_changed = old.server.as_ref().and_then(|s| s.settings.as_ref()) != new.server.as_ref().and_then(|s| s.settings.as_ref());
    d
}

// ================================================================================================= server part

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerInstallRequest {
    pub source: ModpackSource,
    pub consent: bool,
    #[serde(default)]
    pub allow_http: bool,
    /// Switches of optional mods (default: each mod's default); a mod off is not placed.
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    /// Write the pack's server game settings (default true).
    #[serde(default = "yes")]
    pub settings: bool,
    /// Build the server modpack players download (`super::build`) into this folder afterwards.
    #[serde(default)]
    pub modpack_out: Option<PathBuf>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerInstallResult {
    pub mods: Vec<crate::server::ServerMod>,
    pub imported: Vec<LibraryRef>,
    pub overrides: Vec<String>,
    pub settings: bool,
    /// Mods a server does not take (their kind has no server folder), by id.
    pub left_out: Vec<String>,
    pub modpack: Option<super::BuiltModpack>,
}

/// Puts the server part of a pack on a local server: its server mods (env.server not unsupported, optional ones as
/// chosen) and the layer into the Library, then synced into the server's folders (kiln.server's sync: the launcher's
/// mods there become exactly these; hand-placed folders stay), the overrides of both sides and the server side into
/// `<server>/kiln/config`, the game settings into enshrouded_server.json (refused while it runs). With `modpack_out`, the
/// server modpack for players is built right after: the format-1 flow players already get through the join.
pub fn install_server(dirs: &DataDirs, server: &crate::server::instance::ServerInstance, req: &ServerInstallRequest, progress: &mut dyn Progress)
    -> Result<ServerInstallResult> {
    if !req.consent {
        return Err(err(ErrorCode::ConsentRequired, "a pack runs code from other people on this server: agree first"));
    }
    let dir = server.local_dir()?.to_path_buf();
    // the game settings cannot be written while the server runs: refused before any mod moves, not half way
    if req.settings && crate::server::run::status(server)?.pid.is_some() {
        return Err(err(ErrorCode::Conflict, "the server is running and rewrites its settings file: stop it first (or leave the game settings out)"));
    }
    let (zip_file, sha, _) = fetch(dirs, &req.source, req.allow_http, progress)?;
    let takes = |k: ModKind| matches!(k, ModKind::Kiln | ModKind::Native)
        || (k.is_engine() && k != ModKind::Xhl && crate::engine_mods::server_root(k, Path::new("")).is_some());
    let choices = req.optional.clone();
    let wanted = |m: &PackMod| m.env().server != Need::Unsupported && m.on(&choices) && takes(m.kind);
    let (r, imported) = import_pack(dirs, &zip_file, sha.as_deref(), &wanted, progress)?;
    let left_out: Vec<String> = r.index.mods.iter().filter(|m| m.env().server != Need::Unsupported && m.on(&choices) && !takes(m.kind)).map(|m| m.id.clone()).collect();
    let mut refs: Vec<LibraryRef> = r.index.mods.iter().filter(|m| wanted(m)).map(PackMod::library_ref).collect();
    if let Some(l) = &r.index.layer {
        refs.push(LibraryRef { kind: ModKind::Kiln, id: l.id.clone(), version: l.version.clone(), hash: l.hash.clone() });
    }
    progress.report(Some(0.82), "placing the mods on the server");
    let mods = crate::server::control::sync(dirs, server, &refs, &mut Scaled { inner: progress, from: 0.82, to: 0.92 })?;
    let overrides = apply_overrides(&dir.join("kiln/config"), &r, &|s| s.for_server())?;
    let mut settings = false;
    if req.settings && let Some(s) = r.index.server.as_ref().and_then(|s| s.settings.as_ref()) {
        crate::server::settings::write(server, s)?;
        settings = true;
    }
    let modpack = match &req.modpack_out {
        Some(out) => Some(super::build(&dir, out, &r.index.name, &r.index.version, &mut Scaled { inner: progress, from: 0.95, to: 1.0 })?),
        None => None,
    };
    progress.report(Some(1.0), &format!("pack {} {} placed on the server", r.index.name, r.index.version));
    Ok(ServerInstallResult { mods, imported, overrides, settings, left_out, modpack })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_secret_scan_reads_keys_not_words_in_text() {
        // normal settings and text pass
        for (f, t) in [
            ("loot.json", r#"{ "name": "Secret Stash", "description": "the password is under the old tree", "maxTokens": 5, "hotkey": "F9", "author": "Ana", "showPassword": true }"#),
            ("loot.ini", "[Chest]\ntitle = Secret room\npassive = yes\nkeyBinding = K\n"),
            ("notes.txt", "Find the secret passage behind the waterfall.\n"),
        ] {
            assert_eq!(secret_findings(f, t), Vec::<String>::new(), "{f}");
        }
        // secrets are found, also on one line and under other key names
        for (f, t) in [
            ("bot.json", r#"{"discord":{"enabled":true,"token":"abc"}}"#),
            ("bot.json", r#"{ "hook": "https://discord.com/api/v10/webhooks/123/abcDEF" }"#),
            ("bot.json", r#"{ "hook": "https://ptb.discord.com/api/webhooks/123/abcDEF" }"#),
            ("bot.json", r#"{ "owner": 76561198000000001 }"#),
            ("bot.json", r#"{ "admins": ["76561198000000001"] }"#),
            ("bot.json", r#"{ "x": "MTA4NjQ3NzQ0NjMwNjQ3NjA5Mg.GhK2aB.TOKENTAIL" }"#),
            ("bot.json", r#"{ "gh": "ghp_abcdefghijklmnopqrstuvwxyz0123456789" }"#),
            ("bot.json", r#"{ "rcon": { "pass": "hunter2" } }"#),
            ("bot.json", r#"{ "db": { "pwd": "hunter2" } }"#),
            ("bot.json", r#"{ "apiKey": "k-123" }"#),
            ("bot.json", r#"{ "client_secret": "s" }"#),
            ("bot.ini", "[Rcon]\nPassword = hunter2\n"),
            ("bot.ini", "Auth = Bearer abcdefghijklmnopqrstuvwxyz\n"),
            ("bot.cfg", "webhook=https://discordapp.com/api/webhooks/1/x\n"),
            ("bot.txt", "STEAM_0:1:12345678\n"),
        ] {
            // a bot token's last part, made here: written out, it would be a secret to scanners
            let t = &t.replace("TOKENTAIL", &"x".repeat(27));
            assert!(!secret_findings(f, t).is_empty(), "{f}: {t}");
        }
    }

    fn rp(overrides: &[(PartSide, &str)]) -> ReadPack {
        let mut ix: PackIndex = serde_json::from_value(serde_json::json!({ "format": 2, "kind": "pack", "id": "p", "name": "P", "version": "1.0.0", "mods": [] })).unwrap();
        let mut ov = BTreeMap::new();
        for (side, text) in overrides {
            ix.overrides.push(PackOverride { path: "a.json".into(), mode: OverrideMode::MergePatch, side: *side, sha256: crate::hash::sha256_hex(text.as_bytes()), size: None });
            ov.insert((*side, "a.json".to_string()), text.as_bytes().to_vec());
        }
        ReadPack { index: ix, overrides: ov, sha256: String::new(), size: 0, code: BTreeSet::new() }
    }

    #[test]
    fn a_side_override_wins_over_the_both_sides_one_whatever_the_index_order() {
        let dir = std::env::temp_dir().join(format!("km-pack-order-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for order in [[(PartSide::Client, r#"{ "v": "client" }"#), (PartSide::Both, r#"{ "v": "both" }"#)],
                      [(PartSide::Both, r#"{ "v": "both" }"#), (PartSide::Client, r#"{ "v": "client" }"#)]] {
            apply_overrides(&dir, &rp(&order), &|s| s.for_client()).unwrap();
            let v = crate::jsonc::parse_tree(&std::fs::read_to_string(dir.join("a.json")).unwrap()).unwrap().to_value();
            assert_eq!(v["v"], "client", "{order:?}");
            let _ = std::fs::remove_dir_all(&dir);
        }
    }
}
