// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/modloader/read_mod_settings.rs:3-163 (1.7.9), :3-184 (1.8)
// Source: modules/modloader/validate_mod_settings.rs:3-39
// Source: modules/modloader/infer_mod_api_contract.rs:2-103
// Contract pins: 1.7.9 d8a394bc; 1.8.0 38ef448d (1.8 additions are version-gated).
use super::manifest::*;
use crate::json::{self, Value as Doc};
use serde_json::{Value, json};

// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// modules/hotkeys/parse_hotkey.rs:108-181 (both pins); observe_keyboard.rs:106-110 (1.8).
// Device delivery and Windows SDL scancode availability belong to the input host.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hotkey {
    chords: Vec<(bool, u16, u8)>,
}
impl Hotkey {
    pub fn parse(text: &str) -> Result<Self, String> {
        Self::parse_with_scancodes(text, |code| (1..512).contains(&code))
    }
    pub fn parse_with_scancodes(
        text: &str,
        valid_scancode: impl Fn(u16) -> bool,
    ) -> Result<Self, String> {
        if text.len() > 100 {
            return Err("Hotkey is longer than 100 characters".into());
        }
        let mut chords = Vec::new();
        if !text.is_empty() {
            for chord in text.split('|') {
                let parts: Vec<_> = chord.split('+').collect();
                let (key, prefixes) = parts.split_last().ok_or("missing hotkey")?;
                let (mouse, code) = if let Some(n) = key.strip_prefix("SDL:") {
                    (
                        false,
                        n.parse::<u16>()
                            .ok()
                            .filter(|n| valid_scancode(*n))
                            .ok_or("Hotkey requires a valid SDL scancode")?,
                    )
                } else if let Some(n) = key.strip_prefix("Mouse:") {
                    (
                        true,
                        n.parse::<u16>()
                            .ok()
                            .filter(|n| (1..=32).contains(n))
                            .ok_or("Mouse buttons must be between 1 and 32")?,
                    )
                } else {
                    return Err("Hotkey requires an SDL scancode or a mouse button".into());
                };
                if chords.iter().any(|(device, _, _)| *device == mouse) {
                    return Err("A shortcut has at most one keyboard and one mouse binding".into());
                }
                let mut modifiers = 0;
                for prefix in prefixes {
                    let bit = match *prefix {
                        "Ctrl" => 1,
                        "Alt" => 2,
                        "Shift" => 4,
                        "Meta" => 8,
                        _ => return Err(format!("Unknown hotkey modifier '{prefix}'")),
                    };
                    if modifiers & bit != 0 {
                        return Err(format!("Duplicate hotkey modifier '{prefix}'"));
                    }
                    modifiers |= bit;
                }
                let primary = if mouse { 0 } else { primary_modifier(code) };
                chords.push((mouse, code, modifiers & !primary));
            }
        }
        chords.sort_by_key(|c| c.0);
        let key = Self { chords };
        if key.normalized() != text {
            return Err(format!("Use the canonical hotkey '{}'", key.normalized()));
        }
        Ok(key)
    }
    pub fn keyboard_scancodes(&self) -> impl Iterator<Item = u16> + '_ {
        self.chords
            .iter()
            .filter(|(mouse, _, _)| !mouse)
            .map(|(_, code, _)| *code)
    }
    pub fn normalized(&self) -> String {
        self.chords
            .iter()
            .map(|(mouse, code, modifiers)| {
                let mut parts: Vec<String> = [(1, "Ctrl"), (2, "Alt"), (4, "Shift"), (8, "Meta")]
                    .into_iter()
                    .filter(|(bit, _)| modifiers & bit != 0)
                    .map(|(_, s)| s.to_owned())
                    .collect();
                parts.push(format!("{}:{code}", if *mouse { "Mouse" } else { "SDL" }));
                parts.join("+")
            })
            .collect::<Vec<_>>()
            .join("|")
    }
    pub fn overlaps(&self, other: &Self) -> bool {
        self.chords.iter().any(|(mouse, code, mods)| {
            other.chords.iter().any(|(m, c, r)| {
                let primary = if *mouse { 0 } else { primary_modifier(*code) };
                mouse == m && code == c && (mods | primary) == (r | primary)
            })
        })
    }
}
/// UI-only draft expansion; strict parser and byte editor do not call this.
/// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
/// modules/modloader/ui/apply-setting-presets.ts:3-37 (1.8 only).
pub fn apply_setting_change(
    values: &Value,
    presets: &std::collections::BTreeMap<String, SettingPreset>,
    key: &str,
    value: Value,
) -> Value {
    let mut next = values.clone();
    next[key] = value.clone();
    if let Some(chosen) = presets.get(key) {
        if let Some(preset) = value.as_str().and_then(|s| chosen.values.get(s)) {
            for member in &chosen.settings {
                next[member] = preset.get(member).cloned().unwrap_or(Value::Null);
            }
        }
        return next;
    }
    for (controller, preset) in presets {
        if !preset.settings.iter().any(|s| s == key) || next[controller] == preset.custom {
            continue;
        }
        let expected = next[controller].as_str().and_then(|s| preset.values.get(s));
        if expected
            .and_then(|p| p.get(key))
            .is_none_or(|expected| !ui_same_value(expected, &value))
        {
            next[controller] = preset.custom.clone().into();
        }
    }
    next
}
fn ui_same_value(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(a), Value::Number(b)) => a.as_f64() == b.as_f64(),
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(a, b)| ui_same_value(a, b))
        }
        (Value::Object(a), Value::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .zip(b)
                    .all(|((ka, a), (kb, b))| ka == kb && ui_same_value(a, b))
        }
        _ => a == b,
    }
}
fn primary_modifier(code: u16) -> u8 {
    match code {
        224 | 228 => 1,
        226 | 230 => 2,
        225 | 229 => 4,
        227 | 231 => 8,
        _ => 0,
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HotkeyConflict {
    pub first_id: String,
    pub second_id: String,
    pub binding: String,
}
/// SF both pins modules/modloader/check_hotkey_conflicts.rs:77-91. Caller supplies
/// the already filtered, prospective, id-sorted binding set; persistence policy is separate.
pub fn find_hotkey_conflicts(bindings: &[(String, Hotkey)]) -> Vec<HotkeyConflict> {
    let mut conflicts = vec![];
    for (i, (id, first)) in bindings.iter().enumerate() {
        for (other, second) in &bindings[i + 1..] {
            if first.overlaps(second) {
                conflicts.push(HotkeyConflict {
                    first_id: id.clone(),
                    second_id: other.clone(),
                    binding: first.normalized(),
                });
            }
        }
    }
    conflicts
}
/// `value` and `metadata` come from the document: options keep its order.
pub(crate) fn parse_setting_definition(
    key: &str,
    value: &Doc,
    metadata: &Doc,
) -> Result<SettingDefinition, String> {
    let value_type = match value {
        Doc::Bool(_) => SettingValueType::Boolean,
        Doc::String(_) => SettingValueType::String,
        Doc::Number(number) if number.as_i64().is_some() => SettingValueType::Integer,
        Doc::Number(_) => SettingValueType::Number,
        Doc::Array(_) => SettingValueType::Array,
        _ => {
            return Err(format!(
                "setting '{key}' value must be boolean, string, number, or array"
            ));
        }
    };
    let options = metadata
        .get("options")
        .and_then(Doc::as_object)
        .map(|entries| {
            entries
                .iter()
                .map(|(option, label)| {
                    let option_value = match value_type {
                        SettingValueType::Boolean => Value::Bool(option == "true"),
                        SettingValueType::Integer => Value::Number(
                            option
                                .parse::<i64>()
                                .map_err(|_| format!("invalid option for '{key}'"))?
                                .into(),
                        ),
                        SettingValueType::Number => json!(
                            option
                                .parse::<f64>()
                                .map_err(|_| format!("invalid option for '{key}'"))?
                        ),
                        SettingValueType::Array => {
                            match value.as_array().and_then(|items| items.first()) {
                                Some(Doc::Bool(_)) => Value::Bool(option == "true"),
                                Some(Doc::Number(number)) if number.as_i64().is_some() => {
                                    Value::Number(
                                        option
                                            .parse::<i64>()
                                            .map_err(|_| format!("invalid option for '{key}'"))?
                                            .into(),
                                    )
                                }
                                Some(Doc::Number(_)) => json!(
                                    option
                                        .parse::<f64>()
                                        .map_err(|_| format!("invalid option for '{key}'"))?
                                ),
                                _ => Value::String(option.clone()),
                            }
                        }
                        _ => Value::String(option.clone()),
                    };
                    // A label is a string, or { "label": ..., "group": ... }
                    // to sort the choice into a category.
                    let (label, group) = match label {
                        Doc::String(text) => (text.clone(), None),
                        Doc::Object(entry) => (
                            entry
                                .get("label")
                                .and_then(Doc::as_str)
                                .ok_or_else(|| {
                                    format!("option label for '{key}' must be a string")
                                })?
                                .to_owned(),
                            match entry.get("group") {
                                None => None,
                                Some(Doc::String(group)) => Some(group.clone()),
                                Some(_) => {
                                    return Err(format!(
                                        "option group for '{key}' must be a string"
                                    ));
                                }
                            },
                        ),
                        _ => return Err(format!("option label for '{key}' must be a string")),
                    };
                    Ok(SettingOption {
                        value: option_value,
                        label,
                        group,
                    })
                })
                .collect::<Result<Vec<_>, String>>()
        })
        .transpose()?
        .unwrap_or_default();
    let mut display_key = String::with_capacity(key.len());
    for (index, character) in key.chars().enumerate() {
        if index > 0 && character.is_uppercase() {
            display_key.push(' ');
        }
        display_key.push(character);
    }
    let mut control = metadata
        .get("control")
        .map(json::from_value)
        .transpose()
        .map_err(|error| format!("setting '{key}' control: {error}"))?;
    if value_type == SettingValueType::Array && options.is_empty() {
        return Err(format!("array setting '{key}' must declare options"));
    }
    if control.is_none() && !options.is_empty() {
        control = Some(if value_type == SettingValueType::Array {
            SettingControl::Multiselect
        } else {
            SettingControl::Select
        });
    }
    let apply_at_explicit =
        metadata.get("applyAt").is_some() || metadata.get("restartRequired").is_some();
    let apply_at = match metadata.get("applyAt") {
        None if metadata.get("restartRequired") == Some(&Doc::Bool(false)) => "live",
        None => "restart",
        Some(Doc::String(value)) => value.as_str(),
        Some(_) => return Err(format!("setting '{key}' applyAt must be a string")),
    };
    if !matches!(apply_at, "live" | "restart") {
        return Err(format!(
            "setting '{key}' applyAt must be 'live' or 'restart'"
        ));
    }
    let restart_required = match metadata.get("restartRequired") {
        None => apply_at == "restart",
        Some(Doc::Bool(value)) => *value,
        Some(_) => return Err(format!("setting '{key}' restartRequired must be a boolean")),
    };
    if restart_required != (apply_at == "restart") {
        return Err(format!(
            "setting '{key}' applyAt and restartRequired must agree"
        ));
    }
    let asset_preparation = match metadata.get("assetPreparation") {
        None => false,
        Some(Doc::Bool(value)) => *value,
        Some(_) => {
            return Err(format!(
                "setting '{key}' assetPreparation must be a boolean"
            ));
        }
    };
    let enabled_when = metadata
        .get("enabledWhen")
        .map(json::from_value::<SettingCondition>)
        .transpose()
        .map_err(|error| format!("setting '{key}' enabledWhen: {error}"))?;
    Ok(SettingDefinition {
        key: key.into(),
        value_type,
        control,
        label: metadata
            .get("label")
            .and_then(Doc::as_str)
            .unwrap_or(&display_key)
            .into(),
        description: metadata
            .get("description")
            .and_then(Doc::as_str)
            .map(str::to_owned),
        default: value.to_serde(),
        group: None,
        minimum: metadata.get("min").and_then(Doc::as_f64),
        maximum: metadata.get("max").and_then(Doc::as_f64),
        step: metadata.get("step").and_then(Doc::as_f64),
        minimum_length: metadata
            .get("minLength")
            .and_then(Doc::as_u64)
            .map(|value| value as usize),
        maximum_length: metadata
            .get("maxLength")
            .and_then(Doc::as_u64)
            .map(|value| value as usize),
        options,
        enabled_when,
        asset_preparation,
        restart_required,
        apply_at: apply_at.into(),
        apply_at_explicit,
        reserve: None,
        reserved: None,
        wheel: None,
    })
}

pub use super::manifest::{SettingControl, SettingDefinition, SettingValueType};

/// SF validate_mod_settings.rs:3: the first refusal in `values`' own key order wins, so pass
/// an ordered document value (`json::Value::from` keeps a serde_json map's iteration order).
pub fn validate_settings(definitions: &[SettingDefinition], values: &Doc) -> Result<(), String> {
    let object = values.as_object().ok_or("settings must be an object")?;
    for (key, value) in object {
        let definition = definitions
            .iter()
            .find(|d| d.key == *key)
            .ok_or_else(|| format!("unknown setting: {key}"))?;
        // validate_mod_settings.rs:10-12 prefixes every parse_value error with the key,
        // the non-string refusal included (hotkeys/parse_hotkey.rs:174-179, both pins).
        if matches!(definition.control, Some(SettingControl::Keybind)) {
            value
                .as_str()
                .ok_or_else(|| "Hotkey must be an SDL binding string".to_owned())
                .and_then(Hotkey::parse)
                .map_err(|error| format!("{key}: {error}"))?;
        }
        let mut schema = json!({"type": definition.value_type});
        if let Some(v) = definition.minimum {
            schema["minimum"] = json!(v);
        }
        if let Some(v) = definition.maximum {
            schema["maximum"] = json!(v);
        }
        if let Some(v) = definition.minimum_length {
            schema["minLength"] = json!(v);
        }
        if let Some(v) = definition.maximum_length {
            schema["maxLength"] = json!(v);
        }
        if !definition.options.is_empty() {
            let options: Vec<_> = definition.options.iter().map(|v| v.value.clone()).collect();
            if value.is_array() {
                schema["items"] = json!({"enum": options});
            } else {
                schema["enum"] = json!(options);
            }
        }
        super::validate::validate_value(&schema, value).map_err(|e| format!("{key}: {e}"))?;
    }
    Ok(())
}

pub fn infer_api_contract(manifest: &mut Manifest, source: &str) {
    let normalized: String = strip_lua_comments(source)
        .chars()
        .filter(|character| !character.is_whitespace())
        .collect();
    let assets = normalized.contains("runtime.require(\"game.assets.write\")")
        || normalized.contains("runtime.require('game.assets.write')")
        || [
            "game.assets.update_asset(",
            "game.assets.save_assets(",
            "game.assets.reset_assets(",
            "game.assets.create_resource(",
            "game.assets.create_content(",
        ]
        .iter()
        .any(|method| normalized.contains(method));
    let runtime = normalized.contains("runtime.require(\"runtime.lifecycle\")")
        || normalized.contains("runtime.require('runtime.lifecycle')")
        || normalized.contains("runtime.ecs.")
        || normalized.contains("runtime.world.")
        || normalized.contains("runtime.patch.")
        || normalized.contains("runtime.report_effect")
        || normalized.contains("on_load=")
        || normalized.contains("on_update=")
        || normalized.contains("on_unload=");
    // Capabilities are author-declared contract entries. Source inspection may
    // derive execution metadata, but it must never silently grant permissions.
    let apply_at = if runtime && !assets {
        "live"
    } else {
        "restart"
    };
    for setting in &mut manifest.settings {
        if setting.apply_at_explicit {
            continue;
        }
        setting.apply_at = apply_at.into();
        setting.restart_required = apply_at != "live";
    }
}

fn strip_lua_comments(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut chars = source.chars().peekable();
    let mut quote = None;
    let mut escaped = false;
    let mut line_comment = false;
    let mut block_comment = false;
    while let Some(character) = chars.next() {
        if line_comment {
            if character == '\n' {
                line_comment = false;
                output.push(character);
            }
            continue;
        }
        if block_comment {
            if character == ']' && chars.peek() == Some(&']') {
                chars.next();
                block_comment = false;
            }
            continue;
        }
        if let Some(delimiter) = quote {
            output.push(character);
            if escaped {
                escaped = false;
                continue;
            }
            if character == '\\' {
                escaped = true;
                continue;
            }
            if character == delimiter {
                quote = None;
            }
            continue;
        }
        if character == '\'' || character == '"' {
            quote = Some(character);
            output.push(character);
            continue;
        }
        if character == '-' && chars.peek() == Some(&'-') {
            chars.next();
            if chars.peek() == Some(&'[') {
                chars.next();
                if chars.peek() == Some(&'[') {
                    chars.next();
                    block_comment = true;
                    continue;
                }
                output.push_str("--[");
                continue;
            }
            line_comment = true;
            continue;
        }
        output.push(character);
    }
    output
}

// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// modules/modloader/check_hotkey_conflicts.rs:8-22,55-293 (both pins).
// Source selection/IO is host-owned; this function receives validated package snapshots.
use serde::{Deserialize, Serialize};
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ProtectedHotkey {
    pub scancode: u16,
    pub label: String,
    pub emoji: String,
    pub text: String,
}

#[derive(Clone, Debug, Default, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ProtectedHotkeyGroups {
    pub confirm: Vec<ProtectedHotkey>,
    pub cancel: Vec<ProtectedHotkey>,
}

struct ConfiguredBinding {
    id: String,
    binding: Hotkey,
    changed: bool,
}

pub fn protected_hotkeys(config: &Value) -> ProtectedHotkeyGroups {
    let read_group = |name: &str| {
        config
            .pointer(&format!("/hotkeys/{name}"))
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(|value| serde_json::from_value(value.clone()).ok())
            .collect()
    };
    ProtectedHotkeyGroups {
        confirm: read_group("confirm"),
        cancel: read_group("cancel"),
    }
}

fn reject_protected_binding(
    binding: &Hotkey,
    protected: &ProtectedHotkeyGroups,
) -> Result<(), String> {
    for scancode in binding.keyboard_scancodes() {
        if let Some(key) = protected.cancel.iter().find(|key| key.scancode == scancode) {
            return Err(format!(
                "{} is protected by ShroudForge's hotkey configuration",
                key.label
            ));
        }
    }
    Ok(())
}

/// Check the proposed loader configuration and optional partial mod settings.
/// Definitions and values remain in their existing configuration documents.
pub fn validate_hotkey_configuration(
    manifests: &[Manifest],
    previous_config: &Value,
    config: &Value,
    changed_mod: Option<(&str, &Value, Option<bool>)>,
) -> Result<(), String> {
    let mut bindings = Vec::new();
    let protected = protected_hotkeys(config);
    if let Some(modules) = config.get("modules").and_then(Value::as_object) {
        for (id, module) in modules {
            let Some(value) = module.get("toggleKey") else {
                continue;
            };
            let binding = parse_hotkey_value(value)?;
            let previous_module = previous_config
                .get("modules")
                .and_then(|modules| modules.get(id));
            let previous_binding = previous_module
                .and_then(|module| module.get("toggleKey"))
                .and_then(|value| parse_hotkey_value(value).ok().flatten());
            if previous_binding.as_ref() != binding.as_ref()
                && let Some(binding) = &binding
            {
                reject_protected_binding(binding, &protected)?;
            }
            let previously_enabled = previous_module
                .and_then(|module| module.get("enabled"))
                .and_then(Value::as_bool)
                != Some(false);
            let changed = !previously_enabled || previous_binding.as_ref() != binding.as_ref();
            if module.get("enabled").and_then(Value::as_bool) == Some(false) {
                continue;
            }
            if let Some(binding) = binding {
                bindings.push(ConfiguredBinding {
                    id: format!("modules.{id}.toggleKey"),
                    changed,
                    binding,
                });
            }
        }
    }

    for manifest in manifests {
        if let Some((_, overrides, _)) = changed_mod.filter(|(id, _, _)| manifest.id == *id) {
            let overrides = overrides
                .as_object()
                .ok_or("mod settings must be an object")?;
            for setting in &manifest.settings {
                if !matches!(setting.control, Some(SettingControl::Keybind)) {
                    continue;
                }
                let Some(value) = overrides.get(&setting.key) else {
                    continue;
                };
                let binding = parse_hotkey_value(value)?;
                let previous_binding = manifest
                    .setting_values
                    .get(&setting.key)
                    .and_then(|value| parse_hotkey_value(value).ok().flatten());
                if previous_binding.as_ref() != binding.as_ref()
                    && let Some(binding) = &binding
                {
                    reject_protected_binding(binding, &protected)?;
                }
            }
        }
        let enabled = changed_mod
            .filter(|(id, _, _)| manifest.id == *id)
            .and_then(|(_, _, enabled)| enabled)
            .unwrap_or(manifest.enabled);
        if !enabled || !manifest.target.supports_process(false) {
            continue;
        }
        if manifest.id == "world-editor"
            && config
                .pointer("/modules/worldEditor/enabled")
                .and_then(Value::as_bool)
                == Some(false)
        {
            continue;
        }
        let previously_active = manifest.enabled
            && (manifest.id != "world-editor"
                || previous_config
                    .pointer("/modules/worldEditor/enabled")
                    .and_then(Value::as_bool)
                    != Some(false));
        let mut values = manifest.setting_values.clone();
        if let Some((id, overrides, _)) = changed_mod
            && manifest.id == id
        {
            let overrides = overrides
                .as_object()
                .ok_or("mod settings must be an object")?;
            let settings = values
                .as_object_mut()
                .ok_or("mod settings must be an object")?;
            for (key, value) in overrides {
                settings.insert(key.clone(), value.clone());
            }
        }
        for setting in &manifest.settings {
            if !matches!(setting.control, Some(SettingControl::Keybind)) {
                continue;
            }
            if !setting_is_enabled(&setting.key, &manifest.settings, &values) {
                continue;
            }
            let value = values.get(&setting.key).ok_or_else(|| {
                format!("Missing hotkey setting '{}.{}'", manifest.id, setting.key)
            })?;
            if let Some(binding) = parse_hotkey_value(value)? {
                let previous_binding = if previously_active
                    && setting_is_enabled(
                        &setting.key,
                        &manifest.settings,
                        &manifest.setting_values,
                    ) {
                    manifest
                        .setting_values
                        .get(&setting.key)
                        .and_then(|value| parse_hotkey_value(value).ok().flatten())
                } else {
                    None
                };
                bindings.push(ConfiguredBinding {
                    id: format!("{}.{}", manifest.id, setting.key),
                    changed: previous_binding.as_ref() != Some(&binding),
                    binding,
                });
            }
        }
    }
    bindings.sort_by(|left, right| left.id.cmp(&right.id));
    for current in bindings.iter().filter(|binding| binding.changed) {
        reject_protected_binding(&current.binding, &protected)?;
    }
    let changed_ids: Vec<_> = bindings
        .iter()
        .filter(|binding| binding.changed)
        .map(|binding| binding.id.as_str())
        .collect();
    for conflict in find_hotkey_conflicts(
        &bindings
            .iter()
            .map(|binding| (binding.id.clone(), binding.binding.clone()))
            .collect::<Vec<_>>(),
    ) {
        if changed_ids.contains(&conflict.first_id.as_str())
            || changed_ids.contains(&conflict.second_id.as_str())
        {
            return Err(format!(
                "Hotkey {} conflicts between '{}' and '{}'. Choose another binding or clear one shortcut",
                conflict.binding, conflict.first_id, conflict.second_id,
            ));
        }
    }
    Ok(())
}

/// Follow the same enabledWhen chain as the settings editor. Missing
/// controllers and cycles disable a setting instead of admitting its action.
pub fn setting_is_enabled(key: &str, definitions: &[SettingDefinition], values: &Value) -> bool {
    let mut current = key;
    let mut seen = std::collections::BTreeSet::new();
    loop {
        let Some(definition) = definitions.iter().find(|setting| setting.key == current) else {
            return false;
        };
        let Some(condition) = &definition.enabled_when else {
            return true;
        };
        if !seen.insert(current) {
            return false;
        }
        let Some(value) = values.get(&condition.setting) else {
            return false;
        };
        // JavaScript has one numeric type. Preserve the editor's equality for
        // JSON integer and floating representations of the same number.
        let matches = if value.is_number() && condition.equals.is_number() {
            value.as_f64() == condition.equals.as_f64()
        } else {
            value == &condition.equals
        };
        if !matches {
            return false;
        }
        current = &condition.setting;
    }
}

fn parse_hotkey_value(value: &Value) -> Result<Option<Hotkey>, String> {
    let key = Hotkey::parse(
        value
            .as_str()
            .ok_or("Hotkey must be an SDL binding string")?,
    )?;
    Ok((!key.chords.is_empty()).then_some(key))
}
