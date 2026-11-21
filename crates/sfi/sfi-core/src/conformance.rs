// SPDX-License-Identifier: MIT
//! Shared conformance host, not a production sandbox. Only run trusted fixtures/builders.
//!
//! Contract: `tools/sfi/conformance/prelude.lua:6-14`. Protected-call failures remain
//! Lua values, never Rust `mlua::Error` conversions that would stringify table errors.

use mlua::{Function, Lua, Table, Value as LuaValue};
use serde_json::Value;

/// The same prelude used by the oracle; do not maintain a second copy.
pub const PRELUDE: &str = include_str!("../../../../tools/sfi/conformance/prelude.lua");
/// The shared structural transcript contract.
pub const TRANSCRIPT_SCHEMA: &str =
    include_str!("../../../../tools/sfi/conformance/transcript.schema.json");

/// A fresh global table containing all standard libraries, with `_G` pointing to itself.
///
/// Libraries are shared within this VM. This builder is for harness self-tests, not mods;
/// later families supply their own SF sandbox builder instead.
pub fn plain_std_environment(lua: &Lua) -> mlua::Result<Table> {
    let env = lua.create_table()?;
    for entry in lua.globals().pairs::<LuaValue, LuaValue>() {
        let (key, value) = entry?;
        env.raw_set(key, value)?;
    }
    env.raw_set("_G", env.clone())?;
    Ok(env)
}

/// Run one trusted fixture in a fresh VM using a caller-supplied environment builder.
///
/// The prelude loads first, in a private trusted environment, as `=prelude`. The builder
/// supplies the fixture globals; this function installs `T` and loads `=fixture` there.
/// Compile/runtime failures become `fixture_error`, preserving the original Lua value.
/// Builder, prelude and transcript failures remain host errors (`Err`). The result is
/// the prelude's JSON string, unchanged (including its lack of a trailing newline).
pub fn run_fixture(
    fixture: &str,
    build_environment: impl FnOnce(&Lua) -> mlua::Result<Table>,
) -> mlua::Result<String> {
    // SAFETY: this is a trusted test host, deliberately exposing full std (including
    // debug and C module loading) to the prelude/plain builder, not a mod security boundary.
    let lua = unsafe { Lua::unsafe_new() };
    run_fixture_in_vm(&lua, fixture, build_environment)
}

/// Run trusted instrumentation on an existing production VM without restoring `load`,
/// `debug` or any other forbidden global. The builder must return a table in this VM.
/// Prelude globals never become the package's globals. Compilation errors use Lua's
/// original syntax message; evaluation is protected in Lua to preserve arbitrary errors.
pub fn run_fixture_in_vm(
    lua: &Lua,
    fixture: &str,
    build_environment: impl FnOnce(&Lua) -> mlua::Result<Table>,
) -> mlua::Result<String> {
    run_fixture_with_lua(lua, fixture, build_environment)
}

/// Test-host entry point for comparing this runner with the oracle on its existing VM.
/// The oracle supplies trusted standard libraries and a metatable inspector and uses
/// its own SF bindings, not Kiln implementations. Both entry points share one runner
/// so production-VM and oracle instrumentation cannot drift.
pub fn run_fixture_with_lua(
    lua: &Lua,
    fixture: &str,
    build_environment: impl FnOnce(&Lua) -> mlua::Result<Table>,
) -> mlua::Result<String> {
    let prelude_env = plain_std_environment(lua)?;
    if prelude_env
        .raw_get::<LuaValue>("__sfi_getmetatable")?
        .is_nil()
        && prelude_env.raw_get::<LuaValue>("debug")?.is_nil()
    {
        // Production VMs have no debug library. Keep the equivalent trusted inspector
        // only in the prelude's private globals, never the VM or package globals.
        let inspector = lua.create_function(|lua, value: LuaValue| {
            // SAFETY: exec_raw owns the stack. lua_getmetatable accepts every value,
            // invokes no user code and bypasses __metatable. Remove the input so only
            // the metatable (or nil) remains as the single return value.
            unsafe {
                lua.exec_raw::<Option<Table>>(value, |state| {
                    if mlua::ffi::lua_getmetatable(state, -1) == 0 {
                        mlua::ffi::lua_pushnil(state);
                    }
                    mlua::ffi::lua_remove(state, 1);
                })
            }
        })?;
        prelude_env.raw_set("__sfi_getmetatable", inspector)?;
    }
    let t: Table = lua
        .load(PRELUDE)
        .set_name("=prelude")
        .set_environment(prelude_env)
        .eval()?;
    let fail_fixture: Function = t.get("fail_fixture")?;
    let transcript: Function = t.get("transcript")?;
    // Capture before the builder can change globals. Calling Lua's pcall from Rust
    // preserves arbitrary error values without adding a Lua helper frame to tracebacks.
    let pcall: Function = lua.globals().get("pcall")?;
    let env = build_environment(lua)?;
    env.raw_set("T", t)?;
    match lua
        .load(fixture)
        .set_name("=fixture")
        .set_mode(mlua::ChunkMode::Text)
        .set_environment(env)
        .into_function()
    {
        Ok(function) => {
            let (ok, error): (bool, LuaValue) = pcall.call(function)?;
            if !ok {
                fail_fixture.call::<()>(error)?;
            }
        }
        Err(mlua::Error::SyntaxError { message, .. }) => fail_fixture.call::<()>(message)?,
        Err(error) => return Err(error),
    }
    transcript.call(())
}

/// Compare parsed JSON structurally (object key order and whitespace do not matter).
///
/// A mismatch names the first differing record's sequence/label and prints both full
/// records. Header, missing-record and fixture-level error differences are also reported.
/// Malformed JSON is an error naming the input side.
pub fn compare(a: &str, b: &str) -> Result<(), String> {
    let a: Value = serde_json::from_str(a).map_err(|err| format!("invalid JSON in a: {err}"))?;
    let b: Value = serde_json::from_str(b).map_err(|err| format!("invalid JSON in b: {err}"))?;
    if a == b {
        return Ok(());
    }
    if a.get("version") != b.get("version") {
        return Err(difference("version", a.get("version"), b.get("version")));
    }
    if let (Some(ar), Some(br)) = (
        a.get("records").and_then(Value::as_array),
        b.get("records").and_then(Value::as_array),
    ) {
        for index in 0..ar.len().max(br.len()) {
            let (left, right) = (ar.get(index), br.get(index));
            if left != right {
                let record = left.or(right).expect("one side has a record");
                let seq = record
                    .get("seq")
                    .map(Value::to_string)
                    .unwrap_or_else(|| "?".into());
                let label = record
                    .get("label")
                    .map(Value::to_string)
                    .unwrap_or_else(|| "?".into());
                return Err(difference(
                    &format!("record seq={seq} label={label}"),
                    left,
                    right,
                ));
            }
        }
    }
    if a.get("fixture_error") != b.get("fixture_error") {
        return Err(difference(
            "fixture_error",
            a.get("fixture_error"),
            b.get("fixture_error"),
        ));
    }
    Err(difference("transcript", Some(&a), Some(&b)))
}

/// Whether a golden comparison verified the v2 message contract.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GoldenMatch {
    MessageVerifiedV2,
    /// A v1 expectation cannot establish message parity, even if it has no errors.
    LegacyV1NotMessageVerified,
}

/// Compare a golden (first argument) with an actual run. v2-v2 is strictly identical.
/// For legacy v1 expectations only, project actual v2 to v1 by changing `version` and
/// removing only `message` from failed calls. Preserve every other field/value/arity.
/// Never downgrade a v2 expectation or auto-upgrade a golden with invented messages.
pub fn compare_golden(expected: &str, actual: &str) -> Result<GoldenMatch, String> {
    validate_transcript(expected).map_err(|err| format!("invalid golden: {err}"))?;
    validate_transcript(actual).map_err(|err| format!("invalid actual: {err}"))?;
    let expected_value: Value = serde_json::from_str(expected).expect("validated JSON");
    let mut actual_value: Value = serde_json::from_str(actual).expect("validated JSON");
    if expected_value["version"] == 1 {
        if actual_value["version"] == 2 {
            actual_value["version"] = Value::from(1);
            for record in actual_value["records"]
                .as_array_mut()
                .expect("validated records")
            {
                if record["kind"] == "call" && record["ok"] == false {
                    record
                        .as_object_mut()
                        .expect("validated record")
                        .remove("message");
                }
            }
        }
        compare(expected, &actual_value.to_string())?;
        Ok(GoldenMatch::LegacyV1NotMessageVerified)
    } else {
        compare(expected, actual)?;
        Ok(GoldenMatch::MessageVerifiedV2)
    }
}

fn difference(location: &str, a: Option<&Value>, b: Option<&Value>) -> String {
    let render = |value: Option<&Value>| {
        value
            .map(Value::to_string)
            .unwrap_or_else(|| "<missing>".into())
    };
    format!(
        "{location} differs:\n  a: {}\n  b: {}",
        render(a),
        render(b)
    )
}

/// Validate the structural subset used by the shared JSON schema, directly from that file.
///
/// This is not a general JSON-schema implementation or a semantic parity check: the schema
/// itself checks message placement but not other kind-specific fields, canonical numeric
/// text, contiguous sequence numbers, or successful `expect` records. Only its structural keywords
/// are supported; schema extensions may need validator changes. Errors include a JSON location.
pub fn validate_transcript(transcript: &str) -> Result<(), String> {
    let value: Value =
        serde_json::from_str(transcript).map_err(|err| format!("$: invalid JSON: {err}"))?;
    let schema: Value = serde_json::from_str(TRANSCRIPT_SCHEMA)
        .map_err(|err| format!("$: invalid shared schema: {err}"))?;
    validate(&value, &schema, &schema, "$")
}

fn validate(value: &Value, schema: &Value, root: &Value, path: &str) -> Result<(), String> {
    let error = |message: &str| format!("{path}: {message}");
    for keyword in schema
        .as_object()
        .ok_or_else(|| error("invalid schema object"))?
        .keys()
    {
        if !matches!(
            keyword.as_str(),
            "$schema"
                | "$id"
                | "title"
                | "description"
                | "$defs"
                | "$ref"
                | "type"
                | "required"
                | "additionalProperties"
                | "properties"
                | "items"
                | "enum"
                | "const"
                | "minimum"
                | "minItems"
                | "maxItems"
                | "oneOf"
                | "not"
        ) {
            return Err(error(&format!("unsupported schema keyword {keyword}")));
        }
    }
    if let Some(reference) = schema.get("$ref").and_then(Value::as_str) {
        let target = reference
            .strip_prefix('#')
            .and_then(|pointer| root.pointer(pointer))
            .ok_or_else(|| error("unresolved local schema reference"))?;
        validate(value, target, root, path)?;
    }
    if let Some(expected) = schema.get("const")
        && value != expected
    {
        return Err(error(&format!("expected constant {expected}")));
    }
    if let Some(choices) = schema.get("enum").and_then(Value::as_array)
        && !choices.contains(value)
    {
        return Err(error("value is not in schema enum"));
    }
    if let Some(kind) = schema.get("type").and_then(Value::as_str) {
        let matches = match kind {
            "object" => value.is_object(),
            "array" => value.is_array(),
            "string" => value.is_string(),
            "boolean" => value.is_boolean(),
            "integer" => {
                value.as_i64().is_some()
                    || value.as_u64().is_some()
                    || value.as_f64().is_some_and(|number| number.fract() == 0.0)
            }
            _ => return Err(error(&format!("unsupported schema type {kind}"))),
        };
        if !matches {
            return Err(error(&format!("expected {kind}")));
        }
    }
    if let Some(minimum) = schema.get("minimum").and_then(Value::as_f64)
        && value.as_f64().is_some_and(|number| number < minimum)
    {
        return Err(error(&format!("number is below {minimum}")));
    }
    if let Some(object) = value.as_object() {
        if let Some(required) = schema.get("required").and_then(Value::as_array) {
            for field in required {
                let name = field
                    .as_str()
                    .ok_or_else(|| error("invalid required field"))?;
                if !object.contains_key(name) {
                    return Err(error(&format!("missing required field {name}")));
                }
            }
        }
        let properties = schema.get("properties").and_then(Value::as_object);
        for (name, child) in object {
            if let Some(child_schema) = properties.and_then(|properties| properties.get(name)) {
                validate(child, child_schema, root, &format!("{path}.{name}"))?;
            } else if schema.get("additionalProperties") == Some(&Value::Bool(false)) {
                return Err(error(&format!("unexpected field {name}")));
            }
        }
    }
    if let Some(array) = value.as_array() {
        if schema
            .get("minItems")
            .and_then(Value::as_u64)
            .is_some_and(|min| (array.len() as u64) < min)
            || schema
                .get("maxItems")
                .and_then(Value::as_u64)
                .is_some_and(|max| (array.len() as u64) > max)
        {
            return Err(error("array length outside schema bounds"));
        }
        if let Some(items) = schema.get("items") {
            for (index, child) in array.iter().enumerate() {
                validate(child, items, root, &format!("{path}[{index}]"))?;
            }
        }
    }
    if let Some(negated) = schema.get("not")
        && validate(value, negated, root, path).is_ok()
    {
        return Err(error("value matches forbidden schema"));
    }
    if let Some(variants) = schema.get("oneOf").and_then(Value::as_array) {
        let matches = variants
            .iter()
            .filter(|variant| validate(value, variant, root, path).is_ok())
            .count();
        if matches != 1 {
            return Err(error("value must match exactly one schema variant"));
        }
    }
    Ok(())
}
