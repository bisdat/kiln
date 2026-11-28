// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/modloader/validate_mod_manifest.rs:4-172
// Source: modules/modloader/validate_input_reservations.rs:7-57
// Source: modules/modloader/validate_setting_presets.rs:7-119
// Contract pins: 1.7.9 d8a394bc; 1.8.0 38ef448d (1.8 additions are version-gated).
use super::{SourceVersion, is_package_id as valid_identifier, manifest::*};

pub fn validate_manifest(manifest: &Manifest, version: SourceVersion) -> Result<(), String> {
    if !(valid_identifier(&manifest.id) && manifest.id != "." && manifest.id != "..") {
        return Err("mod id is invalid".into());
    }
    let configured_link_ids: Vec<String> =
        serde_json::from_str(r#"["source","source-github","source-gitlab","source-codeberg","issues","wiki","website","store","support-bmac","support-patreon","support-paypal","support-github","support-ko-fi","support-open-collective","support-other"]"#)
            .map_err(|error| format!("invalid configured mod link registry: {error}"))?;
    // The 1.8 line's registry (mod_link_order.json since 1.8.1, 0581d72e) adds a Discord and a YouTube link; ShroudForge's
    // own starter mod carries both, so 1.8 packages built from it do too. 1.7.9 refuses them.
    let link_18 = |name: &str| version != SourceVersion::Sf179 && matches!(name, "community-discord" | "video-youtube");
    for (name, url) in &manifest.links.0 {
        if !configured_link_ids.iter().any(|id| id == name) && !link_18(name) {
            return Err(format!("unsupported mod link '{name}'"));
        }
        if !url.starts_with("https://") {
            return Err(format!("mod link '{name}' must use HTTPS"));
        }
    }
    let mut setting_keys = std::collections::HashSet::new();
    for setting in &manifest.settings {
        if !valid_identifier(&setting.key) || !setting_keys.insert(setting.key.as_str()) {
            return Err(format!(
                "setting key '{}' is invalid or duplicated",
                setting.key
            ));
        }
        let valid_default = match setting.value_type {
            SettingValueType::Boolean => setting.default.is_boolean(),
            SettingValueType::String => setting.default.is_string(),
            SettingValueType::Integer => setting.default.as_i64().is_some(),
            SettingValueType::Number => setting.default.is_number(),
            SettingValueType::Array => setting.default.is_array(),
        };
        if !valid_default {
            return Err(format!(
                "setting '{}' has an invalid default value",
                setting.key
            ));
        }
        if let (Some(minimum), Some(maximum)) = (setting.minimum, setting.maximum)
            && minimum > maximum
        {
            return Err(format!(
                "setting '{}' has minimum greater than maximum",
                setting.key
            ));
        }
        if setting
            .step
            .is_some_and(|step| !step.is_finite() || step <= 0.0)
        {
            return Err(format!("setting '{}' has an invalid step", setting.key));
        }
        if setting.value_type == SettingValueType::Array && setting.options.is_empty() {
            return Err(format!("array setting '{}' requires options", setting.key));
        }
        if setting.control.is_some_and(|control| {
            matches!(
                control,
                SettingControl::Select
                    | SettingControl::Radio
                    | SettingControl::Segmented
                    | SettingControl::Multiselect
            )
        }) && setting.options.is_empty()
        {
            return Err(format!(
                "setting '{}' uses a choice control without options",
                setting.key
            ));
        }
    }
    for setting in &manifest.settings {
        let Some(condition) = &setting.enabled_when else {
            continue;
        };
        if condition.setting == setting.key {
            return Err(format!("setting '{}' cannot depend on itself", setting.key));
        }
        let controller = manifest
            .settings
            .iter()
            .find(|candidate| candidate.key == condition.setting)
            .ok_or_else(|| {
                format!(
                    "setting '{}' depends on unknown setting '{}'",
                    setting.key, condition.setting
                )
            })?;
        if !condition_matches_type(&condition.equals, controller.value_type) {
            return Err(format!(
                "setting '{}' enabledWhen value does not match setting '{}'",
                setting.key, condition.setting
            ));
        }
        if !controller.options.is_empty()
            && !controller
                .options
                .iter()
                .any(|option| option.value == condition.equals)
        {
            return Err(format!(
                "setting '{}' enabledWhen value is not an option of setting '{}'",
                setting.key, condition.setting
            ));
        }
    }
    for setting in &manifest.settings {
        let mut seen = std::collections::HashSet::new();
        let mut current = Some(setting.key.as_str());
        while let Some(key) = current {
            if !seen.insert(key) {
                return Err(format!(
                    "setting enabledWhen dependencies contain a cycle at '{key}'"
                ));
            }
            current = manifest
                .settings
                .iter()
                .find(|candidate| candidate.key == key)
                .and_then(|candidate| candidate.enabled_when.as_ref())
                .map(|condition| condition.setting.as_str());
        }
    }
    validate_input_reservations(manifest)?;
    let mut placed_settings = std::collections::HashSet::new();
    for group in &manifest.groups {
        if group.label.trim().is_empty() {
            return Err("setting group label must not be empty".into());
        }
        for key in &group.settings {
            if !setting_keys.contains(key.as_str()) {
                return Err(format!(
                    "group '{}' references unknown setting '{key}'",
                    group.label
                ));
            }
            if !placed_settings.insert(key.as_str()) {
                return Err(format!("setting '{key}' appears in more than one group"));
            }
        }
        for action in &group.actions {
            if !valid_identifier(&action.id) || action.label.trim().is_empty() {
                return Err(format!(
                    "group '{}' contains an invalid action",
                    group.label
                ));
            }
            if action
                .style
                .as_deref()
                .is_some_and(|style| !matches!(style, "primary" | "secondary" | "danger"))
            {
                return Err(format!("action '{}' has an invalid style", action.id));
            }
        }
    }
    validate_setting_presets(manifest)
}

fn condition_matches_type(value: &serde_json::Value, value_type: SettingValueType) -> bool {
    match value_type {
        SettingValueType::Boolean => value.is_boolean(),
        SettingValueType::String => value.is_string(),
        SettingValueType::Integer => value.as_i64().is_some(),
        SettingValueType::Number => value.as_f64().is_some(),
        SettingValueType::Array => false,
    }
}

/// Checks the input contexts of a mod and the settings that name them: a
/// keybind may be reserved, a boolean may read the mouse wheel, each while a
/// declared context is active.
pub(crate) fn validate_input_reservations(manifest: &Manifest) -> Result<(), String> {
    for (context, label) in &manifest.input_contexts {
        if !valid_identifier(context) {
            return Err(format!(
                "input context '{context}' is not a valid identifier"
            ));
        }
        if label.trim().is_empty() || label.chars().count() > 120 {
            return Err(format!(
                "input context '{context}' needs a label of 1 to 120 characters"
            ));
        }
    }
    let known = |when: &str| manifest.input_contexts.contains_key(when);
    for setting in &manifest.settings {
        if let Some(reserve) = &setting.reserve {
            if !matches!(setting.control, Some(SettingControl::Keybind)) {
                return Err(format!(
                    "setting '{}' reserve is only valid on a keybind",
                    setting.key
                ));
            }
            if !known(&reserve.when) {
                return Err(format!(
                    "setting '{}' reserve names unknown input context '{}'",
                    setting.key, reserve.when
                ));
            }
        } else if setting.reserved.is_some() {
            return Err(format!(
                "setting '{}' has reserved devices but no reserve",
                setting.key
            ));
        }
        if let Some(wheel) = &setting.wheel {
            if setting.value_type != SettingValueType::Boolean || setting.reserve.is_some() {
                return Err(format!(
                    "setting '{}' wheel is only valid on a boolean setting",
                    setting.key
                ));
            }
            if !known(&wheel.when) {
                return Err(format!(
                    "setting '{}' wheel names unknown input context '{}'",
                    setting.key, wheel.when
                ));
            }
        }
    }
    Ok(())
}

use std::collections::HashMap;

/// Check that each preset controller and member is a declared setting, that a
/// setting belongs to one preset at most, and that every preset gives a valid
/// value to each of its members.
pub(crate) fn validate_setting_presets(manifest: &Manifest) -> Result<(), String> {
    let mut owners: HashMap<&str, &str> = HashMap::new();
    for (controller_key, preset) in &manifest.presets {
        let controller = manifest
            .settings
            .iter()
            .find(|setting| setting.key == *controller_key)
            .ok_or_else(|| format!("preset '{controller_key}' needs a setting with that key"))?;
        if controller.value_type != SettingValueType::String {
            return Err(format!(
                "preset '{controller_key}' needs a string setting as its controller"
            ));
        }
        let controller_options: Vec<&str> = controller
            .options
            .iter()
            .filter_map(|option| option.value.as_str())
            .collect();
        if controller_options.is_empty()
            || !matches!(
                controller.control,
                Some(SettingControl::Select | SettingControl::Radio | SettingControl::Segmented)
            )
        {
            return Err(format!(
                "preset '{controller_key}' needs a string choice setting with options"
            ));
        }
        if !controller_options.contains(&preset.custom.as_str()) {
            return Err(format!(
                "preset '{controller_key}' custom value '{}' is not an option of its setting",
                preset.custom
            ));
        }
        if preset.settings.is_empty() {
            return Err(format!("preset '{controller_key}' has no settings"));
        }
        for key in &preset.settings {
            if key == controller_key {
                return Err(format!(
                    "preset '{controller_key}' cannot list its own setting"
                ));
            }
            let member = manifest
                .settings
                .iter()
                .find(|setting| setting.key == *key)
                .ok_or_else(|| {
                    format!("preset '{controller_key}' references unknown setting '{key}'")
                })?;
            if matches!(member.control, Some(SettingControl::Keybind)) {
                return Err(format!(
                    "preset '{controller_key}' cannot set keybind setting '{key}'"
                ));
            }
            if manifest.presets.contains_key(key) {
                return Err(format!(
                    "preset '{controller_key}' cannot set '{key}', which controls another preset"
                ));
            }
            if let Some(owner) = owners.insert(key.as_str(), controller_key.as_str()) {
                return Err(if owner == controller_key {
                    format!("preset '{controller_key}' lists setting '{key}' twice")
                } else {
                    format!("setting '{key}' belongs to presets '{owner}' and '{controller_key}'")
                });
            }
        }
        if preset.values.is_empty() {
            return Err(format!("preset '{controller_key}' has no values"));
        }
        for option in &controller_options {
            if *option != preset.custom && !preset.values.contains_key(*option) {
                return Err(format!(
                    "preset '{controller_key}' has no values for option '{option}'"
                ));
            }
        }
        for (name, values) in &preset.values {
            if *name == preset.custom {
                return Err(format!(
                    "preset '{controller_key}' cannot give values to its custom value '{name}'"
                ));
            }
            if !controller_options.contains(&name.as_str()) {
                return Err(format!(
                    "preset '{controller_key}' value '{name}' is not an option of its setting"
                ));
            }
            for key in &preset.settings {
                if !values.contains_key(key) {
                    return Err(format!(
                        "preset '{controller_key}' value '{name}' does not set '{key}'"
                    ));
                }
            }
            if let Some(extra) = values.keys().find(|key| !preset.settings.contains(key)) {
                return Err(format!(
                    "preset '{controller_key}' value '{name}' sets '{extra}', which is not in its settings"
                ));
            }
            super::settings::validate_settings(
                &manifest.settings,
                &crate::json::Value::Object(
                    values
                        .iter()
                        .map(|(key, value)| (key.clone(), value.into()))
                        .collect(),
                ),
            )
            .map_err(|error| format!("preset '{controller_key}' value '{name}': {error}"))?;
        }
    }
    Ok(())
}
