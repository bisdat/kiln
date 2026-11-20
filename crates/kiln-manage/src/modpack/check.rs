//! The pack analysis (Crucible DESIGN.md section 6): the automated checks of a Crucible pack and the identification
//! of each mod that prefills its credits. The launcher and Crucible's sandboxed check worker share this code
//! (`kilnctl pack check`, docs/kiln/launcher/crucible.md "Checks and the pack analysis").
//!
//! Two inputs: a whole `.kilnpack`, or the form Crucible stores (the index plus a folder of the files it names, each
//! called by its sha256: the mods' and the layer's archives as `pack::mod_archive` writes them, and the raw override
//! files). The stored form is assembled into a `.kilnpack` without unpacking anything (raw entry copies), then both
//! go through `pack::read`, so one set of rules (zip safety, sizes, kdh1, overrides) checks everything.
//!
//! The analysis never touches the network: GitHub and Nexus sources are named from the manifests' `updates`, not
//! fetched (match "github"/"nexus", confidence "unverified"). It never runs anything a pack holds.
//!
//! Output: [`CheckReport`], stable JSON (kind `kiln.pack-check`, format 1). `ok` is false when any finding is an
//! error; warnings and notes never block (code that touches the network, conflicts between data packs, ...).

use super::pack::{self, MAX_ENTRIES, MAX_FILE, MAX_OVERRIDE, PackIndex, PackMod, ReadPack};
use crate::lint::conflicts::{self, DataConflict};
use crate::manifest::{ModKind, UpdatesSpec};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

pub const REPORT_KIND: &str = "kiln.pack-check";
pub const REPORT_FORMAT: u32 = 1;
/// A pack's files together (Crucible's limit; DESIGN.md 6.1).
pub const MAX_PACK: u64 = 2 * 1024 * 1024 * 1024;
/// A code file larger than this is listed, not parsed.
const MAX_SCAN: u64 = 64 * 1024 * 1024;
const MAX_FINDINGS: usize = 1000;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Level {
    /// Blocks a public pack (Crucible) and is shown before install.
    Error,
    /// Shown under "Checks"; never blocks.
    Warning,
    Note,
}

/// One result for people, with a stable code for tools.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Finding {
    pub level: Level,
    /// zip | index | hash | size | missing-file | blocked | secret | resolve | code | capability | KLxxx (lint) | ...
    pub code: String,
    pub message: String,
    #[serde(default, rename = "mod", skip_serializing_if = "Option::is_none")]
    pub mod_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub file: Option<String>,
}

/// Kiln's own mods (Crucible's data/kiln-own-mods.json): identification step 1.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct OwnMods {
    pub author: String,
    pub mods: Vec<OwnMod>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct OwnMod {
    pub kind: String,
    pub id: String,
    pub source: String,
    /// With one listed, a match is "certain"; by id alone "likely".
    #[serde(default)]
    pub kdh1: Vec<String>,
}

/// A credit confirmed in an earlier public pack (Crucible passes them): identification step 2.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct KnownCredit {
    pub kdh1: String,
    pub author: String,
    pub source: String,
}

/// What the analysis is given besides the pack.
#[derive(Debug, Clone, Default)]
pub struct CheckOptions {
    pub own: Option<OwnMods>,
    pub known: Vec<KnownCredit>,
    /// Blocked sha256 (hex) and kdh1 (`kdh1:...`) values, any case.
    pub blocked: BTreeSet<String>,
    /// The catalog for lint and the conflict analysis (`catalog`, `no_catalog`); `profile` is set by the check.
    pub lint: crate::lint::LintOptions,
    /// The Kiln and API versions the resolver checks against (default: this build's).
    pub kiln: Option<String>,
    pub api: Option<String>,
    /// Where to unpack (default: the system temp folder); a folder of its own is made and removed below it.
    pub work: Option<PathBuf>,
}

impl CheckOptions {
    /// A blocklist file: a JSON list of strings, or Crucible's admin shape `{"blocked": [{"sha256": ...}]}`.
    pub fn read_blocklist(v: &Value) -> BTreeSet<String> {
        let list = v.get("blocked").unwrap_or(v);
        list.as_array().into_iter().flatten().filter_map(|x| x.as_str().or_else(|| x.get("sha256").and_then(Value::as_str)))
            .map(|s| s.trim().to_ascii_lowercase()).filter(|s| !s.is_empty()).collect()
    }
}

/// What to check.
#[derive(Debug, Clone, Copy)]
pub enum Input<'a> {
    /// A whole .kilnpack.
    Pack(&'a Path),
    /// Crucible's stored form: the index (kiln-modpack.json as uploaded) and a folder with each file it names, as
    /// `<sha256>` (mods[].file, layer.file, overrides[].sha256).
    Stored { index: &'a Path, files: &'a Path },
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CodeScan {
    /// Managed (.NET) assemblies, native DLLs/EXEs/.so, scripts (Lua, JS, Python, shell): paths in the mod.
    pub managed: Vec<String>,
    pub native: Vec<String>,
    pub scripts: Vec<String>,
    /// network | process | registry
    pub touches: BTreeSet<String>,
    /// Why, one line each: "kiln-x.dll imports ws2_32.dll (connect)".
    pub evidence: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Capabilities {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub features: Vec<String>,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub advanced: bool,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub boot: bool,
    /// kiln.mod.json "writes", as written.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub writes: Option<Value>,
}

/// One mod of the pack as the analysis saw it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModReport {
    pub kind: ModKind,
    pub id: String,
    pub version: String,
    pub hash: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub authors: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub license: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub updates: Option<UpdatesSpec>,
    pub code: CodeScan,
    pub capabilities: Capabilities,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub blocked: bool,
}

/// A prefilled credit row (identification, DESIGN.md 6.2; first match wins).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreditRow {
    pub kind: ModKind,
    pub id: String,
    pub version: String,
    pub kdh1: String,
    pub author: Option<String>,
    /// https only.
    pub source: Option<String>,
    /// kiln | crucible | github | nexus | manifest | manual
    #[serde(rename = "match")]
    pub match_kind: String,
    /// certain | likely | conflict | unverified | creator | unconfirmed | none
    pub confidence: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub nexus_mod_id: Option<u64>,
    /// Why the row needs a look (the creator's credit differs from an earlier one, ...).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub flag: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    /// Of the .kilnpack (a whole pack only).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub size: Option<u64>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Summary {
    pub errors: usize,
    pub warnings: usize,
    pub notes: usize,
}

/// The analysis (`kilnctl pack check --json`). Crucible stores it as it comes.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CheckReport {
    pub format: u32,
    pub kind: &'static str,
    pub tool: &'static str,
    pub version: &'static str,
    /// No errors.
    pub ok: bool,
    /// False when the pack could not be read: the per-mod parts are then from the index alone.
    pub complete: bool,
    pub pack: Option<PackInfo>,
    pub summary: Summary,
    pub findings: Vec<Finding>,
    pub mods: Vec<ModReport>,
    pub conflicts: Vec<DataConflict>,
    pub credits: Vec<CreditRow>,
    /// The catalog lint and the conflict analysis used (none: names of the game not checked).
    pub catalog: bool,
}

struct Out {
    findings: Vec<Finding>,
}

impl Out {
    fn add(&mut self, level: Level, code: &str, message: impl Into<String>, mod_id: Option<&str>, file: Option<&str>) {
        if self.findings.len() < MAX_FINDINGS {
            self.findings.push(Finding { level, code: code.into(), message: message.into(), mod_id: mod_id.map(str::to_string), file: file.map(str::to_string) });
        }
    }
    fn error(&mut self, code: &str, message: impl Into<String>) {
        self.add(Level::Error, code, message, None, None);
    }
}

fn code_of(e: &Error) -> &'static str {
    match e.code {
        ErrorCode::HashMismatch => "hash",
        ErrorCode::Schema => "index",
        ErrorCode::Parse => "zip",
        ErrorCode::Unsupported => "format",
        ErrorCode::Refused => "refused",
        _ => "read",
    }
}

fn is_sha256(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// The entry count a zip's end record claims (zip64 too), read before the zip crate builds its table of every entry:
/// a few MB of central directory can claim millions of entries. None when the end record is not found.
pub fn claimed_entries(path: &Path) -> Option<u64> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let tail = len.min(65_535 + 22);
    f.seek(SeekFrom::Start(len - tail)).ok()?;
    let mut b = vec![0u8; tail as usize];
    f.read_exact(&mut b).ok()?;
    let at = (0..b.len().saturating_sub(21)).rev().find(|&i| b[i..i + 4] == [0x50, 0x4b, 0x05, 0x06])?;
    let n = u16::from_le_bytes([b[at + 10], b[at + 11]]) as u64;
    if n != 0xffff {
        return Some(n);
    }
    // zip64: the locator sits right before the end record
    let loc = at.checked_sub(20)?;
    if b[loc..loc + 4] != [0x50, 0x4b, 0x06, 0x07] {
        return Some(n);
    }
    let off = u64::from_le_bytes(b[loc + 8..loc + 16].try_into().ok()?);
    f.seek(SeekFrom::Start(off)).ok()?;
    let mut r = [0u8; 56];
    f.read_exact(&mut r).ok()?;
    (r[0..4] == [0x50, 0x4b, 0x06, 0x06]).then(|| u64::from_le_bytes(r[32..40].try_into().unwrap()))
}

/// Runs every check and the identification. Problems of the pack are findings; Err only when the analysis itself
/// cannot run (no work folder).
pub fn check(input: Input, opts: &CheckOptions) -> Result<CheckReport> {
    let work_parent = opts.work.clone().unwrap_or_else(std::env::temp_dir);
    std::fs::create_dir_all(&work_parent).map_err(crate::util::io_at(&work_parent, "creating"))?;
    let work = crate::library::TempDir::new(&work_parent, "kiln-pack-check")?;
    let mut out = Out { findings: Vec::new() };
    let mut stored_index: Option<PackIndex> = None;
    let mut archive_shas: Vec<(String, String)> = Vec::new(); // (what, sha256)
    let zip_path: Option<PathBuf> = match input {
        Input::Pack(p) => {
            match std::fs::metadata(p) {
                Ok(m) if m.len() > MAX_PACK => {
                    out.error("size", format!("the pack is {} bytes: at most 2 GiB", m.len()));
                    None
                }
                Ok(_) => Some(p.to_path_buf()),
                Err(e) => return Err(crate::util::io_at(p, "reading")(e)),
            }
        }
        Input::Stored { index, files } => {
            let assembled = work.path().join("pack.kilnpack");
            match assemble(index, files, &assembled, &mut out, &mut archive_shas) {
                Some(ix) => {
                    stored_index = Some(ix);
                    (!out.findings.iter().any(|f| f.level == Level::Error)).then_some(assembled)
                }
                None => None,
            }
        }
    };
    if let Some(z) = &zip_path
        && let Some(n) = claimed_entries(z)
        && n > MAX_ENTRIES as u64
    {
        out.error("zip", format!("the pack claims {n} entries: at most {MAX_ENTRIES}"));
    }
    let unpacked = work.path().join("x");
    let read: Option<ReadPack> = match &zip_path {
        Some(z) if !out.findings.iter().any(|f| f.level == Level::Error) => match pack::read(z, None, Some(&unpacked)) {
            Ok(r) => Some(r),
            Err(e) => {
                let detail = e.detail.as_ref().and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).map(str::to_string).collect::<Vec<_>>());
                match detail {
                    Some(d) if !d.is_empty() => d.into_iter().for_each(|m| out.error(code_of(&e), m)),
                    _ => out.error(code_of(&e), e.message.clone()),
                }
                None
            }
        },
        _ => None,
    };
    let index: Option<PackIndex> = read.as_ref().map(|r| r.index.clone()).or(stored_index).or_else(|| match input {
        Input::Pack(p) => peek_index(p),
        Input::Stored { .. } => None,
    });
    let mut report = CheckReport {
        format: REPORT_FORMAT, kind: REPORT_KIND, tool: "kiln-manage", version: crate::VERSION, ok: false, complete: read.is_some(),
        pack: index.as_ref().map(|ix| PackInfo {
            id: ix.id.clone(), name: ix.name.clone(), version: ix.version.clone(),
            sha256: matches!(input, Input::Pack(_)).then(|| read.as_ref().map(|r| r.sha256.clone())).flatten(),
            size: matches!(input, Input::Pack(_)).then(|| read.as_ref().map(|r| r.size)).flatten(),
        }),
        summary: Summary::default(), findings: Vec::new(), mods: Vec::new(), conflicts: Vec::new(), credits: Vec::new(), catalog: false,
    };
    if let Some(ix) = &index {
        let root = read.as_ref().map(|_| unpacked.as_path());
        // blocklist: the archives, the overrides, every kdh1, the whole pack
        let mut blocked_ids = BTreeSet::new();
        let is_blocked = |s: &str| opts.blocked.contains(&s.trim().to_ascii_lowercase());
        for m in &ix.mods {
            let stored = m.file.as_ref().map(|f| f.sha256.as_str());
            if is_blocked(&m.hash) || stored.is_some_and(is_blocked) {
                blocked_ids.insert(m.id.clone());
                out.add(Level::Error, "blocked", format!("{} {} was taken down (a file Crucible blocked)", m.id, m.version), Some(&m.id), None);
            }
        }
        for (what, sha) in &archive_shas {
            if is_blocked(sha) && !blocked_ids.contains(what) {
                out.add(Level::Error, "blocked", format!("{what}: a file Crucible blocked"), None, None);
            }
        }
        if let Some(l) = &ix.layer
            && (is_blocked(&l.hash) || l.file.as_ref().is_some_and(|f| is_blocked(&f.sha256)))
        {
            out.add(Level::Error, "blocked", "the pack layer was taken down (a file Crucible blocked)", Some(&l.id), None);
        }
        for o in &ix.overrides {
            if is_blocked(&o.sha256) {
                out.add(Level::Error, "blocked", format!("override {} is a file Crucible blocked", o.path), None, Some(&o.path));
            }
        }
        if let Some(r) = &read
            && matches!(input, Input::Pack(_))
            && is_blocked(&r.sha256)
        {
            out.error("blocked", "this pack file was taken down");
        }
        if ix.game.as_ref().is_none_or(|g| g.builds.is_empty()) {
            out.add(Level::Note, "game", "no game build is named: players get no \"made for your game\" check", None, None);
        }
        // per mod
        for m in &ix.mods {
            let dir = root.map(|r| r.join(&m.path));
            let mut mr = mod_report(m, dir.as_deref(), &mut out);
            mr.blocked = blocked_ids.contains(&m.id);
            report.mods.push(mr);
        }
        if let (Some(r), Some(root)) = (&read, root) {
            secrets(r, root, &mut out);
            resolve(ix, root, opts, &mut out);
            let (conf, cat) = data_conflicts(ix, root, opts);
            for c in &conf {
                out.add(Level::Warning, crate::lint::codes::DATA_CONFLICT, c.message(), Some(&c.later.mod_id), Some(&c.later.file));
            }
            report.conflicts = conf;
            report.catalog = cat;
            lint_layer(ix, root, opts, &mut out);
        }
        report.credits = identify(ix, &report.mods, opts);
    } else {
        out.error("index", "no pack index could be read");
    }
    out.findings.sort_by_key(|f| f.level);
    report.summary = Summary {
        errors: out.findings.iter().filter(|f| f.level == Level::Error).count(),
        warnings: out.findings.iter().filter(|f| f.level == Level::Warning).count(),
        notes: out.findings.iter().filter(|f| f.level == Level::Note).count(),
    };
    report.ok = report.summary.errors == 0;
    report.findings = out.findings;
    Ok(report)
}

/// The index of a .kilnpack without the other checks (for a report on a pack that fails them).
fn peek_index(p: &Path) -> Option<PackIndex> {
    use std::io::Read;
    if claimed_entries(p).is_some_and(|n| n > MAX_ENTRIES as u64) {
        return None;
    }
    let f = std::fs::File::open(p).ok()?;
    let mut a = zip::ZipArchive::new(f).ok()?;
    let e = a.by_name(super::MANIFEST).ok()?;
    let mut b = Vec::new();
    e.take(4 << 20).read_to_end(&mut b).ok()?;
    serde_json::from_slice(&b).ok()
}

/// Crucible's stored form -> a .kilnpack at `out_zip` (raw entry copies; nothing is unpacked). Returns the index when it
/// parses; problems go to `out`.
fn assemble(index: &Path, files: &Path, out_zip: &Path, out: &mut Out, shas: &mut Vec<(String, String)>) -> Option<PackIndex> {
    use std::io::{Read, Write};
    let bytes = match std::fs::File::open(index) {
        Ok(f) => {
            let mut b = Vec::new();
            if f.take((4 << 20) + 1).read_to_end(&mut b).is_err() || b.len() > 4 << 20 {
                out.error("index", "the index is not readable or larger than 4 MiB");
                return None;
            }
            b
        }
        Err(e) => {
            out.error("index", format!("the index cannot be read: {e}"));
            return None;
        }
    };
    let ix: PackIndex = match serde_json::from_slice::<Value>(&bytes) {
        Ok(v) if v.get("format").and_then(Value::as_u64) != Some(2) => {
            out.error("format", "not a Crucible pack index (format 2)");
            return None;
        }
        Ok(v) => match serde_json::from_value(v) {
            Ok(ix) => ix,
            Err(e) => {
                out.error("index", format!("{}: {e}", super::MANIFEST));
                return None;
            }
        },
        Err(e) => {
            out.error("index", format!("{}: {e}", super::MANIFEST));
            return None;
        }
    };
    if let Err(e) = pack::check_index(&ix) {
        let d = e.detail.as_ref().and_then(Value::as_array).cloned().unwrap_or_default();
        if d.is_empty() { out.error("index", e.message) } else { d.iter().filter_map(Value::as_str).for_each(|m| out.error("index", m)) }
        return Some(ix);
    }
    // every file first: present, the size and sha256 the index gives, the limits
    let mut total = 0u64;
    let mut ok = true;
    let mut check_file = |what: &str, sha: &str, size: Option<u64>, max: u64, out: &mut Out| -> Option<PathBuf> {
        let sha = sha.to_ascii_lowercase();
        if !is_sha256(&sha) {
            out.error("index", format!("{what}: no sha256 of its file"));
            return None;
        }
        let p = files.join(&sha);
        let meta = match std::fs::symlink_metadata(&p) {
            Ok(m) if m.is_file() => m,
            _ => {
                out.error("missing-file", format!("{what}: its file {sha} is not there"));
                return None;
            }
        };
        if meta.len() > max {
            out.error("size", format!("{what}: {} bytes, at most {max}", meta.len()));
            return None;
        }
        if let Some(s) = size && s != meta.len() {
            out.error("size", format!("{what}: {} bytes, the index says {s}", meta.len()));
            return None;
        }
        total += meta.len();
        match crate::hash::sha256_file(&p) {
            Ok((have, _)) if have == sha => Some(p),
            Ok((have, _)) => {
                out.error("hash", format!("{what}: the file hashes to {have}, the index says {sha}"));
                None
            }
            Err(e) => {
                out.error("read", format!("{what}: {}", e.message));
                None
            }
        }
    };
    let mut parts: Vec<(String, PathBuf, bool)> = Vec::new(); // (zip path or prefix, file, plugin)
    for m in &ix.mods {
        let what = format!("mod {}", m.id);
        let Some(f) = &m.file else {
            out.error("index", format!("{what}: mods[].file (the archive Crucible stores) is missing"));
            ok = false;
            continue;
        };
        shas.push((m.id.clone(), f.sha256.to_ascii_lowercase()));
        match check_file(&what, &f.sha256, Some(f.size), MAX_FILE, out) {
            Some(p) => parts.push((m.path.clone(), p, m.kind == ModKind::Plugin)),
            None => ok = false,
        }
    }
    if let Some(l) = &ix.layer {
        match &l.file {
            Some(f) => {
                shas.push((l.id.clone(), f.sha256.to_ascii_lowercase()));
                match check_file("the pack layer", &f.sha256, Some(f.size), MAX_FILE, out) {
                    Some(p) => parts.push((l.path.clone(), p, false)),
                    None => ok = false,
                }
            }
            None => {
                out.error("index", "layer.file (the archive Crucible stores) is missing");
                ok = false;
            }
        }
    }
    let mut blobs = Vec::new();
    for o in &ix.overrides {
        match check_file(&format!("override {}", o.path), &o.sha256, o.size, MAX_OVERRIDE, out) {
            Some(p) => blobs.push((o.zip_path(), p)),
            None => ok = false,
        }
    }
    if total > MAX_PACK {
        out.error("size", format!("the pack's files are {total} bytes: at most 2 GiB"));
        ok = false;
    }
    if !ok {
        return Some(ix);
    }
    let r = (|| -> std::result::Result<(), String> {
        let f = std::fs::File::create(out_zip).map_err(|e| e.to_string())?;
        let mut w = zip::ZipWriter::new(f);
        let opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
        w.start_file(super::MANIFEST, opts).map_err(|e| e.to_string())?;
        w.write_all(&bytes).map_err(|e| e.to_string())?;
        let mut entries = 0usize;
        for (prefix, p, plugin) in &parts {
            if claimed_entries(p).is_some_and(|n| n > MAX_ENTRIES as u64) {
                return Err(format!("{prefix}: the archive claims more than {MAX_ENTRIES} entries"));
            }
            let mut a = zip::ZipArchive::new(std::fs::File::open(p).map_err(|e| e.to_string())?).map_err(|e| format!("{prefix}: {e}"))?;
            entries += a.len();
            if entries > MAX_ENTRIES {
                return Err(format!("the pack's archives hold more than {MAX_ENTRIES} entries"));
            }
            let mut files_in = 0;
            for i in 0..a.len() {
                let e = a.by_index_raw(i).map_err(|e| format!("{prefix}: {e}"))?;
                let name = e.name().map_err(|e| format!("{prefix}: {e}"))?.to_string();
                if name.ends_with('/') {
                    continue;
                }
                files_in += 1;
                let to = if *plugin {
                    if files_in > 1 {
                        return Err(format!("{prefix}: a plugin's archive holds one file"));
                    }
                    prefix.clone()
                } else {
                    format!("{prefix}/{name}")
                };
                w.raw_copy_file_rename(e, to).map_err(|e| format!("{prefix}: {e}"))?;
            }
        }
        for (to, p) in &blobs {
            w.start_file(to.as_str(), opts).map_err(|e| e.to_string())?;
            w.write_all(&std::fs::read(p).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
        }
        w.finish().map_err(|e| e.to_string())?;
        Ok(())
    })();
    if let Err(e) = r {
        out.error("zip", e);
    }
    Some(ix)
}

fn mod_report(m: &PackMod, dir: Option<&Path>, out: &mut Out) -> ModReport {
    let mut r = ModReport {
        kind: m.kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone(), name: m.name.clone(), authors: vec![], url: None,
        license: None, updates: None, code: CodeScan::default(), capabilities: Capabilities::default(), blocked: false,
    };
    let Some(dir) = dir else { return r };
    match m.kind {
        ModKind::Kiln => {
            if let Ok(k) = crate::manifest::read_kiln(dir) {
                r.name = Some(k.name.clone());
                r.authors = k.authors.clone();
                r.url = k.url.clone();
                r.license = k.license.clone();
                r.updates = k.updates.clone();
                r.capabilities = Capabilities { features: k.features.clone(), advanced: k.advanced, boot: k.boot, writes: None };
            }
            if let Ok(t) = std::fs::read(dir.join(crate::hash::MOD_MANIFEST))
                && let Ok(v) = crate::jsonc::parse(&crate::manifest::decode_text(&t))
            {
                r.capabilities.writes = v.get("writes").cloned();
            }
        }
        ModKind::Native => {
            if let Ok(n) = crate::manifest::read_native(dir) {
                r.name = n.name.clone();
                r.authors = match n.authors { Some(crate::manifest::OneOrMany::One(a)) => vec![a], Some(crate::manifest::OneOrMany::Many(v)) => v, None => vec![] };
                r.url = n.url.clone();
                r.license = n.license.clone();
                r.updates = n.updates.clone();
            }
        }
        ModKind::Plugin => {}
        _ => {
            if let Some(v) = crate::engine_mods::manifest_json(dir) {
                let s = |k: &str| v.get(k).and_then(Value::as_str).map(str::to_string);
                r.name = s("name").or(r.name);
                r.authors = match v.get("authors").or_else(|| v.get("author")) {
                    Some(Value::String(a)) => vec![a.clone()],
                    Some(Value::Array(a)) => a.iter().filter_map(Value::as_str).map(str::to_string).collect(),
                    _ => vec![],
                };
                r.url = s("url").or_else(|| s("homepage")).or_else(|| s("website"));
                r.license = s("license");
            }
        }
    }
    r.code = scan_code(m, dir);
    let id = Some(m.id.as_str());
    let c = &r.code;
    let count = c.managed.len() + c.native.len() + c.scripts.len();
    if count > 0 {
        let mut what = Vec::new();
        if !c.managed.is_empty() { what.push(format!("{} .NET assembl{}", c.managed.len(), if c.managed.len() == 1 { "y" } else { "ies" })); }
        if !c.native.is_empty() { what.push(format!("{} native file{}", c.native.len(), if c.native.len() == 1 { "" } else { "s" })); }
        if !c.scripts.is_empty() { what.push(format!("{} script{}", c.scripts.len(), if c.scripts.len() == 1 { "" } else { "s" })); }
        out.add(Level::Note, "code", format!("{} carries code: {}", m.id, what.join(", ")), id, None);
    }
    for t in &c.touches {
        let what = match t.as_str() { "network" => "uses the network", "process" => "can start or write to other programs", "registry" => "reads or writes the Windows registry", _ => t.as_str() };
        let ev: Vec<&str> = c.evidence.iter().filter(|e| e.contains(&format!("[{t}]"))).map(String::as_str).take(3).collect();
        out.add(Level::Warning, &format!("code-{t}"), format!("{} {what} ({})", m.id, ev.join("; ").replace(&format!(" [{t}]"), "")), id, None);
    }
    let caps = &r.capabilities;
    if caps.advanced {
        out.add(Level::Note, "capability", format!("{} uses Kiln's advanced API (\"advanced\": true: raw game memory and hooks)", m.id), id, None);
    }
    if caps.boot {
        out.add(Level::Note, "capability", format!("{} runs at boot (\"boot\")", m.id), id, None);
    }
    if caps.writes.as_ref().is_some_and(|w| !w.is_null()) {
        out.add(Level::Note, "capability", format!("{} writes files outside its config (\"writes\": {})", m.id, caps.writes.as_ref().unwrap()), id, None);
    }
    if !caps.features.is_empty() {
        out.add(Level::Note, "capability", format!("{} needs features: {}", m.id, caps.features.join(", ")), id, None);
    }
    r
}

const NET_DLLS: [&str; 11] = ["ws2_32.dll", "wsock32.dll", "wininet.dll", "winhttp.dll", "urlmon.dll", "dnsapi.dll", "iphlpapi.dll", "mswsock.dll", "httpapi.dll", "websocket.dll", "netapi32.dll"];
const PROCESS_FNS: [&str; 16] = [
    "CreateProcessA", "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW", "CreateProcessWithLogonW", "CreateProcessWithTokenW",
    "ShellExecuteA", "ShellExecuteW", "ShellExecuteExA", "ShellExecuteExW", "WinExec", "CreateRemoteThread", "CreateRemoteThreadEx",
    "WriteProcessMemory", "NtCreateUserProcess", "RtlCreateUserProcess",
];
/// Names a managed assembly's metadata holds when it uses these (type and namespace names).
const MANAGED_MARKERS: [(&str, &str); 9] = [
    ("System.Net.Http", "network"), ("System.Net.Sockets", "network"), ("System.Net.WebSockets", "network"), ("HttpWebRequest", "network"),
    ("WebClient", "network"), ("ProcessStartInfo", "process"), ("RegistryKey", "registry"), ("Microsoft.Win32.Registry", "registry"),
    ("System.Net.NetworkInformation", "network"),
];

fn ext(rel: &str) -> String {
    rel.rsplit('/').next().and_then(|n| n.rsplit_once('.')).map(|(_, x)| x.to_ascii_lowercase()).unwrap_or_default()
}

fn scan_code(m: &PackMod, root: &Path) -> CodeScan {
    let mut c = CodeScan::default();
    let mut files = Vec::new();
    if root.is_file() {
        files.push((root.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(), root.to_path_buf()));
    } else {
        walk(root, "", &mut files, 0);
    }
    for (rel, p) in files {
        let x = ext(&rel);
        match x.as_str() {
            "dll" | "exe" | "asi" | "sys" => {
                let small = std::fs::metadata(&p).is_ok_and(|m| m.len() <= MAX_SCAN);
                let bytes = if small { std::fs::read(&p).ok() } else { None };
                let facts = bytes.as_deref().and_then(|b| crate::pe::code_facts(b).ok());
                match (&facts, &bytes) {
                    (Some(f), Some(b)) if f.managed => {
                        c.managed.push(rel.clone());
                        for (marker, what) in MANAGED_MARKERS {
                            if crate::pe::contains_text(b, marker) {
                                c.touches.insert(what.to_string());
                                c.evidence.push(format!("{rel} references {marker} [{what}]"));
                            }
                        }
                    }
                    (Some(f), _) => {
                        c.native.push(rel.clone());
                        for (dll, fns) in &f.imports {
                            if NET_DLLS.contains(&dll.as_str()) {
                                c.touches.insert("network".into());
                                c.evidence.push(format!("{rel} imports {dll} ({}) [network]", fns.iter().take(4).cloned().collect::<Vec<_>>().join(", ")));
                            }
                            let pf: Vec<&str> = fns.iter().map(String::as_str).filter(|f| PROCESS_FNS.contains(f)).collect();
                            if !pf.is_empty() {
                                c.touches.insert("process".into());
                                c.evidence.push(format!("{rel} imports {} [process]", pf.join(", ")));
                            }
                            let reg: Vec<&str> = fns.iter().map(String::as_str)
                                .filter(|f| f.starts_with("Reg") && (dll == "advapi32.dll" || dll == "kernelbase.dll" || dll.starts_with("api-ms-win-core-registry"))).collect();
                            if !reg.is_empty() {
                                c.touches.insert("registry".into());
                                c.evidence.push(format!("{rel} imports {} [registry]", reg.iter().take(4).cloned().collect::<Vec<_>>().join(", ")));
                            }
                        }
                    }
                    _ => c.native.push(rel.clone()), // too large, or not a PE we read: listed as native code
                }
            }
            "so" | "dylib" => c.native.push(rel),
            "lua" | "js" | "mjs" | "py" | "ps1" | "psm1" | "bat" | "cmd" | "sh" | "vbs" => c.scripts.push(rel),
            _ => {}
        }
    }
    let _ = m;
    c.evidence.truncate(50);
    c
}

fn walk(dir: &Path, rel: &str, out: &mut Vec<(String, PathBuf)>, depth: usize) {
    if depth > 32 || out.len() > MAX_ENTRIES {
        return;
    }
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    let mut v: Vec<_> = rd.flatten().collect();
    v.sort_by_key(|e| e.file_name());
    for e in v {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name } else { format!("{rel}/{name}") };
        match e.file_type() {
            Ok(t) if t.is_dir() => walk(&e.path(), &r, out, depth + 1),
            Ok(t) if t.is_file() => out.push((r, e.path())),
            _ => {}
        }
    }
}

/// The overrides, the server settings and the layer's settings-like files: the secret scan of the build, again.
fn secrets(r: &ReadPack, root: &Path, out: &mut Out) {
    for ((_, path), bytes) in &r.overrides {
        for f in pack::secret_findings(path, &crate::manifest::decode_text(bytes)) {
            out.add(Level::Error, "secret", format!("something private may be in an override: {f}"), None, Some(path));
        }
    }
    if let Some(s) = r.index.server.as_ref().and_then(|s| s.settings.as_ref()) {
        for f in pack::secret_value_findings("server settings", s) {
            out.add(Level::Error, "secret", format!("something private may be in the server settings: {f}"), None, None);
        }
    }
    if let Some(l) = &r.index.layer {
        let mut files = Vec::new();
        walk(&root.join(&l.path), "", &mut files, 0);
        for (rel, p) in files {
            if crate::settings_files::is_settings_extension(&rel) && std::fs::metadata(&p).is_ok_and(|m| m.len() <= 4 << 20)
                && let Ok(b) = std::fs::read(&p)
            {
                for f in pack::secret_findings(&format!("layer/{rel}"), &crate::manifest::decode_text(&b)) {
                    out.add(Level::Error, "secret", format!("something private may be in the pack layer: {f}"), Some(&l.id), Some(&format!("layer/{rel}")));
                }
            }
        }
    }
}

/// The Kiln mods (and the layer) through the resolver on each side they run on: dependencies, incompatible mods,
/// load order cycles, inside the pack.
fn resolve(ix: &PackIndex, root: &Path, opts: &CheckOptions, out: &mut Out) {
    use crate::manifest::RunSide;
    use crate::resolver::{Candidate, Environment, ModState, resolve};
    use pack::Need;
    let kiln = opts.kiln.clone().unwrap_or_else(|| crate::VERSION.to_string());
    let api = opts.api.clone().unwrap_or_else(|| crate::profile::KILN_API.to_string());
    let mut seen = BTreeSet::new();
    for side in [RunSide::Server, RunSide::Client] {
        let mut cands = Vec::new();
        let mut required = BTreeMap::new();
        for m in ix.mods.iter().filter(|m| m.kind == ModKind::Kiln && m.on(&BTreeMap::new())) {
            let need = if side == RunSide::Server { m.env().server } else { m.env().client };
            if need == Need::Unsupported {
                continue;
            }
            let dir = root.join(&m.path);
            let (manifest, error) = match crate::manifest::read_kiln(&dir) {
                Ok(k) => (Some(k), None),
                Err(e) => (None, Some(e.message)),
            };
            required.insert(m.id.clone(), m.dep == pack::Dep::Required);
            cands.push(Candidate { dir: m.path.clone(), manifest, error });
        }
        if let Some(l) = &ix.layer {
            let dir = root.join(&l.path);
            if let Ok(k) = crate::manifest::read_kiln(&dir) {
                required.insert(l.id.clone(), true);
                cands.push(Candidate { dir: l.path.clone(), manifest: Some(k), error: None });
            }
        }
        if cands.is_empty() {
            continue;
        }
        let env = Environment { kiln: kiln.clone(), api: api.clone(), side, unavailable_features: vec![], loaded: vec![], hosting: side == RunSide::Client };
        let res = match resolve(&cands, &env) {
            Ok(r) => r,
            Err(e) => {
                out.add(Level::Warning, "resolve", format!("the load order could not be worked out: {}", e.message), None, None);
                continue;
            }
        };
        for r in res.rejected {
            if r.state == ModState::Skipped || !seen.insert((r.id.clone(), r.reason.clone())) {
                continue;
            }
            // a mod newer than this checker's Kiln is the player's launcher's question, not the pack's fault
            let newer_kiln = r.reason.starts_with("needs Kiln");
            let level = if newer_kiln || !required.get(&r.id).copied().unwrap_or(true) { Level::Warning } else { Level::Error };
            let on = if side == RunSide::Server { "servers" } else { "game clients" };
            out.add(level, "resolve", format!("{} does not load on {on}: {}", r.id, r.reason), Some(&r.id), None);
        }
    }
}

/// Kiln mods' data packs in load order (the creator's order, else the index's), the layer last.
fn data_conflicts(ix: &PackIndex, root: &Path, opts: &CheckOptions) -> (Vec<DataConflict>, bool) {
    let kiln: Vec<&PackMod> = ix.mods.iter().filter(|m| m.kind == ModKind::Kiln).collect();
    let mut order: Vec<&PackMod> = ix.order.kiln.iter().filter_map(|id| kiln.iter().find(|m| &m.id == id).copied()).collect();
    for m in &kiln {
        if !order.iter().any(|o| o.id == m.id) {
            order.push(m);
        }
    }
    let mut packs: Vec<conflicts::ModPacks> = order.iter().map(|m| conflicts::read_mod(&m.id, &root.join(&m.path))).collect();
    if let Some(l) = &ix.layer {
        packs.push(conflicts::read_mod(&l.id, &root.join(&l.path)));
    }
    packs.retain(|p| !p.files.is_empty());
    conflicts::analyze_with(&packs, &opts.lint)
}

/// kiln lint on the layer with the pack's Kiln mods as its profile (DESIGN.md 6.1). Errors block; warnings show.
fn lint_layer(ix: &PackIndex, root: &Path, opts: &CheckOptions, out: &mut Out) {
    let Some(l) = &ix.layer else { return };
    let layer = root.join(&l.path);
    if !layer.is_dir() {
        return;
    }
    let mut lo = opts.lint.clone();
    let mods = root.join(pack::MODS_DIR).join("kiln");
    lo.profile = mods.is_dir().then_some(mods);
    let rep = crate::lint::lint_paths(&[layer], &lo);
    let prefix = format!("{}/", root.display());
    for d in rep.diagnostics {
        let level = match d.severity {
            crate::lint::Severity::Error => Level::Error,
            crate::lint::Severity::Warning => Level::Warning,
            _ => continue,
        };
        if d.code == crate::lint::codes::DATA_CONFLICT {
            continue; // the pack's own conflict analysis lists every pair
        }
        let file = d.file.strip_prefix(&prefix).unwrap_or(&d.file).to_string();
        out.add(level, &d.code, format!("{} ({}:{})", d.message, file, d.line), Some(&l.id), Some(&file));
    }
}

fn https(s: &str) -> Option<String> {
    let t = s.trim();
    (t.starts_with("https://") && t.len() > 8 && !t.contains(char::is_whitespace)).then(|| t.to_string())
}

/// Credits, first match wins: Kiln's own list, earlier credits on Crucible, GitHub / Nexus from the manifest's
/// "updates" (named, not fetched), the manifest's authors and url, the creator's credit, nothing.
pub fn identify(ix: &PackIndex, mods: &[ModReport], opts: &CheckOptions) -> Vec<CreditRow> {
    ix.mods.iter().map(|m| {
        let mr = mods.iter().find(|r| r.id == m.id && r.kind == m.kind);
        let row = |author: Option<String>, source: Option<String>, kind: &str, conf: &str| CreditRow {
            kind: m.kind, id: m.id.clone(), version: m.version.clone(), kdh1: m.hash.clone(), author, source, match_kind: kind.into(),
            confidence: conf.into(), nexus_mod_id: None, flag: None,
        };
        let creator = m.credit.as_ref();
        let mut r = (|| {
            if let Some(own) = &opts.own
                && let Some(o) = own.mods.iter().find(|o| o.id == m.id && o.kind == m.kind.as_str())
            {
                let sure = o.kdh1.iter().any(|h| h.eq_ignore_ascii_case(&m.hash));
                return row(Some(own.author.clone()), https(&o.source), "kiln", if sure { "certain" } else { "likely" });
            }
            let known: Vec<&KnownCredit> = opts.known.iter().filter(|k| k.kdh1.eq_ignore_ascii_case(&m.hash)).collect();
            if let Some(k) = known.first() {
                let differ = known.iter().any(|o| o.author != k.author || o.source != k.source);
                let mut r = row(Some(k.author.clone()), https(&k.source), "crucible", if differ { "conflict" } else { "likely" });
                if differ {
                    r.flag = Some("earlier packs credit this mod differently".into());
                }
                return r;
            }
            let authors = mr.map(|r| r.authors.join(", ")).filter(|a| !a.is_empty());
            match mr.and_then(|r| r.updates.as_ref()) {
                Some(UpdatesSpec::Github { repository, .. }) => {
                    let owner = repository.split('/').next().unwrap_or_default().to_string();
                    return row(authors.or(Some(owner)), Some(format!("https://github.com/{repository}")), "github", "unverified");
                }
                Some(UpdatesSpec::Nexus { mod_id, .. }) => {
                    let mut r = row(authors, Some(format!("https://www.nexusmods.com/enshrouded/mods/{mod_id}")), "nexus", "unverified");
                    r.nexus_mod_id = Some(*mod_id);
                    return r;
                }
                None => {}
            }
            let url = mr.and_then(|r| r.url.as_deref()).and_then(https);
            if authors.is_some() || url.is_some() {
                return row(authors, url, "manifest", "unconfirmed");
            }
            if let Some(c) = creator {
                return row(Some(c.author.clone()), https(&c.source), "manual", "creator");
            }
            row(None, None, "manual", "none")
        })();
        // the creator's own credit that names someone else than an earlier, confirmed one
        if let Some(c) = creator
            && r.match_kind == "crucible"
            && r.source.as_deref() != Some(c.source.as_str())
            && r.flag.is_none()
        {
            r.flag = Some(format!("the creator credits {} ({}), earlier packs {}", c.author, c.source, r.source.clone().unwrap_or_default()));
        }
        if r.nexus_mod_id.is_none()
            && let Some(id) = r.source.as_deref().and_then(nexus_id)
        {
            r.nexus_mod_id = Some(id);
        }
        r
    }).collect()
}

/// The Nexus mod id of an Enshrouded mod page URL.
fn nexus_id(url: &str) -> Option<u64> {
    let rest = url.strip_prefix("https://www.nexusmods.com/enshrouded/mods/").or_else(|| url.strip_prefix("https://nexusmods.com/enshrouded/mods/"))?;
    rest.split(['/', '?', '#']).next()?.parse().ok()
}

#[cfg(test)]
#[path = "check_tests.rs"]
mod tests;
