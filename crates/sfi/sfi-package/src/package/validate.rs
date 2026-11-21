// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/modloader/manifest/{manifest,extended.mod,mod-locale}.schema.json:1
// Both pins: 1.7.9 d8a394bc and 1.8.0 38ef448d; independent fixed-schema evaluator below.
// compiled_pattern adapted from jsonschema 0.33.0 (MIT, Copyright (c) 2020-2025 Dmitry Dygalo)
// Source: src/ecma.rs:109-132 (the Perl-class replacement sets), keywords/pattern.rs:66-89.
//! Fixed embedded-schema validation, not a public general-purpose JSON Schema engine.
use super::SourceVersion;
use crate::json::Value as Doc;
use serde_json::Value;
use std::{
    collections::HashMap,
    sync::{Mutex, OnceLock},
};

pub(crate) enum Schema {
    Manifest,
    Extension,
    Locale,
}
impl Schema {
    // modules/modloader/documents.rs:2-14 in both SF pins.
    pub(crate) fn name(&self) -> &'static str {
        match self {
            Self::Manifest => "mod",
            Self::Extension => "extended-mod",
            Self::Locale => "mod-locale",
        }
    }
}

pub(crate) fn document_schema(version: SourceVersion, kind: Schema) -> &'static Value {
    static SCHEMAS: OnceLock<[Value; 6]> = OnceLock::new();
    let schemas = SCHEMAS.get_or_init(|| {
        [
            MANIFEST_179,
            EXTENSION_179,
            LOCALE_179,
            MANIFEST_180,
            EXTENSION_180,
            LOCALE_180,
        ]
        .map(|s| serde_json::from_str(s).expect("embedded reviewed SF schema"))
    });
    let offset = if version == SourceVersion::Sf179 {
        0
    } else {
        3
    };
    &schemas[offset
        + match kind {
            Schema::Manifest => 0,
            Schema::Extension => 1,
            Schema::Locale => 2,
        }]
}

/// JSON Schema numeric equality deliberately differs from serde's representation-sensitive
/// condition/options comparison (SF validate_mod_settings.rs:27-37 versus validate_mod_manifest.rs:99-103).
pub(crate) fn schema_equal(a: &Doc, b: &Doc) -> bool {
    match (a, b) {
        (Doc::Number(a), Doc::Number(b)) => number_cmp(a, b) == std::cmp::Ordering::Equal,
        (Doc::Array(a), Doc::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(a, b)| schema_equal(a, b))
        }
        (Doc::Object(a), Doc::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(k, a)| b.get(k).is_some_and(|b| schema_equal(a, b)))
        }
        _ => a == b,
    }
}
// Independent exact comparison of serde's finite doubles and i64/u64 integers.
// Do not round an integer to f64 before an enum/range comparison.
pub(crate) fn number_cmp(a: &serde_json::Number, b: &serde_json::Number) -> std::cmp::Ordering {
    use std::cmp::Ordering;
    let integer = |n: &serde_json::Number| {
        n.as_i64()
            .map(i128::from)
            .or_else(|| n.as_u64().map(i128::from))
    };
    fn int_float(i: i128, f: f64) -> Ordering {
        // The integer side fits i64/u64, well inside this wider cast's bounds.
        let order = i.cmp(&(f as i128));
        if order != Ordering::Equal {
            return order;
        }
        if f.fract() > 0.0 {
            Ordering::Less
        } else if f.fract() < 0.0 {
            Ordering::Greater
        } else {
            Ordering::Equal
        }
    }
    match (integer(a), integer(b)) {
        (Some(a), Some(b)) => a.cmp(&b),
        (Some(a), None) => int_float(a, b.as_f64().expect("finite JSON number")),
        (None, Some(b)) => int_float(b, a.as_f64().expect("finite JSON number")).reverse(),
        (None, None) => a
            .as_f64()
            .partial_cmp(&b.as_f64())
            .expect("finite JSON numbers"),
    }
}
fn type_matches(ty: &str, value: &Doc) -> bool {
    match ty {
        "object" => value.is_object(),
        "array" => value.is_array(),
        "string" => value.is_string(),
        "boolean" => value.is_boolean(),
        "null" => value.is_null(),
        "number" => value.is_number(),
        "integer" => {
            value.is_i64() || value.is_u64() || value.as_f64().is_some_and(|n| n.fract() == 0.0)
        }
        _ => false,
    }
}
/// SF's jsonschema 0.33.0 compiles each ECMA-262 `pattern` to a Rust regex before use,
/// rewriting the Perl classes \d, \w and \s (also negated, also inside a set) as explicit
/// ECMA sets, and its "does not match" error prints that compiled text (src/ecma.rs:11-72,
/// 108-133; keywords/pattern.rs:28-32,66-89; error.rs:991-993). Matching and messages use
/// the compiled text, so \d is ASCII [0-9] here, never Unicode Nd. The embedded schemas
/// use no \c escape, look-around or backreference (schema_guard below), the other cases
/// jsonschema treats specially.
fn compiled_pattern(pattern: &str) -> std::borrow::Cow<'_, str> {
    if !pattern.contains('\\') {
        return pattern.into();
    }
    let mut out = String::with_capacity(pattern.len());
    let mut chars = pattern.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        let Some(next) = chars.next() else {
            out.push(c);
            break;
        };
        match next {
            'd' => out.push_str("[0-9]"),
            'D' => out.push_str("[^0-9]"),
            'w' => out.push_str("[A-Za-z0-9_]"),
            'W' => out.push_str("[^A-Za-z0-9_]"),
            's' => out.push_str("[ \t\n\r\u{000b}\u{000c}\u{00a0}\u{feff}\u{2003}\u{2029}]"),
            'S' => out.push_str("[^ \t\n\r\u{000b}\u{000c}\u{00a0}\u{feff}\u{2003}\u{2029}]"),
            other => {
                out.push(c);
                out.push(other);
            }
        }
    }
    out.into()
}
fn pattern_matches(pattern: &str, value: &str) -> Result<bool, String> {
    static PATTERNS: OnceLock<Mutex<HashMap<String, regex::Regex>>> = OnceLock::new();
    let mut patterns = PATTERNS
        .get_or_init(Mutex::default)
        .lock()
        .map_err(|e| e.to_string())?;
    if !patterns.contains_key(pattern) {
        patterns.insert(
            pattern.to_owned(),
            regex::Regex::new(pattern).map_err(|e| e.to_string())?,
        );
    }
    Ok(patterns[pattern].is_match(value))
}
/// `value` is an ordered document value: closed records and messages follow its key order.
pub(crate) fn validate_value(schema: &Value, value: &Doc) -> Result<(), String> {
    check(schema, schema, value)
}
// SF's validator returns jsonschema 0.33.0 Display text, without a JSON path
// (loader-config/validate_document.rs:77-79; validate_mod_settings.rs:34-37, both pins).
// Keep this fixed-schema evaluator independent; match that public diagnostic shape.
fn check(root: &Value, schema: &Value, value: &Doc) -> Result<(), String> {
    if schema == &Value::Bool(false) {
        return Err(format!("False schema does not allow {value}"));
    }
    if let Some(reference) = schema.get("$ref").and_then(Value::as_str) {
        let target = reference
            .strip_prefix('#')
            .and_then(|p| root.pointer(p))
            .ok_or("invalid embedded schema reference")?;
        check(root, target, value)?;
    }
    if let Some(ty) = schema.get("type") {
        let valid = match ty {
            Value::String(ty) => type_matches(ty, value),
            Value::Array(types) => types
                .iter()
                .any(|t| t.as_str().is_some_and(|t| type_matches(t, value))),
            _ => false,
        };
        if !valid {
            return Err(match ty {
                Value::Array(types) => {
                    let names = [
                        "array", "boolean", "integer", "null", "number", "object", "string",
                    ]
                    .into_iter()
                    .filter(|ty| types.iter().any(|v| v.as_str() == Some(ty)))
                    .map(|ty| format!("\"{ty}\""))
                    .collect::<Vec<_>>()
                    .join(", ");
                    format!("{value} is not of types {names}")
                }
                _ => format!("{value} is not of type {ty}"),
            });
        }
    }
    if let Some(expected) = schema.get("const")
        && !schema_equal(&Doc::from(expected), value)
    {
        return Err(format!("{expected} was expected"));
    }
    if let Some(options) = schema.get("enum").and_then(Value::as_array)
        && !options.iter().any(|c| schema_equal(&Doc::from(c), value))
    {
        let mut candidates = String::new();
        let count = if options.len() > 3 { 2 } else { options.len() };
        for (i, option) in options.iter().take(count).enumerate() {
            if i > 0 {
                candidates.push_str(if options.len() <= 3 && i == count - 1 {
                    " or "
                } else {
                    ", "
                });
            }
            candidates.push_str(&option.to_string());
        }
        if options.len() > 3 {
            candidates.push_str(&format!(" or {} other candidates", options.len() - count));
        }
        return Err(format!("{value} is not one of {candidates}"));
    }
    if let Some(choices) = schema.get("oneOf").and_then(Value::as_array) {
        let count = choices
            .iter()
            .filter(|s| check(root, s, value).is_ok())
            .count();
        if count != 1 {
            let validity = if count == 0 {
                "not valid under any"
            } else {
                "valid under more than one"
            };
            return Err(format!(
                "{value} is {validity} of the schemas listed in the 'oneOf' keyword"
            ));
        }
    }
    if let Some(all) = schema.get("allOf").and_then(Value::as_array) {
        for s in all {
            check(root, s, value)?;
        }
    }
    if let Some(condition) = schema.get("if") {
        let branch = if check(root, condition, value).is_ok() {
            "then"
        } else {
            "else"
        };
        if let Some(s) = schema.get(branch) {
            check(root, s, value)?;
        }
    }
    if let Some(text) = value.as_str() {
        let len = text.chars().count() as u64;
        for (key, comparison, adjective) in [
            ("minLength", std::cmp::Ordering::Less, "shorter"),
            ("maxLength", std::cmp::Ordering::Greater, "longer"),
        ] {
            if let Some(limit) = schema.get(key).and_then(Value::as_u64)
                && len.cmp(&limit) == comparison
            {
                return Err(format!(
                    "{value} is {adjective} than {limit} character{}",
                    if limit == 1 { "" } else { "s" }
                ));
            }
        }
        if let Some(pattern) = schema.get("pattern").and_then(Value::as_str) {
            let pattern = compiled_pattern(pattern);
            if !pattern_matches(&pattern, text)? {
                return Err(format!("{value} does not match \"{pattern}\""));
            }
        }
    }
    if let Doc::Number(n) = value {
        use std::cmp::Ordering::{Equal, Greater, Less};
        for (key, comparison, message) in [
            ("minimum", Less, "less than the minimum"),
            ("maximum", Greater, "greater than the maximum"),
            (
                "exclusiveMinimum",
                Less,
                "less than or equal to the minimum",
            ),
        ] {
            if let Some(Value::Number(limit)) = schema.get(key) {
                let order = number_cmp(n, limit);
                if order == comparison || (key == "exclusiveMinimum" && order == Equal) {
                    return Err(format!("{value} is {message} of {limit}"));
                }
            }
        }
    }
    if let Some(items) = value.as_array() {
        for (key, comparison, adjective) in [
            ("minItems", std::cmp::Ordering::Less, "less"),
            ("maxItems", std::cmp::Ordering::Greater, "more"),
        ] {
            if let Some(limit) = schema.get(key).and_then(Value::as_u64)
                && (items.len() as u64).cmp(&limit) == comparison
            {
                return Err(format!(
                    "{value} has {adjective} than {limit} item{}",
                    if limit == 1 { "" } else { "s" }
                ));
            }
        }
        if schema.get("uniqueItems") == Some(&Value::Bool(true)) {
            for (i, a) in items.iter().enumerate() {
                if items[i + 1..].iter().any(|b| schema_equal(a, b)) {
                    return Err(format!("{value} has non-unique elements"));
                }
            }
        }
        if let Some(s) = schema.get("items") {
            for v in items {
                check(root, s, v)?;
            }
        }
    }
    if let Some(object) = value.as_object() {
        if let Some(limit) = schema.get("minProperties").and_then(Value::as_u64)
            && (object.len() as u64) < limit
        {
            return Err(format!(
                "{value} has less than {limit} propert{}",
                if limit == 1 { "y" } else { "ies" }
            ));
        }
        let closed = schema.get("additionalProperties") == Some(&Value::Bool(false));
        if closed {
            // Both SF pins use jsonschema 0.33.0 Validator::validate, whose
            // nonempty closed records check known values and unknown keys together
            // in instance order (keywords/additional_properties.rs:282-302).
            for (key, v) in object {
                let Some(s) = schema.get("properties").and_then(|p| p.get(key)) else {
                    return Err(format!(
                        "Additional properties are not allowed ('{key}' was unexpected)"
                    ));
                };
                check(root, s, v)?;
            }
        }
        if let Some(required) = schema.get("required").and_then(Value::as_array) {
            for key in required {
                if !object.contains_key(key.as_str().ok_or("invalid required field")?) {
                    return Err(format!("{key} is a required property"));
                }
            }
        }
        for (key, v) in object {
            if let Some(s) = schema.get("propertyNames") {
                check(root, s, &Doc::String(key.clone()))?;
            }
            if !closed
                && let Some(s) = schema
                    .get("properties")
                    .and_then(|p| p.get(key))
                    .or_else(|| schema.get("additionalProperties"))
            {
                check(root, s, v)?;
            }
        }
    }
    Ok(())
}
// Complete frozen schemas; kept local so a runtime never reads the contract checkouts.
const MANIFEST_179: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://bonsaibauer.github.io/shroudforge/schemas/manifest.schema.json","title":"EML mod manifest","type":"object","additionalProperties":false,"required":["id","name","version"],"properties":{"id":{"type":"string","pattern":"^[A-Za-z0-9._-]+$","maxLength":80},"name":{"type":"string","minLength":1,"maxLength":120},"version":{"type":"string","pattern":"^\\d+\\.\\d+\\.\\d+(?:[-+][0-9A-Za-z.-]+)?$"},"description":{"type":"string","maxLength":2000},"authors":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"license":{"description":"Optional license identifier (for example, MIT). Use null when no license is declared.","type":["string","null"]},"icon":{"description":"Optional mod icon filename in the package root or path under assets/. Use null when the mod has no icon.","type":["string","null"],"pattern":"^(?:assets/[A-Za-z0-9._/-]+|[A-Za-z0-9_-][A-Za-z0-9._-]*)$"},"dependencies":{"description":"Mods that must be available before this mod starts. Each id is another mod's manifest id. Version is a SemVer version requirement. If optional is true, the dependency does not block startup when missing, disabled, or outside the version range.","type":"array","items":{"type":"object","additionalProperties":false,"description":"One mod dependency.","required":["id","version"],"properties":{"id":{"description":"Exact id from the dependency mod's mod.json.","type":"string","pattern":"^[A-Za-z0-9._-]+$","maxLength":80},"version":{"description":"SemVer version requirement, for example ^1.2.0 or >=1.2.0, <2.0.0.","type":"string"},"optional":{"description":"When true, this mod may start if the dependency is unavailable. Defaults to false.","type":"boolean","default":false}}}},"$schema":{"type":"string"},"capabilities":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["patch","export","runtime","runtime-register-dll"]}}}}"##;
const EXTENSION_179: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://bonsaibauer.github.io/shroudforge/schemas/extended.mod.schema.json","title":"ShroudForge mod extension","type":"object","additionalProperties":false,"required":["schemaVersion","enabled"],"properties":{"$schema":{"type":"string"},"schemaVersion":{"const":1},"enabled":{"type":"boolean"},"targets":{"description":"Processes this mod runs in. Legacy EML packages without targets default to client and server. ShroudForge packages default to client.","type":"array","minItems":1,"uniqueItems":true,"items":{"enum":["client","server"]}},"launcher":{"description":"Launcher that originally identified this mod. Omit for ShroudForge mods. Use EML to preserve EML provenance.","enum":["EML","SF"]},"links":{"type":"object","propertyNames":{"pattern":"^[a-z][a-z0-9-]*$"},"additionalProperties":{"type":"string","pattern":"^https://","maxLength":2048}},"changelog":{"type":"array","maxItems":50,"items":{"type":"string","minLength":1,"maxLength":500}},"settings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"oneOf":[{"type":["boolean","string","number","array"]},{"$ref":"#/$defs/setting"}]}},"groups":{"type":"array","items":{"$ref":"#/$defs/group"}}},"$defs":{"setting":{"type":"object","additionalProperties":false,"required":["value"],"properties":{"value":{"type":["boolean","string","number","array"]},"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"control":{"enum":["toggle","checkbox","text","textarea","number","slider","select","radio","segmented","multiselect","keybind","color"]},"applyAt":{"enum":["live","restart"]},"restartRequired":{"type":"boolean"},"assetPreparation":{"type":"boolean"},"enabledWhen":{"type":"object","additionalProperties":false,"required":["setting","equals"],"properties":{"setting":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"},"equals":{"type":["boolean","string","number"]}}},"min":{"type":"number"},"max":{"type":"number"},"step":{"type":"number","exclusiveMinimum":0},"minLength":{"type":"integer","minimum":0},"maxLength":{"type":"integer","minimum":0},"options":{"type":"object","minProperties":1,"additionalProperties":{"type":"string","minLength":1,"maxLength":120}}},"allOf":[{"if":{"required":["control"],"properties":{"control":{"const":"keybind"}}},"then":{"properties":{"value":{"type":"string","maxLength":96,"description":"One shortcut string containing an SDL keyboard chord, a Mouse button chord, or keyboard|mouse alternatives. Each chord supports ordered Ctrl, Alt, Shift and Meta prefixes. Empty disables the shortcut.","pattern":"^(?:(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?SDL:[1-9][0-9]{0,2}(?:\\|(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?Mouse:[1-9][0-9]?)?|(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?Mouse:[1-9][0-9]?)?$"}}}}]},"action":{"type":"object","additionalProperties":false,"required":["id","label"],"properties":{"id":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"},"label":{"type":"string","minLength":1,"maxLength":120},"style":{"enum":["primary","secondary","danger"]},"confirm":{"type":"string","maxLength":500}}},"group":{"type":"object","additionalProperties":false,"required":["label"],"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"settings":{"type":"array","uniqueItems":true,"items":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"}},"actions":{"type":"array","items":{"$ref":"#/$defs/action"}}}}}}"##;
const LOCALE_179: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://bonsaibauer.github.io/shroudforge/schemas/mod-locale.schema.json","title":"ShroudForge mod locale catalog","type":"object","additionalProperties":false,"properties":{"$schema":{"type":"string"},"mod":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":2000}}},"settings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"$ref":"#/$defs/setting"}},"groups":{"type":"array","maxItems":50,"items":{"$ref":"#/$defs/group"}},"changelog":{"type":"array","maxItems":50,"items":{"type":"string","minLength":1,"maxLength":500}},"strings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"string","minLength":1,"maxLength":4000}}},"$defs":{"setting":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"options":{"type":"object","propertyNames":{"type":"string"},"additionalProperties":{"type":"string","minLength":1,"maxLength":120}}}},"group":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"actions":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"$ref":"#/$defs/action"}}}},"action":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"confirm":{"type":"string","maxLength":500}}}}}"##;
const MANIFEST_180: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://shroudforge.net/schemas/1.8.0/manifest.schema.json","title":"EML mod manifest","type":"object","additionalProperties":false,"required":["id","name","version"],"properties":{"id":{"type":"string","pattern":"^[A-Za-z0-9._-]+$","maxLength":80},"name":{"type":"string","minLength":1,"maxLength":120},"version":{"type":"string","pattern":"^\\d+\\.\\d+\\.\\d+(?:[-+][0-9A-Za-z.-]+)?$"},"description":{"type":"string","maxLength":2000},"authors":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"license":{"description":"Optional license identifier (for example, MIT). Use null when no license is declared.","type":["string","null"]},"icon":{"description":"Optional mod icon filename in the package root or path under assets/. Use null when the mod has no icon.","type":["string","null"],"pattern":"^(?:assets/[A-Za-z0-9._/-]+|[A-Za-z0-9_-][A-Za-z0-9._-]*)$"},"dependencies":{"description":"Mods that must be available before this mod starts. Each id is another mod's manifest id. Version is a SemVer version requirement. If optional is true, the dependency does not block startup when missing, disabled, or outside the version range.","type":"array","items":{"type":"object","additionalProperties":false,"description":"One mod dependency.","required":["id","version"],"properties":{"id":{"description":"Exact id from the dependency mod's mod.json.","type":"string","pattern":"^[A-Za-z0-9._-]+$","maxLength":80},"version":{"description":"SemVer version requirement, for example ^1.2.0 or >=1.2.0, <2.0.0.","type":"string"},"optional":{"description":"When true, this mod may start if the dependency is unavailable. Defaults to false.","type":"boolean","default":false}}}},"$schema":{"type":"string"},"capabilities":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["patch","export","runtime","runtime-register-dll"]}}}}"##;
const EXTENSION_180: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://shroudforge.net/schemas/1.8.0/extended.mod.schema.json","title":"ShroudForge mod extension","type":"object","additionalProperties":false,"required":["schemaVersion","enabled"],"properties":{"$schema":{"type":"string"},"schemaVersion":{"const":1},"enabled":{"type":"boolean"},"targets":{"description":"Processes this mod runs in. Legacy EML packages without targets default to client and server. ShroudForge packages default to client.","type":"array","minItems":1,"uniqueItems":true,"items":{"enum":["client","server"]}},"launcher":{"description":"Launcher that originally identified this mod. Omit for ShroudForge mods. Use EML to preserve EML provenance.","enum":["EML","SF"]},"links":{"type":"object","propertyNames":{"pattern":"^[a-z][a-z0-9-]*$"},"additionalProperties":{"type":"string","pattern":"^https://","maxLength":2048}},"changelog":{"type":"array","maxItems":50,"items":{"type":"string","minLength":1,"maxLength":500}},"settings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"oneOf":[{"type":["boolean","string","number","array"]},{"$ref":"#/$defs/setting"}]}},"groups":{"type":"array","items":{"$ref":"#/$defs/group"}},"inputReservations":{"$ref":"#/$defs/inputReservations"},"presets":{"description":"Setting presets, keyed by the string setting that chooses them. Choosing a preset sets each listed setting. Editing one of them selects the custom value.","type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"$ref":"#/$defs/preset"}}},"$defs":{"setting":{"type":"object","additionalProperties":false,"required":["value"],"properties":{"value":{"type":["boolean","string","number","array"]},"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"control":{"enum":["toggle","checkbox","text","textarea","number","slider","select","radio","segmented","multiselect","keybind","color"]},"applyAt":{"enum":["live","restart"]},"restartRequired":{"type":"boolean"},"assetPreparation":{"type":"boolean"},"enabledWhen":{"type":"object","additionalProperties":false,"required":["setting","equals"],"properties":{"setting":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"},"equals":{"type":["boolean","string","number"]}}},"min":{"type":"number"},"max":{"type":"number"},"step":{"type":"number","exclusiveMinimum":0},"minLength":{"type":"integer","minimum":0},"maxLength":{"type":"integer","minimum":0},"options":{"type":"object","minProperties":1,"additionalProperties":{"oneOf":[{"type":"string","minLength":1,"maxLength":120},{"type":"object","additionalProperties":false,"required":["label"],"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"group":{"type":"string","minLength":1,"maxLength":60}}}]}}},"allOf":[{"if":{"required":["control"],"properties":{"control":{"const":"keybind"}}},"then":{"properties":{"value":{"type":"string","maxLength":96,"description":"One shortcut string containing an SDL keyboard chord, a Mouse button chord, or keyboard|mouse alternatives. Each chord supports ordered Ctrl, Alt, Shift and Meta prefixes. Empty disables the shortcut.","pattern":"^(?:(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?SDL:[1-9][0-9]{0,2}(?:\\|(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?Mouse:[1-9][0-9]?)?|(?:Ctrl\\+)?(?:Alt\\+)?(?:Shift\\+)?(?:Meta\\+)?Mouse:[1-9][0-9]?)?$"}}}}]},"action":{"type":"object","additionalProperties":false,"required":["id","label"],"properties":{"id":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"},"label":{"type":"string","minLength":1,"maxLength":120},"style":{"enum":["primary","secondary","danger"]},"confirm":{"type":"string","maxLength":500}}},"group":{"type":"object","additionalProperties":false,"required":["label"],"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"settings":{"type":"array","uniqueItems":true,"items":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"}},"actions":{"type":"array","items":{"$ref":"#/$defs/action"}},"tab":{"type":"boolean"}}},"inputReservations":{"description":"Keys and mouse wheel the game may be denied while the mod reports one of its contexts. A ShroudForge without input reservations ignores this block.","type":"object","additionalProperties":false,"properties":{"contexts":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"string","minLength":1,"maxLength":120}},"keys":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"object","additionalProperties":false,"required":["when"],"properties":{"when":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"},"keyboard":{"type":"boolean"},"mouse":{"type":"boolean"}}}},"wheels":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"object","additionalProperties":false,"required":["when"],"properties":{"when":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"}}}},"choices":{"description":"The player's padlocks, written by the Modloader.","type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"object","additionalProperties":false,"properties":{"keyboard":{"type":"boolean"},"mouse":{"type":"boolean"}}}}}},"preset":{"type":"object","additionalProperties":false,"required":["settings","values"],"properties":{"custom":{"description":"Controller value meaning hand-tuned settings. Defaults to custom.","type":"string","minLength":1},"settings":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string","pattern":"^[A-Za-z0-9._-]{1,80}$"}},"values":{"description":"One entry per preset value of the controller, giving a value to every listed setting.","type":"object","minProperties":1,"additionalProperties":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":["boolean","string","number","array"]}}}}}}}"##;
const LOCALE_180: &str = r##"{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://shroudforge.net/schemas/1.8.0/mod-locale.schema.json","title":"ShroudForge mod locale catalog","type":"object","additionalProperties":false,"properties":{"$schema":{"type":"string"},"mod":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":2000}}},"settings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"$ref":"#/$defs/setting"}},"groups":{"type":"array","maxItems":50,"items":{"$ref":"#/$defs/group"}},"changelog":{"type":"array","maxItems":50,"items":{"type":"string","minLength":1,"maxLength":500}},"strings":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"type":"string","minLength":1,"maxLength":4000}}},"$defs":{"setting":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"options":{"type":"object","propertyNames":{"type":"string"},"additionalProperties":{"type":"string","minLength":1,"maxLength":120}},"optionGroups":{"description":"Translations for option category labels declared by the setting's options.","type":"object","propertyNames":{"type":"string"},"additionalProperties":{"type":"string","minLength":1,"maxLength":120}}}},"group":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"description":{"type":"string","maxLength":500},"actions":{"type":"object","propertyNames":{"pattern":"^[A-Za-z0-9._-]{1,80}$"},"additionalProperties":{"$ref":"#/$defs/action"}}}},"action":{"type":"object","additionalProperties":false,"properties":{"label":{"type":"string","minLength":1,"maxLength":120},"confirm":{"type":"string","maxLength":500}}}}}"##;

#[cfg(test)]
mod schema_guard {
    use super::*;
    fn inspect(root: &Value, node: &Value) {
        if node.is_boolean() {
            return;
        }
        for (key, value) in node.as_object().expect("schema object") {
            match key.as_str() {
                "$schema" | "$id" | "title" | "description" | "default" | "type" | "required"
                | "const" | "enum" | "minLength" | "maxLength" | "minimum" | "maximum"
                | "exclusiveMinimum" | "minItems" | "maxItems" | "minProperties"
                | "uniqueItems" => {}
                "pattern" => {
                    let pattern = value.as_str().unwrap();
                    for special in ["(?=", "(?!", "(?<=", "(?<!"] {
                        assert!(!pattern.contains(special), "{pattern}");
                    }
                    let mut chars = pattern.chars();
                    while let Some(c) = chars.next() {
                        if c == '\\' {
                            // \c control escapes, \1-\9 backreferences and the \a bell
                            // literal jsonschema refuses (src/ecma.rs:157-166).
                            assert!(
                                !matches!(chars.next(), Some('1'..='9' | 'c' | 'a')),
                                "{pattern}"
                            );
                        }
                    }
                    pattern_matches(&compiled_pattern(pattern), "").unwrap();
                }
                "$ref" => {
                    assert!(
                        root.pointer(value.as_str().unwrap().strip_prefix('#').unwrap())
                            .is_some()
                    );
                }
                "properties" | "$defs" => {
                    for s in value.as_object().unwrap().values() {
                        inspect(root, s);
                    }
                }
                "items" | "additionalProperties" | "propertyNames" | "if" | "then" | "else" => {
                    inspect(root, value)
                }
                "oneOf" | "allOf" => {
                    for s in value.as_array().unwrap() {
                        inspect(root, s);
                    }
                }
                _ => panic!("unimplemented schema keyword: {key}"),
            }
        }
    }
    #[test]
    fn patterns_compile_perl_classes_as_jsonschema_does() {
        // jsonschema 0.33.0 src/ecma.rs:179-191 test_case table.
        for (input, compiled) in [
            (r"\d", "[0-9]"),
            (r"[\d]", "[[0-9]]"),
            (r"\d+\w*", "[0-9]+[A-Za-z0-9_]*"),
            (r"[\d\w]", "[[0-9][A-Za-z0-9_]]"),
            (r"[^\d\w]", "[^[0-9][A-Za-z0-9_]]"),
            (r"\D\W", "[^0-9][^A-Za-z0-9_]"),
            (r"^\\d\.", r"^\\d\."),
            (r"^[A-Za-z0-9._-]+$", r"^[A-Za-z0-9._-]+$"),
        ] {
            assert_eq!(compiled_pattern(input), compiled, "{input}");
        }
        let version = r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$";
        assert!(pattern_matches(&compiled_pattern(version), "1.2.3").unwrap());
        assert!(!pattern_matches(&compiled_pattern(version), "\u{661}.\u{662}.\u{663}").unwrap());
        assert!(pattern_matches(&compiled_pattern(r"^\s$"), "\u{2003}").unwrap());
        assert!(!pattern_matches(&compiled_pattern(r"^\s$"), "\u{2002}").unwrap());
    }
    #[test]
    fn every_embedded_schema_keyword_pattern_and_reference_is_supported() {
        for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
            for kind in [Schema::Manifest, Schema::Extension, Schema::Locale] {
                let root = document_schema(version, kind);
                inspect(root, root);
            }
        }
    }
}
