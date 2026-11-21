// SPDX-License-Identifier: MIT
//! The native side of the scan: the facts sfi-policy's DLL Manager selection needs about every installed
//! folder under `<game>/mods`, the selection itself, and the read-only import gate on each selected DLL's
//! package-local closure. Files are listed and read, never loaded or run. The gate's advisory verdict decides only what
//! the inventory lets it decide: a refused or needs-contract DLL is an authoritative skip of its package; pass and
//! admitted-with-enforcement stay eligible (kiln-sfi re-inspects and binds at load). Nothing here builds a load permit.
use sfi_package::package::profiles::{self, DllSelection, ProfileCatalog};
use sfi_policy::native_contracts::{AdmissionVerdict, NativeContracts};
use sfi_policy::native_selection::{
    self as sel, Exclusions, ExecutionPlan, Manifest, ManifestTarget, NativePermission,
    NativeSelection, PluginIni, Registration,
};
use sfi_policy::pe_gate::{Image, MAX_IMAGE_BYTES};
use std::fs;
use std::path::{Path, PathBuf};

/// Per-folder listing budgets (files and depth): past them the folder's facts are incomplete and the DLL entry that
/// needs a missing file fails as ShroudForge's resolver would.
const MAX_FILES: usize = 20_000;
const MAX_DEPTH: usize = 32;

/// Kiln-owned native inputs. In kiln mode none of them is read from `<game>/shroudforge`: the caller passes Kiln's own
/// settings (the Kiln menu's own files); the defaults are ShroudForge's (manager on, nothing selected,
/// nothing excluded) and kiln.json's `native.policy` default `always`.
pub struct NativeConfig {
    pub manager_enabled: bool,
    pub exclusions: Exclusions,
    pub selections: Vec<DllSelection>,
    /// Declared `register_dll` targets (static declarations, reviewed contracts). The scan never derives these from Lua.
    pub registrations: Vec<Registration>,
    pub permission: NativePermission,
    /// The host-owned review registry (the shipped one by default).
    pub contracts: NativeContracts,
}
impl NativeConfig {
    pub fn shipped() -> Result<Self, String> {
        Ok(Self {
            manager_enabled: true,
            exclusions: Exclusions::default(),
            selections: Vec::new(),
            registrations: Vec::new(),
            permission: NativePermission::ALWAYS,
            contracts: NativeContracts::shipped()?,
        })
    }
}

/// One selected DLL with its gate verdict.
#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Dll {
    /// Package-relative, as declared.
    pub path: String,
    /// library or dinput8.
    pub mode: &'static str,
    /// Launch-plan names: selection, profile, ini, lua-registration.
    pub source: &'static str,
    pub status_id: String,
    /// Later declarations of the same file: one process-owned record, loaded once.
    pub aliases: Vec<String>,
    pub sha256: Option<String>,
    /// pass, admitted-with-enforcement, needs-contract or refused.
    pub verdict: &'static str,
    pub eligible: bool,
    pub reasons: Vec<String>,
}

fn verdict_name(v: AdmissionVerdict) -> &'static str {
    match v {
        AdmissionVerdict::Pass => "pass",
        AdmissionVerdict::AdmittedWithEnforcement => "admitted-with-enforcement",
        AdmissionVerdict::NeedsContract => "needs-contract",
        AdmissionVerdict::Refused => "refused",
    }
}

/// Package-relative `/` paths of the regular files under `dir`, symlinks and anything past the budgets left out.
pub fn list_files(dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    let mut stack: Vec<(PathBuf, String, usize)> = vec![(dir.to_path_buf(), String::new(), 0)];
    while let Some((path, rel, depth)) = stack.pop() {
        let Ok(entries) = fs::read_dir(&path) else {
            continue;
        };
        let mut entries: Vec<_> = entries.flatten().collect();
        entries.sort_by_key(|e| e.file_name());
        for entry in entries {
            if out.len() >= MAX_FILES {
                return out;
            }
            let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
                continue;
            };
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            let child = if rel.is_empty() {
                name
            } else {
                format!("{rel}/{name}")
            };
            if kind.is_file() {
                out.push(child);
            } else if kind.is_dir() && depth < MAX_DEPTH {
                stack.push((entry.path(), child, depth + 1));
            }
        }
    }
    out.sort();
    out
}

/// The spelling a Windows-style open of `wanted` lands on among `names`: the exact name first (Wine on a case-sensitive
/// file system), else the first that differs only in ASCII letter case (Windows and NTFS hold one of them at most).
fn windows_lookup<'a>(
    names: impl IntoIterator<Item = &'a str> + Clone,
    wanted: &str,
) -> Option<&'a str> {
    names
        .clone()
        .into_iter()
        .find(|n| *n == wanted)
        .or_else(|| names.into_iter().find(|n| n.eq_ignore_ascii_case(wanted)))
}

/// `native-plugin.ini` directly in the folder, its name in any case (Windows opens it so; Wine prefers the exact name).
pub fn read_ini(dir: &Path) -> PluginIni {
    let mut names: Vec<String> = fs::read_dir(dir)
        .map(|entries| {
            entries
                .flatten()
                .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
                .filter_map(|e| e.file_name().to_str().map(str::to_owned))
                .filter(|n| n.eq_ignore_ascii_case("native-plugin.ini"))
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    let Some(name) = windows_lookup(names.iter().map(String::as_str), "native-plugin.ini") else {
        return PluginIni::Missing;
    };
    let entry = dir.join(name);
    match fs::metadata(&entry) {
        Ok(m) if m.len() <= 64 * 1024 => {}
        _ => return PluginIni::Invalid,
    }
    match fs::read(&entry) {
        Ok(bytes) => PluginIni::parse(&String::from_utf8_lossy(&bytes)),
        Err(_) => PluginIni::Invalid,
    }
}

pub fn manifest_target(t: sfi_package::package::Target) -> ManifestTarget {
    match t {
        sfi_package::package::Target::Client => ManifestTarget::Client,
        sfi_package::package::Target::Server => ManifestTarget::Server,
        sfi_package::package::Target::ClientServer => ManifestTarget::Both,
    }
}

fn dll_target(t: profiles::DllTarget) -> sel::Target {
    match t {
        profiles::DllTarget::Client => sel::Target::Client,
        profiles::DllTarget::Server => sel::Target::Server,
        profiles::DllTarget::Both => sel::Target::Both,
    }
}
fn dll_files(files: &[profiles::DllFile]) -> Vec<sel::DllFile> {
    files
        .iter()
        .map(|f| sel::DllFile {
            path: f.path.clone(),
            mode: match f.mode {
                profiles::DllMode::Library => sel::Mode::Library,
                profiles::DllMode::Dinput8 => sel::Mode::Dinput8,
            },
        })
        .collect()
}

pub fn selections(config: &NativeConfig) -> Vec<sel::Selection> {
    config
        .selections
        .iter()
        .map(|s| sel::Selection {
            package_directory: s.package_directory.clone(),
            enabled: s.enabled,
            approved: s.approved,
            target: dll_target(s.target),
            compatibility_profile: s.compatibility_profile.clone(),
            files: dll_files(&s.files),
        })
        .collect()
}

/// The compatibility profile ShroudForge's selection would use for a folder (sfi-package's `select_profile`).
pub fn profile_for(
    catalog: &ProfileCatalog,
    selections: &[DllSelection],
    directory: &str,
    mod_id: Option<&str>,
) -> Option<sel::Profile> {
    let selected = profiles::selection_for_package(selections, directory);
    profiles::select_profile(&catalog.profiles, directory, mod_id, selected).map(|p| sel::Profile {
        id: p.id.clone(),
        target: dll_target(p.target),
        files: dll_files(&p.files),
    })
}

/// The folder facts the selection takes.
pub struct Folder {
    pub directory: String,
    pub manifest: Manifest,
    pub ini: PluginIni,
    pub profile: Option<sel::Profile>,
    pub files: Vec<String>,
}

pub fn select(
    config: &NativeConfig,
    server: bool,
    folders: &[Folder],
    plan: &ExecutionPlan,
) -> NativeSelection {
    let packages: Vec<sel::Package> = folders
        .iter()
        .map(|f| sel::Package {
            directory: f.directory.clone(),
            manifest: f.manifest.clone(),
            ini: f.ini.clone(),
            profile: f.profile.clone(),
            files: f.files.clone(),
        })
        .collect();
    let selections = selections(config);
    sel::select(&sel::Inputs {
        manager_enabled: config.manager_enabled,
        server,
        exclusions: &config.exclusions,
        selections: &selections,
        packages: &packages,
        plan,
        registrations: &config.registrations,
        permission: config.permission,
    })
}

/// The gate over one selected DLL: every `.dll` of its folder is the package-local closure the gate may follow.
pub fn gate(config: &NativeConfig, folder: &Path, files: &[String], entry: &sel::Entry) -> Dll {
    let mut images: Vec<(String, Vec<u8>)> = Vec::new();
    let mut too_large = Vec::new();
    for rel in files
        .iter()
        .filter(|f| f.to_ascii_lowercase().ends_with(".dll"))
    {
        let path = folder.join(rel);
        match fs::symlink_metadata(&path) {
            Ok(m) if m.is_file() && m.len() as usize <= MAX_IMAGE_BYTES => {
                if let Ok(bytes) = fs::read(&path) {
                    images.push((rel.clone(), bytes));
                }
            }
            Ok(m) if m.is_file() => too_large.push(rel.clone()),
            _ => {}
        }
    }
    // The file the declared path opens: empty components dropped as the selection's resolver drops them, then the
    // exact spelling, else one that differs only in letter case (as Windows resolves it).
    let wanted = entry
        .path
        .split('/')
        .filter(|c| !c.is_empty())
        .collect::<Vec<_>>()
        .join("/");
    let actual = windows_lookup(images.iter().map(|(p, _)| p.as_str()), &wanted).map(str::to_owned);
    let sha256 = images
        .iter()
        .find(|(p, _)| Some(p) == actual.as_ref())
        .map(|(_, b)| NativeContracts::sha256(b));
    let mut dll = Dll {
        path: entry.path.clone(),
        mode: match entry.mode {
            sel::Mode::Library => "library",
            sel::Mode::Dinput8 => "dinput8",
        },
        source: entry.source.plan_name(),
        status_id: entry.status_id.clone(),
        aliases: entry.aliases.clone(),
        sha256,
        verdict: "refused",
        eligible: false,
        reasons: Vec::new(),
    };
    if !too_large.is_empty() {
        dll.reasons.push(format!(
            "larger than the inspection budget: {}",
            too_large.join(", ")
        ));
        return dll;
    }
    let Some(entry_path) = actual else {
        dll.reasons
            .push(format!("{}: not found in the package folder", entry.path));
        return dll;
    };
    let views: Vec<Image<'_>> = images
        .iter()
        .map(|(p, b)| Image { path: p, bytes: b })
        .collect();
    let report = config.contracts.inventory(&entry_path, &views);
    dll.verdict = verdict_name(report.verdict());
    dll.eligible = report.eligible();
    dll.reasons = report.reasons().to_vec();
    dll
}
