// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/modloader/parse_mod_manifest.rs:75-235 (1.8), :70-215 (1.7.9)
// Source: modules/modloader/read_input_reservations.rs:24-62 (1.8)
//! Package documents and normalized SF semantics. No installation paths, admission,
//! discovery, dependency planning or disk writes are selected by this module.
pub mod discover;
pub mod document;
pub mod edit;
pub mod extension;
pub mod locales;
pub mod manifest;
pub mod plan;
pub mod profiles;
pub mod revisions;
pub mod rules_file;
pub mod settings;
mod validate;

use crate::json::{self, Value};
pub use crate::source::SourceVersion;
use document::Document;
pub use manifest::{Manifest, Target};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};

pub const MAX_FILE_BYTES: usize = 2 * 1024 * 1024;
#[derive(Debug, Clone, Copy)]
pub struct ParseOptions {
    /// Schema dialect only. Choosing this does not grant release/admission permission.
    pub version: SourceVersion,
    /// SF modDetection.enabled && tolerateUnknownManifestFields (both default true).
    /// Only unknown *top-level* manifest/extension fields are ignored.
    pub tolerate_unknown_fields: bool,
    /// Host scancode predicate: SF uses SDL on Windows and 1..512 elsewhere
    /// (modules/hotkeys/observe_keyboard.rs:86-90 in 1.7.9, :106-110 in 1.8).
    /// No native input service is selected or called by core.
    pub valid_scancode: fn(u16) -> bool,
}
impl Default for ParseOptions {
    fn default() -> Self {
        Self {
            version: SourceVersion::Sf180,
            tolerate_unknown_fields: true,
            valid_scancode: |code| (1..512).contains(&code),
        }
    }
}
#[derive(Debug, Clone)]
pub struct PackageDocuments {
    pub manifest: Document,
    pub extension: Option<Document>,
}
#[derive(Debug, Clone)]
pub struct Package {
    pub documents: PackageDocuments,
    pub manifest: Manifest,
    options: ParseOptions,
    pub locale_documents: BTreeMap<String, Document>,
}
impl Package {
    /// Strict SF acceptance. In particular this NEVER uses the damaged-fraction
    /// recovery projection in `edit`, or Kiln's editor-only 2 MiB policy.
    pub fn parse(
        manifest: &[u8],
        extension: Option<&[u8]>,
        options: ParseOptions,
    ) -> Result<Self, String> {
        let documents = PackageDocuments {
            manifest: Document::parse(manifest)?,
            extension: extension.map(Document::parse).transpose()?,
        };
        // Both pins validate extended.mod.json before mod.json (parse_mod_manifest.rs:
        // 105-123 in 1.7.9, 110-128 in 1.8), so with both invalid the extension's error wins.
        let ext = documents
            .extension
            .as_ref()
            .map(|d| normalized_document(d, options, validate::Schema::Extension))
            .transpose()?
            .unwrap_or_else(|| Value::Object(json::Map::new()));
        let mut m = normalized_document(&documents.manifest, options, validate::Schema::Manifest)?;
        let target = match ext.get("targets").and_then(Value::as_array) {
            Some(a) if a.iter().any(|v| v == "client") && a.iter().any(|v| v == "server") => {
                Target::ClientServer
            }
            Some(a) if a.iter().any(|v| v == "server") => Target::Server,
            Some(_) => Target::Client,
            None if documents.extension.is_none() || ext["launcher"] == "EML" => {
                Target::ClientServer
            }
            None => Target::Client,
        };
        let mut definitions = vec![];
        let mut effective = json::Map::new();
        if let Some(settings) = ext.get("settings").and_then(Value::as_object) {
            for (key, raw) in settings {
                let value = if raw.is_object() {
                    raw.get("value").ok_or("setting requires value")?
                } else {
                    raw
                };
                definitions.push(settings::parse_setting_definition(key, value, raw)?);
                effective.insert(key.clone(), value.clone());
            }
        }
        settings::validate_settings(&definitions, &Value::Object(effective.clone()))?;
        for setting in &definitions {
            if matches!(setting.control, Some(manifest::SettingControl::Keybind)) {
                settings::Hotkey::parse_with_scancodes(
                    effective[&setting.key]
                        .as_str()
                        .ok_or("Hotkey must be an SDL binding string")?,
                    options.valid_scancode,
                )?;
            }
        }
        let explicit: BTreeSet<_> = definitions
            .iter()
            .filter(|d| d.apply_at_explicit)
            .map(|d| d.key.clone())
            .collect();
        let object = m.as_object_mut().ok_or("mod.json must be an object")?;
        // serde_json's preserve_order Map::remove is swap_remove: the last key takes its place.
        object.swap_remove("$schema");
        object.insert(
            "enabled".into(),
            ext.get("enabled").cloned().unwrap_or(Value::Bool(false)),
        );
        let typed = |value: Result<serde_json::Value, serde_json::Error>| {
            value.map(Value::from).map_err(|e| e.to_string())
        };
        object.insert("target".into(), typed(serde_json::to_value(target))?);
        object.insert("settingValues".into(), Value::Object(effective));
        for (field, empty) in [
            ("groups", Value::Array(vec![])),
            ("presets", Value::Object(json::Map::new())),
            ("changelog", Value::Array(vec![])),
            ("links", Value::Object(json::Map::new())),
        ] {
            object.insert(field.into(), ext.get(field).cloned().unwrap_or(empty));
        }
        let contexts = attach_reservations(ext.get("inputReservations"), &mut definitions)?;
        object.insert(
            "inputContexts".into(),
            typed(serde_json::to_value(&contexts))?,
        );
        object.insert(
            "settings".into(),
            typed(serde_json::to_value(&definitions))?,
        );
        // Fields in document order, so a first error and the preset value maps follow it.
        let mut manifest: Manifest = json::from_value(&m)?;
        for setting in &mut manifest.settings {
            setting.apply_at_explicit = explicit.contains(&setting.key);
        }
        extension::validate_manifest(&manifest)?;
        Ok(Self {
            documents,
            manifest,
            options,
            locale_documents: BTreeMap::new(),
        })
    }
    /// Caller supplies immediate lowercase-`.json` catalog files and bytes, selected from
    /// its package source, named by the path its errors should carry (discovery passes
    /// `locales/<file>`, as SF does); the locale code is the file name's stem. Replacement
    /// is atomic; invalid catalogs reject the package.
    /// SF read_mod_locale_catalogs.rs:29-61; resolve_mod_locale.rs:39-100 (both pins).
    pub fn set_locales<'a>(
        &mut self,
        files: impl IntoIterator<Item = (&'a str, &'a [u8])>,
    ) -> Result<(), String> {
        let mut catalogs = BTreeMap::new();
        let mut docs = BTreeMap::new();
        let mut codes = BTreeSet::new();
        for (name, bytes) in files {
            if !name.ends_with(".json") {
                continue;
            }
            // SF takes Utf8Path::file_stem of the listed path, so the platform's
            // separators split it (both / and \ on Windows, / elsewhere).
            let file = std::path::Path::new(name)
                .file_name()
                .and_then(|file| file.to_str())
                .unwrap_or(name);
            let code = file
                .strip_suffix(".json")
                .ok_or_else(|| format!("{name}: invalid locale filename"))?;
            if !locales::valid_locale_code(code) {
                return Err(format!("{name}: invalid locale filename"));
            }
            if !codes.insert(code.to_ascii_lowercase()) {
                return Err(format!("locales: duplicate locale code '{code}'"));
            }
            let doc = Document::read(name, bytes)?;
            let mut value = doc.value().clone();
            let kind = validate::Schema::Locale;
            let label = kind.name();
            validate::validate_value(
                validate::document_schema(self.options.version, kind),
                &value,
            )
            .map_err(|error| format!("{name}: {label}: {error}"))?;
            value
                .as_object_mut()
                .ok_or("locale must be an object")?
                .swap_remove("$schema");
            catalogs.insert(
                code.to_owned(),
                json::from_value(&value).map_err(|e| format!("{name}: {e}"))?,
            );
            docs.insert(code.to_owned(), doc);
        }
        let mut candidate = self.manifest.clone();
        candidate.locales = catalogs;
        locales::validate_mod_locale_catalogs(&candidate)?;
        self.manifest = candidate;
        self.locale_documents = docs;
        Ok(())
    }
}
fn normalized_document(
    doc: &Document,
    options: ParseOptions,
    kind: validate::Schema,
) -> Result<Value, String> {
    let name = kind.name();
    let schema = validate::document_schema(options.version, kind);
    let mut value = doc.value().clone();
    let fields = schema["properties"]
        .as_object()
        .ok_or("invalid embedded schema")?;
    let object = value
        .as_object_mut()
        .ok_or_else(|| format!("{name} must be an object"))?;
    let unknown: Vec<_> = object
        .keys()
        .filter(|key| !fields.contains_key(key.as_str()))
        .cloned()
        .collect();
    if !options.tolerate_unknown_fields && !unknown.is_empty() {
        return Err(format!(
            "{name}: unknown field{}: {}. Enable Mod Detection to ignore unknown top-level fields.",
            if unknown.len() == 1 { "" } else { "s" },
            unknown.join(", ")
        ));
    }
    // SF drops the unknown keys one at a time, in document order, with serde_json
    // preserve_order's Map::remove, which is swap_remove (serde_json 1.0.151
    // map.rs:156-165; parse_mod_manifest.rs:94-96 in 1.7.9, :99-101 in 1.8): the last key moves into
    // each gap. The closed schema reports errors in instance order, so that order
    // decides which error comes first.
    for key in &unknown {
        object.swap_remove(key);
    }
    validate::validate_value(schema, &value).map_err(|error| format!("{name}: {error}"))?;
    Ok(value)
}
#[derive(Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct Reservations {
    #[serde(default)]
    contexts: BTreeMap<String, String>,
    #[serde(default)]
    keys: BTreeMap<String, manifest::KeyReservation>,
    #[serde(default)]
    wheels: BTreeMap<String, manifest::WheelReservation>,
    #[serde(default)]
    choices: BTreeMap<String, manifest::ReservedDevices>,
}
fn attach_reservations(
    block: Option<&Value>,
    settings: &mut [manifest::SettingDefinition],
) -> Result<BTreeMap<String, String>, String> {
    let Some(block) = block else {
        return Ok(BTreeMap::new());
    };
    let reservations: Reservations = json::from_value(block)?;
    let positions: BTreeMap<_, _> = settings
        .iter()
        .enumerate()
        .map(|(i, s)| (s.key.clone(), i))
        .collect();
    let index = |key: &str| {
        positions
            .get(key)
            .copied()
            .ok_or_else(|| format!("inputReservations names unknown setting '{key}'"))
    };
    for (key, reserve) in reservations.keys {
        settings[index(&key)?].reserve = Some(reserve);
    }
    for (key, wheel) in reservations.wheels {
        settings[index(&key)?].wheel = Some(wheel);
    }
    for (key, choice) in reservations.choices {
        settings[index(&key)?].reserved = Some(choice);
    }
    Ok(reservations.contexts)
}
/// SF package_lookup.rs: valid_identifier, without the manifest-only `.`/`..` exclusion.
pub fn is_package_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 80
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
}
