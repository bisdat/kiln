// SPDX-License-Identifier: MIT
//! The pre-select inventory: every ShroudForge package folder under `<game>/mods`, discovered through
//! sfi-package the way Kiln's interpreter reads them (ShroudForge 1.7.9 schema dialect, the compatibility-profile
//! overlay, SF's discovery and startup planner), classified (dialect, side, Lua, startup data, DLLs, save-sensitive per
//! Kiln's shipped policy), with two kinds of findings kept apart:
//!
//! - **blockers** are authoritative: a manifest ShroudForge refuses, a package that needs 1.8 (held), SF's own plan
//!   issues (missing or disabled dependency, version, cycle, wrong side, compatibility rule), the DLL Manager
//!   selection's withdrawals and the read-only import gate's refused / needs-contract verdicts, and every planned
//!   package that depends on a blocked one. Only these may keep a package out of a `kiln` launch plan.
//! - **advisories** are warnings and never change eligibility: the static Lua scan (`lua`), what the parity catalog
//!   says about each API reference (`parity`), uncertainty the scan could not resolve, XHL-Core interplay.
//!
//! Nothing here executes Lua, loads a DLL,
//! opens a network connection or writes a file. Nothing is read from `<game>/shroudforge`: Kiln mode never touches
//! ShroudForge's own data. The caller passes Kiln-owned inputs (compatibility profiles, rules, DLL Manager settings,
//! the native policy, the parity catalog); where they live is decided by the callers. Not decided here: the backend
//! (one backend for all packages), how a later profile change re-plans, the loud warning text.
pub mod lua;
pub mod native;
pub mod parity;
/// Kiln's save-sensitive policy reader and matcher, shared with kiln-sfi (one definition; generic on purpose, see its
/// header). A later shared crate can take it over unchanged.
#[path = "../../../sfi/kiln-sfi/src/state/safety.rs"]
pub mod safety;

use crate::manifest::RunSide;
use native::{Dll, Folder, NativeConfig};
use parity::{Answer, CatalogInput, Coverage};
use sfi_package::package::discover::{self, DiscoveryReport, PackageRecord};
use sfi_package::package::manifest::Capability;
use sfi_package::package::plan::{self, Plan};
use sfi_package::package::profiles::{self, CompatibilityRules, ProfileCatalog};
use sfi_package::package::{ParseOptions, Target};
use sfi_package::source::{DiskSource, PackageSource, SourceKind, SourceVersion};
use sfi_policy::native_selection::{ExecutionPlan, Manifest as NativeManifest, PluginIni};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Component, Path};
use std::rc::Rc;

/// The shipped save-sensitive policy (`schema/sfi/sfi-safety-policy.json`).
pub const SHIPPED_POLICY: &[u8] = include_bytes!("../../../../schema/sfi/sfi-safety-policy.json");
pub fn shipped_policy() -> Result<safety::Policy, String> {
    safety::Policy::parse(SHIPPED_POLICY)
}

/// Lua budgets per package: files and bytes read from `src/`.
const MAX_LUA_FILES: usize = 2048;
const MAX_LUA_BYTES: usize = 16 * 1024 * 1024;

pub struct Inputs<'a> {
    /// The game folder. Packages are read from `<game>/mods` only (the launch plan's `mods/<folder>` roots).
    pub game: &'a Path,
    pub side: RunSide,
    /// Kiln's compatibility-profile folder, if any. A folder inside `<game>/shroudforge` is never read.
    pub profiles: Option<&'a Path>,
    /// Compatibility rules the caller read, if any.
    pub rules: Option<&'a CompatibilityRules>,
    pub native: &'a NativeConfig,
    pub policy: &'a safety::Policy,
    pub catalog: CatalogInput<'a>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Kind {
    /// ShroudForge's own package format (an extended.mod.json that is not EML's, or a ShroudForge `$schema`).
    Shroudforge,
    /// An EML-style package in ShroudForge's mods folder (no extended.mod.json, or one whose launcher is EML).
    Eml,
    /// A folder without mod.json whose DLLs the DLL Manager would still select.
    NativeSelection,
}

/// Where the package stands for this side, before any finding.
#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Presence {
    OnSide,
    OtherSide,
    Disabled,
    /// The manifest could not be read: enabled and side are unknown.
    Unknown,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Dependency {
    pub id: String,
    pub range: String,
    pub optional: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Parts {
    /// Lua under `src/`.
    pub lua: bool,
    /// `src/mod.lua`, the entry ShroudForge runs.
    pub lua_entry: bool,
    /// Startup data (the `patch` capability, or `export` without a runtime): ShroudForge's requires_pregame.
    pub data: bool,
    /// A runtime part (`runtime` / `runtime-register-dll`).
    pub runtime: bool,
    /// Native code: a selected DLL, an enabled native-plugin.ini, or `runtime-register-dll`.
    pub native: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveSensitive {
    /// policy, unknown-mixed or none.
    pub basis: &'static str,
    pub row_id: Option<String>,
    pub row_version: Option<String>,
    pub text: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Blocker {
    /// discovery, requires-1.8, plan:<SF code>, native-selection, native-gate, dependency-skipped.
    pub code: String,
    pub detail: String,
    /// The dependency chain to the package that failed (empty for that one).
    pub caused_by: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Advisory {
    /// lua-scan, lua-uncertain:<class>, api-unproven, api-absent, api-1.8-only, api-excluded, api-not-a-global,
    /// api-coverage-unknown, require, register-dll, xhl, native.
    pub class: String,
    pub detail: String,
    pub file: Option<String>,
    pub line: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackageEntry {
    /// The folder under `<game>/mods`.
    pub directory: String,
    pub id: Option<String>,
    pub name: Option<String>,
    pub version: Option<String>,
    pub kind: Kind,
    pub enabled: bool,
    pub targets: Vec<&'static str>,
    pub presence: Presence,
    pub capabilities: Vec<String>,
    pub dependencies: Vec<Dependency>,
    pub parts: Parts,
    pub dlls: Vec<Dll>,
    /// XHL-Core's own plugin (it may load the DLL itself, whatever Kiln decides).
    pub xhl_plugin: bool,
    pub save_sensitive: SaveSensitive,
    pub requires180: bool,
    /// SF's load order position for this side, when planned.
    pub planned: bool,
    pub blockers: Vec<Blocker>,
    pub advisories: Vec<Advisory>,
}
impl PackageEntry {
    /// No authoritative blocker. Advisories never change this.
    pub fn eligible(&self) -> bool {
        self.blockers.is_empty()
    }
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Inventory {
    pub side: &'static str,
    /// Always `mods`: the game-relative root every package lives in.
    pub mods_root: &'static str,
    /// sf179: the schema dialect and the catalog revision.
    pub dialect: &'static str,
    pub catalog: Coverage,
    pub policy_revision: u64,
    /// every-backend or interpreter-only (every-backend is the rule; the shipped file still says what it says).
    pub policy_scope: &'static str,
    /// SF's load order of the planned packages for this side.
    pub load_order: Vec<String>,
    pub warnings: Vec<String>,
    pub packages: Vec<PackageEntry>,
}

/// True when `path` lies in `<game>/shroudforge` (any letter case, as on Windows), compared lexically and, when both
/// resolve, after resolving links.
pub fn inside_shroudforge(game: &Path, path: &Path) -> bool {
    let lexical = |p: &Path| -> Vec<String> {
        p.components()
            .filter_map(|c| match c {
                Component::Normal(s) => Some(s.to_string_lossy().to_ascii_lowercase()),
                _ => None,
            })
            .collect()
    };
    let under = |base: &Path, p: &Path| {
        let base = lexical(base);
        let p = lexical(p);
        p.len() > base.len() && p[..base.len()] == base[..] && p[base.len()] == "shroudforge"
            || p.len() == base.len() + 1
                && p[..base.len()] == base[..]
                && p[base.len()] == "shroudforge"
    };
    if under(game, path) {
        return true;
    }
    match (game.canonicalize(), path.canonicalize()) {
        (Ok(g), Ok(p)) => under(&g, &p),
        _ => false,
    }
}

fn side_name(side: RunSide) -> &'static str {
    match side {
        RunSide::Client => "client",
        RunSide::Server => "server",
    }
}

fn targets(t: Target) -> Vec<&'static str> {
    match t {
        Target::Client => vec!["client"],
        Target::Server => vec!["server"],
        Target::ClientServer => vec!["client", "server"],
    }
}

fn capability_name(c: Capability) -> String {
    serde_json::to_value(c)
        .ok()
        .and_then(|v| v.as_str().map(str::to_owned))
        .unwrap_or_default()
}

fn kind_of(record: &PackageRecord) -> Kind {
    let docs = &record.parsed().documents;
    match &docs.extension {
        Some(ext) => {
            let eml = ext
                .value()
                .get("launcher")
                .and_then(|v| v.as_str())
                .is_some_and(|s| s.eq_ignore_ascii_case("EML"));
            if eml { Kind::Eml } else { Kind::Shroudforge }
        }
        None => {
            let sf = docs
                .manifest
                .value()
                .get("$schema")
                .and_then(|v| v.as_str())
                .is_some_and(|s| s.to_ascii_lowercase().contains("shroudforge"));
            if sf { Kind::Shroudforge } else { Kind::Eml }
        }
    }
}

fn options(version: SourceVersion) -> ParseOptions {
    ParseOptions {
        version,
        ..ParseOptions::default()
    }
}

/// The raw `"id"` of a folder's mod.json, kept even when the manifest is invalid (ShroudForge keeps it too).
fn raw_id(source: &dyn PackageSource) -> Option<String> {
    let bytes = source.read_file("mod.json").ok()?;
    let value: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
    value.get("id")?.as_str().map(str::to_owned)
}

/// Every `.lua` under `src/` through the package's (overlay) source, in listing order, without following a cycle, and
/// whether any was seen (read or not). `disk` is the package folder: a file whose size on disk is over what is left of
/// the byte budget is not read at all (the source has no size query; an overlay's own files are in memory already).
fn lua_files(
    source: &dyn PackageSource,
    disk: &Path,
    warnings: &mut Vec<Advisory>,
) -> (Vec<(String, String)>, bool) {
    let mut out = Vec::new();
    let mut seen = false;
    let mut bytes = 0usize;
    let mut stack = vec![("src".to_owned(), BTreeSet::from(["src".to_owned()]))];
    while let Some((dir, active)) = stack.pop() {
        let Ok(entries) = source.read_dir(&dir) else {
            continue;
        };
        for entry in entries.into_iter().flatten() {
            if source.is_directory(&entry.path) {
                if active.contains(&entry.path) {
                    warnings.push(advisory(
                        "lua-scan",
                        format!("{}: directory cycle, not followed", entry.path),
                        None,
                        None,
                    ));
                    continue;
                }
                let mut next = active.clone();
                next.insert(entry.path.clone());
                stack.push((entry.path.clone(), next));
            } else if entry.path.ends_with(".lua") && entry.kind != SourceKind::Other {
                seen = true;
                if out.len() >= MAX_LUA_FILES || bytes >= MAX_LUA_BYTES {
                    warnings.push(advisory(
                        "lua-scan",
                        "Lua budget reached: the rest of src/ was not scanned".into(),
                        None,
                        None,
                    ));
                    return (out, seen);
                }
                // Contained (the source refuses a link out of the package) before the size is looked up on disk.
                if source.metadata(&entry.path).is_ok()
                    && let Ok(m) = std::fs::metadata(disk.join(&entry.path))
                    && m.len() > (MAX_LUA_BYTES - bytes) as u64
                {
                    warnings.push(advisory(
                        "lua-scan",
                        format!(
                            "{} bytes, larger than the Lua budget: not scanned; API use unknown",
                            m.len()
                        ),
                        Some(entry.path.clone()),
                        None,
                    ));
                    continue;
                }
                let Ok(content) = source.read_file(&entry.path) else {
                    continue;
                };
                bytes += content.len();
                match String::from_utf8(content) {
                    Ok(text) => out.push((entry.path.clone(), text)),
                    Err(_) => warnings.push(advisory(
                        "lua-scan",
                        "not UTF-8, not scanned".into(),
                        Some(entry.path.clone()),
                        None,
                    )),
                }
            }
        }
    }
    (out, seen)
}

fn advisory(class: &str, detail: String, file: Option<String>, line: Option<u32>) -> Advisory {
    Advisory {
        class: class.to_owned(),
        detail,
        file,
        line,
    }
}

/// Where a literal require lands, as ShroudForge resolves it (sfi-core lua/require.rs): `mods.<owner>[.<path>]` is
/// another package's module, anything else `src/<a/b>.lua` of this package.
enum Target2 {
    Local(String),
    Package(String),
    Bad,
}
fn require_target(module: &str) -> Target2 {
    if let Some(rest) = module.strip_prefix("mods.") {
        let owner = rest.split('.').next().unwrap_or(rest);
        return if sfi_package::package::is_package_id(owner) {
            Target2::Package(owner.to_owned())
        } else {
            Target2::Bad
        };
    }
    let parts: Vec<&str> = module.split('.').collect();
    if parts.iter().any(|p| {
        p.is_empty()
            || *p == ".."
            || p.contains(['/', '\\', ':', '\0'])
            || p.chars().any(char::is_control)
    }) {
        return Target2::Bad;
    }
    Target2::Local(format!("src/{}.lua", parts.join("/")))
}

/// The static Lua scan of one package: references, uncertainty, the literal require closure, catalog answers.
fn scan_lua(
    files: &[(String, String)],
    entry_exists: bool,
    roots: &BTreeSet<String>,
    catalog: Option<&sfi_package::parity::Catalog>,
    registry_ids: &BTreeSet<String>,
    advisories: &mut Vec<Advisory>,
) {
    let mut budget = lua::MAX_TOKENS;
    let mut edges: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let known: BTreeSet<&str> = files.iter().map(|(p, _)| p.as_str()).collect();
    let mut answered: BTreeSet<String> = BTreeSet::new();
    let mut unanswered = 0usize;
    for (path, text) in files {
        let tokens = match lua::tokenize(text, &mut budget) {
            Ok(t) => t,
            Err(e) => {
                advisories.push(advisory(
                    "lua-scan",
                    format!("not scanned: {e}; API use unknown"),
                    Some(path.clone()),
                    None,
                ));
                if budget == 0 {
                    break;
                }
                continue;
            }
        };
        let scan = lua::scan(&tokens, roots);
        for f in &scan.findings {
            let class = serde_json::to_value(f.class)
                .ok()
                .and_then(|v| v.as_str().map(str::to_owned))
                .unwrap_or_default();
            advisories.push(advisory(
                &format!("lua-uncertain:{class}"),
                f.detail.clone(),
                Some(path.clone()),
                Some(f.line),
            ));
        }
        for (module, line) in &scan.requires {
            match require_target(module) {
                Target2::Local(file) => {
                    if !known.contains(file.as_str()) {
                        advisories.push(advisory(
                            "require",
                            format!("require \"{module}\": {file} is not in the package"),
                            Some(path.clone()),
                            Some(*line),
                        ));
                    }
                    edges.entry(path.clone()).or_default().push(file);
                }
                Target2::Package(owner) => {
                    if !registry_ids.contains(&owner) {
                        advisories.push(advisory(
                            "require",
                            format!("require \"{module}\": package {owner} is not installed"),
                            Some(path.clone()),
                            Some(*line),
                        ));
                    }
                }
                Target2::Bad => advisories.push(advisory(
                    "require",
                    format!(
                        "require \"{module}\": not a module path inside a package; not followed"
                    ),
                    Some(path.clone()),
                    Some(*line),
                )),
            }
        }
        for r in &scan.references {
            if r.chain.ends_with(".register_dll")
                && let Some(arg) = &r.literal_arg
            {
                advisories.push(advisory("register-dll", format!("register_dll(\"{arg}\") seen by the scan; a scan hint is not a declared target"), Some(path.clone()), Some(r.line)));
            }
            let Some(catalog) = catalog else {
                unanswered += 1;
                continue;
            };
            if !answered.insert(r.chain.clone()) {
                continue;
            }
            let (class, text) = match parity::assess(catalog, &r.chain) {
                Answer::Supported => continue,
                Answer::Unknown => {
                    unanswered += 1;
                    continue;
                }
                Answer::NotAGlobal(scope) => (
                    "api-not-a-global",
                    format!(
                        "{} names a {} in the catalog, not a global",
                        r.chain,
                        serde_json::to_value(scope)
                            .ok()
                            .and_then(|v| v.as_str().map(str::to_owned))
                            .unwrap_or_default()
                    ),
                ),
                Answer::Unproven(p) => (
                    "api-unproven",
                    format!(
                        "{}: Kiln's parity with ShroudForge 1.7.9 is {}",
                        r.chain,
                        serde_json::to_value(p)
                            .ok()
                            .and_then(|v| v.as_str().map(str::to_owned))
                            .unwrap_or_default()
                    ),
                ),
                Answer::Absent => (
                    "api-absent",
                    format!("{}: ShroudForge 1.7.9 has no such callable", r.chain),
                ),
                Answer::NotInRevision => (
                    "api-1.8-only",
                    format!("{}: only in ShroudForge 1.8", r.chain),
                ),
                Answer::Excluded => (
                    "api-excluded",
                    format!("{}: not provided by Kiln (excluded)", r.chain),
                ),
            };
            advisories.push(advisory(class, text, Some(path.clone()), Some(r.line)));
        }
    }
    if unanswered > 0 {
        let why = if catalog.is_some() {
            "not in the parity catalog"
        } else {
            "no verified parity catalog"
        };
        advisories.push(advisory(
            "api-coverage-unknown",
            format!("{unanswered} API reference(s) not checked: {why}"),
            None,
            None,
        ));
    }
    // Literal require closure from src/mod.lua, and any cycle on the way.
    let mut seen = BTreeSet::new();
    let mut path = Vec::new();
    fn walk(
        at: &str,
        edges: &BTreeMap<String, Vec<String>>,
        seen: &mut BTreeSet<String>,
        path: &mut Vec<String>,
        advisories: &mut Vec<Advisory>,
    ) {
        if let Some(i) = path.iter().position(|p| p == at) {
            let mut cycle = path[i..].to_vec();
            cycle.push(at.to_owned());
            advisories.push(advisory(
                "require",
                format!("require cycle: {}", cycle.join(" -> ")),
                Some(at.to_owned()),
                None,
            ));
            return;
        }
        if !seen.insert(at.to_owned()) {
            return;
        }
        path.push(at.to_owned());
        for next in edges.get(at).into_iter().flatten() {
            walk(next, edges, seen, path, advisories);
        }
        path.pop();
    }
    if known.contains("src/mod.lua") {
        walk("src/mod.lua", &edges, &mut seen, &mut path, advisories);
    } else if !files.is_empty() && !entry_exists {
        advisories.push(advisory(
            "lua-scan",
            "Lua under src/ but no src/mod.lua entry".into(),
            None,
            None,
        ));
    }
}

/// Builds the inventory. Read-only; never fails as a whole: what cannot be read becomes a warning or a blocker.
pub fn inventory(inputs: &Inputs<'_>) -> Inventory {
    let server = inputs.side == RunSide::Server;
    let (catalog, coverage) = parity::load(&inputs.catalog);
    let roots = parity::roots(catalog.as_ref());
    let mut inv = Inventory {
        side: side_name(inputs.side),
        mods_root: "mods",
        dialect: "sf179",
        catalog: coverage.clone(),
        policy_revision: inputs.policy.revision,
        policy_scope: if inputs.policy.every_backend {
            "every-backend"
        } else {
            "interpreter-only"
        },
        load_order: Vec::new(),
        warnings: Vec::new(),
        packages: Vec::new(),
    };
    if let Some(detail) = &coverage.detail {
        inv.warnings.push(detail.clone());
    }
    // Compatibility profiles: Kiln's folder only.
    let profile_catalog = match inputs.profiles {
        Some(dir) if inside_shroudforge(inputs.game, dir) => {
            inv.warnings.push("compatibility profiles: the folder is inside the game's shroudforge folder; not read".into());
            ProfileCatalog::default()
        }
        Some(dir) => match DiskSource::new(dir).and_then(|s| profiles::read_catalog(&s)) {
            Ok(c) => c,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => ProfileCatalog::default(),
            Err(e) => ProfileCatalog {
                profiles: Vec::new(),
                errors: vec![e.to_string()],
            },
        },
        None => ProfileCatalog::default(),
    };
    let mods = inputs.game.join("mods");
    if !mods.is_dir() {
        inv.warnings
            .push("no mods folder in the game folder".into());
        return inv;
    }
    let root = match DiskSource::new(&mods) {
        Ok(r) => r,
        Err(e) => {
            inv.warnings.push(format!("mods folder: {e}"));
            return inv;
        }
    };
    let open = |e: &sfi_package::source::SourceEntry| -> std::io::Result<Rc<dyn PackageSource>> {
        Ok(Rc::new(DiskSource::new(mods.join(&e.path))?) as Rc<dyn PackageSource>)
    };
    let report: DiscoveryReport = match discover::discover_source(
        &root,
        open,
        options(SourceVersion::Sf179),
        &profile_catalog,
        &inputs.native.selections,
    ) {
        Ok(r) => r,
        Err(e) => {
            inv.warnings.push(format!("mods folder: {e}"));
            return inv;
        }
    };
    for e in &report.profile_errors {
        inv.warnings.push(format!("compatibility profiles: {e}"));
    }
    let registry = &report.registry;
    let sf_plan: Plan = plan::startup_plan(registry, server, inputs.rules);
    inv.load_order = sf_plan.ids.clone();
    let mut plan_issues: BTreeMap<String, Vec<&plan::PlanIssue>> = BTreeMap::new();
    for issue in &sf_plan.issues {
        plan_issues
            .entry(issue.mod_id.clone())
            .or_default()
            .push(issue);
    }
    let execution = ExecutionPlan {
        order: sf_plan.ids.clone(),
        issues: sf_plan.issues.iter().fold(BTreeMap::new(), |mut m, i| {
            m.entry(i.mod_id.clone())
                .or_insert_with(|| i.detail.clone());
            m
        }),
        dependencies: sf_plan
            .ids
            .iter()
            .filter_map(|id| registry.get(id))
            .map(|r| {
                let m = &r.parsed().manifest;
                (
                    m.id.clone(),
                    m.dependencies.iter().map(|d| d.id.clone()).collect(),
                )
            })
            .collect(),
        error: sf_plan
            .issues
            .iter()
            .find(|i| i.mod_id == "__loader__")
            .map(|i| i.detail.clone()),
    };
    let mut errors_by_dir: BTreeMap<String, Vec<&discover::DiscoveryIssue>> = BTreeMap::new();
    for e in &report.errors {
        if e.directory.is_empty() {
            inv.warnings.push(format!("mods folder: {}", e.detail));
        } else {
            errors_by_dir
                .entry(e.directory.clone())
                .or_default()
                .push(e);
        }
    }
    let by_dir: BTreeMap<&str, &PackageRecord> =
        registry.values().map(|r| (r.directory(), r)).collect();
    let registry_ids: BTreeSet<String> = registry
        .values()
        .map(|r| r.parsed().manifest.id.clone())
        .collect();
    // Every installed folder, as the DLL Manager sees them.
    let mut folders: Vec<Folder> = Vec::new();
    for entry in root.read_dir("").into_iter().flatten().flatten() {
        if entry.name.starts_with('.') || entry.kind != SourceKind::Directory {
            continue;
        }
        let dir = mods.join(&entry.path);
        let Ok(source) = DiskSource::new(&dir) else {
            continue;
        };
        let has_manifest = source.metadata("mod.json").is_ok();
        let manifest = match by_dir.get(entry.name.as_str()) {
            Some(r) => {
                let m = &r.parsed().manifest;
                NativeManifest::Valid {
                    id: m.id.clone(),
                    enabled: m.enabled,
                    target: native::manifest_target(m.target),
                }
            }
            None if has_manifest => {
                let id = raw_id(&source);
                let error = errors_by_dir
                    .get(&entry.name)
                    .and_then(|e| e.first())
                    .map(|e| e.detail.clone())
                    .unwrap_or_default();
                NativeManifest::Invalid { id, error }
            }
            None => NativeManifest::Missing,
        };
        let mod_id = match &manifest {
            NativeManifest::Valid { id, .. } => Some(id.as_str()),
            NativeManifest::Invalid { id, .. } => id.as_deref(),
            NativeManifest::Missing => None,
        };
        let profile = native::profile_for(
            &profile_catalog,
            &inputs.native.selections,
            &entry.name,
            mod_id,
        );
        folders.push(Folder {
            directory: entry.name.clone(),
            ini: native::read_ini(&dir),
            files: native::list_files(&dir),
            profile,
            manifest,
        });
    }
    let selection = native::select(inputs.native, server, &folders, &execution);
    for folder in &folders {
        let dir = mods.join(&folder.directory);
        let record = by_dir.get(folder.directory.as_str()).copied();
        let has_dll_or_ini = folder.ini != PluginIni::Missing
            || folder
                .files
                .iter()
                .any(|f| f.to_ascii_lowercase().ends_with(".dll"))
            || folder.profile.is_some();
        if record.is_none() && matches!(folder.manifest, NativeManifest::Missing) && !has_dll_or_ini
        {
            continue;
        }
        let mut entry = PackageEntry {
            directory: folder.directory.clone(),
            id: None,
            name: None,
            version: None,
            kind: Kind::NativeSelection,
            enabled: false,
            targets: Vec::new(),
            presence: Presence::Unknown,
            capabilities: Vec::new(),
            dependencies: Vec::new(),
            parts: Parts::default(),
            dlls: Vec::new(),
            xhl_plugin: crate::engine_mods::has_xhl_plugin_ini(&dir)
                && crate::engine_mods::is_xhl_plugin(&dir),
            save_sensitive: SaveSensitive {
                basis: "none",
                row_id: None,
                row_version: None,
                text: None,
            },
            requires180: false,
            planned: false,
            blockers: Vec::new(),
            advisories: Vec::new(),
        };
        let mut has_data = false;
        if let Some(r) = record {
            let m = &r.parsed().manifest;
            entry.id = Some(m.id.clone());
            entry.name = Some(m.name.clone());
            entry.version = Some(m.version.to_string());
            entry.kind = kind_of(r);
            entry.enabled = m.enabled;
            entry.targets = targets(m.target);
            entry.presence = if !m.enabled {
                Presence::Disabled
            } else if m.target.supports_process(server) {
                Presence::OnSide
            } else {
                Presence::OtherSide
            };
            entry.capabilities = m
                .capabilities
                .iter()
                .copied()
                .map(capability_name)
                .collect();
            entry.dependencies = m
                .dependencies
                .iter()
                .map(|d| Dependency {
                    id: d.id.clone(),
                    range: d.version.to_string(),
                    optional: d.optional.unwrap_or(false),
                })
                .collect();
            entry.planned = sf_plan.ids.contains(&m.id);
            has_data = m.requires_pregame();
            entry.parts.data = has_data;
            entry.parts.runtime = m.capabilities.iter().any(|c| c.requires_runtime());
            for issue in plan_issues.get(&m.id).into_iter().flatten() {
                entry.blockers.push(Blocker {
                    code: format!("plan:{}", issue.code),
                    detail: issue.detail.clone(),
                    caused_by: Vec::new(),
                });
            }
            let source = r.runtime().source.as_ref();
            let (files, any_lua) = lua_files(source, &dir, &mut entry.advisories);
            entry.parts.lua = any_lua;
            entry.parts.lua_entry = source.is_file("src/mod.lua");
            scan_lua(
                &files,
                entry.parts.lua_entry,
                &roots,
                catalog.as_ref(),
                &registry_ids,
                &mut entry.advisories,
            );
        } else if let NativeManifest::Invalid { id, error } = &folder.manifest {
            entry.id = id.clone();
            entry.kind = Kind::Shroudforge;
            for e in errors_by_dir.get(&folder.directory).into_iter().flatten() {
                entry.blockers.push(Blocker {
                    code: "discovery".into(),
                    detail: e.detail.clone(),
                    caused_by: Vec::new(),
                });
            }
            if entry.blockers.is_empty() {
                entry.blockers.push(Blocker {
                    code: "discovery".into(),
                    detail: error.clone(),
                    caused_by: Vec::new(),
                });
            }
            // A manifest 1.7.9 reads on its own was dropped by discovery as a duplicate ID (SF drops every copy): it does
            // not need 1.8. Only a manifest the 1.8 schema alone accepts needs 1.8, which stays held.
            let read = |version| {
                DiskSource::new(&dir).ok().and_then(|source| {
                    discover::read_package(
                        &folder.directory,
                        Rc::new(source),
                        options(version),
                        &profile_catalog,
                        profiles::selection_for_package(
                            &inputs.native.selections,
                            &folder.directory,
                        ),
                    )
                    .ok()
                })
            };
            if let Some(r) = read(SourceVersion::Sf179) {
                let m = &r.parsed().manifest;
                entry.id = Some(m.id.clone());
                entry.name = Some(m.name.clone());
                entry.version = Some(m.version.to_string());
                entry.kind = kind_of(&r);
                if errors_by_dir
                    .get(&folder.directory)
                    .is_none_or(Vec::is_empty)
                {
                    entry.blockers = vec![Blocker {
                        code: "discovery".into(),
                        detail: format!("duplicate mod ID: {}", m.id),
                        caused_by: Vec::new(),
                    }];
                }
            } else if let Ok(source) = DiskSource::new(&dir) {
                let held = discover::read_package(
                    &folder.directory,
                    Rc::new(source),
                    options(SourceVersion::Sf180),
                    &profile_catalog,
                    profiles::selection_for_package(&inputs.native.selections, &folder.directory),
                );
                if let Ok(r) = held {
                    entry.requires180 = true;
                    entry.name = Some(r.parsed().manifest.name.clone());
                    entry.version = Some(r.parsed().manifest.version.to_string());
                    entry.kind = kind_of(&r);
                    entry.blockers.insert(0, Blocker {
                        code: "requires-1.8".into(),
                        detail: "uses ShroudForge 1.8 features; Kiln runs ShroudForge 1.7.9 packages only".into(),
                        caused_by: Vec::new(),
                    });
                }
            }
        }
        // Native: the selection's entries for this folder, each through the gate.
        let key = folder.directory.to_ascii_lowercase();
        for e in selection
            .entries
            .iter()
            .filter(|e| e.package_directory.eq_ignore_ascii_case(&folder.directory))
        {
            let dll = native::gate(inputs.native, &dir, &folder.files, e);
            if !dll.eligible {
                entry.blockers.push(Blocker {
                    code: "native-gate".into(),
                    detail: format!("{}: {} ({})", dll.path, dll.verdict, dll.reasons.join("; ")),
                    caused_by: Vec::new(),
                });
            } else if !dll.reasons.is_empty() {
                entry.advisories.push(advisory(
                    "native",
                    format!("{}: {} ({})", dll.path, dll.verdict, dll.reasons.join("; ")),
                    Some(dll.path.clone()),
                    None,
                ));
            }
            entry.dlls.push(dll);
        }
        for issue in selection.issues.iter().filter(|i| {
            i.status_id
                .strip_prefix("dll:")
                .is_some_and(|s| s.starts_with(&format!("{key}:")))
        }) {
            if entry
                .id
                .as_ref()
                .is_none_or(|id| !selection.is_withdrawn(id))
            {
                entry
                    .advisories
                    .push(advisory("native", issue.detail.clone(), None, None));
            }
        }
        if let Some(id) = &entry.id {
            for w in selection.withdrawn.iter().filter(|w| &w.mod_id == id) {
                entry.blockers.push(Blocker {
                    code: "native-selection".into(),
                    detail: w.reason.clone(),
                    caused_by: w.caused_by.clone(),
                });
            }
        }
        entry.parts.native = !entry.dlls.is_empty()
            || matches!(folder.ini, PluginIni::Enabled { .. })
            || entry
                .capabilities
                .iter()
                .any(|c| c == "runtime-register-dll");
        if folder.ini != PluginIni::Missing && entry.xhl_plugin {
            entry.advisories.push(advisory("xhl", "an XHL-Core plugin: with XHL-Core installed it may load this DLL itself, whatever Kiln decides".into(), None, None));
        }
        if let (Some(id), Some(version)) = (&entry.id, &entry.version) {
            let files = safety::PackageFiles {
                id,
                version,
                dir: &dir,
                has_data,
                has_native: entry.parts.native,
            };
            entry.save_sensitive = match inputs.policy.classify(&files) {
                safety::Verdict::Policy { id, version, text } => SaveSensitive {
                    basis: "policy",
                    row_id: Some(id),
                    row_version: Some(version),
                    text: Some(text),
                },
                safety::Verdict::UnknownMixed => SaveSensitive {
                    basis: "unknown-mixed",
                    row_id: None,
                    row_version: None,
                    text: None,
                },
                safety::Verdict::NotSensitive => SaveSensitive {
                    basis: "none",
                    row_id: None,
                    row_version: None,
                    text: None,
                },
            };
        }
        inv.packages.push(entry);
    }
    // A planned package that needs a blocked one is skipped with it (the package runs whole or not at all).
    let blocked: BTreeSet<String> = inv
        .packages
        .iter()
        .filter(|p| !p.eligible())
        .filter_map(|p| p.id.clone())
        .collect();
    let deps: BTreeMap<String, Vec<String>> = inv
        .packages
        .iter()
        .filter_map(|p| {
            Some((
                p.id.clone()?,
                p.dependencies.iter().map(|d| d.id.clone()).collect(),
            ))
        })
        .collect();
    for p in inv
        .packages
        .iter_mut()
        .filter(|p| p.blockers.is_empty() && p.planned)
    {
        let Some(id) = p.id.clone() else { continue };
        if let Some(chain) = chain_to_blocked(&id, &deps, &blocked, &mut BTreeSet::new()) {
            let failed = chain.last().cloned().unwrap_or_default();
            p.blockers.push(Blocker {
                code: "dependency-skipped".into(),
                detail: format!("needs {failed}, which is skipped"),
                caused_by: chain,
            });
        }
    }
    inv.packages
        .sort_by_key(|p| p.directory.to_ascii_lowercase());
    inv
}

fn chain_to_blocked(
    id: &str,
    deps: &BTreeMap<String, Vec<String>>,
    blocked: &BTreeSet<String>,
    seen: &mut BTreeSet<String>,
) -> Option<Vec<String>> {
    if !seen.insert(id.to_owned()) {
        return None;
    }
    for dep in deps.get(id).into_iter().flatten() {
        if blocked.contains(dep) {
            return Some(vec![dep.clone()]);
        }
        if let Some(mut rest) = chain_to_blocked(dep, deps, blocked, seen) {
            rest.insert(0, dep.clone());
            return Some(rest);
        }
    }
    None
}
