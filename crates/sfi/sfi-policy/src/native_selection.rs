// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc; identical at 1.8.0 38ef448d): modules/modloader/dll_manager.rs:76-123
// (exclusions), :207-322 (resolve_load_plan), :323-568 (inspect_packages), :571-623
// (read_plugin_ini), :823-860 (resolve_package_dll_path); modules/mod-runtime/native_dlls.rs:122-196
// (queue: selection, exclusion and process-key dedupe); session/native_plugins.rs:12-67 and
// lua/eml/v1/create_loader_api.rs:31-45 (register_dll).
// Adaptations: no filesystem access (the caller supplies the facts, see `Package`); chain entries
// are left to Kiln's proxy; a selected DLL that cannot be established withdraws its package and
// every planned dependent; declared register_dll targets are established before startup; Kiln's
// native.policy can refuse every package DLL (kiln mode only).
//! Which package DLLs Kiln's interpreter would load, decided before anything runs.
//!
//! This is ShroudForge's DLL Manager selection (source precedence manual > profile > ini, the
//! manager switch, approvals, exclusions, targets, the execution plan's ranks and the
//! case-insensitive process key) over Kiln-owned inputs. It never reads a ShroudForge folder or
//! config, never touches the filesystem and never loads anything; kiln-manage's plan producer
//! and kiln-sfi's executor call the same function, so they cannot disagree.
//!
//! Kiln rules on top of the source:
//! - `dinput8-chain.ini` `[chain]` entries are not selected: Kiln's proxy already loads that file
//!   (first loader wins). A chained DLL that is also a package DLL is foreign-owned
//!   at load time, never initialized twice.
//! - A package must run whole: a DLL it selected that cannot be established (a
//!   plan issue in ShroudForge, where the Lua part would run without it) withdraws the package and
//!   every planned package that depends on it, with the reason.
//! - `register_dll` targets are established before startup from the caller's
//!   declarations (static literals, reviewed contracts). An unestablished declared target
//!   withdraws the package; at runtime [`RegistrationTable`] only reuses established targets.
//! - Kiln's own kiln.json `native.policy` ([`NativePermission`]) applies to package
//!   DLLs as to Kiln's native mods: when it refuses (`never`, or `verified-only` on a build that is
//!   not verified), no DLL is selected, and every planned package that would load or declares one
//!   is withdrawn with Kiln's words, with its dependents (through Lua-only packages too).
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

/// DLL Manager selection target (`DllProfileTarget`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Target {
    Client,
    Server,
    #[default]
    Both,
}
impl Target {
    pub fn applies_to(self, server: bool) -> bool {
        matches!(
            (self, server),
            (Self::Client, false) | (Self::Server, true) | (Self::Both, _)
        )
    }
}
/// How the DLL is started (`DllLoadMode`). `Dinput8` registers a DirectInput8 provider.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Mode {
    #[default]
    Library,
    Dinput8,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DllFile {
    pub path: String,
    pub mode: Mode,
}
/// One ordered DLL Manager selection (discovery's `DllSelection`; the caller maps it).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Selection {
    pub package_directory: String,
    pub enabled: bool,
    pub approved: bool,
    pub target: Target,
    pub compatibility_profile: Option<String>,
    pub files: Vec<DllFile>,
}
/// The compatibility profile discovery's `select_profile` chose for a package. The caller passes the
/// selection's non-empty `compatibilityProfile` as `selected`, so `None` with a named profile
/// means "not found".
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Profile {
    pub id: String,
    pub target: Target,
    pub files: Vec<DllFile>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestTarget {
    Client,
    Server,
    Both,
}
/// The package's `mod.json` as the package model parsed it (with any profile overlay applied).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Manifest {
    Missing,
    /// `id` is the raw document's `"id"` string, which ShroudForge keeps even when invalid.
    Invalid {
        id: Option<String>,
        error: String,
    },
    Valid {
        id: String,
        enabled: bool,
        target: ManifestTarget,
    },
}
/// `native-plugin.ini` `[Plugin]` state (read_plugin_ini).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PluginIni {
    Missing,
    Disabled,
    Enabled { dll: String },
    Invalid,
}
impl PluginIni {
    /// Parse the file's text. A missing file is [`PluginIni::Missing`]; any other read error is
    /// [`PluginIni::Invalid`] (the caller decides which).
    pub fn parse(text: &str) -> Self {
        let (mut section, mut found, mut enabled, mut dll, mut invalid) =
            (false, false, None, None::<String>, false);
        for line in text
            .lines()
            .map(|line| line.trim().trim_start_matches('\u{feff}'))
        {
            if line.is_empty() || line.starts_with([';', '#']) {
                continue;
            }
            if line.starts_with('[') && line.ends_with(']') {
                section = line[1..line.len() - 1]
                    .trim()
                    .eq_ignore_ascii_case("plugin");
                found |= section;
                continue;
            }
            if !section {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                invalid = true;
                continue;
            };
            let value = value.trim().trim_matches(['"', '\'']);
            if key.trim().eq_ignore_ascii_case("enabled") {
                invalid |= enabled.is_some();
                enabled = match value.to_ascii_lowercase().as_str() {
                    "1" | "true" | "yes" | "on" => Some(true),
                    "0" | "false" | "no" | "off" => Some(false),
                    _ => {
                        invalid = true;
                        None
                    }
                };
            } else if key.trim().eq_ignore_ascii_case("dll") {
                invalid |= dll.is_some();
                dll = Some(value.replace('\\', "/"));
            }
        }
        if !found || invalid || (enabled != Some(false) && dll.as_deref().is_none_or(str::is_empty))
        {
            Self::Invalid
        } else if enabled == Some(false) {
            Self::Disabled
        } else {
            Self::Enabled {
                dll: dll.unwrap_or_default(),
            }
        }
    }
}
/// What the caller knows about one installed package folder (`mods/<directory>`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Package {
    pub directory: String,
    pub manifest: Manifest,
    pub ini: PluginIni,
    pub profile: Option<Profile>,
    /// Package-relative paths (`/`-separated) of the folder's regular files, recursively,
    /// symlinks and paths that leave the folder excluded. Matching is case-insensitive.
    pub files: Vec<String>,
}
/// The execution plan (discovery's `startup_plan`) for this process.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ExecutionPlan {
    /// Planned mod ids in load order.
    pub order: Vec<String>,
    /// First plan issue per mod id that is not planned.
    pub issues: BTreeMap<String, String>,
    /// Declared dependency ids per planned mod (optional ones included), for withdrawal.
    pub dependencies: BTreeMap<String, Vec<String>>,
    /// The environment could not be planned at all (`Could not resolve mod dependencies`).
    pub error: Option<String>,
}
/// A `register_dll` target the caller established before startup for a package.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Registration {
    pub package_directory: String,
    pub path: String,
}
/// The DLL Manager's file-name filter (`excludedDllNames`), normalized as ShroudForge reads it:
/// any invalid entry or more than 64 entries drops the whole filter.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Exclusions(BTreeSet<String>);
impl Exclusions {
    pub fn from_config(value: Option<&Value>) -> Self {
        Self(
            value
                .and_then(|v| normalize_excluded(v).ok())
                .unwrap_or_default(),
        )
    }
    pub fn excludes(&self, path: &str) -> bool {
        self.0.contains(&file_name(path).to_ascii_lowercase())
    }
    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.0.iter().map(String::as_str)
    }
}
fn normalize_excluded(value: &Value) -> Result<BTreeSet<String>, String> {
    let entries = value
        .as_array()
        .ok_or("excludedDllNames must be an array")?;
    if entries.len() > 64 {
        return Err("At most 64 DLL exclusions are supported".into());
    }
    let mut names = BTreeSet::new();
    for entry in entries {
        let name = entry
            .as_str()
            .ok_or("DLL exclusions must be text")?
            .trim()
            .to_ascii_lowercase();
        if name.len() > 255
            || !single_folder(&name)
            || name.contains(['*', '?', '"', '<', '>', '|'])
            || !name.ends_with(".dll")
        {
            return Err(format!("Invalid excluded DLL name: {name}"));
        }
        names.insert(name);
    }
    Ok(names)
}
/// kiln.json `native.policy`, Kiln's own switch for native code (`native/core/src/config.h`): the value the core froze
/// in kiln.launch (`KILN_LAUNCH_NATIVE_POLICY_*`), which Kiln's native mods follow the same start.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NativePolicy {
    /// `"always"` (kiln.json's default).
    Always,
    /// `"verified-only"`: only on a game build in the verified-builds list.
    VerifiedOnly,
    /// `"never"`.
    Never,
}
impl NativePolicy {
    /// kiln.launch's `KILN_LAUNCH_NATIVE_POLICY_*` value; `None` for a value this build does not know.
    pub fn from_launch(value: u32) -> Option<Self> {
        match value {
            0 => Some(Self::Always),
            1 => Some(Self::VerifiedOnly),
            2 => Some(Self::Never),
            _ => None,
        }
    }
    /// kiln.json's word for it.
    pub fn name(self) -> &'static str {
        match self {
            Self::Always => "always",
            Self::VerifiedOnly => "verified-only",
            Self::Never => "never",
        }
    }
}
/// Whether Kiln's native policy lets ShroudForge package DLLs load this start (kiln mode only; hosted and installed
/// ShroudForge never read it). `verified` is whether this game build is verified: at boot the kiln.launch record's
/// `build_verified` (the core's own answer, the one `KilnApi.build_info().verified` serves from Kiln's plugin step on).
/// kiln-manage's plan producer (not written yet) is to pass what it reads from kiln.json and the verified-builds list;
/// at boot kiln-sfi passes the frozen policy and the core's answer, and boot wins.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NativePermission {
    pub policy: NativePolicy,
    pub verified: bool,
}
impl NativePermission {
    /// native.policy `"always"`: ShroudForge's selection, unchanged.
    pub const ALWAYS: Self = Self {
        policy: NativePolicy::Always,
        verified: false,
    };
    /// `None` when package DLLs may load. Otherwise Kiln's own words, as its native mods report the same refusal
    /// (`native/core/src/native_mods.cpp`), from the same expression: `never`, or `verified-only` on a build that is
    /// not verified.
    pub fn refusal(self) -> Option<&'static str> {
        match self.policy {
            NativePolicy::Never => Some(r#"kiln.json native.policy is "never""#),
            NativePolicy::VerifiedOnly if !self.verified => Some(
                r#"kiln.json native.policy is "verified-only" and this game build is not verified"#,
            ),
            _ => None,
        }
    }
}
pub struct Inputs<'a> {
    /// `modules.dllManager.enabled` (default true in ShroudForge).
    pub manager_enabled: bool,
    pub server: bool,
    pub exclusions: &'a Exclusions,
    pub selections: &'a [Selection],
    pub packages: &'a [Package],
    pub plan: &'a ExecutionPlan,
    pub registrations: &'a [Registration],
    /// Kiln's native.policy for this start. When it refuses, no package DLL is selected: every planned package
    /// that would load one is withdrawn whole, with Kiln's words, and so is every package that depends on it.
    pub permission: NativePermission,
}
/// Where an entry came from. Launch-plan names: `selection`, `profile`, `ini`,
/// `lua-registration`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// ShroudForge's `manual`: the package's DLL Manager selection lists files.
    Selection,
    Profile,
    Ini,
    /// A declared `register_dll` target (ShroudForge's `Lua register_dll`).
    Registration,
}
impl Source {
    pub fn plan_name(self) -> &'static str {
        match self {
            Self::Selection => "selection",
            Self::Profile => "profile",
            Self::Ini => "ini",
            Self::Registration => "lua-registration",
        }
    }
    /// ShroudForge's status `source` text.
    pub fn source_text(self) -> &'static str {
        match self {
            Self::Selection => "manual",
            Self::Profile => "profile",
            Self::Ini => "ini",
            Self::Registration => "Lua register_dll",
        }
    }
}
/// One DLL to load, in load order. `path` is package-relative as declared.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub status_id: String,
    /// Status ids of later declarations of the same file (one process-owned record).
    pub aliases: Vec<String>,
    pub mod_id: Option<String>,
    pub package_directory: String,
    pub path: String,
    pub mode: Mode,
    pub source: Source,
}
impl Entry {
    /// The case-insensitive process key: one record and one load per file.
    pub fn key(&self) -> String {
        process_key(&self.package_directory, &self.path)
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Issue {
    pub status_id: String,
    pub detail: String,
}
/// A planned package that will not run, so none of its DLLs load and its Lua never starts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Withdrawn {
    pub mod_id: String,
    pub reason: String,
    /// The dependency chain from this package to the package that failed (empty for that one).
    pub caused_by: Vec<String>,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NativeSelection {
    pub entries: Vec<Entry>,
    pub issues: Vec<Issue>,
    pub withdrawn: Vec<Withdrawn>,
}
impl NativeSelection {
    pub fn is_withdrawn(&self, mod_id: &str) -> bool {
        self.withdrawn.iter().any(|w| w.mod_id == mod_id)
    }
}

/// Match SF's path-component validation (mod_profiles.rs `relative_file`, as sfi-package ports it).
fn relative_file(path: &str) -> bool {
    !path.is_empty()
        && !path.contains(['\\', ':'])
        && std::path::Path::new(path)
            .components()
            .all(|p| matches!(p, std::path::Component::Normal(_)))
}
fn single_folder(path: &str) -> bool {
    relative_file(path) && std::path::Path::new(path).components().count() == 1
}
fn file_name(path: &str) -> &str {
    path.rsplit(['/', '\\']).next().unwrap_or(path)
}
fn normalized(path: &str) -> String {
    path.split('/')
        .filter(|c| !c.is_empty())
        .collect::<Vec<_>>()
        .join("/")
        .to_ascii_lowercase()
}
fn process_key(directory: &str, path: &str) -> String {
    format!("{}/{}", directory.to_ascii_lowercase(), normalized(path))
}
fn status_id(directory: &str, path: &str) -> String {
    format!(
        "dll:{}:{}",
        directory.to_ascii_lowercase(),
        path.to_ascii_lowercase()
    )
}
fn manifest_id(manifest: &Manifest) -> Option<&str> {
    match manifest {
        Manifest::Missing => None,
        Manifest::Invalid { id, .. } => id.as_deref(),
        Manifest::Valid { id, .. } => Some(id),
    }
}
/// resolve_package_dll_path without the filesystem: `files` stands for the folder's
/// resolved regular files. A missing file gives the Windows error ShroudForge prints.
fn resolve(
    package: &Package,
    exclusions: &Exclusions,
    path: &str,
    directory_ok: bool,
) -> Result<(), String> {
    if !directory_ok || !relative_file(path) || !path.to_ascii_lowercase().ends_with(".dll") {
        return Err("DLL path must stay inside one installed mod folder".into());
    }
    if exclusions.excludes(path) {
        return Err(format!("DLL is excluded: {path}"));
    }
    let wanted = normalized(path);
    if package.files.iter().any(|f| normalized(f) == wanted) {
        return Ok(());
    }
    let parent = wanted.rsplit_once('/').map(|(dir, _)| format!("{dir}/"));
    let parent_exists = parent
        .as_ref()
        .is_none_or(|dir| package.files.iter().any(|f| normalized(f).starts_with(dir)));
    Err(if parent_exists {
        format!("DLL {path}: The system cannot find the file specified. (os error 2)")
    } else {
        format!("DLL {path}: The system cannot find the path specified. (os error 3)")
    })
}
fn directory_ok(directory: &str) -> bool {
    single_folder(directory) && !directory.starts_with('.')
}

/// ShroudForge's selection, then Kiln's withdrawal and registration rules. Pure and
/// deterministic: the producer and the executor get the same answer from the same inputs.
pub fn select(inputs: &Inputs<'_>) -> NativeSelection {
    let mut result = NativeSelection::default();
    if !inputs.manager_enabled {
        // resolve_load_plan returns an empty plan; declared register_dll targets still fail.
        register(inputs, &mut result, &mut BTreeSet::new());
        return withdraw(inputs, result, inputs.permission.refusal());
    }
    let mut packages: Vec<&Package> = inputs
        .packages
        .iter()
        .filter(|p| !p.directory.starts_with('.'))
        .collect();
    packages.sort_by_key(|p| p.directory.to_ascii_lowercase());
    // inspect_packages
    let mut entries: Vec<(Entry, Target)> = Vec::new();
    for package in packages {
        let directory = package.directory.as_str();
        let selection = selection_for(inputs.selections, directory);
        let selected = selection
            .and_then(|s| s.compatibility_profile.as_deref())
            .filter(|id| !id.is_empty());
        let profile = package.profile.as_ref();
        // WalkDir + Path::extension: a file named just ".dll" has no extension.
        let has_dll = package.files.iter().any(|f| {
            std::path::Path::new(f)
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("dll"))
        });
        if !has_dll && package.ini == PluginIni::Missing && profile.is_none() {
            continue;
        }
        let manifest_target = match &package.manifest {
            Manifest::Valid { target, .. } => *target,
            _ => ManifestTarget::Both,
        };
        let (mut files, mut source, mut load_target) = (
            Vec::new(),
            Source::Selection,
            selection
                .map(|s| s.target)
                .or_else(|| profile.map(|p| p.target))
                .unwrap_or_default(),
        );
        let selection_active = selection.is_none_or(|s| s.enabled && s.approved)
            && selected.is_none_or(|id| id == "none" || profile.is_some());
        if let Some(selection) = selection.filter(|s| !s.files.is_empty()) {
            files = selection.files.clone();
            load_target = selection.target;
        } else if let Some(profile) = profile.filter(|p| !p.files.is_empty()) {
            files = profile.files.clone();
            source = Source::Profile;
            load_target = selection.map(|s| s.target).unwrap_or(profile.target);
        } else if let PluginIni::Enabled { dll } = &package.ini {
            source = Source::Ini;
            files.push(DllFile {
                path: dll.clone(),
                mode: Mode::Library,
            });
            if let Some(selection) = selection {
                load_target = selection.target;
            }
        }
        let active = selection_active
            && !matches!(package.manifest, Manifest::Invalid { .. })
            && match &package.manifest {
                Manifest::Valid { enabled, .. } => *enabled,
                _ => true,
            };
        let effective = match manifest_target {
            ManifestTarget::Client if load_target.applies_to(false) => Some(Target::Client),
            ManifestTarget::Server if load_target.applies_to(true) => Some(Target::Server),
            ManifestTarget::Both => Some(load_target),
            _ => None,
        };
        let Some(target) = effective.filter(|_| active) else {
            continue;
        };
        if source == Source::Selection
            && files.is_empty()
            && package.ini == PluginIni::Invalid
            && target.applies_to(inputs.server)
        {
            result.issues.push(Issue {
                status_id: format!("dll:{}:ini", directory.to_ascii_lowercase()),
                detail: "native-plugin.ini has an invalid [Plugin] declaration".into(),
            });
        }
        let mod_id = manifest_id(&package.manifest).map(str::to_owned);
        for file in files {
            let id = status_id(directory, &file.path);
            match resolve(
                package,
                inputs.exclusions,
                &file.path,
                directory_ok(directory),
            ) {
                Ok(()) => entries.push((
                    Entry {
                        status_id: id,
                        aliases: Vec::new(),
                        mod_id: mod_id.clone(),
                        package_directory: directory.to_owned(),
                        path: file.path,
                        mode: file.mode,
                        source,
                    },
                    target,
                )),
                Err(detail) if target.applies_to(inputs.server) => result.issues.push(Issue {
                    status_id: id,
                    detail,
                }),
                Err(_) => {}
            }
        }
    }
    // resolve_load_plan: process target, then the selection's own switches.
    entries.retain(|(entry, target)| {
        target.applies_to(inputs.server)
            && selection_for(inputs.selections, &entry.package_directory).is_none_or(|s| {
                s.enabled
                    && s.approved
                    && s.target.applies_to(inputs.server)
                    && s.compatibility_profile
                        .as_deref()
                        .filter(|id| !id.is_empty() && *id != "none")
                        .is_none_or(|_| {
                            package_for(inputs.packages, &entry.package_directory)
                                .is_some_and(|p| p.profile.is_some())
                        })
            })
    });
    let mut entries: Vec<Entry> = entries.into_iter().map(|(entry, _)| entry).collect();
    // Native components follow their Lua package's activation, dependency and conflict plan.
    if let Some(error) = &inputs.plan.error {
        entries.retain(|e| e.mod_id.is_none());
        result.issues.push(Issue {
            status_id: "dll:plan".into(),
            detail: format!("Could not resolve mod dependencies: {error}"),
        });
    } else {
        let ranks = ranks(inputs.plan);
        entries.retain(|entry| {
            let allowed = entry
                .mod_id
                .as_ref()
                .is_none_or(|id| ranks.contains_key(id));
            if !allowed {
                let id = entry.mod_id.as_deref().unwrap_or_default();
                result.issues.push(Issue {
                    status_id: entry.status_id.clone(),
                    detail: inputs.plan.issues.get(id).cloned().unwrap_or_else(|| {
                        "Mod is disabled or unavailable in the execution plan".into()
                    }),
                });
            }
            allowed
        });
        entries.sort_by_key(|entry| {
            entry
                .mod_id
                .as_ref()
                .and_then(|id| ranks.get(id))
                .copied()
                .unwrap_or(usize::MAX)
        });
    }
    // The ordered process worker: one record per file; a different non-library mode refuses.
    let mut seen: BTreeMap<String, usize> = BTreeMap::new();
    for entry in entries {
        let key = entry.key();
        if let Some(&at) = seen.get(&key) {
            if result.entries[at].mode != entry.mode && entry.mode != Mode::Library {
                result.issues.push(Issue {
                    status_id: entry.status_id,
                    detail: "This DLL was already scheduled with another initialization mode. Restart the game after changing its selection".into(),
                });
            } else {
                result.entries[at].aliases.push(entry.status_id);
            }
            continue;
        }
        seen.insert(key, result.entries.len());
        result.entries.push(entry);
    }
    let mut keys = seen.into_keys().collect();
    register(inputs, &mut result, &mut keys);
    withdraw(inputs, result, inputs.permission.refusal())
}
fn selection_for<'a>(selections: &'a [Selection], directory: &str) -> Option<&'a Selection> {
    selections
        .iter()
        .find(|s| s.package_directory.eq_ignore_ascii_case(directory))
}
fn package_for<'a>(packages: &'a [Package], directory: &str) -> Option<&'a Package> {
    packages
        .iter()
        .find(|p| p.directory.eq_ignore_ascii_case(directory))
}
fn ranks(plan: &ExecutionPlan) -> BTreeMap<String, usize> {
    plan.order
        .iter()
        .enumerate()
        .map(|(rank, id)| (id.clone(), rank))
        .collect()
}
/// Declared register_dll targets, established now (load_mod_native_dll + queue before startup).
fn register(inputs: &Inputs<'_>, result: &mut NativeSelection, keys: &mut BTreeSet<String>) {
    if inputs.plan.error.is_some() {
        return;
    }
    let ranks = ranks(inputs.plan);
    let mut planned: Vec<(usize, usize, &Registration, &Package, String)> = Vec::new();
    for (index, registration) in inputs.registrations.iter().enumerate() {
        let Some(package) = package_for(inputs.packages, &registration.package_directory) else {
            continue;
        };
        let Some(mod_id) = manifest_id(&package.manifest) else {
            continue;
        };
        // An unplanned or disabled mod never runs its Lua, so it never calls register_dll.
        let Some(&rank) = ranks.get(mod_id) else {
            continue;
        };
        planned.push((rank, index, registration, package, mod_id.to_owned()));
    }
    planned.sort_by_key(|(rank, index, ..)| (*rank, *index));
    for (_, _, registration, package, mod_id) in planned {
        let path = registration.path.replace('\\', "/");
        let path = path.trim_start_matches("./").to_owned();
        let id = status_id(&package.directory, &path);
        let outcome = establish_registration(
            inputs.manager_enabled,
            inputs.server,
            inputs.exclusions,
            selection_for(inputs.selections, &package.directory),
            package,
            &path,
        );
        match outcome {
            Err(detail) => result.issues.push(Issue {
                status_id: id,
                detail,
            }),
            Ok(()) => {
                let key = process_key(&package.directory, &path);
                if keys.contains(&key) {
                    if let Some(existing) = result.entries.iter_mut().find(|e| e.key() == key) {
                        existing.aliases.push(id);
                    }
                    continue;
                }
                keys.insert(key);
                result.entries.push(Entry {
                    status_id: id,
                    aliases: Vec::new(),
                    mod_id: Some(mod_id),
                    package_directory: package.directory.clone(),
                    path,
                    mode: Mode::Library,
                    source: Source::Registration,
                });
            }
        }
    }
}
fn establish_registration(
    manager_enabled: bool,
    server: bool,
    exclusions: &Exclusions,
    selection: Option<&Selection>,
    package: &Package,
    path: &str,
) -> Result<(), String> {
    if !manager_enabled {
        return Err("EML runtime DLL support is unavailable in this phase".into());
    }
    if selection.is_some_and(|s| !s.target.applies_to(server)) {
        return Err("The DLL Manager selection targets a different game process".into());
    }
    resolve(package, exclusions, path, directory_ok(&package.directory))?;
    if selection.is_some_and(|s| !s.enabled || !s.approved) {
        return Err("Native DLL loading for this mod folder is disabled or not confirmed in DLL Manager. Update the selection and restart the game".into());
    }
    Ok(())
}
/// A planned package with a native issue cannot run whole: withdraw it and its dependents.
/// `refusal`: Kiln's native.policy refuses package DLLs this start (its words). Then nothing is
/// selected: every entry is dropped with an issue in those words (a manifest-less folder's too),
/// and every planned package with an issue is withdrawn for that reason.
fn withdraw(
    inputs: &Inputs<'_>,
    mut result: NativeSelection,
    refusal: Option<&str>,
) -> NativeSelection {
    let ranks = ranks(inputs.plan);
    let mut failed: BTreeMap<String, String> = BTreeMap::new();
    if let Some(text) = refusal {
        for entry in std::mem::take(&mut result.entries) {
            for status_id in std::iter::once(entry.status_id).chain(entry.aliases) {
                result.issues.push(Issue {
                    status_id,
                    detail: text.to_owned(),
                });
            }
        }
    }
    for issue in &result.issues {
        let Some(directory) = issue
            .status_id
            .strip_prefix("dll:")
            .and_then(|rest| rest.split_once(':'))
            .map(|(directory, _)| directory)
        else {
            continue;
        };
        let Some(mod_id) = package_for(inputs.packages, directory)
            .and_then(|p| manifest_id(&p.manifest))
            .filter(|id| ranks.contains_key(*id))
        else {
            continue;
        };
        failed.entry(mod_id.to_owned()).or_insert_with(|| {
            refusal.map_or_else(
                || format!("native DLL cannot load: {}", issue.detail),
                str::to_owned,
            )
        });
    }
    let mut withdrawn: BTreeMap<String, (String, Vec<String>)> = failed
        .into_iter()
        .map(|(id, reason)| (id, (reason, Vec::new())))
        .collect();
    // Dependents in plan order: a dependency always ranks before its dependent.
    for id in &inputs.plan.order {
        if withdrawn.contains_key(id) {
            continue;
        }
        let cause = inputs
            .plan
            .dependencies
            .get(id)
            .into_iter()
            .flatten()
            .find(|dep| withdrawn.contains_key(*dep));
        if let Some(dep) = cause {
            let mut chain = vec![dep.clone()];
            chain.extend(withdrawn[dep].1.iter().cloned());
            withdrawn.insert(
                id.clone(),
                (format!("depends on withdrawn package {dep}"), chain),
            );
        }
    }
    result.entries.retain(|e| {
        e.mod_id
            .as_ref()
            .is_none_or(|id| !withdrawn.contains_key(id))
    });
    let mut order: Vec<_> = withdrawn.into_iter().collect();
    order.sort_by_key(|(id, _)| ranks.get(id).copied().unwrap_or(usize::MAX));
    result.withdrawn = order
        .into_iter()
        .map(|(mod_id, (reason, caused_by))| Withdrawn {
            mod_id,
            reason,
            caused_by,
        })
        .collect();
    result
}

/// What a runtime `loader.runtime.register_dll(path)` call does in kiln mode.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RegisterOutcome {
    /// The target was established and loaded before startup: reuse its process record.
    Reuse { key: String, status_id: String },
    /// The mod is disabled: ShroudForge skips it with a restart-required effect, no error.
    SkippedDisabled,
    /// ShroudForge's own refusal text, raised as the Lua error.
    Refused(String),
    /// Not established before startup. Kiln never loads a new DLL after the seal; the text is
    /// the Lua error, and the package's parity for this call is partial.
    NotEstablished(String),
}
/// The sealed register_dll view of one launch: the same facts the selection used, frozen.
#[derive(Clone, Debug)]
pub struct RegistrationTable {
    manager_enabled: bool,
    server: bool,
    /// Kiln's native.policy refusal for this start, when it refuses.
    refusal: Option<&'static str>,
    exclusions: Exclusions,
    selections: Vec<Selection>,
    packages: Vec<Package>,
    established: BTreeSet<String>,
}
impl RegistrationTable {
    /// Seal after [`select`]: only the selection's surviving entries are reusable.
    pub fn seal(inputs: &Inputs<'_>, selection: &NativeSelection) -> Self {
        Self {
            manager_enabled: inputs.manager_enabled,
            server: inputs.server,
            refusal: inputs.permission.refusal(),
            exclusions: inputs.exclusions.clone(),
            selections: inputs.selections.to_vec(),
            packages: inputs.packages.to_vec(),
            established: selection.entries.iter().map(Entry::key).collect(),
        }
    }
    /// `runtime_dll`: ShroudForge's RUNTIME_DLL feature (in-game phase and manager on).
    pub fn register(
        &self,
        package_directory: &str,
        mod_enabled: bool,
        runtime_dll: bool,
        path: &str,
    ) -> RegisterOutcome {
        if !runtime_dll || !self.manager_enabled {
            return RegisterOutcome::Refused(
                "EML runtime DLL support is unavailable in this phase".into(),
            );
        }
        if !mod_enabled {
            return RegisterOutcome::SkippedDisabled;
        }
        let Some(package) = package_for(&self.packages, package_directory) else {
            return RegisterOutcome::Refused(
                "DLL path must stay inside one installed mod folder".into(),
            );
        };
        let path = path.replace('\\', "/");
        let path = path.trim_start_matches("./");
        if let Err(text) = establish_registration(
            self.manager_enabled,
            self.server,
            &self.exclusions,
            selection_for(&self.selections, &package.directory),
            package,
            path,
        ) {
            return RegisterOutcome::Refused(text);
        }
        // A call ShroudForge would accept: under Kiln's native.policy refusal nothing was established, and the Lua
        // error says why in Kiln's words.
        if let Some(text) = self.refusal {
            return RegisterOutcome::NotEstablished(format!(
                "Native DLL {path} does not load: {text}"
            ));
        }
        let key = process_key(&package.directory, path);
        if self.established.contains(&key) {
            return RegisterOutcome::Reuse {
                key,
                status_id: status_id(&package.directory, path),
            };
        }
        RegisterOutcome::NotEstablished(format!(
            "Native DLL {path} was not established before the game started, so Kiln does not load it now. Declare it in the mod's DLL selection and restart the game"
        ))
    }
}
