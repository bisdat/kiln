// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.values v1's result packets: the frozen JSON packets `value_result_copy` returns, written exactly as
//! schema/sfi/eml-values/ describes them ( RC-1..RC-12 over PAYLOAD.md).
//!
//! - Every JSON packet is one closed object with `schema_version: 1` and a `packet` discriminator, counted UTF-8 without NUL.
//! - A Lua scalar is the archive schema's tagged `value` restricted to Lua's scalars: `nil`, `bool`, `i64` (two's complement, 16 hex
//!   digits), `f64` (16 hex digits of the IEEE bits) and `string` (the Lua string's bytes in hex).
//! - A type is `{index, qualified_name}`: the registry index as `u64:` and the registry type's own qualified name.
//! - The error tree carries each level's complete Source Display text as `bytes:`; registry- or Rust-formatted names
//!   (`expected`, `field`, `type`, `path`) are plain JSON strings; Lua-derived texts (`found`, `key`, `name`, `guid`) are
//!   `bytes:`. An `internal` level is only ever the root.
//!
//! STRINGIFY, SOURCE_BYTES and export payloads are raw counted bytes with no envelope.
use super::{
    lifetime::{Adapter, CheckKind, Fault, Found, Pin, UserdataKind},
    node::{Body, Node, Val},
};
use crate::sf_startup::resource_values::TypeHandle;
use kiln_abi::services::kiln_eml_values::{
    KILN_VALUE_ARRAY_MAPPED, KILN_VALUE_ARRAY_OWNED, KILN_VALUE_STRUCT_MAPPED, KILN_VALUE_STRUCT_OWNED,
    KILN_VALUE_VARIANT_MAPPED, KILN_VALUE_VARIANT_OWNED,
};

/// The only packet version this provider writes.
pub const SCHEMA_VERSION: u32 = 1;

pub(crate) fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 15) as usize] as char);
    }
    out
}

fn i64s(value: i64) -> String {
    format!("\"i64:{:016x}\"", value as u64)
}

fn u64s(value: u64) -> String {
    format!("\"u64:{value:016x}\"")
}

fn f64s(value: f64) -> String {
    format!("\"f64:{:016x}\"", value.to_bits())
}

fn bytes(value: &[u8]) -> String {
    format!("\"bytes:{}\"", hex(value))
}

fn string(text: &str) -> String {
    serde_json::to_string(text).expect("a str serializes")
}

fn envelope(packet: &str, rest: &str) -> Vec<u8> {
    format!("{{\"schema_version\":{SCHEMA_VERSION},\"packet\":\"{packet}\",{rest}}}").into_bytes()
}

/// A Lua scalar (`lua_value`). A scalar node is its value; a typed node has no scalar form (callers never pass one).
pub(crate) fn lua_value(val: &Val) -> String {
    match val {
        Val::Nil => "{\"tag\":\"nil\"}".into(),
        Val::Bool(b) => format!("{{\"tag\":\"bool\",\"value\":{b}}}"),
        Val::Int(i) => format!("{{\"tag\":\"i64\",\"value\":{}}}", i64s(*i)),
        Val::Num(n) => format!("{{\"tag\":\"f64\",\"value\":{}}}", f64s(*n)),
        Val::Str(s) => format!("{{\"tag\":\"string\",\"value\":\"string:{}\"}}", hex(s)),
        Val::Node(node) => match node.scalar_val() {
            Some(val) => lua_value(val),
            None => "{\"tag\":\"item\"}".into(),
        },
    }
}

fn type_ref(r#type: &TypeHandle) -> String {
    format!(
        "{{\"index\":{},\"qualified_name\":{}}}",
        u64s(r#type.index.as_usize() as u64),
        string(&r#type.qualified_name)
    )
}

/// The `value_kind` word of a `KILN_VALUE_*` typed class.
pub(crate) fn value_kind_word(kind: u32) -> &'static str {
    match kind as i32 {
        KILN_VALUE_STRUCT_OWNED => "struct_owned",
        KILN_VALUE_STRUCT_MAPPED => "struct_mapped",
        KILN_VALUE_ARRAY_OWNED => "array_owned",
        KILN_VALUE_ARRAY_MAPPED => "array_mapped",
        KILN_VALUE_VARIANT_OWNED => "variant_owned",
        KILN_VALUE_VARIANT_MAPPED => "variant_mapped",
        _ => "struct_owned",
    }
}

/// VIEW of a node: its scalar, or its typed class, storage, declared type and (variants) selected type.
pub(crate) fn view(node: &Node) -> Vec<u8> {
    if let Some(val) = node.scalar_val() {
        return envelope("view", &format!("\"value\":{}", lua_value(val)));
    }
    let (class, storage) = match &node.body {
        Body::Struct { .. } => ("struct", "owned"),
        Body::MStruct { .. } => ("struct", "mapped"),
        Body::Array { .. } => ("array", "owned"),
        Body::MArray { .. } => ("array", "mapped"),
        Body::Variant { .. } => ("variant", "owned"),
        Body::MVariant { .. } => ("variant", "mapped"),
        Body::Scalar(_) => unreachable!("scalars returned above"),
    };
    let declared = node.classify().map(|(_, ty)| type_ref(ty)).unwrap_or_default();
    let selected = match node.variant_type() {
        Some(variant) => format!(",\"variant_type\":{}", type_ref(variant)),
        None => String::new(),
    };
    envelope(
        "view",
        &format!(
            "\"value\":{{\"tag\":\"typed\",\"class\":\"{class}\",\"storage\":\"{storage}\",\"type\":{declared}{selected}}}"
        ),
    )
}

/// VIEW of a scalar value.
pub(crate) fn view_scalar(val: &Val) -> Vec<u8> {
    envelope("view", &format!("\"value\":{}", lua_value(val)))
}

/// VIEW of an iterator lease.
pub(crate) fn view_iterator() -> Vec<u8> {
    envelope("view", "\"value\":{\"tag\":\"iterator\"}")
}

/// LEN: Source's `#`, always an integer.
pub(crate) fn length(len: i64) -> Vec<u8> {
    envelope("length", &format!("\"value\":{}", i64s(len)))
}

/// DIRTY: Source's recursive `is_dirty`.
pub(crate) fn dirty(dirty: bool) -> Vec<u8> {
    envelope("dirty", &format!("\"value\":{dirty}"))
}

/// ITER_NEXT: the key and the value; a typed value is `{"tag":"item"}` (the reply's item lease). The end is key nil,
/// value nil; a non-nil key with a nil value is not the end.
pub(crate) fn iter_next(key: &Val, value: &Val) -> Vec<u8> {
    envelope(
        "iter_next",
        &format!("\"key\":{},\"value\":{}", lua_value(key), lua_value(value)),
    )
}

/// ERROR: the error tree, and the host-error pins the error lease keeps (released once, on the worker, when the lease
/// closes).
pub(crate) fn error(fault: Fault) -> (Vec<u8>, Vec<Pin>) {
    let mut pins = Vec::new();
    let tree = cause(fault.lift_internal(), &mut pins);
    (envelope("error", &format!("\"error\":{tree}")), pins)
}

fn cause(fault: Fault, pins: &mut Vec<Pin>) -> String {
    let message = bytes(&fault.message());
    let level = |class: &str, rest: String| format!("{{\"class\":\"{class}\",\"message\":{message}{rest}}}");
    match fault {
        Fault::Runtime(_) => level("runtime", String::new()),
        Fault::OutOfBounds { index, start, end } => level(
            "position_out_of_bounds",
            format!(
                ",\"index\":{},\"start\":{},\"end\":{}",
                u64s(index),
                u64s(start),
                u64s(end)
            ),
        ),
        Fault::StaticArray => level("static_array", String::new()),
        Fault::InvalidProperty { key, r#type } => level(
            "invalid_property",
            format!(",\"key\":{},\"type\":{}", bytes(&key), string(&r#type)),
        ),
        Fault::Borrow { mutable } => level(
            "borrow",
            format!(",\"mode\":\"{}\"", if mutable { "mutable" } else { "shared" }),
        ),
        Fault::Validation { path, kind } => level("validation", check(&path, kind, pins)),
        Fault::Conversion { path, kind } => level("conversion", check(&path, kind, pins)),
        Fault::Lua(host) => {
            let rest = match host.pin {
                Some(pin) => {
                    let rest = format!(",\"host_error\":{}", u64s(pin.object()));
                    pins.push(pin);
                    rest
                }
                None => String::new(),
            };
            level("lua", rest)
        }
        Fault::Internal(_) => level("internal", String::new()),
    }
}

fn check(path: &str, kind: CheckKind, pins: &mut Vec<Pin>) -> String {
    let path = string(path);
    match kind {
        CheckKind::IncompatibleType { expected, found } => {
            let found = match found {
                Found::Text(text) => format!(",\"found\":{}", bytes(&text)),
                Found::Userdata {
                    kind,
                    adapter,
                    value_kind,
                } => {
                    let lua_kind = match kind {
                        UserdataKind::TypedValue => "typed_value",
                        UserdataKind::KnownUserdata => "known_userdata",
                        UserdataKind::OtherUserdata => "other_userdata",
                    };
                    let adapter = match adapter {
                        Adapter::None => "none",
                        Adapter::Guid => "guid",
                        Adapter::Content => "content",
                        Adapter::Resource => "resource",
                    };
                    let value_kind = match value_kind {
                        Some(kind) => format!(",\"value_kind\":\"{}\"", value_kind_word(kind)),
                        None => String::new(),
                    };
                    format!(",\"found_userdata\":{{\"lua_kind\":\"{lua_kind}\",\"adapter\":\"{adapter}\"{value_kind}}}")
                }
            };
            format!(
                ",\"path\":{path},\"kind\":\"incompatible_type\",\"expected\":{}{found}",
                string(&expected)
            )
        }
        CheckKind::MissingField { field, r#type } => format!(
            ",\"path\":{path},\"kind\":\"missing_field\",\"field\":{},\"type\":{}",
            string(&field),
            string(&r#type)
        ),
        CheckKind::InvalidType(name) => {
            format!(
                ",\"path\":{path},\"kind\":\"invalid_type\",\"name\":{}",
                bytes(name.as_bytes())
            )
        }
        CheckKind::InvalidGuid(guid) => {
            format!(
                ",\"path\":{path},\"kind\":\"invalid_guid\",\"guid\":{}",
                bytes(guid.as_bytes())
            )
        }
        CheckKind::Lua(inner) => format!(",\"path\":{path},\"kind\":\"lua\",\"cause\":{}", cause(*inner, pins)),
        CheckKind::Mapping(_) => format!(",\"path\":{path},\"kind\":\"mapping\""),
    }
}

#[cfg(test)]
mod tests {
    //! The serializer against the frozen fixtures: every fixture that needs no registry is written here
    //! from the value or Source error it describes and must equal the fixture, byte for byte when serde_json keeps key
    //! order (sfi-core's `preserve_order` reaches this graph); m6_value_graph checks the typed views and the item step
    //! over a real registry.
    use std::{
        cell::RefCell,
        collections::BTreeSet,
        rc::Rc,
        sync::{Arc, atomic::AtomicUsize},
    };

    use kiln_abi::services::kiln_eml_values::{KILN_VALUE_ARRAY_MAPPED, KILN_VALUE_STRUCT_OWNED};
    use serde_json::{Value as Json, json};

    use super::*;
    use crate::archive_values::{
        input::transport,
        lifetime::{Access, HostError, HostValue, LuaHost, Sink, Transport},
    };

    const PACKETS: &str = include_str!("../../../../../schema/sfi/eml-values/fixtures/packets.json");

    /// Retains every object (a host error's token), as the C table's reader would.
    struct Retains;
    impl LuaHost for Retains {
        fn access(&self, _: &Access<'_>) -> Result<HostValue, Transport> {
            unreachable!("no reader call")
        }
        fn copy(&self, _: u64) -> Result<Vec<u8>, Transport> {
            unreachable!("no reader call")
        }
        fn retain(&self, _: u64) -> Result<(), Transport> {
            Ok(())
        }
        fn release(&self, _: u64) {}
    }

    fn thrown(text: &str, object: Option<u64>) -> Fault {
        let sink = Rc::new(Sink::new(Arc::new(AtomicUsize::new(0))));
        let pin = object.map(|object| Pin::retain(&Retains, &sink, object).unwrap());
        Fault::Lua(HostError {
            pin,
            text: text.as_bytes().to_vec(),
        })
    }

    #[test]
    fn written_packets_equal_the_fixtures() {
        let all: Json = serde_json::from_str(PACKETS).unwrap();
        let ordered = serde_json::to_string(&json!({"b": 0, "a": 0})).unwrap() == r#"{"b":0,"a":0}"#;
        let checked = RefCell::new(BTreeSet::new());
        let check = |name: &str, bytes: Vec<u8>| {
            let fixture = &all[name];
            assert!(fixture.is_object(), "no fixture {name}");
            let written: Json = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(&written, fixture, "{name}");
            if ordered {
                assert_eq!(
                    String::from_utf8(bytes).unwrap(),
                    serde_json::to_string(fixture).unwrap(),
                    "{name}"
                );
            }
            checked.borrow_mut().insert(name.to_owned());
        };
        let view = |val: Val| view_scalar(&val);
        check("view_nil", view(Val::Nil));
        check("view_bool", view(Val::Bool(true)));
        check("view_i64_negative", view(Val::Int(-1)));
        check("view_i64_min", view(Val::Int(i64::MIN)));
        check("view_f64", view(Val::Num(0.5)));
        check("view_f64_negative_zero", view(Val::Num(-0.0)));
        check(
            "view_f64_nan_payload",
            view(Val::Num(f64::from_bits(0x7ff8_0000_0000_0001))),
        );
        check("view_string_bytes", view(Val::Str(Rc::from(&b"a\0b\xff"[..]))));
        check("view_string_empty", view(Val::Str(Rc::from(&b""[..]))));
        check("view_iterator", view_iterator());
        check("length", length(2));
        check("dirty", dirty(true));
        check("iter_next_scalar", iter_next(&Val::str("extra"), &Val::Int(6)));
        check("iter_next_key_nil", iter_next(&Val::Int(2), &Val::Nil));
        check("iter_next_end", iter_next(&Val::Nil, &Val::Nil));

        let fail = |fault: Fault| error(fault).0;
        let validation = |path: &str, kind| Fault::Validation {
            path: path.into(),
            kind,
        };
        let conversion = |path: &str, kind| Fault::Conversion {
            path: path.into(),
            kind,
        };
        let incompatible = |expected: &str, found| CheckKind::IncompatibleType {
            expected: expected.into(),
            found,
        };
        let text = |bytes: &[u8]| Found::Text(bytes.to_vec());
        let typed = |kind: i32| Found::Userdata {
            kind: UserdataKind::TypedValue,
            adapter: Adapter::None,
            value_kind: Some(kind as u32),
        };
        let lua = |fault: Fault| CheckKind::Lua(Box::new(fault));
        check(
            "error_runtime",
            fail(Fault::runtime("expected a valid field in struct")),
        );
        check(
            "error_position_out_of_bounds",
            fail(Fault::OutOfBounds {
                index: 4,
                start: 1,
                end: 3,
            }),
        );
        check("error_static_array", fail(Fault::StaticArray));
        check(
            "error_invalid_property",
            fail(Fault::InvalidProperty {
                key: b"n\0pe".to_vec(),
                r#type: "m6.Child".into(),
            }),
        );
        check("error_borrow_shared", fail(Fault::Borrow { mutable: false }));
        check("error_borrow_mutable", fail(Fault::Borrow { mutable: true }));
        check(
            "error_validation_incompatible",
            fail(validation(
                ".",
                incompatible("unsigned integer in range [0, 4294967295]", text(b"x")),
            )),
        );
        check(
            "error_validation_incompatible_non_utf8",
            fail(validation("child.base", incompatible("boolean", text(b"\xff\0")))),
        );
        check(
            "error_validation_found_userdata",
            fail(validation(
                ".",
                incompatible("m6.Children", typed(KILN_VALUE_STRUCT_OWNED)),
            )),
        );
        check(
            "error_validation_found_known_userdata",
            fail(validation(
                "ref",
                incompatible(
                    "object reference",
                    Found::Userdata {
                        kind: UserdataKind::KnownUserdata,
                        adapter: Adapter::Content,
                        value_kind: None,
                    },
                ),
            )),
        );
        check(
            "error_validation_missing_field",
            fail(validation(
                "child",
                CheckKind::MissingField {
                    field: "base".into(),
                    r#type: "m6.Child".into(),
                },
            )),
        );
        check(
            "error_validation_invalid_type",
            fail(validation("type", CheckKind::InvalidType("m6.Nope".into()))),
        );
        check(
            "error_validation_invalid_guid",
            fail(validation("ref", CheckKind::InvalidGuid("zz".into()))),
        );
        check(
            "error_validation_lua_host",
            fail(validation(
                "type",
                lua(thrown("runtime error: [string \"?\"]:1: no type", Some(0x2a))),
            )),
        );
        check(
            "error_validation_lua_borrow",
            fail(validation("value", lua(Fault::Borrow { mutable: false }))),
        );
        check(
            "error_validation_lua_runtime",
            fail(validation("guid", lua(Fault::external("guid expected, got boolean")))),
        );
        check(
            "error_validation_mapping",
            fail(validation(".", CheckKind::Mapping("unexpected end of data".into()))),
        );
        check(
            "error_conversion_incompatible",
            fail(conversion("0", incompatible("m6.Child", text(b"nil")))),
        );
        check(
            "error_conversion_found_userdata",
            fail(conversion(
                ".",
                incompatible("m6.Child", typed(KILN_VALUE_ARRAY_MAPPED)),
            )),
        );
        check(
            "error_conversion_invalid_guid",
            fail(conversion("guid", CheckKind::InvalidGuid("zz".into()))),
        );
        check(
            "error_conversion_lua_host",
            fail(conversion(
                "choice",
                lua(thrown("runtime error: tostring failed", Some(0x2a))),
            )),
        );
        check(
            "error_conversion_mapping",
            fail(conversion(
                "children",
                CheckKind::Mapping("unexpected end of data".into()),
            )),
        );
        check(
            "error_lua_root",
            fail(thrown("runtime error: [string \"?\"]:1: boom", Some(0x2a))),
        );
        check(
            "error_lua_unretained",
            fail(thrown("runtime error: [string \"?\"]:1: boom", None)),
        );
        check("error_internal", fail(transport("GET_FIELD_VALUE", Transport(7))));
        // a Kiln fault nested under a Source level is written at the root
        check(
            "error_internal",
            fail(validation("type", lua(transport("GET_FIELD_VALUE", Transport(7))))),
        );

        // every other fixture needs a registry: m6_value_graph writes those from real nodes
        let rest: BTreeSet<String> = all
            .as_object()
            .unwrap()
            .keys()
            .filter(|name| !checked.borrow().contains(*name))
            .cloned()
            .collect();
        let typed_only = [
            "iter_next_item",
            "view_array_mapped",
            "view_struct_owned",
            "view_variant_mapped",
        ];
        assert_eq!(rest, typed_only.iter().map(|s| s.to_string()).collect());
    }
}
