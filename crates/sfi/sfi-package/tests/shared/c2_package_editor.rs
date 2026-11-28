// SPDX-License-Identifier: MIT
//! Independent core adapter to the unmodified shared Kiln editor corpus.
#![allow(dead_code)]
use serde_json::{Value, json};
use sfi_package::package::{MAX_FILE_BYTES, edit};

fn corpus() -> Value {
    serde_json::from_str(include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../schema/fixtures/shroudforge/cases.json"
    )))
    .unwrap()
}
fn view(m: &edit::EditorPackage) -> Value {
    let settings: Vec<_> = m.settings.iter().map(|s| {
        let mut out = json!({"key": s.key, "type": edit::value_type(s), "label": edit::label_of(s),
            "control": edit::control_of(s), "applyAt": s.apply_at, "live": s.live});
        if let Some(w) = &s.enabled_when { out["enabledWhen"] = w.clone(); }
        if let Some(options) = s.options.as_ref().filter(|o| !o.is_empty()) {
            // a label is text or (ShroudForge 1.8) { label, group }: the view lists the text and the group when there is one
            out["options"] = json!(edit::option_keys(s).iter().map(|k| {
                let raw = &options[k.as_str()];
                let mut o = json!({"value": edit::option_value(s, k).unwrap(), "label": raw.get("label").unwrap_or(raw)});
                if let Some(g) = raw.get("group") { o["group"] = g.clone(); }
                o
            }).collect::<Vec<_>>());
        }
        out
    }).collect();
    let values = edit::settings_values(m);
    let now: Vec<_> = m
        .settings
        .iter()
        .filter(|s| edit::setting_enabled(&s.key, &m.settings, &values))
        .map(|s| &s.key)
        .collect();
    let mut out = json!({"enabled": m.enabled, "switchLive": m.switch_live, "settings": settings, "enabledNow": now});
    if !m.repair.is_empty() {
        out["repair"] = json!(m.repair);
    }
    out
}

pub fn all_shared_cases_match_views_decisions_and_exact_bytes() {
    let doc = corpus();
    let cases = doc["cases"].as_array().unwrap();
    assert_eq!(cases.len(), 96);
    for c in cases {
        let name = c["name"].as_str().unwrap();
        let manifest = c["modText"]
            .as_str()
            .map(str::to_owned)
            .unwrap_or_else(|| c["mod"].to_string());
        let mut bytes = if let Some(s) = c["extendedLatin1"].as_str() {
            Some(
                s.chars()
                    .map(|ch| u8::try_from(u32::from(ch)).unwrap())
                    .collect::<Vec<_>>(),
            )
        } else {
            c["extended"].as_str().map(|s| s.as_bytes().to_vec())
        };
        let refused = c["refused"] == true;
        let read = edit::read(manifest.as_bytes(), bytes.as_deref());
        let strict = sfi_package::package::Package::parse(
            manifest.as_bytes(),
            bytes.as_deref(),
            Default::default(),
        );
        if c["sfRefused"] == true {
            assert!(
                strict.is_err(),
                "{name}: recovery must not change strict acceptance"
            );
        } else if !refused {
            assert!(
                strict.is_ok(),
                "{name}: unexpected strict refusal {strict:?}"
            );
        } else {
            // "refused" means ShroudForge refuses it: the strict model (ShroudForge's schema and reader) says so too, so
            // every refusal the corpus asks of Kiln's readers is one ShroudForge makes
            assert!(
                strict.is_err(),
                "{name}: ShroudForge's rules accept a case the corpus says it refuses"
            );
        }
        if refused {
            let error = read.expect_err(name);
            if let Some(reason) = c["errorContains"].as_str() {
                assert!(
                    error.contains("ShroudForge would refuse") && error.contains(reason),
                    "{name}: {error}"
                );
            }
        } else {
            let m = read.unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(view(&m), c["read"], "{name}: read projection");
            if c["sfRefused"] == true {
                assert!(!m.repair.is_empty(), "{name}");
            }
        }
        let original = bytes.clone();
        for e in c["edits"].as_array().unwrap() {
            let request = if let Some(on) = e.get("enabled") {
                edit::Edit::Enabled(on.as_bool().unwrap())
            } else {
                edit::Edit::Setting(e["setting"].as_str().unwrap(), &e["value"])
            };
            let before = bytes.clone();
            let result = edit::apply(manifest.as_bytes(), bytes.as_deref(), request);
            assert_eq!(
                result.is_ok(),
                e["ok"].as_bool().unwrap(),
                "{name}: {e}: {result:?}"
            );
            if let Ok(proposal) = result {
                sfi_package::package::Package::parse(
                    manifest.as_bytes(),
                    Some(&proposal),
                    Default::default(),
                )
                .unwrap_or_else(|error| panic!("{name}: successful edit is invalid: {error}"));
                bytes = Some(proposal);
            } else {
                assert_eq!(bytes, before, "{name}: refusal changed input");
            }
        }
        if refused {
            assert_eq!(bytes, original, "{name}: refused input changed");
        } else {
            assert_eq!(
                bytes.as_deref(),
                Some(c["expect"].as_str().unwrap().as_bytes()),
                "{name}: exact output"
            );
            let m = edit::read(manifest.as_bytes(), bytes.as_deref()).unwrap();
            assert!(m.repair.is_empty(), "{name}: repair left");
            if let Some(after) = c.get("enabledAfter") {
                assert_eq!(&view(&m)["enabledNow"], after, "{name}");
            }
        }
    }
    eprintln!(
        "core editor corpus: 96/96 read views, accept/refuse decisions and byte-exact outputs"
    );
}

pub fn numeric_edits_refuse_distinct_large_options_and_out_of_range_values() {
    let manifest = br#"{"id":"exact","name":"Exact","version":"1.0.0","capabilities":["runtime"]}"#;
    for (setting, proposed) in [
        (
            json!({"value":9007199254740992i64,"options":{"9007199254740992":"Only"}}),
            json!(9007199254740993i64),
        ),
        (
            json!({"value":-9007199254740992i64,"options":{"-9007199254740992":"Only"}}),
            json!(-9007199254740993i64),
        ),
        (
            json!({"value":[9007199254740992i64],"options":{"9007199254740992":"Only"}}),
            json!([9007199254740993i64]),
        ),
        (
            json!({"value":9007199254740992i64,"max":9007199254740992.0}),
            json!(9007199254740993i64),
        ),
        (
            json!({"value":-9007199254740992i64,"min":-9007199254740992.0}),
            json!(-9007199254740993i64),
        ),
    ] {
        let extension =
            json!({"schemaVersion":1,"enabled":true,"settings":{"x":setting}}).to_string();
        sfi_package::package::Package::parse(
            manifest,
            Some(extension.as_bytes()),
            Default::default(),
        )
        .unwrap();
        let model = edit::read(manifest, Some(extension.as_bytes())).unwrap();
        assert!(
            edit::apply(
                manifest,
                Some(extension.as_bytes()),
                edit::Edit::Setting("x", &proposed)
            )
            .is_err(),
            "successful edit could invalidate package: {setting} -> {proposed}"
        );
        assert!(
            edit::check_value(&model.settings[0], &proposed).is_err(),
            "accepted lossy numeric comparison: {setting} -> {proposed}"
        );
    }
}

pub fn successful_numeric_edits_preserve_equivalence_and_strict_parseability() {
    let manifest = br#"{"id":"exact","name":"Exact","version":"1.0.0","capabilities":["runtime"]}"#;
    for (setting, proposed) in [
        (json!({"value":1,"options":{"1":"Only"}}), json!(1.0)),
        (json!({"value":1.0,"options":{"1":"Only"}}), json!(1)),
        (json!({"value":[1],"options":{"1":"Only"}}), json!([1.0])),
        (json!({"value":[1.0],"options":{"1":"Only"}}), json!([1])),
        (json!({"value":0,"min":-1,"max":1}), json!(-0.0)),
        (
            json!({"value":9007199254740992i64,"max":9007199254740992.0}),
            json!(9007199254740992i64),
        ),
    ] {
        let extension =
            json!({"schemaVersion":1,"enabled":true,"settings":{"x":setting}}).to_string();
        let proposal = edit::apply(
            manifest,
            Some(extension.as_bytes()),
            edit::Edit::Setting("x", &proposed),
        )
        .unwrap();
        sfi_package::package::Package::parse(manifest, Some(&proposal), Default::default())
            .unwrap();
    }
}

pub fn exact_2_mib_bound_and_refused_edit_do_not_mutate_input() {
    assert_eq!(corpus()["limits"]["fileBytes"], MAX_FILE_BYTES);
    let manifest = br#"{"id":"big","name":"Big","version":"1.0.0","capabilities":["runtime"]}"#;
    let mut body = br#"{"schemaVersion":1,"enabled":true,"settings":{"a":{"value":1}}}"#.to_vec();
    body.resize(MAX_FILE_BYTES, b' ');
    assert!(edit::read(manifest, Some(&body)).is_ok());
    let changed = edit::apply(manifest, Some(&body), edit::Edit::Enabled(false));
    // `false` adds one byte, and therefore must also be checked after splicing.
    assert!(changed.is_err());
    body.push(b' ');
    assert!(edit::read(manifest, Some(&body)).is_err());
    assert!(edit::apply(manifest, Some(&body), edit::Edit::Enabled(false)).is_err());
    assert_eq!(body.len(), MAX_FILE_BYTES + 1);
    // SF has no Kiln editor file-size bound; keep those acceptance policies separate.
    assert!(
        sfi_package::package::Package::parse(manifest, Some(&body), Default::default()).is_ok()
    );
}

// Conservative subcase proof only. Missing host, typed and callback cases and legacy
// v1 error-userdata goldens do not authorize complete/message parity.
