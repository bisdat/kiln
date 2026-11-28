// SPDX-License-Identifier: MIT
//! Kiln's byte-preserving editor projection, intentionally separate from strict SF acceptance.
//! Adapted from Kiln's MIT crates/kiln-manage/src/shroudforge.rs:107-188,301-524,689-1087.
//! No dependency on kiln-manage, installation state or a filesystem writer.
//! SF semantic anchors: both pins modules/modloader/read_mod_settings.rs:3-166,
//! validate_mod_manifest.rs:73-126. Kiln chooses its own formatting/recovery semantics.
use super::document::{Document, Node};
use crate::json::Value as Doc;
use serde::{Deserialize, Serialize};
type Result<T> = std::result::Result<T, String>;

/// A pure edit request. The caller owns durable publication of the returned bytes.
pub enum Edit<'a> {
    Enabled(bool),
    Setting(&'a str, &'a serde_json::Value),
}
pub fn apply(manifest: &[u8], extension: Option<&[u8]>, edit: Edit<'_>) -> Result<Vec<u8>> {
    let m = read(manifest, extension)?;
    let initial = match extension {
        Some(bytes) => std::str::from_utf8(bytes)
            .map_err(|e| e.to_string())?
            .to_owned(),
        None => {
            // kiln-manage uses a sorted serde map; keep its exact legacy EML bytes
            // even though SF's ordered document model uses preserve_order.
            let value = serde_json::json!({"$schema":"https://bonsaibauer.github.io/shroudforge/schemas/extended.mod.schema.json",
                "enabled":m.enabled,"launcher":"EML","schemaVersion":1,"settings":{},"targets":["client","server"]});
            serde_json::to_string_pretty(&value).map_err(|e| e.to_string())? + "\n"
        }
    };
    if let Edit::Enabled(on) = &edit
        && m.extended
        && m.enabled == *on
        && m.repair.is_empty()
    {
        return Ok(initial.into_bytes());
    }
    let text = repair_fractions(&initial, &m)?;
    let doc = Document::parse(text.as_bytes())?;
    let (span, replacement) = match edit {
        Edit::Enabled(on) => (
            doc.root
                .get("enabled")
                .ok_or("missing enabled")?
                .span
                .clone(),
            on.to_string(),
        ),
        Edit::Setting(key, value) => {
            let setting = m
                .settings
                .iter()
                .find(|s| s.key == key)
                .ok_or_else(|| format!("{} has no setting {key}", m.id))?;
            check_value(setting, value)?;
            let node = doc
                .root
                .get("settings")
                .and_then(|s| s.get(key))
                .ok_or("missing setting token")?;
            let node = if setting.shorthand {
                node
            } else {
                node.get("value").ok_or("missing setting value")?
            };
            (node.span.clone(), render_setting_value(setting, value))
        }
    };
    let mut out = text;
    out.replace_range(span, &replacement);
    bounded(out.as_bytes())?;
    // Validate the actual rendered bytes, after fractional repair and token splicing.
    // A successful proposal must remain loadable, not merely valid bounded JSON.
    super::Package::parse(
        manifest,
        Some(out.as_bytes()),
        super::ParseOptions::default(),
    )
    .map_err(|e| format!("ShroudForge would refuse: {e}"))?;
    Ok(out.into_bytes())
}
fn repair_fractions(text: &str, m: &EditorPackage) -> Result<String> {
    let doc = Document::parse(text.as_bytes())?;
    let mut positions = vec![];
    for setting in m.settings.iter().filter(|s| option_type(s) == "number") {
        let Some(node) = doc.root.get("settings").and_then(|s| s.get(&setting.key)) else {
            continue;
        };
        let node = if setting.shorthand {
            node
        } else {
            node.get("value").ok_or("missing setting value")?
        };
        let tokens: Vec<&Node> = if node.raw(text).starts_with('[') {
            node.elements.iter().collect()
        } else {
            vec![node]
        };
        let integer = |n: &Node| {
            serde_json::from_str::<serde_json::Value>(n.raw(text))
                .is_ok_and(|v| v.as_i64().is_some())
        };
        if tokens.first().is_some_and(|n| integer(n)) {
            positions.extend(
                tokens
                    .into_iter()
                    .filter(|n| integer(n))
                    .map(|n| n.span.end),
            );
        }
    }
    positions.sort_unstable();
    let mut out = text.to_owned();
    for at in positions.into_iter().rev() {
        out.insert_str(at, ".0");
    }
    Ok(out)
}
// Kiln MIT jsonc.rs:612-670 scalar/list rendering; exact uppercase control escapes.
fn quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04X}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
fn render(v: &serde_json::Value, indent: &str) -> String {
    use serde_json::Value;
    match v {
        Value::String(s) => quote(s),
        Value::Number(n)
            if n.is_f64()
                && n.as_f64()
                    .is_some_and(|n| n.fract() == 0.0 && n.abs() < 1e15) =>
        {
            format!("{}", n.as_f64().unwrap() as i64)
        }
        Value::Array(items) if items.iter().all(|v| !v.is_array() && !v.is_object()) => format!(
            "[{}]",
            items
                .iter()
                .map(|v| render(v, indent))
                .collect::<Vec<_>>()
                .join(", ")
        ),
        Value::Array(items) => {
            let inner = format!("{indent}  ");
            format!(
                "[\n{}\n{indent}]",
                items
                    .iter()
                    .map(|v| format!("{inner}{}", render(v, &inner)))
                    .collect::<Vec<_>>()
                    .join(",\n")
            )
        }
        Value::Object(items) if !items.is_empty() => {
            let inner = format!("{indent}  ");
            let sorted: std::collections::BTreeMap<_, _> = items.iter().collect();
            format!(
                "{{\n{}\n{indent}}}",
                sorted
                    .into_iter()
                    .map(|(k, v)| format!("{inner}{}: {}", quote(k), render(v, &inner)))
                    .collect::<Vec<_>>()
                    .join(",\n")
            )
        }
        _ => v.to_string(),
    }
}
fn invalid(message: impl Into<String>) -> String {
    message.into()
}
fn bounded(bytes: &[u8]) -> Result<Document> {
    Document::bounded(bytes).map_err(|e| format!("ShroudForge would refuse: {e}"))
}
/// One setting of a ShroudForge mod (extended.mod.json `settings.<key>`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EditorSetting {
    pub key: String,
    pub value: serde_json::Value,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    /// toggle, checkbox, text, textarea, number, slider, select, radio, segmented, multiselect, keybind, color
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub control: Option<String>,
    /// What the mod declares: "live" or "restart" (ShroudForge's default is "restart" unless `restartRequired` is false).
    /// Whether a change really applies while the game runs is [`EditorSetting::live`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub apply_at: Option<String>,
    /// A change applies while the game runs (ShroudForge's `settings_to_apply_live`): the mod has a runtime part, and it
    /// has no startup (`patch`) part or the setting is declared live. Filled by `read`.
    #[serde(default)]
    pub live: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub step: Option<f64>,
    /// value -> label (select, radio, segmented, multiselect)
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub options: Option<serde_json::Map<String, serde_json::Value>>,
    /// The option keys in the order the mod declares them (the map above may be sorted:
    /// serde_json sorts without preserve_order); [`option_keys`]. Filled by `read`.
    #[serde(skip)]
    pub option_order: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled_when: Option<serde_json::Value>,
    /// text length limits (`minLength`, `maxLength`, in characters)
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_length: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_length: Option<u64>,
    /// the shorthand form `settings.<key> = value` (no label, no control)
    #[serde(default)]
    pub shorthand: bool,
}

/// A ShroudForge mod package.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EditorPackage {
    pub id: String,
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub authors: Vec<String>,
    /// extended.mod.json `enabled` (false when the file is missing; mod.json is not ShroudForge's switch).
    pub enabled: bool,
    /// "client", "server" (extended.mod.json `targets`; ShroudForge's defaults when missing).
    pub targets: Vec<String>,
    /// mod.json `capabilities`: patch (startup asset changes), export, runtime (live game access), runtime-register-dll
    pub capabilities: Vec<String>,
    /// Switching it on or off applies while the game runs (a runtime mod without startup asset changes); otherwise at
    /// the next start. ShroudForge's `refresh_runtime_configuration`.
    #[serde(default)]
    pub switch_live: bool,
    #[serde(default)]
    pub dependencies: Vec<serde_json::Value>,
    pub settings: Vec<EditorSetting>,
    #[serde(default)]
    pub groups: Vec<serde_json::Value>,
    /// "EML" for a legacy EML package, "SF" or none for ShroudForge's own
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub launcher: Option<String>,
    /// extended.mod.json exists
    pub extended: bool,
    /// Settings whose whole numbers an earlier Kiln wrote without their fraction: ShroudForge refuses the mod until the
    /// next change (of the switch or any setting) puts the fraction back. Read as the fractional settings they were.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub repair: Vec<String>,
}

/// (has a runtime part, has a startup asset part): ShroudForge's `Capability::requires_runtime` and `Patch`.
fn runtime_parts(capabilities: &[String]) -> (bool, bool) {
    (
        capabilities
            .iter()
            .any(|c| c == "runtime" || c == "runtime-register-dll"),
        capabilities.iter().any(|c| c == "patch"),
    )
}

fn str_of(v: &Doc, k: &str) -> Option<String> {
    v.get(k).and_then(|s| s.as_str()).map(str::to_owned)
}

fn parse_setting(key: &str, raw: &Doc) -> EditorSetting {
    let obj = raw.as_object().filter(|o| o.contains_key("value"));
    let Some(o) = obj else {
        return EditorSetting {
            key: key.into(),
            value: raw.to_serde(),
            label: None,
            description: None,
            control: None,
            apply_at: Some("restart".into()),
            live: false,
            min: None,
            max: None,
            step: None,
            options: None,
            option_order: Vec::new(),
            enabled_when: None,
            min_length: None,
            max_length: None,
            shorthand: true,
        };
    };
    let f = |k: &str| o.get(k).and_then(|v| v.as_f64());
    let options = o.get("options").and_then(|v| v.as_object());
    EditorSetting {
        key: key.into(),
        value: o["value"].to_serde(),
        label: o.get("label").and_then(|s| s.as_str()).map(str::to_owned),
        description: o
            .get("description")
            .and_then(|s| s.as_str())
            .map(str::to_owned),
        control: o.get("control").and_then(|s| s.as_str()).map(str::to_owned),
        // ShroudForge (read_mod_settings.rs): applyAt, else "live" when restartRequired is false, else "restart"
        apply_at: Some(
            o.get("applyAt")
                .and_then(|s| s.as_str())
                .map(str::to_owned)
                .unwrap_or_else(|| {
                    if o.get("restartRequired").and_then(|b| b.as_bool()) == Some(false) {
                        "live"
                    } else {
                        "restart"
                    }
                    .to_owned()
                }),
        ),
        live: false,
        min: f("min"),
        max: f("max"),
        step: f("step"),
        options: options.map(|map| {
            map.iter()
                .map(|(key, label)| (key.clone(), label.to_serde()))
                .collect()
        }),
        option_order: options
            .map(|map| map.keys().cloned().collect())
            .unwrap_or_default(),
        enabled_when: o.get("enabledWhen").map(Doc::to_serde),
        min_length: o.get("minLength").and_then(|v| v.as_u64()),
        max_length: o.get("maxLength").and_then(|v| v.as_u64()),
        shorthand: false,
    }
}

/// ShroudForge's own rule for a package id (mod.json `id`): 1 to 80 ASCII letters (either case), digits, `.`, `-`, `_`.
/// Wider than a Kiln id: Kiln imports such a package under a slug Library id and keeps the files as they are.
pub fn is_package_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 80
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
}

/// The intersection of SF's version schema and its strict SemVer deserializer, not Kiln's permissive Library version.
fn package_version(version: &str) -> bool {
    let split = version.find(['-', '+']);
    let core = &version[..split.unwrap_or(version.len())];
    let parts: Vec<_> = core.split('.').collect();
    if parts.len() != 3
        || parts.iter().any(|p| {
            p.is_empty()
                || !p.bytes().all(|b| b.is_ascii_digit())
                || (p.len() > 1 && p.starts_with('0'))
                || p.parse::<u64>().is_err()
        })
    {
        return false;
    }
    split.is_none_or(|i| {
        version[i + 1..].split('.').all(|p| {
            !p.is_empty()
                && p.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
                && !(version.as_bytes()[i] == b'-'
                    && p.len() > 1
                    && p.starts_with('0')
                    && p.bytes().all(|b| b.is_ascii_digit()))
        })
    })
}

/// A setting's fields in ShroudForge's `$defs.setting`, the same in its 1.7.9 schema and every 1.8 snapshot. The schema
/// closes it (`additionalProperties: false`) and ShroudForge checks extended.mod.json against it when it reads a package
/// (parse_mod_manifest.rs: only unknown top-level fields are tolerated), so a setting with any other field refuses the mod.
pub const SETTING_FIELDS: [&str; 14] = [
    "value",
    "label",
    "description",
    "control",
    "applyAt",
    "restartRequired",
    "assetPreparation",
    "enabledWhen",
    "min",
    "max",
    "step",
    "minLength",
    "maxLength",
    "options",
];

/// What ShroudForge's own reader and schema refuse in one setting's metadata (`raw`: the setting as declared), in words
/// naming the field and what it expects; kiln-manage's shroudforge.rs check_metadata says the same. ShroudForge's
/// `$defs.setting` in its 1.7 and every 1.8 schema snapshot, and read_mod_settings.rs: anything those accept passes.
/// The rest of the schema is the strict model's, which `read` runs last (its message then is the schema's).
fn check_metadata(s: &EditorSetting, raw: &Doc) -> std::result::Result<(), String> {
    if s.value.is_null() || s.value.is_object() {
        return Err(format!(
            "value must be a boolean, text, a number or a list, not {}",
            kind(&s.value)
        ));
    }
    if let Some(field) = raw
        .as_object()
        .and_then(|o| o.keys().find(|k| !SETTING_FIELDS.contains(&k.as_str())))
    {
        return Err(format!(
            "field {} is not one a setting may have ({})",
            quoted(field),
            SETTING_FIELDS.join(", ")
        ));
    }
    // ShroudForge's reader (read_mod_settings.rs) and schema: control is one of its controls, applyAt "live" or "restart"
    // (absent: restart, or live when restartRequired is false), restartRequired and assetPreparation true or false
    if let Some(c) = raw
        .get("control")
        .filter(|c| !c.as_str().is_some_and(|c| CONTROLS.contains(&c)))
    {
        return Err(format!(
            "control must be one of {}, not {}",
            CONTROLS.join(", "),
            shown(c)
        ));
    }
    if let Some(a) = raw
        .get("applyAt")
        .filter(|a| !matches!(a.as_str(), Some("live" | "restart")))
    {
        return Err(format!(
            "applyAt must be \"live\" or \"restart\", not {}",
            shown(a)
        ));
    }
    for field in ["restartRequired", "assetPreparation"] {
        if let Some(v) = raw.get(field).filter(|v| !v.is_boolean()) {
            return Err(format!("{field} must be true or false, not {}", shown(v)));
        }
    }
    if raw
        .get("restartRequired")
        .and_then(|v| v.as_bool())
        .is_some_and(|r| r != (s.apply_at.as_deref() == Some("restart")))
    {
        return Err(
            "applyAt and restartRequired must agree (restartRequired: true goes with applyAt \"restart\")"
                .into(),
        );
    }
    // ShroudForge's schema: options name at least one choice ("options": {} refuses the mod)
    if raw
        .get("options")
        .is_some_and(|o| o.as_object().is_none_or(|o| o.is_empty()))
    {
        return Err("options must name at least one choice".into());
    }
    if s.value.is_array() && s.options.as_ref().is_none_or(|o| o.is_empty()) {
        return Err("a list value needs options naming its choices".into());
    }
    for (k, label) in s.options.iter().flatten() {
        if option_value(s, k).is_none() {
            let ty = option_type(s);
            let article = if ty.starts_with(['a', 'e', 'i', 'o', 'u']) {
                "an"
            } else {
                "a"
            };
            return Err(format!(
                "option {} must be written as {article} {ty} like the value",
                quoted(k)
            ));
        }
        if let Some(why) = label_problem(label) {
            return Err(format!("option {}'s {why}", quoted(k)));
        }
    }
    // ShroudForge's SettingCondition (deny_unknown_fields) and schema: the two fields and nothing else
    if s.enabled_when.as_ref().is_some_and(|w| {
        w.get("setting").and_then(|v| v.as_str()).is_none()
            || w.get("equals").is_none()
            || w.as_object().is_some_and(|o| o.len() > 2)
    }) {
        return Err(
            "enabledWhen must name a setting and the value it equals, and nothing else ({ \"setting\": key, \"equals\": value })"
                .into(),
        );
    }
    Ok(())
}

/// `control`'s values in ShroudForge's `$defs.setting` (its SettingControl).
pub const CONTROLS: [&str; 12] = [
    "toggle",
    "checkbox",
    "text",
    "textarea",
    "number",
    "slider",
    "select",
    "radio",
    "segmented",
    "multiselect",
    "keybind",
    "color",
];

/// A value of a package in a message: its JSON text, at most 60 characters of it.
fn shown(v: &Doc) -> String {
    let text = v.to_serde().to_string();
    match text.char_indices().nth(60) {
        Some((at, _)) => format!("{}...", &text[..at]),
        None => text,
    }
}

/// A key or field name of a package in a message: quoted, and at most 80 characters of it.
fn quoted(text: &str) -> String {
    match text.char_indices().nth(80) {
        Some((at, _)) => format!("{:?}...", &text[..at]),
        None => format!("{text:?}"),
    }
}

/// Why ShroudForge refuses an option's label, or None: `$defs.setting.options` in its 1.8 schemas (which ShroudForge checks
/// a package against when it reads it) and read_mod_settings.rs. A label is 1 to 120 characters of text, or
/// `{ "label": text, "group": text }` with nothing else, its label 1 to 120 characters and its group 1 to 60
/// (jsonschema counts characters, not bytes); kiln-manage's shroudforge.rs label_problem says the same.
fn label_problem(label: &serde_json::Value) -> Option<String> {
    const FORM: &str = "label must be text, or { \"label\": text, \"group\": text }";
    let text = match label {
        serde_json::Value::String(text) => text,
        serde_json::Value::Object(o) => {
            if let Some(field) = o.keys().find(|k| *k != "label" && *k != "group") {
                return Some(format!(
                    "label may hold only \"label\" and \"group\", not {}",
                    quoted(field)
                ));
            }
            if o.get("group").is_some_and(|g| {
                !g.as_str()
                    .is_some_and(|g| (1..=60).contains(&g.chars().count()))
            }) {
                return Some("group must be text of 1 to 60 characters".into());
            }
            match o.get("label").and_then(|l| l.as_str()) {
                Some(text) => text,
                None => return Some(FORM.into()),
            }
        }
        _ => return Some(FORM.into()),
    };
    (!(1..=120).contains(&text.chars().count()))
        .then(|| "label must be text of 1 to 120 characters".into())
}

/// An option's label text: the label itself, or (ShroudForge 1.8) the `label` of `{ "label": text, "group": text }`.
/// None when ShroudForge refuses it ([`label_problem`]).
pub fn option_label(label: &serde_json::Value) -> Option<&str> {
    if label_problem(label).is_some() {
        return None;
    }
    label.as_str().or_else(|| label.get("label")?.as_str())
}

/// The group an option sorts into (ShroudForge 1.8's `{ "label": text, "group": text }`).
pub fn option_group(label: &serde_json::Value) -> Option<&str> {
    label.get("group")?.as_str()
}

/// Validate package conditions, not the editor's defensive numeric equality. Walk the dependency graph only once.
fn validate_conditions(settings: &[EditorSetting]) -> Result<()> {
    use std::collections::{HashMap, HashSet};
    let by_key: HashMap<_, _> = settings.iter().map(|s| (s.key.as_str(), s)).collect();
    for s in settings {
        let Some(w) = &s.enabled_when else { continue };
        let target = w["setting"].as_str().unwrap(); // metadata shape was checked first
        let controller = by_key.get(target).ok_or_else(|| {
            invalid(format!(
                "ShroudForge would refuse setting {}: enabledWhen names unknown setting {target}",
                s.key
            ))
        })?;
        let value = &w["equals"];
        let matches = match value_type(controller) {
            "boolean" => value.is_boolean(),
            "string" => value.is_string(),
            "integer" => value.as_i64().is_some(),
            "number" => value.as_f64().is_some(),
            _ => false,
        };
        if !matches
            || controller.options.as_ref().is_some_and(|opts| {
                !opts
                    .keys()
                    .any(|k| option_value(controller, k).as_ref() == Some(value))
            })
        {
            return Err(invalid(format!(
                "ShroudForge would refuse setting {}: enabledWhen equals must match {target}'s type and options",
                s.key
            )));
        }
    }
    let mut done = HashSet::new();
    for s in settings.iter().filter(|s| s.enabled_when.is_some()) {
        let mut chain = HashSet::new();
        let mut current = s.key.as_str();
        while !done.contains(current) {
            if !chain.insert(current) {
                return Err(invalid(format!(
                    "ShroudForge would refuse setting {}: enabledWhen has a dependency cycle",
                    s.key
                )));
            }
            let Some(w) = &by_key[current].enabled_when else {
                break;
            };
            current = w["setting"].as_str().unwrap();
        }
        done.extend(chain);
    }
    Ok(())
}

pub fn read(manifest: &[u8], extension: Option<&[u8]>) -> Result<EditorPackage> {
    let m = bounded(manifest)?.value().clone();
    let id = str_of(&m, "id").ok_or_else(|| invalid("mod.json has no id"))?;
    let version = str_of(&m, "version").filter(|v| package_version(v))
        .ok_or_else(|| invalid("ShroudForge would refuse mod.json version: expected a strict three-part version with at most a prerelease or build suffix"))?;
    let extended = extension.is_some();
    let document = extension.map(bounded).transpose()?;
    let ext = document
        .as_ref()
        .map(|d| d.value().clone())
        .unwrap_or_else(|| Doc::Object(Default::default()));
    if !ext.is_object() {
        return Err(invalid("extended.mod.json is not a JSON object"));
    }
    if extended && ext.get("schemaVersion").and_then(|v| v.as_f64()) != Some(1.0) {
        return Err(invalid(
            "ShroudForge would refuse extended.mod.json: required schemaVersion must be the number 1",
        ));
    }
    if extended && ext.get("enabled").and_then(|v| v.as_bool()).is_none() {
        return Err(invalid(
            "ShroudForge would refuse extended.mod.json: required enabled must be a boolean",
        ));
    }
    if ext.get("settings").is_some_and(|s| !s.is_object()) {
        return Err(invalid("extended.mod.json settings must be an object"));
    }
    if ext
        .get("launcher")
        .is_some_and(|v| !matches!(v.as_str(), Some("EML" | "SF")))
    {
        return Err(invalid(
            "ShroudForge would refuse extended.mod.json: launcher must be EML or SF",
        ));
    }
    if let Some(targets) = ext.get("targets") {
        let valid = targets.as_array().is_some_and(|a| {
            !a.is_empty()
                && a.len() <= 2
                && a.iter()
                    .all(|v| matches!(v.as_str(), Some("client" | "server")))
                && (a.len() == 1 || a[0] != a[1])
        });
        if !valid {
            return Err(invalid(
                "ShroudForge would refuse extended.mod.json: targets must name client or server without duplicates",
            ));
        }
    }
    let launcher = str_of(&ext, "launcher").or_else(|| (!extended).then(|| "EML".to_owned()));
    let targets = match ext.get("targets").and_then(|t| t.as_array()) {
        Some(a) => a
            .iter()
            .filter_map(|v| v.as_str().map(str::to_owned))
            .collect(),
        // ShroudForge: legacy EML packages run on both sides, ShroudForge packages on the client
        None if launcher.as_deref() == Some("EML") => vec!["client".into(), "server".into()],
        None => vec!["client".into()],
    };
    let mut settings: Vec<EditorSetting> = ext
        .get("settings")
        .and_then(|s| s.as_object())
        .map(|o| o.iter().map(|(k, v)| parse_setting(k, v)).collect())
        .unwrap_or_default();
    let capabilities: Vec<String> = m
        .get("capabilities")
        .and_then(|a| a.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(str::to_owned))
                .collect()
        })
        .unwrap_or_default();
    let (runtime, patch) = runtime_parts(&capabilities);
    let mut repair = Vec::new();
    for s in &mut settings {
        if recover_fraction(s) {
            repair.push(s.key.clone());
        }
        check_metadata(s, &ext["settings"][&s.key])
            .map_err(|why| invalid(format!("ShroudForge would refuse setting {}: {why}", s.key)))?;
        s.live = runtime && (!patch || s.apply_at.as_deref() == Some("live"));
    }
    validate_conditions(&settings)?;
    let package = EditorPackage {
        name: str_of(&m, "name").unwrap_or_else(|| id.clone()),
        id,
        version,
        description: str_of(&m, "description").unwrap_or_default(),
        authors: m
            .get("authors")
            .and_then(|a| a.as_array())
            .map(|a| {
                a.iter()
                    .filter_map(|v| v.as_str().map(str::to_owned))
                    .collect()
            })
            .unwrap_or_default(),
        enabled: ext
            .get("enabled")
            .and_then(|b| b.as_bool())
            .unwrap_or(false),
        targets,
        switch_live: runtime && !patch,
        capabilities,
        dependencies: m
            .get("dependencies")
            .and_then(|a| a.as_array())
            .map(|a| a.iter().map(Doc::to_serde).collect())
            .unwrap_or_default(),
        settings,
        groups: ext
            .get("groups")
            .and_then(|a| a.as_array())
            .map(|a| a.iter().map(Doc::to_serde).collect())
            .unwrap_or_default(),
        launcher,
        extended,
        repair,
    };
    // The sole runtime-invalid projection is repair of Kiln-damaged fractional
    // values. Validate the recovered candidate too, so that this exception can
    // never excuse unrelated invalid schema, capabilities or metadata.
    let repaired = extension
        .map(|bytes| {
            repair_fractions(
                std::str::from_utf8(bytes).map_err(|e| e.to_string())?,
                &package,
            )
        })
        .transpose()?;
    super::Package::parse(
        manifest,
        repaired.as_deref().map(str::as_bytes),
        super::ParseOptions::default(),
    )
    .map_err(|e| format!("ShroudForge would refuse: {e}"))?;
    Ok(package)
}

fn recover_fraction(s: &mut EditorSetting) -> bool {
    let Some(opts) = s.options.as_ref().filter(|o| !o.is_empty()) else {
        return false;
    };
    if option_type(s) != "integer"
        || !opts
            .keys()
            .all(|k| k.parse::<f64>().is_ok_and(f64::is_finite))
        || opts.keys().all(|k| k.parse::<i64>().is_ok())
    {
        return false;
    }
    let fractional = |v: &mut serde_json::Value| {
        if let Some(n) = v.as_f64().and_then(serde_json::Number::from_f64) {
            *v = serde_json::Value::Number(n);
        }
    };
    match &mut s.value {
        serde_json::Value::Array(items) if items.iter().all(|v| v.is_number()) => {
            items.iter_mut().for_each(fractional)
        }
        v @ serde_json::Value::Number(_) => fractional(v),
        _ => return false,
    }
    true
}

fn render_setting_value(s: &EditorSetting, value: &serde_json::Value) -> String {
    let ty = option_type(s);
    let one = |v: &serde_json::Value| -> String {
        match v {
            serde_json::Value::Number(n) if ty == "number" => n
                .as_f64()
                .and_then(serde_json::Number::from_f64)
                .map(|f| f.to_string())
                .unwrap_or_else(|| n.to_string()),
            serde_json::Value::Number(n) if ty == "integer" && n.is_f64() => n
                .as_f64()
                .filter(|f| f.fract() == 0.0 && f.abs() < 9.0e15)
                .map(|f| (f as i64).to_string())
                .unwrap_or_else(|| n.to_string()),
            v => render(v, ""),
        }
    };
    match value {
        serde_json::Value::Array(items)
            if items.iter().all(|v| !v.is_array() && !v.is_object()) =>
        {
            format!("[{}]", items.iter().map(one).collect::<Vec<_>>().join(", "))
        }
        serde_json::Value::Array(_) | serde_json::Value::Object(_) => render(value, ""),
        v => one(v),
    }
}

pub fn check_value(s: &EditorSetting, value: &serde_json::Value) -> Result<()> {
    use serde_json::Value as V;
    let same_kind = matches!(
        (&s.value, value),
        (V::Bool(_), V::Bool(_))
            | (V::String(_), V::String(_))
            | (V::Number(_), V::Number(_))
            | (V::Array(_), V::Array(_))
    );
    if !same_kind {
        return Err(invalid(format!(
            "setting {} holds a {}, not a {}",
            s.key,
            kind(&s.value),
            kind(value)
        )));
    }
    // Without a first member ShroudForge retypes every option as text. Keep numeric/boolean lists typed, rather than
    // silently turning the next selection into strings (an already textual list may be cleared).
    if value.as_array().is_some_and(|a| a.is_empty()) && option_type(s) != "string" {
        return Err(invalid(format!(
            "setting {} must keep at least one choice so ShroudForge keeps its option type",
            s.key
        )));
    }
    // (a number is always finite here: serde_json holds no NaN or infinity, `Value::from(f64::NAN)` is null)
    // ShroudForge types a list by its first member: a choice without a value (null) there would make it read the list as
    // text and refuse the mod
    if value
        .as_array()
        .and_then(|a| a.first())
        .is_some_and(|v| v.is_null())
    {
        return Err(invalid(format!(
            "setting {}: a choice without a value cannot come first in the list (ShroudForge would refuse the mod)",
            s.key
        )));
    }
    // ShroudForge types a setting by its declared value: a whole number is an integer setting
    if s.value.is_i64() {
        let whole = value.is_i64()
            || value
                .as_f64()
                .is_some_and(|n| n.fract() == 0.0 && n.abs() < 9.0e15);
        if !whole {
            return Err(invalid(format!(
                "setting {} holds a whole number, not {value}",
                s.key
            )));
        }
    }
    if let Some(text) = value.as_str() {
        if s.control.as_deref() == Some("keybind") && !valid_shortcut(text) {
            return Err(invalid(format!(
                "setting {} requires a canonical SDL shortcut (e.g. Ctrl+SDL:66 or Mouse:4)",
                s.key
            )));
        }
        let n = text.chars().count() as u64;
        if s.min_length.is_some_and(|m| n < m) || s.max_length.is_some_and(|m| n > m) {
            return Err(invalid(format!(
                "setting {} must be {}..{} characters long",
                s.key,
                s.min_length.unwrap_or(0),
                s.max_length.map(|v| v.to_string()).unwrap_or_default()
            )));
        }
    }
    if let serde_json::Value::Number(n) = value
        && (s
            .min
            .and_then(serde_json::Number::from_f64)
            .is_some_and(|m| super::validate::number_cmp(n, &m).is_lt())
            || s.max
                .and_then(serde_json::Number::from_f64)
                .is_some_and(|m| super::validate::number_cmp(n, &m).is_gt()))
    {
        return Err(invalid(format!(
            "setting {} must be within {}..{}",
            s.key,
            s.min.map(|v| v.to_string()).unwrap_or_default(),
            s.max.map(|v| v.to_string()).unwrap_or_default()
        )));
    }
    if let Some(opts) = s.options.as_ref().filter(|o| !o.is_empty()) {
        let ok = |v: &V| {
            opts.keys()
                .filter_map(|k| option_value(s, k))
                .any(|o| super::validate::schema_equal(&Doc::from(&o), &Doc::from(v)))
        };
        let all = match value {
            V::Array(items) => items.iter().all(ok),
            v => ok(v),
        };
        if !all {
            return Err(invalid(format!(
                "setting {}: not one of {}",
                s.key,
                option_keys(s)
                    .iter()
                    .map(|k| k.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            )));
        }
    }
    Ok(())
}

/// SF's canonical portable shortcut syntax. SF's SDL backend additionally checks keyboard scancode availability.
pub fn valid_shortcut(text: &str) -> bool {
    if text.is_empty() {
        return true;
    }
    if text.len() > 100 {
        return false;
    }
    let (mut keyboard, mut mouse) = (false, false);
    for chord in text.split('|') {
        let parts: Vec<_> = chord.split('+').collect();
        let Some((device, number)) = parts[parts.len() - 1].split_once(':') else {
            return false;
        };
        let Ok(code) = number.parse::<u16>() else {
            return false;
        };
        if code.to_string() != number {
            return false;
        }
        let primary = match device {
            "SDL" if !keyboard && !mouse && (1..512).contains(&code) => {
                keyboard = true;
                match code {
                    224 | 228 => 1,
                    226 | 230 => 2,
                    225 | 229 => 4,
                    227 | 231 => 8,
                    _ => 0,
                }
            }
            "Mouse" if !mouse && (1..=32).contains(&code) => {
                mouse = true;
                0
            }
            _ => return false,
        };
        let mut previous = 0;
        for modifier in &parts[..parts.len() - 1] {
            let bit = match *modifier {
                "Ctrl" => 1,
                "Alt" => 2,
                "Shift" => 4,
                "Meta" => 8,
                _ => 0,
            };
            if bit <= previous || bit & primary != 0 {
                return false;
            }
            previous = bit;
        }
    }
    true
}

fn kind(v: &serde_json::Value) -> &'static str {
    match v {
        serde_json::Value::Bool(_) => "boolean",
        serde_json::Value::String(_) => "string",
        serde_json::Value::Number(_) => "number",
        serde_json::Value::Array(_) => "list",
        serde_json::Value::Object(_) => "object",
        serde_json::Value::Null => "null",
    }
}

/// ShroudForge's type of a setting, from its declared value: boolean, integer, number, string or array.
pub fn value_type(s: &EditorSetting) -> &'static str {
    match &s.value {
        serde_json::Value::Bool(_) => "boolean",
        serde_json::Value::Number(n) if n.is_i64() => "integer",
        serde_json::Value::Number(_) => "number",
        serde_json::Value::Array(_) => "array",
        _ => "string",
    }
}

fn option_type(s: &EditorSetting) -> &'static str {
    if let Some(a) = s.value.as_array() {
        match a.first() {
            Some(serde_json::Value::Bool(_)) => "boolean",
            Some(serde_json::Value::Number(n)) if n.is_i64() => "integer",
            Some(serde_json::Value::Number(_)) => "number",
            _ => "string",
        }
    } else {
        value_type(s)
    }
}

/// An option key typed as ShroudForge types it (the scalar value, or the first array member; an empty array uses text),
/// with its rules (read_mod_settings.rs): a yes/no key is true only as "true"; numbers are read by Rust's `str::parse`, and
/// a fractional key that is not a finite double is an option whose value is null. None: ShroudForge refuses the key.
pub fn option_value(s: &EditorSetting, key: &str) -> Option<serde_json::Value> {
    match option_type(s) {
        "boolean" => Some((key == "true").into()),
        "integer" => key.parse::<i64>().ok().map(serde_json::Value::from),
        "number" => key.parse::<f64>().ok().map(|f| {
            serde_json::Number::from_f64(f)
                .map_or(serde_json::Value::Null, serde_json::Value::Number)
        }),
        _ => Some(key.into()),
    }
}

/// The option keys in the order the mod declares them.
pub fn option_keys(s: &EditorSetting) -> Vec<&String> {
    let Some(opts) = &s.options else {
        return Vec::new();
    };
    let declared: Vec<&String> = s
        .option_order
        .iter()
        .filter(|k| opts.contains_key(k.as_str()))
        .collect();
    if declared.len() == opts.len() {
        declared
    } else {
        opts.keys().collect()
    }
}

fn same_value(a: &serde_json::Value, b: &serde_json::Value) -> bool {
    match (a.as_f64(), b.as_f64()) {
        (Some(a), Some(b)) => a == b,
        _ => a == b,
    }
}

/// The label ShroudForge shows: `label`, else the key with a space before each capital (`noFallDamage` -> "no Fall
/// Damage").
pub fn label_of(s: &EditorSetting) -> String {
    if let Some(l) = &s.label {
        return l.clone();
    }
    let mut out = String::with_capacity(s.key.len() + 4);
    for (i, c) in s.key.chars().enumerate() {
        if i > 0 && c.is_uppercase() {
            out.push(' ');
        }
        out.push(c);
    }
    out
}

/// The control ShroudForge draws: `control`, else a select (a multiselect for a list) when the setting has options.
pub fn control_of(s: &EditorSetting) -> Option<String> {
    s.control.clone().or_else(|| {
        s.options.as_ref().filter(|o| !o.is_empty()).map(|_| {
            if s.value.is_array() {
                "multiselect"
            } else {
                "select"
            }
            .to_owned()
        })
    })
}

/// Whether the setting is editable now: its `enabledWhen` chain holds for `values` (ShroudForge's
/// `setting_is_enabled`: a missing controller or a cycle disables it; numbers compare by value, 5 = 5.0).
pub fn setting_enabled(key: &str, settings: &[EditorSetting], values: &serde_json::Value) -> bool {
    let mut current = key.to_owned();
    let mut seen = std::collections::BTreeSet::new();
    loop {
        let Some(s) = settings.iter().find(|s| s.key == current) else {
            return false;
        };
        let Some(cond) = &s.enabled_when else {
            return true;
        };
        if !seen.insert(current.clone()) {
            return false;
        }
        let (Some(target), Some(equals)) = (
            cond.get("setting").and_then(|v| v.as_str()),
            cond.get("equals"),
        ) else {
            return false;
        };
        let Some(value) = values.get(target) else {
            return false;
        };
        if !same_value(value, equals) {
            return false;
        }
        current = target.to_owned();
    }
}

/// The settings' current values as one object (key -> value), the SchemaForm's data.
pub fn settings_values(m: &EditorPackage) -> serde_json::Value {
    serde_json::Value::Object(
        m.settings
            .iter()
            .map(|s| (s.key.clone(), s.value.clone()))
            .collect(),
    )
}
