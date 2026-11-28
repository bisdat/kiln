// SPDX-License-Identifier: MIT
//! Source-derived parser scenarios: the seeded Lua oracle cannot inject documents.
#![allow(dead_code)]
use serde_json::{Value, json};
use sfi_package::package::{Package, ParseOptions, SourceVersion, Target, edit, locales};

fn manifest() -> Value {
    json!({"id":"Example.ID","name":"Example","version":"1.2.3","capabilities":["runtime"]})
}
fn extension() -> Value {
    json!({"schemaVersion":1,"enabled":true})
}
fn parse(m: &Value, e: &Value) -> Result<Package, String> {
    Package::parse(
        m.to_string().as_bytes(),
        Some(e.to_string().as_bytes()),
        ParseOptions::default(),
    )
}
/// `e` with `settings` taken as text, in its own key order.
fn parse_with_settings(m: &Value, e: &Value, settings: &str) -> Result<Package, String> {
    let mut e = sfi_package::json::Value::from(e);
    e["settings"] = serde_json::from_str(settings).unwrap();
    Package::parse(
        m.to_string().as_bytes(),
        Some(e.to_string().as_bytes()),
        ParseOptions::default(),
    )
}
pub fn host_scancode_validation_and_179_top_level_tolerance_are_explicit() {
    let mut e = extension();
    e["settings"] = json!({"key":{"value":"SDL:66","control":"keybind"}});
    assert!(parse(&manifest(), &e).is_ok());
    assert!(
        Package::parse(
            manifest().to_string().as_bytes(),
            Some(e.to_string().as_bytes()),
            ParseOptions {
                valid_scancode: |code| code != 66 && (1..512).contains(&code),
                ..Default::default()
            }
        )
        .is_err()
    );
    // Older SF discards unknown *top-level* 1.8 blocks with its default policy.
    e["inputReservations"] = json!({"unvalidated-by-179":true});
    assert!(
        Package::parse(
            manifest().to_string().as_bytes(),
            Some(e.to_string().as_bytes()),
            ParseOptions {
                version: SourceVersion::Sf179,
                ..Default::default()
            }
        )
        .is_ok()
    );
    assert!(parse(&manifest(), &e).is_err());
}
pub fn every_control_capability_and_registered_link_has_a_labeled_case() {
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        for control in [
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
        ] {
            let mut e = extension();
            let setting = match control {
                "select" | "radio" | "segmented" => {
                    json!({"value":"one","control":control,"options":{"one":"One"}})
                }
                "multiselect" => json!({"value":["one"],"control":control,"options":{"one":"One"}}),
                "keybind" => json!({"value":"SDL:66","control":control}),
                "number" | "slider" => {
                    json!({"value":0.5,"control":control,"min":0,"max":1,"step":0.1})
                }
                "toggle" | "checkbox" => json!({"value":true,"control":control}),
                _ => json!({"value":"Text","control":control}),
            };
            e["settings"] = json!({"a":setting});
            let p = Package::parse(
                manifest().to_string().as_bytes(),
                Some(e.to_string().as_bytes()),
                ParseOptions {
                    version,
                    ..Default::default()
                },
            )
            .unwrap_or_else(|error| panic!("{version:?} / control={control}: {error}"));
            assert_eq!(
                serde_json::to_value(p.manifest.settings[0].control).unwrap(),
                control
            );
        }
        for capability in ["patch", "export", "runtime", "runtime-register-dll"] {
            let mut m = manifest();
            m["capabilities"] = json!([capability]);
            let p = Package::parse(
                m.to_string().as_bytes(),
                None,
                ParseOptions {
                    version,
                    ..Default::default()
                },
            )
            .unwrap();
            assert_eq!(
                serde_json::to_value(&p.manifest.capabilities).unwrap(),
                json!([capability])
            );
        }
    }
    let mut e = extension();
    let mut links = serde_json::Map::new();
    for id in [
        "source",
        "source-github",
        "source-gitlab",
        "source-codeberg",
        "issues",
        "wiki",
        "website",
        "store",
        "support-bmac",
        "support-patreon",
        "support-paypal",
        "support-github",
        "support-ko-fi",
        "support-open-collective",
        "support-other",
    ] {
        links.insert(id.into(), json!("https://example.org"));
    }
    e["links"] = links.into();
    let p = parse(&manifest(), &e).unwrap();
    assert_eq!(p.manifest.links.0.len(), 15);
}
/// ShroudForge 1.8.1's link registry (mod_link_order.json, 0581d72e) adds community-discord and video-youtube, and its
/// starter mod (docs/examples/starter-mod) carries both: the 1.8 dialect reads them, 1.7.9 still refuses them.
pub fn the_18_dialect_reads_the_discord_and_youtube_links() {
    let mut e = extension();
    e["links"] = json!({"community-discord":"https://discord.com/invite/example","video-youtube":"https://www.youtube.com/"});
    let read = |e: &Value, version| {
        Package::parse(
            manifest().to_string().as_bytes(),
            Some(e.to_string().as_bytes()),
            ParseOptions {
                version,
                ..ParseOptions::default()
            },
        )
    };
    let p = read(&e, SourceVersion::Sf180).unwrap();
    assert_eq!(p.manifest.links.0.len(), 2);
    assert_eq!(
        read(&e, SourceVersion::Sf179).err().as_deref(),
        Some("unsupported mod link 'community-discord'")
    );
    // still HTTPS only (the schema's pattern says so first)
    e["links"] = json!({"community-discord":"http://discord.com/invite/example"});
    let error = read(&e, SourceVersion::Sf180).err().unwrap();
    assert!(error.contains("https://"), "{error}");
}
pub fn strict_documents_preserve_bytes_order_duplicates_and_numeric_representation() {
    let raw = b"{\"schemaVersion\":1,\"enabled\":true,\"settings\":{\"z\":1,\"a\":-0,\"z\":2,\"n\":1e2}}\r\n";
    let p = Package::parse(
        manifest().to_string().as_bytes(),
        Some(raw),
        ParseOptions::default(),
    )
    .unwrap();
    assert_eq!(p.documents.extension.as_ref().unwrap().bytes(), raw);
    assert_eq!(
        p.manifest
            .settings
            .iter()
            .map(|s| s.key.as_str())
            .collect::<Vec<_>>(),
        ["z", "a", "n"]
    );
    assert_eq!(p.manifest.settings[0].default, 2);
    assert_eq!(
        p.manifest.settings[1].value_type,
        sfi_package::package::settings::SettingValueType::Number
    );
    for raw in [
        b"\xef\xbb\xbf{}".as_slice(),
        b"{/*no*/}",
        b"{\"x\":1,}",
        b"{\"x\":\"\\uD800\"}",
        b"{\"x\":1e999}",
        b"{\"x\":\"\xff\"}",
    ] {
        assert!(
            Package::parse(raw, None, ParseOptions::default()).is_err(),
            "{raw:?}"
        );
    }
}
pub fn schema_and_cross_field_refusals() {
    let mut cases: Vec<(Value, Value)> = vec![];
    for (key, value) in [
        ("id", json!("bad id")),
        ("name", json!("")),
        ("version", json!("01.2.3")),
        ("version", json!("1.2.3-a+b")),
        ("version", json!("18446744073709551616.0.0")),
        ("icon", json!("../bad.png")),
        ("authors", json!(["a", "a"])),
        ("capabilities", json!(["unknown"])),
        ("capabilities", json!(["runtime", "runtime"])),
        ("dependencies", json!([{"id":"a","version":"not a range"}])),
    ] {
        let mut m = manifest();
        m[key] = value;
        cases.push((m, extension()));
    }
    for patch in [
        json!({"schemaVersion":2}),
        json!({"enabled":null}),
        json!({"targets":[]}),
        json!({"targets":["server","server"]}),
        json!({"links":{"made-up":"https://example.org"}}),
        json!({"links":{"source-github":"http://example.org"}}),
        json!({"settings":{"a":{"value":1,"surprise":true}}}),
        json!({"settings":{"a":{"value":1,"min":2}}}),
        json!({"settings":{"a":{"value":1,"min":3,"max":2}}}),
        json!({"settings":{"a":{"value":1,"step":0}}}),
        json!({"settings":{"a":{"value":1,"control":"select"}}}),
        json!({"settings":{"a":{"value":[],"options":{}}}}),
        json!({"settings":{"a":{"value":1,"applyAt":"live","restartRequired":true}}}),
        json!({"settings":{"a":{"value":1,"enabledWhen":{"setting":"missing","equals":true}}}}),
        json!({"settings":{"a":{"value":true,"enabledWhen":{"setting":"a","equals":true}}}}),
        json!({"groups":[{"label":" ","settings":[]}]}),
        json!({"groups":[{"label":"G","settings":["missing"]}]}),
        json!({"settings":{"a":true},"groups":[{"label":"G","settings":["a"]},{"label":"H","settings":["a"]}]}),
    ] {
        let mut e = extension();
        for (k, v) in patch.as_object().unwrap() {
            e[k] = v.clone();
        }
        cases.push((manifest(), e));
    }
    for (m, e) in &cases {
        assert!(parse(m, e).is_err(), "accepted invalid input {m} / {e}");
    }
    eprintln!(
        "{} synthetic schema/cross-field refusal scenarios",
        cases.len()
    );
    // Step is presentation, not runtime quantization; numeric controls do not impose invented type restrictions.
    let mut e = extension();
    e["settings"] = json!({"a":{"value":1.5,"step":1},"b":{"value":true,"control":"number"}});
    assert!(parse(&manifest(), &e).is_ok());
}
pub fn synthetic_schema_limits_and_closed_records_cover_non_editor_fields() {
    let invalid_settings = vec![
        json!({"bad key":true}),
        json!({"a":null}),
        json!({"a":{"value":true,"label":""}}),
        json!({"a":{"value":true,"label":"a".repeat(121)}}),
        json!({"a":{"value":true,"description":"a".repeat(501)}}),
        json!({"a":{"value":true,"control":"button"}}),
        json!({"a":{"value":true,"applyAt":"later"}}),
        json!({"a":{"value":true,"restartRequired":null}}),
        json!({"a":{"value":true,"assetPreparation":1}}),
        json!({"a":{"value":"a","minLength":1.5}}),
        json!({"a":{"value":"a","maxLength":-1}}),
        json!({"a":{"value":"a","options":{"a":""}}}),
        json!({"a":{"value":"a","options":{"a":{"label":"A","extra":true}}}}),
        json!({"a":{"value":"a","options":{"a":{"label":"A","group":"g".repeat(61)}}}}),
        json!({"a":{"value":true,"enabledWhen":{"setting":"b","equals":true,"extra":1}},"b":true}),
        json!({"a":{"value":"SDL:999","control":"keybind"}}),
        json!({"a":{"value":false,"control":"keybind"}}),
    ];
    let mut n = 0;
    for settings in invalid_settings {
        let mut e = extension();
        e["settings"] = settings;
        assert!(parse(&manifest(), &e).is_err(), "{e}");
        n += 1;
    }
    for patch in [
        json!({"groups":[{"label":"G","tab":"yes"}]}),
        json!({"groups":[{"label":"G","actions":[{"id":"a","label":"A","style":"other"}]}]}),
        json!({"groups":[{"label":"G","actions":[{"id":"a"}]}]}),
        json!({"groups":[{"label":"G","actions":[{"id":"bad id","label":"A"}]}]}),
        json!({"links":{"Source":"https://example.org"}}),
        json!({"links":{"website":"https://".to_owned()+&"a".repeat(2049)}}),
        json!({"changelog":[""]}),
        json!({"changelog":vec!["entry";51]}),
        json!({"inputReservations":{"extra":true}}),
        json!({"inputReservations":{"contexts":{"held":"H"},"keys":{"key":{"when":"held","extra":true}}}}),
        json!({"presets":{"mode":{"settings":["x"],"values":{"fast":{"x":1}},"extra":true}}}),
    ] {
        let mut e = extension();
        for (k, v) in patch.as_object().unwrap() {
            e[k] = v.clone();
        }
        assert!(parse(&manifest(), &e).is_err(), "{e}");
        n += 1;
    }
    eprintln!("{n} additional synthetic schema limit/closed-record scenarios");
    let mut e = extension();
    e["settings"] = json!({"a":{"value":[true,false,false],"options":{"true":"Yes","anything":"No"}},"b":{"value":1.0,"options":{"NaN":"Nothing","1":"One"}}});
    assert!(
        parse(&manifest(), &e).is_ok(),
        "duplicate list choices and non-true boolean option names are SF behavior"
    );
}
pub fn unknown_top_level_policy_is_explicit_but_nested_schema_stays_closed() {
    let mut m = manifest();
    m["foreign"] = json!({"keep":true});
    assert!(parse(&m, &extension()).is_ok());
    assert!(
        Package::parse(
            m.to_string().as_bytes(),
            None,
            ParseOptions {
                tolerate_unknown_fields: false,
                ..Default::default()
            }
        )
        .is_err()
    );
    let p = parse(&m, &extension()).unwrap();
    assert!(p.documents.manifest.value().get("foreign").is_some());
}
pub fn targets_capabilities_and_missing_extension_defaults() {
    let p = Package::parse(
        manifest().to_string().as_bytes(),
        None,
        ParseOptions::default(),
    )
    .unwrap();
    assert!(!p.manifest.enabled);
    assert_eq!(p.manifest.target, Target::ClientServer);
    assert_eq!(
        parse(&manifest(), &extension()).unwrap().manifest.target,
        Target::Client
    );
    let mut e = extension();
    e["targets"] = json!(["server", "client"]);
    assert_eq!(
        parse(&manifest(), &e).unwrap().manifest.target,
        Target::ClientServer
    );
    let mut m = manifest();
    m["capabilities"] = json!(["export"]);
    assert!(parse(&m, &e).unwrap().manifest.requires_pregame());
    m["capabilities"] = json!(["runtime", "export"]);
    assert!(!parse(&m, &e).unwrap().manifest.requires_pregame());
    m["capabilities"] = json!(["runtime-register-dll", "patch"]);
    assert!(parse(&m, &e).unwrap().manifest.requires_pregame());
}
pub fn typed_metadata_input_reservations_and_18_fields() {
    let mut m = manifest();
    m["license"] = json!("MIT");
    m["icon"] = json!("assets/icon.png");
    let mut e = extension();
    e["links"] = json!({"source-github":"https://example.org"});
    e["changelog"] = json!(["First"]);
    e["groups"] = json!([{"label":"Tab","tab":true,"settings":["choice"]}]);
    e["inputReservations"] = json!({"contexts":{"held":"Held"},"keys":{"key":{"when":"held","keyboard":true,"mouse":true}},"wheels":{"wheel":{"when":"held"}},"choices":{"key":{"keyboard":false}}});
    // Settings keep the document's order (SF preserve_order), not serde_json's.
    let p = parse_with_settings(
        &m,
        &e,
        r#"{"key":{"value":"SDL:66","control":"keybind","assetPreparation":true},"wheel":true,"choice":{"value":"b","options":{"b":{"label":"Bee","group":"letters"}}}}"#,
    )
    .unwrap();
    assert_eq!(p.manifest.license.as_deref(), Some("MIT"));
    assert_eq!(p.manifest.icon.as_deref(), Some("assets/icon.png"));
    assert!(p.manifest.groups[0].tab);
    let s = &p.manifest.settings[0];
    assert!(s.asset_preparation);
    assert!(!s.reserves_keyboard());
    assert!(s.reserves_mouse());
    assert_eq!(
        p.manifest.settings[2].options[0].group.as_deref(),
        Some("letters")
    );
    assert!(
        Package::parse(
            m.to_string().as_bytes(),
            Some(e.to_string().as_bytes()),
            ParseOptions {
                version: SourceVersion::Sf179,
                ..Default::default()
            }
        )
        .is_err()
    );
    for patch in [
        json!({"keys":{"missing":{"when":"held"}}}),
        json!({"keys":{"wheel":{"when":"held"}}}),
        json!({"keys":{"key":{"when":"missing"}}}),
        json!({"choices":{"wheel":{"keyboard":false}}}),
        json!({"wheels":{"key":{"when":"held"}}}),
        json!({"contexts":{"held":" "}}),
    ] {
        let mut bad = e.clone();
        bad["inputReservations"] = patch;
        assert!(parse(&m, &bad).is_err(), "{bad}");
    }
}
pub fn presets_validate_without_expanding_backend_saves() {
    let mut e = extension();
    e["settings"] = json!({"mode":{"value":"custom","options":{"custom":"Custom","fast":"Fast"}},"speed":{"value":1,"min":0,"max":10}});
    e["presets"] = json!({"mode":{"settings":["speed"],"values":{"fast":{"speed":5}}}});
    let p = parse(&manifest(), &e).unwrap();
    assert_eq!(p.manifest.presets["mode"].custom, "custom");
    assert_eq!(p.manifest.setting_values["speed"], 1);
    for preset in [
        json!({"settings":["mode"],"values":{"fast":{"mode":"fast"}}}),
        json!({"settings":["missing"],"values":{"fast":{"missing":1}}}),
        json!({"settings":["speed"],"values":{"fast":{"speed":11}}}),
        json!({"settings":["speed"],"values":{"fast":{}}}),
        json!({"settings":["speed"],"values":{"custom":{"speed":2}}}),
    ] {
        let mut bad = e.clone();
        bad["presets"]["mode"] = preset;
        assert!(parse(&manifest(), &bad).is_err(), "{bad}");
    }
}
pub fn strict_parser_never_uses_editor_fraction_recovery() {
    let m = manifest().to_string();
    let e=br#"{"schemaVersion":1,"enabled":true,"settings":{"x":{"value":2,"options":{"2.0":"Whole","1.5":"Half"}}}}"#;
    assert!(Package::parse(m.as_bytes(), Some(e), ParseOptions::default()).is_err());
    assert_eq!(edit::read(m.as_bytes(), Some(e)).unwrap().repair, ["x"]);
    let repaired = edit::apply(m.as_bytes(), Some(e), edit::Edit::Enabled(true)).unwrap();
    assert!(Package::parse(m.as_bytes(), Some(&repaired), ParseOptions::default()).is_ok());
    let mut unrelated: Value = serde_json::from_slice(e).unwrap();
    unrelated["settings"]["x"]["unsupported"] = json!(true);
    let bad = unrelated.to_string();
    assert!(
        edit::read(m.as_bytes(), Some(bad.as_bytes())).is_err(),
        "recovery must not excuse unrelated SF schema failures"
    );
    assert!(
        edit::apply(
            m.as_bytes(),
            Some(bad.as_bytes()),
            edit::Edit::Enabled(true)
        )
        .is_err()
    );
}
pub fn locales_validate_references_case_and_resolve_presentation() {
    let mut e = extension();
    e["settings"] = json!({"x":{"value":"b","options":{"b":{"label":"Bee","group":"letters"}}}});
    e["groups"] = json!([{"label":"Group","actions":[{"id":"go","label":"Go"}]}]);
    e["changelog"] = json!(["First"]);
    let mut p = parse(&manifest(), &e).unwrap();
    let cat=json!({"mod":{"name":"Localized"},"settings":{"x":{"label":"Ex","options":{"b":"B"},"optionGroups":{"letters":"Letters"}}},"groups":[{"actions":{"go":{"label":"Run"}}}],"changelog":["New"],"strings":{"greet":"Hello"}}).to_string();
    p.set_locales([
        ("en-GB.json", cat.as_bytes()),
        ("en.json", br#"{"strings":{"greet":"Base"}}"#.as_slice()),
    ])
    .unwrap();
    let localized = locales::localized_manifest(&p.manifest, "en");
    assert_eq!(localized.name, "Localized");
    assert_eq!(
        localized.settings[0].options[0].group.as_deref(),
        Some("Letters")
    );
    assert_eq!(localized.groups[0].actions[0].label, "Run");
    assert_eq!(
        locales::localized_string(&p.manifest.locales, "greet", "en", None),
        "Hello"
    );
    assert_eq!(
        locales::localized_string(&p.manifest.locales, "greet", "de", None),
        "greet"
    );
    assert!(
        p.set_locales([("en.json", b"{}".as_slice()), ("EN.json", b"{}".as_slice())])
            .is_err()
    );
    for (file, cat) in [
        ("-en.json", json!({})),
        ("de.json", json!({"unknown":true})),
        ("de.json", json!({"settings":{"missing":{}}})),
        (
            "de.json",
            json!({"settings":{"x":{"options":{"missing":"Bad"}}}}),
        ),
        ("de.json", json!({"groups":[{},{}]})),
        ("de.json", json!({"changelog":["a","b"]})),
    ] {
        assert!(
            p.set_locales([(file, cat.to_string().as_bytes())]).is_err(),
            "{file}: {cat}"
        );
    }
    // Failed replacement is atomic, rather than losing the previous validated snapshot.
    assert_eq!(
        locales::localized_string(&p.manifest.locales, "greet", "en", None),
        "Hello"
    );
}
pub fn schema_numeric_enum_does_not_round_neighboring_integers() {
    // SF validate_mod_settings.rs:27-37 uses JSON Schema numeric equality, not
    // a lossy comparison obtained by converting both operands to doubles.
    let mut e = extension();
    e["settings"] = json!({"x":{"value":[1.0,9007199254740993u64],"options":{"1":"One","9007199254740992":"Large"}}});
    assert!(parse(&manifest(), &e).is_err());
    e["settings"]["x"]["value"] = json!([1.0, 9007199254740992u64]);
    assert!(parse(&manifest(), &e).is_ok());
    e["settings"] = json!({"x":{"value":9007199254740993u64,"max":9007199254740992.0}});
    assert!(parse(&manifest(), &e).is_err());
    e["settings"] = json!({"x":{"value":-9007199254740993i64,"min":-9007199254740992.0}});
    assert!(parse(&manifest(), &e).is_err());
}
pub fn source_inference_and_ui_presets_are_explicit_projections() {
    use sfi_package::package::settings::{apply_setting_change, infer_api_contract};
    let mut e = extension();
    e["presets"] = json!({"mode":{"settings":["speed"],"values":{"fast":{"speed":5}}}});
    let mut p = parse_with_settings(
        &manifest(),
        &e,
        r#"{"mode":{"value":"custom","options":{"custom":"Custom","fast":"Fast"}},"speed":1,"explicit":{"value":true,"applyAt":"restart"}}"#,
    )
    .unwrap();
    infer_api_contract(
        &mut p.manifest,
        "-- game.assets.save_assets()\nreturn {on_update=update}",
    );
    assert_eq!(p.manifest.settings[1].apply_at, "live");
    assert!(p.manifest.settings[2].restart_required);
    assert_eq!(p.manifest.capabilities.len(), 1);
    infer_api_contract(
        &mut p.manifest,
        "game.assets.save_assets();return {on_update=update}",
    );
    assert_eq!(p.manifest.settings[1].apply_at, "restart");
    let next = apply_setting_change(
        &p.manifest.setting_values,
        &p.manifest.presets,
        "mode",
        json!("fast"),
    );
    assert_eq!(next["speed"], 5);
    assert_eq!(p.manifest.setting_values["speed"], 1);
    let same = apply_setting_change(&next, &p.manifest.presets, "speed", json!(5.0));
    assert_eq!(same["mode"], "fast");
    let custom = apply_setting_change(&next, &p.manifest.presets, "speed", json!(4));
    assert_eq!(custom["mode"], "custom");
}
pub fn prospective_hotkey_validation_grandfathers_unchanged_conflicts_and_checks_activation() {
    use sfi_package::package::settings::validate_hotkey_configuration;
    let mut e = extension();
    e["settings"] = json!({"key":{"value":"SDL:66","control":"keybind"}});
    let a = parse(&manifest(), &e).unwrap().manifest;
    let mut m = manifest();
    m["id"] = json!("other");
    let b = parse(&m, &e).unwrap().manifest;
    let cfg = json!({"modules":{}});
    assert!(validate_hotkey_configuration(&[a.clone(), b.clone()], &cfg, &cfg, None).is_ok());
    let mut disabled = b.clone();
    disabled.enabled = false;
    assert!(
        validate_hotkey_configuration(
            &[a.clone(), disabled],
            &cfg,
            &cfg,
            Some(("other", &json!({}), Some(true)))
        )
        .is_err()
    );
    let protected = json!({"modules":{},"hotkeys":{"cancel":[{"scancode":66,"label":"Protected","emoji":"","text":""}]}});
    assert!(
        validate_hotkey_configuration(std::slice::from_ref(&a), &protected, &protected, None)
            .is_ok()
    );
    assert!(
        validate_hotkey_configuration(
            &[a],
            &protected,
            &protected,
            Some(("Example.ID", &json!({"key":"Ctrl+SDL:66"}), None))
        )
        .is_err()
    );
}
pub fn canonical_hotkeys_and_duplicate_conflicts() {
    use sfi_package::package::settings::{Hotkey, find_hotkey_conflicts};
    assert!(Hotkey::parse("Ctrl+SDL:224").is_err());
    assert!(Hotkey::parse("SDL:0").is_err());
    assert!(Hotkey::parse("Mouse:4|SDL:66").is_err());
    assert!(Hotkey::parse("SDL:66|SDL:67").is_err());
    let bindings = vec![
        ("a".into(), Hotkey::parse("Ctrl+SDL:66|Mouse:4").unwrap()),
        ("b".into(), Hotkey::parse("Mouse:4").unwrap()),
        ("c".into(), Hotkey::parse("SDL:66").unwrap()),
    ];
    let conflicts = find_hotkey_conflicts(&bindings);
    assert_eq!(conflicts.len(), 1);
    assert_eq!(conflicts[0].first_id, "a");
    assert_eq!(conflicts[0].second_id, "b");
}

// Conservative subcase proof only. Missing host, typed and callback cases and legacy
// v1 error-userdata goldens do not authorize complete/message parity.
