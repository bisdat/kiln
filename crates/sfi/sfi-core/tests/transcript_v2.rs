// SPDX-License-Identifier: MIT
use mlua::Error;
use serde_json::{Value, json};
use sfi_core::conformance::{compare, plain_std_environment, run_fixture, validate_transcript};

fn callback_error(message: &str) -> String {
    let message = message.to_owned();
    run_fixture("T.call('callback', fail)", |lua| {
        let env = plain_std_environment(lua)?;
        env.set(
            "fail",
            lua.create_function(move |_, ()| -> mlua::Result<()> {
                Err(Error::external(message.clone()))
            })?,
        )?;
        Ok(env)
    })
    .unwrap()
}

fn production_vm() -> sfi_core::lua::LuaVm {
    use sfi_core::host::{HostContext, Phase, Side, SourceVersion};
    sfi_core::lua::LuaVm::new(HostContext::new(
        Phase::Startup,
        Side::Server,
        SourceVersion::Sf180,
    ))
    .unwrap()
}

#[test]
fn merge_runner_adds_no_lua_host_frame_to_error_messages() {
    let parsed: Value = serde_json::from_str(&callback_error("failure")).unwrap();
    let message = parsed["records"][0]["message"].as_str().unwrap();
    assert!(message.contains("fixture:1:"), "{message}");
    assert!(!message.contains("conformance-host"), "{message}");
}

#[test]
fn merge_production_vm_diagnostics_do_not_restore_or_expose_host_globals() {
    use sfi_core::conformance::{run_fixture_in_vm, run_fixture_with_lua};
    for oracle_entry in [false, true] {
        let vm = production_vm();
        let lua = vm.lua();
        let env = vm
            .add_package(
                sfi_core::host::Package::new("fixture", std::rc::Rc::new(EmptySource)),
                |lua, env, _| {
                    env.set(
                        "fail",
                        lua.create_function(|_, ()| -> mlua::Result<()> {
                            Err(Error::external("production callback failure"))
                        })?,
                    )
                },
            )
            .unwrap();
        let fixture = "T.call('callback', fail); T.call('protected', function() error(setmetatable({}, {__metatable=false, __tostring=function() return 'protected text' end}), 0) end)";
        let builder = |_: &mlua::Lua| Ok(env.clone());
        let text = if oracle_entry {
            run_fixture_with_lua(lua, fixture, builder)
        } else {
            run_fixture_in_vm(lua, fixture, builder)
        }
        .unwrap();
        let parsed: Value = serde_json::from_str(&text).unwrap();
        assert_eq!(parsed["version"], 2);
        assert!(
            parsed["records"][0]["message"]
                .as_str()
                .unwrap()
                .contains("production callback failure")
        );
        assert_eq!(parsed["records"][1]["message"], "protected text");
        for name in ["load", "dofile", "loadfile", "debug", "__sfi_getmetatable"] {
            assert!(
                lua.globals().raw_get::<mlua::Value>(name).unwrap().is_nil(),
                "host {name}"
            );
            assert!(
                env.raw_get::<mlua::Value>(name).unwrap().is_nil(),
                "package {name}"
            );
        }
        validate_transcript(&text).unwrap();
    }
}

struct EmptySource;
impl sfi_core::host::PackageSource for EmptySource {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}

#[test]
fn merge_runner_captures_pcall_before_builder_changes_globals() {
    let text = run_fixture("error({tag=7}, 0)", |lua| {
        let env = plain_std_environment(lua)?;
        lua.globals().raw_remove("pcall")?;
        Ok(env)
    })
    .unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    assert_eq!(parsed["fixture_error"]["t"], "table");
}

#[test]
fn merge_production_vm_keeps_arbitrary_fixture_errors_and_original_syntax_message() {
    use sfi_core::conformance::run_fixture_in_vm;
    for (fixture, expected) in [("error(nil, 0)", "nil"), ("error({tag=7}, 0)", "table")] {
        let vm = production_vm();
        let text = run_fixture_in_vm(vm.lua(), fixture, plain_std_environment).unwrap();
        let parsed: Value = serde_json::from_str(&text).unwrap();
        assert_eq!(parsed["fixture_error"]["t"], expected);
    }
    let vm = production_vm();
    let fixture = "local =";
    let error = vm
        .lua()
        .load(fixture)
        .set_name("=fixture")
        .into_function()
        .unwrap_err();
    let Error::SyntaxError { message, .. } = error else {
        panic!("expected syntax error")
    };
    let text = run_fixture_in_vm(vm.lua(), fixture, plain_std_environment).unwrap();
    let quoted: String = vm
        .lua()
        .load("return string.format('%q', ...)")
        .call(message)
        .unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    assert_eq!(parsed["fixture_error"], json!({"t":"string", "v":quoted}));
}

#[test]
fn merge_runner_rejects_binary_chunks_in_text_mode() {
    // A binary chunk header is valid UTF-8 even though its compiled body usually is not.
    let vm = production_vm();
    let text =
        sfi_core::conformance::run_fixture_in_vm(vm.lua(), "\u{1b}Lua", plain_std_environment)
            .unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    assert!(
        parsed["fixture_error"]["v"]
            .as_str()
            .unwrap()
            .contains("binary chunk"),
        "{parsed}"
    );
}

#[test]
fn same_userdata_class_does_not_hide_different_callback_messages() {
    let a = callback_error("first failure");
    let b = callback_error("second failure");
    let av: Value = serde_json::from_str(&a).unwrap();
    let bv: Value = serde_json::from_str(&b).unwrap();
    assert_eq!(av["records"][0]["error"], bv["records"][0]["error"]);
    assert!(
        compare(&a, &b).is_err(),
        "different mlua messages compared equal"
    );
    assert!(
        av["records"][0]["message"]
            .as_str()
            .unwrap()
            .contains("first failure")
    );
    assert_eq!(av["version"], 2);
}

#[test]
fn address_mask_only_changes_pointer_shaped_tokens_in_messages() {
    for class in ["table", "userdata", "function", "thread", "Buffer", "Image"] {
        for template in [
            "bad argument #2 (number or integer expected, got VALUE)",
            "attempt to call method on invalid receiver (Buffer expected, got VALUE)",
            "32-bit unsigned integer expected, got VALUE",
        ] {
            let a = callback_error(&template.replace("VALUE", &format!("{class}: 0x1")));
            let b =
                callback_error(&template.replace("VALUE", &format!("{class}: 0XABCDEF0123456789")));
            compare(&a, &b).unwrap();
            let parsed: Value = serde_json::from_str(&a).unwrap();
            let message = parsed["records"][0]["message"].as_str().unwrap();
            assert!(message.contains(&template.replace("VALUE", &format!("{class}: <address>"))));
            assert_eq!(message.matches("<address>").count(), 1);
        }
    }
    for text in [
        "argument 0x1234",
        "number 0Xff",
        "table: 0x123456789abcdef01",
        "table: 0x1g",
        "not_table: 0x1",
        "bad argument #2 (number expected, got Buffer: 0x123456789abcdef01)",
        "bad argument #2 (number expected, got Image: 0x1g)",
        "bad argument #2 (number expected, got Buffer: 0x1) trailing input",
    ] {
        let parsed: Value = serde_json::from_str(&callback_error(text)).unwrap();
        assert!(
            parsed["records"][0]["message"]
                .as_str()
                .unwrap()
                .contains(text),
            "{parsed}"
        );
    }
    assert!(
        compare(
            &callback_error("argument 0x1"),
            &callback_error("argument 0x2")
        )
        .is_err()
    );
}

#[test]
fn named_userdata_addresses_compare_equal_without_hiding_other_text() {
    let message = "bad argument #2 (number or integer expected, got Buffer: 0x1)\nstack traceback:\n\tImage: 0XABCDEF0123456789; argument 0x17";
    let a = callback_error(message);
    let b = callback_error(
        "bad argument #2 (number or integer expected, got Buffer: 0Xfedcba9876543210)\nstack traceback:\n\tImage: 0XABCDEF0123456789; argument 0x17",
    );
    compare(&a, &b).unwrap();
    let av: Value = serde_json::from_str(&a).unwrap();
    let bv: Value = serde_json::from_str(&b).unwrap();
    assert_eq!(av["records"][0]["error"], bv["records"][0]["error"]);
    assert!(
        av["records"][0]["message"].as_str().unwrap().contains(
            "bad argument #2 (number or integer expected, got Buffer: <address>)\nstack traceback:\n\tImage: 0XABCDEF0123456789; argument 0x17"
        )
    );
    for (from, to) in [
        ("argument #2", "argument #1"),
        ("number or integer", "integer"),
        ("Buffer:", "Image:"),
        ("0x17", "0x18"),
    ] {
        assert!(compare(&a, &callback_error(&message.replace(from, to))).is_err());
    }
    for text in [
        "Buffer: 0x123456789abcdef01",
        "Image: 0x1g",
        "OtherBuffer: 0x1",
        "Bufferish: 0x1",
        "buffer: 0x1",
        "image: 0x1",
        "counter: 0x1",
        "Buffer 0x1",
        "0x123456789abcdef0",
    ] {
        let parsed: Value = serde_json::from_str(&callback_error(text)).unwrap();
        assert!(
            parsed["records"][0]["message"]
                .as_str()
                .unwrap()
                .contains(text),
            "{parsed}"
        );
    }
}

#[test]
fn literal_identifiers_and_interpolated_text_are_not_addresses() {
    for class in ["Buffer", "Image", "table", "userdata", "function", "thread"] {
        for template in [
            "mod not found: CLASS: HEX",
            "module not found: src/CLASS: HEX.lua",
            "failed to load module: src/CLASS: HEX.lua (not found)",
            "mod not found: bad argument #2 (number expected, got CLASS: HEX)",
            "mod not found: literal\nbad argument #2 (number expected, got CLASS: HEX)",
            "counter CLASS: HEX",
        ] {
            let first = template.replace("CLASS", class).replace("HEX", "0x1");
            let second = first.replace("0x1", "0x2");
            let a = callback_error(&first);
            let b = callback_error(&second);
            assert!(
                compare(&a, &b).is_err(),
                "literal errors compared equal: {first}"
            );
            let parsed: Value = serde_json::from_str(&a).unwrap();
            assert!(
                parsed["records"][0]["message"]
                    .as_str()
                    .unwrap()
                    .contains(&first)
            );
        }
    }
    // Only the formatter's value slot is a pointer, never text in a traceback/chunk name.
    let diagnostic = "bad argument #2 (number expected, got Buffer: 0x1)\nstack traceback:\n\tImage: 0x2: in main chunk";
    let parsed: Value = serde_json::from_str(&callback_error(diagnostic)).unwrap();
    assert!(parsed["records"][0]["message"].as_str().unwrap().contains(
        "bad argument #2 (number expected, got Buffer: <address>)\nstack traceback:\n\tImage: 0x2: in main chunk"
    ));
}

#[test]
fn literal_lua_errors_and_custom_tostring_are_preserved_even_in_diagnostic_shape() {
    struct Literal;
    impl mlua::UserData for Literal {
        fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
            methods.add_meta_method(mlua::MetaMethod::ToString, |_, _, ()| {
                Ok("bad argument #2 (number expected, got Buffer: 0x1)")
            });
        }
    }
    let text = run_fixture(
        r#"
local message = 'bad argument #2 (number expected, got Buffer: 0x1)'
T.call('string', function() error(message, 0) end)
T.call('custom', function() error(setmetatable({}, {__tostring=function() return message end}), 0) end)
T.call('userdata', function() error(literal_userdata, 0) end)
"#,
        |lua| {
            let env = plain_std_environment(lua)?;
            env.set("literal_userdata", Literal)?;
            Ok(env)
        },
    ).unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    for record in parsed["records"].as_array().unwrap() {
        assert_eq!(
            record["message"],
            "bad argument #2 (number expected, got Buffer: 0x1)"
        );
    }
}

#[test]
fn canonical_errors_and_protected_call_returns_survive_diagnostics() {
    let text = run_fixture(r#"
local plain = {tag=7}
local protected = setmetatable({tag=8}, {__metatable='hidden', __tostring=function() return 'safe text\n  0x1234' end})
local broken = setmetatable({tag=9}, {__metatable=false, __tostring=function() error('diagnostic broke', 0) end})
local invalid = setmetatable({}, {__tostring=function() return {} end})
local values = {plain, protected, broken, invalid}
T.call('string', function() error('literal\0\255\n', 0) end)
T.call('nil', function() error(nil, 0) end)
for i, value in ipairs(values) do
  local ok, err = T.call('table' .. i, function() error(value, 0) end)
  T.expect('identity' .. i, not ok and rawequal(err, value), true)
end
T.call('number', function() error(17, 0) end)
T.call('no values', function() end)
T.call('trailing nil', function() return nil, false, 1, nil end)
"#, plain_std_environment).unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    let records = parsed["records"].as_array().unwrap();
    assert_eq!(
        records[0]["error"],
        json!({"t":"string", "v":"\"literal\\0ÿ\\\n\""})
    );
    assert_eq!(records[0]["message"], "literal\0ÿ\n");
    assert_eq!(records[1]["error"], json!({"t":"nil"}));
    assert!(records[1].get("message").is_none());
    assert_eq!(records[2]["error"]["t"], "table");
    assert!(records[2].get("message").is_none());
    assert_eq!(records[4]["message"], "safe text\n  0x1234");
    for index in [6, 8, 10] {
        assert!(
            records[index].get("message").is_none(),
            "{}",
            records[index]
        );
    }
    for index in [3, 5, 7, 9] {
        assert_eq!(records[index]["pass"], true);
    }
    assert_eq!(records[11]["values"], json!([]));
    assert_eq!(
        records[12]["values"],
        json!([{"t":"nil"},{"t":"boolean","v":false},{"t":"integer","v":"1"},{"t":"nil"}])
    );
    assert!(records[11].get("message").is_none());
    validate_transcript(&text).unwrap();
}

#[test]
fn protected_userdata_tostring_success_failure_and_absence() {
    struct Diagnostic(bool);
    impl mlua::UserData for Diagnostic {
        fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
            methods.add_meta_method(mlua::MetaMethod::ToString, |_, this, ()| {
                if this.0 {
                    Ok("protected userdata text")
                } else {
                    Err(Error::runtime("diagnostic failed"))
                }
            });
        }
    }
    struct Plain;
    impl mlua::UserData for Plain {}
    let text = run_fixture(
        r#"
T.value('protected', getmetatable(good))
local ok, original = T.call('good', function() error(good,0) end)
T.expect('same original', not ok and rawequal(original,good), true)
T.call('broken', function() error(broken,0) end)
T.call('plain', function() error(plain,0) end)
"#,
        |lua| {
            let env = plain_std_environment(lua)?;
            env.set("good", Diagnostic(true))?;
            env.set("broken", Diagnostic(false))?;
            env.set("plain", Plain)?;
            Ok(env)
        },
    )
    .unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    assert_eq!(
        parsed["records"][0]["values"][0],
        json!({"t":"boolean","v":false})
    );
    assert_eq!(parsed["records"][1]["message"], "protected userdata text");
    assert_eq!(parsed["records"][2]["pass"], true);
    for index in [3, 4] {
        assert_eq!(parsed["records"][index]["error"]["t"], "userdata");
        assert!(parsed["records"][index].get("message").is_none());
    }
}

#[test]
fn diagnostic_introspection_failure_does_not_replace_the_original_error() {
    // Inject a failing trusted-host inspector before loading the prelude.
    let lua = unsafe { mlua::Lua::unsafe_new() };
    lua.load("debug.getmetatable = function() error('inspection failed', 0) end")
        .exec()
        .unwrap();
    let t: mlua::Table = lua.load(sfi_core::conformance::PRELUDE).eval().unwrap();
    lua.globals().set("T", t.clone()).unwrap();
    lua.load("local e={}; local ok,err=T.call('original',function() error(e,0) end); T.expect('identity',not ok and rawequal(e,err),true)").exec().unwrap();
    let text: String = t
        .get::<mlua::Function>("transcript")
        .unwrap()
        .call(())
        .unwrap();
    let parsed: Value = serde_json::from_str(&text).unwrap();
    assert!(parsed["records"][0].get("message").is_none());
    assert_eq!(parsed["records"][0]["error"]["t"], "table");
    assert_eq!(parsed["records"][1]["pass"], true);
}

#[test]
fn legacy_projection_is_directional_and_reports_not_message_verified() {
    use sfi_core::conformance::{GoldenMatch, compare_golden};
    let legacy = r#"{"version":1,"records":[{"seq":1,"label":"x","kind":"call","ok":false,"error":{"t":"userdata","name":"?"}}]}"#;
    let actual = r#"{"version":2,"records":[{"seq":1,"label":"x","kind":"call","ok":false,"error":{"t":"userdata","name":"?"},"message":"first"}]}"#;
    assert_eq!(
        compare_golden(legacy, actual).unwrap(),
        GoldenMatch::LegacyV1NotMessageVerified
    );
    assert_eq!(
        compare_golden(legacy, legacy).unwrap(),
        GoldenMatch::LegacyV1NotMessageVerified
    );
    assert_eq!(
        compare_golden(actual, actual).unwrap(),
        GoldenMatch::MessageVerifiedV2
    );
    assert!(compare(legacy, actual).is_err());
    assert!(compare_golden(actual, legacy).is_err());
    assert!(compare_golden(actual, &actual.replace("first", "second")).is_err());
    for (from, to) in [
        ("userdata", "nil"),
        ("\"label\":\"x\"", "\"label\":\"y\""),
        ("\"seq\":1", "\"seq\":2"),
    ] {
        assert!(compare_golden(legacy, &actual.replace(from, to)).is_err());
    }
    assert!(
        compare_golden(
            legacy,
            &actual.replace("\"records\":", "\"extra\":0,\"records\":")
        )
        .is_err()
    );
}

#[test]
fn shared_schema_accepts_both_versions_and_only_v2_failed_call_messages() {
    for version in [1, 2] {
        validate_transcript(&json!({"version":version,"records":[]}).to_string()).unwrap();
    }
    let failed = json!({"seq":1,"label":"failure","kind":"call","ok":false,"error":{"t":"nil"},"message":"text"});
    validate_transcript(&json!({"version":2,"records":[failed.clone()]}).to_string()).unwrap();
    let mut invalid = vec![json!({"version":1,"records":[failed.clone()]})];
    for version in [json!(0), json!(3), json!("2"), json!(null), json!(1.5)] {
        invalid.push(json!({"version":version,"records":[]}));
    }
    for message in [json!(null), json!(true), json!(7), json!({}), json!([])] {
        let mut record = failed.clone();
        record["message"] = message;
        invalid.push(json!({"version":2,"records":[record]}));
    }
    for kind in ["call", "note", "value", "expect"] {
        let mut record = failed.clone();
        record["kind"] = json!(kind);
        record["ok"] = json!(true);
        invalid.push(json!({"version":2,"records":[record]}));
    }
    let mut missing_ok = failed.clone();
    missing_ok.as_object_mut().unwrap().remove("ok");
    invalid.push(json!({"version":2,"records":[missing_ok]}));
    invalid.push(json!({"version":2,"records":[],"message":"bad"}));
    invalid.push(json!({"version":2,"records":[],"fixture_error":{"t":"nil","message":"bad"}}));
    invalid.push(json!({"version":2,"records":[{"seq":1,"label":"x","kind":"value","values":[{"t":"nil","message":"bad"}]}]}));
    for text in invalid {
        assert!(
            validate_transcript(&text.to_string()).is_err(),
            "accepted {text}"
        );
    }
}
