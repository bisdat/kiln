// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins): modules/modloader/mod_profiles.rs:11-174,
// read_mod_manifest.rs:35-101, dll_manager.rs:28-57, check_mod_conflicts.rs:5-45,
// conflicts/rules.schema.json:1-19. Data only: never installs or loads a DLL.
use super::{document::Document, manifest::Capability};
use crate::json::{self, Value};
use crate::source::PackageSource;
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum DllTarget {
    Client,
    Server,
    #[default]
    Both,
}
impl DllTarget {
    pub fn supports_process(self, server: bool) -> bool {
        matches!(
            (self, server),
            (Self::Both, _) | (Self::Client, false) | (Self::Server, true)
        )
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum DllMode {
    #[default]
    Library,
    Dinput8,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DllFile {
    pub path: String,
    #[serde(default)]
    pub mode: DllMode,
}
/// Loader-config selections are ORDERED, unlike profile file maps. Reading
/// these records grants neither approval nor permission to load any DLL.
/// SF dll_manager.rs:12-27,69-75; mod_profiles.rs:181-186 (both pins).
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DllSelection {
    pub id: String,
    pub package_directory: String,
    pub name: String,
    pub target: DllTarget,
    #[serde(default)]
    pub enabled: bool,
    #[serde(default)]
    pub approved: bool,
    #[serde(default)]
    pub compatibility_profile: Option<String>,
    #[serde(default)]
    pub files: Vec<DllFile>,
}
pub fn selections_from_config(config: &serde_json::Value) -> Vec<DllSelection> {
    config
        .pointer("/modules/dllManager/profiles")
        .cloned()
        .and_then(|v| serde_json::from_value(v).ok())
        .unwrap_or_default()
}
pub fn selection_for_package<'a>(
    selections: &'a [DllSelection],
    directory: &str,
) -> Option<&'a str> {
    selections
        .iter()
        .find(|s| s.package_directory.eq_ignore_ascii_case(directory))
        .and_then(|s| s.compatibility_profile.as_deref())
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Profile {
    pub schema_version: u32,
    pub id: String,
    #[serde(default)]
    pub mod_id: String,
    pub package_directory: String,
    pub name: String,
    #[serde(default)]
    pub target: DllTarget,
    /// Additions in document order: an overlay appends new keys in this order.
    #[serde(default)]
    pub mod_json: Option<Value>,
    #[serde(default)]
    pub extended_mod_json: Option<Value>,
    #[serde(default)]
    pub package_files: BTreeMap<String, String>,
    #[serde(default)]
    pub import_files: BTreeMap<String, String>,
    #[serde(default)]
    pub files: Vec<DllFile>,
}
#[derive(Clone, Debug)]
pub struct ProfileDocument {
    pub document: Document,
    pub profile: Profile,
}
impl ProfileDocument {
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let document = Document::parse(bytes)?;
        let profile: Profile = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
        profile.validate()?;
        Ok(Self { document, profile })
    }
}
impl Profile {
    pub fn validate(&self) -> Result<(), String> {
        if self.schema_version != 1
            || self.id.trim().is_empty()
            || self.id == "none"
            || self.id.len() > 128
            || self.name.trim().is_empty()
            || self.name.len() > 160
            || !single_folder(&self.package_directory)
        {
            return Err("invalid compatibility profile identity".into());
        }
        for v in [&self.mod_json, &self.extended_mod_json]
            .into_iter()
            .flatten()
        {
            if !v.is_object() {
                return Err("manifest additions must be JSON objects".into());
            }
        }
        if self
            .mod_json
            .as_ref()
            .and_then(Value::as_object)
            .is_some_and(|v| v.contains_key("id") || v.contains_key("version"))
        {
            return Err("a profile cannot replace the mod ID or version".into());
        }
        if let Some(v) = self.mod_json.as_ref().and_then(|v| v.get("capabilities")) {
            json::from_value::<Vec<Capability>>(v)
                .map_err(|e| format!("invalid profile capabilities: {e}"))?;
        }
        if self.files.len() > 16 || self.package_files.len() > 64 || self.import_files.len() > 64 {
            return Err("compatibility profile has too many files".into());
        }
        let mut seen = BTreeSet::new();
        for file in &self.files {
            if !relative_file(&file.path)
                || !file.path.to_ascii_lowercase().ends_with(".dll")
                || !seen.insert(file.path.to_ascii_lowercase())
            {
                return Err(format!("invalid or duplicate DLL path: {}", file.path));
            }
        }
        let mut bytes = 0usize;
        for (path, contents) in &self.package_files {
            bytes = bytes.saturating_add(contents.len());
            if !relative_file(path)
                || matches!(path.as_str(), "mod.json" | "extended.mod.json")
                || contents.len() > 1024 * 1024
            {
                return Err(format!("invalid virtual package file: {path}"));
            }
        }
        if bytes > 4 * 1024 * 1024 {
            return Err("virtual package files exceed 4 MiB".into());
        }
        for (source, destination) in &self.import_files {
            if !relative_file(source) || !relative_file(destination) {
                return Err("invalid installation file mapping".into());
            }
        }
        Ok(())
    }
}
/// Match SF's path-component validation, including platform path normalization.
pub fn relative_file(path: &str) -> bool {
    !path.is_empty()
        && !path.contains(['\\', ':'])
        && std::path::Path::new(path)
            .components()
            .all(|p| matches!(p, std::path::Component::Normal(_)))
}
pub fn single_folder(path: &str) -> bool {
    relative_file(path) && std::path::Path::new(path).components().count() == 1
}
#[derive(Debug, Default)]
pub struct ProfileCatalog {
    pub profiles: Vec<ProfileDocument>,
    pub errors: Vec<String>,
}
/// Catalog is sorted by source path; malformed and later duplicate profiles are
/// skipped with diagnostics. A missing catalog is empty; other directory errors survive.
pub fn read_catalog(source: &dyn PackageSource) -> io::Result<ProfileCatalog> {
    let entries = match source.read_dir("") {
        Ok(e) => e,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(ProfileCatalog::default()),
        Err(e) => return Err(e),
    };
    let mut entries: Vec<_> = entries
        .into_iter()
        .flatten()
        .filter(|e| {
            std::path::Path::new(&e.path)
                .extension()
                .is_some_and(|ext| ext.eq_ignore_ascii_case("json"))
                && source.is_file(&e.path)
        })
        .collect();
    entries.sort_by(|a, b| a.path.cmp(&b.path));
    let mut result = ProfileCatalog::default();
    let mut ids = BTreeSet::new();
    for entry in entries {
        let parsed = source
            .read_file(&entry.path)
            .map_err(|e| e.to_string())
            .and_then(|b| ProfileDocument::parse(&b));
        match parsed {
            Ok(profile) if ids.insert(profile.profile.id.clone()) => result.profiles.push(profile),
            Ok(_) => result.errors.push(format!(
                "{}: Duplicate compatibility profile ID. File skipped",
                entry.path
            )),
            Err(e) => result.errors.push(format!(
                "{}: Invalid compatibility profile. File skipped: {e}",
                entry.path
            )),
        }
    }
    Ok(result)
}
pub fn select_profile<'a>(
    profiles: &'a [ProfileDocument],
    directory: &str,
    mod_id: Option<&str>,
    selected: Option<&str>,
) -> Option<&'a Profile> {
    if let Some(selected) = selected.filter(|s| !s.is_empty()) {
        return profiles
            .iter()
            .map(|p| &p.profile)
            .find(|p| p.id == selected);
    }
    profiles.iter().map(|p| &p.profile).find(|p| match mod_id {
        Some(id) if !p.mod_id.is_empty() => p.mod_id == id,
        _ => p.package_directory.eq_ignore_ascii_case(directory),
    })
}
/// Object overlays recurse ONLY into existing objects. An object over an existing
/// scalar/array is a no-op, not a replacement (both pins read_mod_manifest.rs:90-101).
pub fn merge_object(target: &mut Value, overlay: &Value) {
    let (Some(target), Some(overlay)) = (target.as_object_mut(), overlay.as_object()) else {
        return;
    };
    for (key, value) in overlay {
        match (target.get_mut(key), value) {
            (Some(existing), Value::Object(_)) => merge_object(existing, value),
            _ => {
                target.insert(key.clone(), value.clone());
            }
        }
    }
}
pub fn apply_overlay(manifest: &mut Value, extension: &mut Option<Value>, profile: &Profile) {
    if let Some(mut overlay) = profile.mod_json.clone() {
        // serde_json's preserve_order Map::remove is swap_remove: the last key takes its place.
        if let Some(additions) = overlay
            .as_object_mut()
            .and_then(|o| o.swap_remove("capabilities"))
        {
            let existing = manifest.get("capabilities");
            if existing.is_none_or(Value::is_array) {
                let mut values = Vec::new();
                for v in existing
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .chain(additions.as_array().into_iter().flatten())
                {
                    if !values.contains(v) {
                        values.push(v.clone());
                    }
                }
                manifest["capabilities"] = Value::Array(values);
            }
        }
        merge_object(manifest, &overlay);
    }
    if let Some(mut overlay) = profile.extended_mod_json.clone() {
        if let Some(o) = overlay.as_object_mut() {
            o.swap_remove("enabled");
        }
        merge_object(
            extension.get_or_insert_with(|| {
                Value::Object(json::Map::from_iter([
                    ("schemaVersion".to_owned(), Value::Number(1.into())),
                    ("enabled".to_owned(), Value::Bool(false)),
                ]))
            }),
            &overlay,
        );
    }
}
#[derive(Clone, Debug)]
pub struct CompatibilityRules {
    pub document: Document,
    pub schema_version: u32,
    pub conflicts: Vec<Conflict>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Conflict {
    pub first: Constraint,
    pub second: Constraint,
    pub reason: String,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Constraint {
    pub id: String,
    pub version: semver::VersionReq,
}
impl CompatibilityRules {
    /// ShroudForge's document check (check_mod_conflicts.rs:8-14): `serde_json::from_slice`
    /// (its invalid-UTF-8 column as serde_json 1.0.151, which both pins lock, reports it),
    /// then the embedded schema `rules_file::EMBEDDED_RULES_SCHEMA`, "rules: <diagnostic>".
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let value: Value = serde_json::from_slice(bytes)
            .map_err(|e| super::document::pinned_reader_error(bytes, &e))?;
        let document = Document::parse(bytes)?;
        // Reuse the package model's fixed-schema evaluator rather than inventing a second
        // diagnostic/closed-record validator. Schema is identical at both pins.
        static SCHEMA: std::sync::OnceLock<serde_json::Value> = std::sync::OnceLock::new();
        let schema = SCHEMA.get_or_init(|| {
            serde_json::from_str(super::rules_file::EMBEDDED_RULES_SCHEMA)
                .expect("ShroudForge's embedded rules schema")
        });
        super::validate::validate_value(schema, &value).map_err(|e| format!("rules: {e}"))?;
        let conflicts = json::from_value(&value["conflicts"])?;
        Ok(Self {
            document,
            schema_version: 1,
            conflicts,
        })
    }
}
