// SPDX-License-Identifier: MIT
use mlua::{Lua, Table};
use serde_json::{Value, json};
use sfi_core::conformance::{compare, plain_std_environment, run_fixture, validate_transcript};

#[test]
fn builder_controls_fixture_but_prelude_keeps_standard_libraries() {
    let transcript = run_fixture(
        "T.value('no math', math); T.value('answer', answer)",
        |lua| {
            let env = lua.create_table()?;
            env.set("answer", 42)?;
            Ok(env)
        },
    )
    .unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(parsed["records"][0]["values"][0], json!({"t":"nil"}));
    assert_eq!(
        parsed["records"][1]["values"][0],
        json!({"t":"integer","v":"42"})
    );
    validate_transcript(&transcript).unwrap();
}

#[test]
fn prelude_and_fixture_chunk_names_follow_host_contract() {
    let transcript = run_fixture(
        "T.value('fixture name', debug.getinfo(1, 'S').source)\n\
         T.value('prelude name', debug.getinfo(T.call, 'S').source)",
        plain_std_environment,
    )
    .unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(parsed["records"][0]["values"][0]["v"], "\"=fixture\"");
    assert_eq!(parsed["records"][1]["values"][0]["v"], "\"=prelude\"");
}

#[test]
fn fixture_errors_keep_the_original_lua_value() {
    let transcript = run_fixture("error({reason = 7})", plain_std_environment).unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(
        parsed["fixture_error"],
        json!({
            "t":"table", "mt":false,
            "v":[[{"t":"string","v":"\"reason\""},{"t":"integer","v":"7"}]]
        })
    );
}

#[test]
fn syntax_errors_are_transcribed_instead_of_host_errors() {
    let transcript = run_fixture("local =", plain_std_environment).unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(parsed["fixture_error"]["t"], "string");
    assert!(
        parsed["fixture_error"]["v"]
            .as_str()
            .unwrap()
            .contains("fixture:1:")
    );
    assert_eq!(parsed["records"], json!([]));
    validate_transcript(&transcript).unwrap();
}

#[test]
fn environments_and_transcripts_do_not_leak_between_runs() {
    run_fixture("leak = 17; T.value('old', leak)", plain_std_environment).unwrap();
    let transcript = run_fixture("T.value('fresh', leak)", plain_std_environment).unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(parsed["records"].as_array().unwrap().len(), 1);
    assert_eq!(parsed["records"][0]["values"][0], json!({"t":"nil"}));
}

#[test]
fn builder_errors_remain_host_errors() {
    let result = run_fixture("", |_| Err(mlua::Error::runtime("builder failed")));
    assert!(result.unwrap_err().to_string().contains("builder failed"));
}

#[test]
fn plain_environment_has_full_std_and_its_own_global_table() {
    let transcript = run_fixture(
        "T.expect('global', _G == _ENV, true); T.value('debug', type(debug)); \
         T.value('io', type(io)); T.value('os', type(os)); \
         T.value('package', type(package)); T.value('coroutine', type(coroutine))",
        plain_std_environment,
    )
    .unwrap();
    let parsed: Value = serde_json::from_str(&transcript).unwrap();
    assert_eq!(parsed["records"][0]["pass"], true);
    for rec in &parsed["records"].as_array().unwrap()[1..] {
        assert_eq!(rec["values"][0]["v"], "\"table\"");
    }
}

#[test]
fn empty_fixture_has_an_empty_transcript_without_newline() {
    let transcript = run_fixture("", plain_std_environment).unwrap();
    assert_eq!(transcript, r#"{"version":2,"records":[]}"#);
}

#[test]
fn comparison_ignores_object_key_order_and_whitespace() {
    compare(
        r#"{"version":1,"records":[{"seq":1,"label":"x","kind":"note","text":"ok"}]}"#,
        r#"{ "records": [{"text":"ok","kind":"note","label":"x","seq":1}], "version":1 }"#,
    )
    .unwrap();
}

#[test]
fn comparison_reports_first_record_sequence_label_and_both_values() {
    let a = r#"{"version":1,"records":[{"seq":1,"label":"first","kind":"note","text":"a"},{"seq":2,"label":"later"}]}"#;
    let b = a.replace("\"a\"", "\"b\"").replace("later", "different");
    let difference = compare(a, &b).unwrap_err();
    for expected in ["seq=1", "first", "\"a\"", "\"b\""] {
        assert!(difference.contains(expected), "{difference}");
    }
    assert!(!difference.contains("different"), "{difference}");
}

#[test]
fn comparison_reports_missing_records_and_fixture_errors() {
    let a = r#"{"version":1,"records":[]}"#;
    let b = r#"{"version":1,"records":[{"seq":1,"label":"extra","kind":"note","text":"x"}]}"#;
    let difference = compare(a, b).unwrap_err();
    assert!(
        difference.contains("extra") && difference.contains("missing"),
        "{difference}"
    );
    let b = r#"{"version":1,"records":[],"fixture_error":{"t":"nil"}}"#;
    assert!(compare(a, b).unwrap_err().contains("fixture_error"));
}

#[test]
fn comparison_never_accepts_bad_json_or_unexamined_fields() {
    let valid = r#"{"version":1,"records":[]}"#;
    assert!(compare("not json", valid).unwrap_err().contains("a"));
    assert!(compare(valid, "not json").unwrap_err().contains("b"));
    assert!(compare(valid, &valid.replace("1", "2")).is_err());
    assert!(compare(valid, r#"{"version":1,"records":[],"extra":true}"#).is_err());
}

#[test]
fn validator_accepts_every_structural_shape_in_the_schema() {
    let transcript = json!({"version":1,"records":[
        {"seq":1,"label":"call","kind":"call","ok":false,"error":{"t":"nil"}},
        {"seq":2,"label":"expect","kind":"expect","pass":true,
         "values":[{"t":"table","mt":false,"v":[[{"t":"string","v":"x"},{"t":"cycle"}]]}],
         "expected":{"t":"boolean","v":true}},
        {"seq":3,"label":"note","kind":"note","text":"x"},
        {"seq":4,"label":"value","kind":"value","values":[
            {"t":"integer","v":"-9223372036854775808"},
            {"t":"float","v":"nan"},{"t":"function"},{"t":"thread"},
            {"t":"userdata","name":"?"}
        ]}
    ],"fixture_error":{"t":"string","v":"problem"}});
    validate_transcript(&transcript.to_string()).unwrap();
}

#[test]
fn validator_rejects_bad_shapes_with_a_location() {
    let invalid = [
        json!(null),
        json!({}),
        json!({"version":3,"records":[]}),
        json!({"version":1,"records":{}}),
        json!({"version":1,"records":[],"extra":true}),
        json!({"version":1,"records":[{"seq":0,"label":"x","kind":"note"}]}),
        json!({"version":1,"records":[{"seq":1.5,"label":"x","kind":"note"}]}),
        json!({"version":1,"records":[{"seq":1,"label":7,"kind":"note"}]}),
        json!({"version":1,"records":[{"seq":1,"label":"x","kind":"unknown"}]}),
        json!({"version":1,"records":[{"seq":1,"label":"x","kind":"call","ok":"true"}]}),
        json!({"version":1,"records":[{"seq":1,"label":"x","kind":"note","text":8}]}),
        json!({"version":1,"records":[],"fixture_error":{"t":"unknown"}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"table","mt":"yes"}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"integer","v":42}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"table","v":[[]]}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"table","v":[[{"t":"nil"},{"t":"nil"},{"t":"nil"}]]}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"table","v":[[{"t":"nil"},{}]]}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"userdata","name":7}}),
        json!({"version":1,"records":[],"fixture_error":{"t":"nil","extra":0}}),
    ];
    for transcript in invalid {
        let error = validate_transcript(&transcript.to_string()).unwrap_err();
        assert!(error.contains('$'), "missing location: {error}");
    }
    assert!(validate_transcript("not json").is_err());
}

// This signature is the family registry's extension point for later sandbox builders.
const _: fn(&Lua) -> mlua::Result<Table> = plain_std_environment;
