// SPDX-License-Identifier: MIT
//! Contract-derived diagnostics, not oracle observations: both SF pins use
//! parse_mod_manifest.rs:70-98 (1.7.9), :75-103 (1.8.0), documents.rs:2-14,
//! loader-config/validate_document.rs:77-79, read_mod_locale_catalogs.rs:45-59,
//! validate_mod_settings.rs:34-37 and jsonschema 0.33.0's error Display.
#![allow(dead_code)]
// Documents keep their written key order (SF reads them with preserve_order): `oj!` is
// serde_json's json! as an ordered value, and serde_json::from_str::<Value> keeps text order.
use sfi_package::json::Value;
use sfi_package::package::{Package, ParseOptions, SourceVersion, settings::validate_settings};
macro_rules! oj {
    ($($json:tt)+) => {
        Value::from(serde_json::json!($($json)+))
    };
}

fn manifest() -> Value {
    oj!({"id":"example","name":"Example","version":"1.0.0"})
}
fn extension() -> Value {
    oj!({"schemaVersion":1,"enabled":true})
}
fn parse(m: &Value, e: &Value, options: ParseOptions) -> Result<Package, String> {
    Package::parse(
        m.to_string().as_bytes(),
        Some(e.to_string().as_bytes()),
        options,
    )
}
fn versions() -> impl Iterator<Item = ParseOptions> {
    [SourceVersion::Sf179, SourceVersion::Sf180]
        .map(|version| ParseOptions {
            version,
            ..Default::default()
        })
        .into_iter()
}

pub fn strict_unknown_fields_use_document_names_pluralization_and_declaration_order() {
    for options in versions() {
        let options = ParseOptions {
            tolerate_unknown_fields: false,
            ..options
        };
        for (unknown, suffix) in [
            (oj!({"foreign":true}), "field: foreign"),
            (
                serde_json::from_str::<Value>(r#"{"z":true,"a":false,"z":false}"#).unwrap(),
                "fields: z, a",
            ),
        ] {
            let mut m = manifest();
            m.as_object_mut()
                .unwrap()
                .extend(unknown.as_object().unwrap().clone());
            assert_eq!(
                parse(&m, &extension(), options).unwrap_err(),
                format!(
                    "mod: unknown {suffix}. Enable Mod Detection to ignore unknown top-level fields."
                )
            );
            let mut e = extension();
            e.as_object_mut()
                .unwrap()
                .extend(unknown.as_object().unwrap().clone());
            assert_eq!(
                parse(&manifest(), &e, options).unwrap_err(),
                format!(
                    "extended-mod: unknown {suffix}. Enable Mod Detection to ignore unknown top-level fields."
                )
            );
        }
        // Strict unknown-field diagnostics precede ordinary schema errors.
        let mut m = manifest();
        m["name"] = oj!(false);
        m["foreign"] = oj!(true);
        assert_eq!(
            parse(&m, &extension(), options).unwrap_err(),
            "mod: unknown field: foreign. Enable Mod Detection to ignore unknown top-level fields."
        );
    }
}

pub fn nonobject_documents_name_the_correct_document() {
    for options in versions() {
        for value in [oj!([]), oj!(null), oj!(false), oj!("text")] {
            assert_eq!(
                parse(&value, &extension(), options).unwrap_err(),
                "mod must be an object"
            );
            assert_eq!(
                parse(&manifest(), &value, options).unwrap_err(),
                "extended-mod must be an object"
            );
        }
        // Both pins validate extended.mod.json before mod.json (parse_mod_manifest.rs:
        // 105-123 in 1.7.9, 110-128 in 1.8.0), so with both invalid the extension wins.
        assert_eq!(
            parse(&oj!([]), &oj!([]), options).unwrap_err(),
            "extended-mod must be an object"
        );
        let mut m = manifest();
        m["name"] = oj!(false);
        let mut e = extension();
        e["enabled"] = oj!(null);
        assert_eq!(
            parse(&m, &e, options).unwrap_err(),
            r#"extended-mod: null is not of type "boolean""#
        );
    }
}

pub fn manifest_and_extension_schema_messages_match_pinned_shapes() {
    for options in versions() {
        for (field, value, expected) in [
            ("name", oj!(false), r#"mod: false is not of type "string""#),
            ("name", oj!(""), r#"mod: "" is shorter than 1 character"#),
            (
                "id",
                oj!("bad id"),
                r#"mod: "bad id" does not match "^[A-Za-z0-9._-]+$""#,
            ),
            (
                "license",
                oj!(false),
                r#"mod: false is not of types "null", "string""#,
            ),
            (
                "authors",
                oj!(["a", "a"]),
                r#"mod: ["a","a"] has non-unique elements"#,
            ),
            // jsonschema 0.33.0 compiles the ECMA pattern to Rust with \d as [0-9]
            // (src/ecma.rs:110-114) and prints the compiled text (error.rs:991-993); the
            // ASCII class also refuses non-ASCII decimal digits.
            (
                "version",
                oj!("1.2"),
                r#"mod: "1.2" does not match "^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$""#,
            ),
            (
                "version",
                oj!("\u{661}.\u{662}.\u{663}"),
                "mod: \"\u{661}.\u{662}.\u{663}\" does not match \"^[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$\"",
            ),
            (
                "capabilities",
                oj!(["unknown"]),
                r#"mod: "unknown" is not one of "patch", "export" or 2 other candidates"#,
            ),
        ] {
            let mut m = manifest();
            m[field] = value;
            assert_eq!(
                parse(&m, &extension(), options).unwrap_err(),
                expected,
                "{:?} / {field}",
                options.version
            );
        }
        let mut m = manifest();
        m.as_object_mut().unwrap().shift_remove("id");
        assert_eq!(
            parse(&m, &extension(), options).unwrap_err(),
            r#"mod: "id" is a required property"#
        );
        for (field, value, expected) in [
            ("schemaVersion", oj!(2), "extended-mod: 1 was expected"),
            (
                "enabled",
                oj!(null),
                r#"extended-mod: null is not of type "boolean""#,
            ),
            ("targets", oj!([]), "extended-mod: [] has less than 1 item"),
            (
                "targets",
                oj!(["elsewhere"]),
                r#"extended-mod: "elsewhere" is not one of "client" or "server""#,
            ),
            (
                "changelog",
                oj!([""]),
                r#"extended-mod: "" is shorter than 1 character"#,
            ),
            (
                "links",
                oj!({"Bad":"https://example.org"}),
                r#"extended-mod: "Bad" does not match "^[a-z][a-z0-9-]*$""#,
            ),
            (
                "groups",
                oj!([{"label":"Group","foreign":true}]),
                "extended-mod: Additional properties are not allowed ('foreign' was unexpected)",
            ),
            (
                "groups",
                serde_json::from_str::<Value>(r#"[{"label":"Group","z":true,"a":false}]"#).unwrap(),
                "extended-mod: Additional properties are not allowed ('z' was unexpected)",
            ),
            (
                "settings",
                serde_json::from_str::<Value>(r#"{"x":{"value":1,"foreign":true}}"#).unwrap(),
                r#"extended-mod: {"value":1,"foreign":true} is not valid under any of the schemas listed in the 'oneOf' keyword"#,
            ),
        ] {
            let mut e = extension();
            e[field] = value;
            assert_eq!(
                parse(&manifest(), &e, options).unwrap_err(),
                expected,
                "{:?} / {field}",
                options.version
            );
        }
    }
}

// Both pins call Validator::validate, not iter_errors (loader-config/
// validate_document.rs:77-79). jsonschema 0.33.0's closed-record validator
// checks known values and rejects the first unknown key in instance order
// (keywords/additional_properties.rs:282-302).
pub fn nested_closed_record_diagnostics_follow_instance_order() {
    for options in versions() {
        for (group, expected) in [
            (
                r#"{"label":false,"foreign":true}"#,
                r#"extended-mod: false is not of type "string""#,
            ),
            (
                r#"{"foreign":true,"label":false}"#,
                "extended-mod: Additional properties are not allowed ('foreign' was unexpected)",
            ),
            (
                r#"{"label":"","foreign":true}"#,
                r#"extended-mod: "" is shorter than 1 character"#,
            ),
            (
                r#"{"label":"Group","z":true,"a":false}"#,
                "extended-mod: Additional properties are not allowed ('z' was unexpected)",
            ),
            (
                r#"{"label":"Group","a":false,"z":true}"#,
                "extended-mod: Additional properties are not allowed ('a' was unexpected)",
            ),
        ] {
            let mut e = extension();
            e["groups"] = Value::Array(vec![serde_json::from_str(group).unwrap()]);
            assert_eq!(
                parse(&manifest(), &e, options).unwrap_err(),
                expected,
                "{:?} / {group}",
                options.version
            );
        }
        let mut m = manifest();
        m["dependencies"] =
            serde_json::from_str(r#"[{"id":false,"version":"*","z":true,"a":false}]"#).unwrap();
        assert_eq!(
            parse(&m, &extension(), options).unwrap_err(),
            r#"mod: false is not of type "string""#,
            "{:?}",
            options.version
        );
        let mut package = parse(&manifest(), &extension(), options).unwrap();
        for (bytes, expected) in [
            (
                br#"{"mod":{"name":false,"z":true,"a":false}}"#.as_slice(),
                r#"de.json: mod-locale: false is not of type "string""#,
            ),
            (
                br#"{"mod":{"z":true,"name":false,"a":false}}"#.as_slice(),
                "de.json: mod-locale: Additional properties are not allowed ('z' was unexpected)",
            ),
        ] {
            assert_eq!(
                package.set_locales([("de.json", bytes)]).unwrap_err(),
                expected,
                "{:?}",
                options.version
            );
        }
    }
}

pub fn settings_schema_messages_keep_the_setting_key_prefix() {
    for options in versions() {
        for (definition, value, expected) in [
            (
                oj!({"value":1}),
                oj!("bad"),
                r#"x: "bad" is not of type "integer""#,
            ),
            (
                oj!({"value":1,"min":0}),
                oj!(-1),
                "x: -1 is less than the minimum of 0.0",
            ),
            (
                oj!({"value":1,"max":2}),
                oj!(3),
                "x: 3 is greater than the maximum of 2.0",
            ),
            (
                oj!({"value":"a","minLength":1}),
                oj!(""),
                r#"x: "" is shorter than 1 character"#,
            ),
            (
                oj!({"value":"a","maxLength":1}),
                oj!("ab"),
                r#"x: "ab" is longer than 1 character"#,
            ),
            (
                oj!({"value":1,"options":{"1":"One","2":"Two","3":"Three"}}),
                oj!(4),
                "x: 4 is not one of 1, 2 or 3",
            ),
            (
                oj!({"value":[1],"options":{"1":"One"}}),
                oj!([2]),
                "x: 2 is not one of 1",
            ),
        ] {
            let mut e = extension();
            e["settings"] = oj!({"x":definition});
            let package = parse(&manifest(), &e, options).unwrap();
            assert_eq!(
                validate_settings(&package.manifest.settings, &oj!({"x":value})).unwrap_err(),
                expected,
                "{:?} / {definition}",
                options.version
            );
        }
        // validate_mod_settings.rs:10-12 prefixes every hotkey parse_value error with
        // the key, including the non-string refusal (hotkeys/parse_hotkey.rs:174-179).
        let mut e = extension();
        e["settings"] = oj!({"k":{"value":"SDL:4","control":"keybind"}});
        let package = parse(&manifest(), &e, options).unwrap();
        for value in [oj!(4), oj!(null), oj!(["SDL:4"])] {
            assert_eq!(
                validate_settings(&package.manifest.settings, &oj!({ "k": value })).unwrap_err(),
                "k: Hotkey must be an SDL binding string",
                "{:?} / {value}",
                options.version
            );
        }
    }
}

pub fn locale_schema_messages_include_the_supplied_source_and_document_name() {
    for options in versions() {
        let mut package = parse(&manifest(), &extension(), options).unwrap();
        for (bytes, expected) in [
            (
                b"[]".as_slice(),
                r#"de.json: mod-locale: [] is not of type "object""#,
            ),
            (
                br#"{"foreign":true}"#,
                "de.json: mod-locale: Additional properties are not allowed ('foreign' was unexpected)",
            ),
            (
                br#"{"mod":{"name":false}}"#,
                r#"de.json: mod-locale: false is not of type "string""#,
            ),
            (
                br#"{"strings":{"hello":""}}"#,
                r#"de.json: mod-locale: "" is shorter than 1 character"#,
            ),
            (
                br#"{"mod":{"foreign":true}}"#,
                "de.json: mod-locale: Additional properties are not allowed ('foreign' was unexpected)",
            ),
        ] {
            assert_eq!(
                package.set_locales([("de.json", bytes)]).unwrap_err(),
                expected,
                "{:?}",
                options.version
            );
        }
    }
}

// Conservative subcase proof only. Missing host, typed and callback cases and legacy
// v1 error-userdata goldens do not authorize complete/message parity.

// The differential package-document goldens. tests/conformance/package-model/cases.json
// holds 793 package directories (mod.json, extended.mod.json, locales/*.json; bytes as
// written). sf-1.7.9.json and sf-1.8.0.json are the outputs of ShroudForge's own
// read_manifest_path (modloader/read_mod_manifest.rs:129 both pins; parse_mod_manifest.rs,
// validate_mod_manifest.rs, read_mod_locale_catalogs.rs, jsonschema 0.33.0), its
// validate_settings (validate_mod_settings.rs:3), localized_manifest and localized_string
// (resolve_mod_locale.rs:8,20), built natively from the pinned checkouts; provenance in
// goldens.toml. Kiln reads the same directories through discover::read_package. Each
// result is the error string or the complete serde serialization of the normalized
// manifest. divergences.json lists reviewed Kiln/SF differences, pinned exactly.
mod package_documents {
    use serde_json::{Value, json};
    use sfi_package::{
        package::{
            ParseOptions, SourceVersion,
            discover::read_package,
            locales::{localized_manifest, localized_string},
            profiles::ProfileCatalog,
            revisions::{config_revision, loaded_revision, manifest_revision},
            settings::{setting_is_enabled, validate_settings},
        },
        source::{DiskSource, PackageSource},
    };
    use std::{
        fs,
        rc::Rc,
        sync::atomic::{AtomicU64, Ordering},
    };
    static SEQ: AtomicU64 = AtomicU64::new(0);
    const CASES: &str = include_str!("../conformance/package-model/cases.json");
    const DIVERGENCES: &str = include_str!("../conformance/package-model/divergences.json");

    fn kiln_output(case: &Value, version: SourceVersion) -> Value {
        let dir = std::env::temp_dir().join(format!(
            "c8b-pkg-{}-{}",
            std::process::id(),
            SEQ.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&dir).unwrap();
        for (name, text) in case["files"].as_object().unwrap() {
            let path = dir.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, text.as_str().unwrap()).unwrap();
        }
        for (name, hex) in case
            .get("files_hex")
            .and_then(Value::as_object)
            .into_iter()
            .flatten()
        {
            let hex = hex.as_str().unwrap();
            let bytes: Vec<u8> = (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                .collect();
            fs::write(dir.join(name), bytes).unwrap();
        }
        let options = ParseOptions {
            version,
            tolerate_unknown_fields: case["strict"] != true,
            ..Default::default()
        };
        let source = Rc::new(DiskSource::new(&dir).unwrap()) as Rc<dyn PackageSource>;
        let record = read_package("pkg", source, options, &ProfileCatalog::default(), None);
        let mut out = json!({"id": case["id"]});
        match &record {
            Ok(r) => out["ok"] = serde_json::to_value(&r.parsed().manifest).unwrap(),
            Err(e) => out["err"] = json!(e),
        }
        if let Ok(r) = &record {
            let m = &r.parsed().manifest;
            out["rev"] = json!({
                "config": config_revision(m),
                "loaded": loaded_revision(m),
                "manifest": manifest_revision(r.raw_source().as_ref())
                    .unwrap_or_else(|e| e.replace(&dir.display().to_string(), "<package>")),
            });
            let values = case
                .get("enabled_values")
                .cloned()
                .unwrap_or_else(|| m.setting_values.clone());
            out["enabled"] = m
                .settings
                .iter()
                .map(|s| {
                    (
                        s.key.clone(),
                        json!(setting_is_enabled(&s.key, &m.settings, &values)),
                    )
                })
                .collect::<serde_json::Map<_, _>>()
                .into();
            if case.get("save").is_some() {
                // The values in the case file's own key order: the first refusal wins.
                let values = ordered_case(case["id"].as_str().unwrap())["save"].clone();
                out["save"] = match validate_settings(&m.settings, &values) {
                    Ok(()) => Value::Null,
                    Err(e) => json!(e),
                };
            }
            if let Some(locales) = case.get("resolve").and_then(Value::as_array) {
                out["resolve"] = locales
                    .iter()
                    .map(|l| {
                        serde_json::to_value(localized_manifest(m, l.as_str().unwrap())).unwrap()
                    })
                    .collect();
            }
            if let Some(lookups) = case.get("strings").and_then(Value::as_array) {
                out["strings"] = lookups
                    .iter()
                    .map(|s| {
                        json!(localized_string(
                            &m.locales,
                            s[0].as_str().unwrap(),
                            s[1].as_str().unwrap(),
                            s[2].as_str()
                        ))
                    })
                    .collect();
            }
        }
        fs::remove_dir_all(&dir).unwrap();
        out
    }

    /// A case as ShroudForge's harness read it: objects in file order.
    fn ordered_case(id: &str) -> &'static sfi_package::json::Value {
        static ORDERED: std::sync::OnceLock<Vec<sfi_package::json::Value>> =
            std::sync::OnceLock::new();
        ORDERED
            .get_or_init(|| serde_json::from_str(CASES).unwrap())
            .iter()
            .find(|case| case["id"] == id)
            .unwrap()
    }

    /// The error read_package gives for one case directory.
    pub fn read_error(case: &Value, version: SourceVersion) -> String {
        let out = kiln_output(case, version);
        out["err"]
            .as_str()
            .unwrap_or_else(|| panic!("{} was accepted: {out}", case["id"]))
            .to_owned()
    }

    pub fn run(version: SourceVersion, pin: &str, golden: &str) {
        let cases: Vec<Value> = serde_json::from_str(CASES).unwrap();
        let golden: Vec<Value> = serde_json::from_str(golden).unwrap();
        let divergences: Value = serde_json::from_str(DIVERGENCES).unwrap();
        assert_eq!(cases.len(), golden.len());
        let mut failures = vec![];
        let mut used = 0;
        let mut unbuildable = vec![];
        for (case, sf) in cases.iter().zip(&golden) {
            assert_eq!(case["id"], sf["id"]);
            let id = case["id"].as_str().unwrap();
            // A case-insensitive file system (NTFS, Wine) cannot hold two file names
            // that differ only in case, so neither SF nor Kiln can be shown such a
            // directory there; the Linux build covers those cases.
            if cfg!(windows) {
                let names: Vec<String> = case["files"]
                    .as_object()
                    .unwrap()
                    .keys()
                    .map(|name| name.to_ascii_lowercase())
                    .collect();
                if (1..names.len()).any(|i| names[..i].contains(&names[i])) {
                    unbuildable.push(id);
                    continue;
                }
            }
            let mut kiln = kiln_output(case, version);
            // These goldens come from a Linux build. SF prints a locale catalog by its
            // listed path, with the platform separator, so on Windows the reader's
            // errors start "locales\\"; the Windows spelling itself is proven by
            // locale_errors_name_the_listed_path_with_the_platform_separator.
            if cfg!(windows)
                && let Some(rest) = kiln["err"]
                    .as_str()
                    .and_then(|e| e.strip_prefix("locales\\"))
            {
                kiln["err"] = json!(format!("locales/{rest}"));
            }
            let expected = match divergences.get(format!("{pin}/{id}")) {
                Some(d) => {
                    used += 1;
                    assert_ne!(&d["kiln"], sf, "{pin}/{id}: listed divergence equals SF");
                    &d["kiln"]
                }
                None => sf,
            };
            if &kiln != expected {
                failures.push(format!("{pin}/{id}:\n  kiln {kiln}\n  want {expected}"));
            }
        }
        let listed = divergences
            .as_object()
            .unwrap()
            .keys()
            .filter(|k| k.starts_with(&format!("{pin}/")))
            .count();
        assert_eq!(used, listed, "{pin}: a listed divergence names no case");
        let expected_unbuildable: &[&str] = if cfg!(windows) {
            &[
                "loc.name.uppercase-ignored",
                "loc.dup-case",
                "loc.dup-case-lower",
            ]
        } else {
            &[]
        };
        assert_eq!(unbuildable, expected_unbuildable, "{pin}");
        // Neither pin's disk reader bounds a package file (read_mod_manifest.rs:9-13
        // reads it whole); Kiln's 2 MiB limit belongs to the editor alone.
        let manifest = r#"{"id":"example","name":"Example","version":"1.0.0"}"#;
        let large = json!({
            "id": "mod.json.over-2-mib",
            "files": {"mod.json": format!("{manifest}{}", " ".repeat(2 * 1024 * 1024))},
        });
        assert_eq!(
            kiln_output(&large, version)["ok"]["id"],
            "example",
            "{pin}: a mod.json over 2 MiB"
        );
        assert!(
            failures.is_empty(),
            "{} of {} cases differ:\n{}",
            failures.len(),
            cases.len(),
            failures.join("\n")
        );
    }
}

/// SF prints each locale catalog error with the path its package reader listed: the disk
/// listing strips the canonical root from std::fs::read_dir paths and the archive listing
/// joins with camino's Utf8Path::join (mod_package_filesystem.rs:261-298,379-390), so the
/// separator is the platform's: `locales\de.json` on Windows, `locales/de.json` elsewhere
/// (read_mod_locale_catalogs.rs:37-59, both pins). The cross-reference errors spell
/// `locales/<code>.json` literally on every platform (resolve_mod_locale.rs:46-85).
pub fn locale_errors_name_the_listed_path_with_the_platform_separator() {
    let sep = if cfg!(windows) { '\\' } else { '/' };
    for (version, pin) in [
        (SourceVersion::Sf179, "1.7.9"),
        (SourceVersion::Sf180, "1.8.0"),
    ] {
        for (file, text, expected) in [
            (
                "e.json",
                "{}",
                format!("locales{sep}e.json: invalid locale filename"),
            ),
            (
                "de.json",
                "not json",
                format!("locales{sep}de.json: expected ident at line 1 column 2"),
            ),
            (
                "de.json",
                "[]",
                format!(r#"locales{sep}de.json: mod-locale: [] is not of type "object""#),
            ),
            (
                "de.json",
                r#"{"settings":{"missing":{}}}"#,
                "locales/de.json: unknown setting 'missing'".to_owned(),
            ),
        ] {
            assert_eq!(
                package_documents::read_error(
                    &serde_json::json!({
                        "id": "loc.separator",
                        "files": {
                            "mod.json": manifest().to_string(),
                            "extended.mod.json": extension().to_string(),
                            format!("locales/{file}"): text,
                        },
                    }),
                    version,
                ),
                expected,
                "{pin}: {file} {text}"
            );
        }
    }
}

pub fn sf_179_package_documents_match_shroudforge_reader_goldens() {
    package_documents::run(
        SourceVersion::Sf179,
        "1.7.9",
        include_str!("../conformance/package-model/sf-1.7.9.json"),
    );
}

pub fn sf_180_package_documents_match_shroudforge_reader_goldens() {
    package_documents::run(
        SourceVersion::Sf180,
        "1.8.0",
        include_str!("../conformance/package-model/sf-1.8.0.json"),
    );
}
