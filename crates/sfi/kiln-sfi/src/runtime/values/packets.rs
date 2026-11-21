// SPDX-License-Identifier: MIT
//! The packets `value_result_copy` returns, read as the frozen kiln.eml.values v1 schemas say
//! (`schema/sfi/eml-values/`, schema_version 1; `docs/kiln/services/eml-values.md` "Payloads and errors").
//!
//! Only neutral data crosses: tagged scalars (the provider's `i64:`/`f64:` 16-digit big-endian hex of the two's-complement
//! or IEEE bits, `string:`/`bytes:` hex of the exact bytes), type names, error texts and opaque tokens. A packet of
//! another schema version, an unknown discriminator, tag, class or key, or a malformed field is refused as a whole
//! ([`PacketError`]): the frontend never guesses at a packet it does not know (every object is closed in v1).
//! Raw packets (STRINGIFY, SOURCE_BYTES, value_export) are bytes with no envelope and are not parsed here.

use serde_json::{Map, Value as Json};

/// The schema version this reader knows.
pub const SCHEMA_VERSION: u64 = 1;

/// A refused packet: what was wrong with it (a provider fault, never a Source error).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PacketError(pub String);

impl std::fmt::Display for PacketError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "unreadable kiln.eml.values packet: {}", self.0)
    }
}
impl std::error::Error for PacketError {}

fn bad(why: impl Into<String>) -> PacketError {
    PacketError(why.into())
}

/// A Lua scalar (`lua_value`).
#[derive(Clone, Debug, PartialEq)]
pub enum Scalar {
    Nil,
    Bool(bool),
    Int(i64),
    /// The exact IEEE bits (NaN payloads and -0 kept).
    Num(u64),
    Str(Vec<u8>),
}

/// A registry type: its index and its own qualified name.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TypeRef {
    pub index: u64,
    pub qualified_name: String,
}

/// A `view` packet's value.
#[derive(Clone, Debug, PartialEq)]
pub enum View {
    Scalar(Scalar),
    Typed {
        class: String,
        storage: String,
        r#type: TypeRef,
        variant_type: Option<TypeRef>,
    },
    Iterator,
}

/// An `iter_next` value: a scalar inline, or the reply's item (a typed value).
#[derive(Clone, Debug, PartialEq)]
pub enum IterValue {
    Scalar(Scalar),
    Item,
}

/// `found_userdata`: the class of the userdata Source labels by its Rust TypeId.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FoundUserdata {
    pub lua_kind: String,
    pub adapter: String,
    pub value_kind: Option<String>,
}

/// One level of the error tree: the complete Source text of that level plus its class fields.
#[derive(Clone, Debug, PartialEq)]
pub struct ErrorLevel {
    pub class: String,
    pub message: Vec<u8>,
    /// `borrow`: `shared` or `mutable`.
    pub mode: Option<String>,
    /// `lua`: the retained host error object, when GPL could retain it.
    pub host_error: Option<u64>,
    /// `validation` / `conversion`: the kind and, for `incompatible_type`, a userdata found.
    pub kind: Option<String>,
    pub found_userdata: Option<FoundUserdata>,
    /// `validation` of kind `lua`: the next level.
    pub cause: Option<Box<ErrorLevel>>,
}

/// A JSON packet.
#[derive(Clone, Debug, PartialEq)]
pub enum Packet {
    View(View),
    Length(i64),
    Dirty(bool),
    IterNext { key: Scalar, value: IterValue },
    Error(ErrorLevel),
}

/// Parses one JSON packet.
pub fn parse(bytes: &[u8]) -> Result<Packet, PacketError> {
    let json: Json = serde_json::from_slice(bytes).map_err(|e| bad(format!("not JSON ({e})")))?;
    let object = json.as_object().ok_or_else(|| bad("not an object"))?;
    match object.get("schema_version").and_then(Json::as_u64) {
        Some(SCHEMA_VERSION) => {}
        Some(other) => return Err(bad(format!("schema_version {other}"))),
        None => return Err(bad("no schema_version")),
    }
    let packet = object
        .get("packet")
        .and_then(Json::as_str)
        .ok_or_else(|| bad("no packet discriminator"))?;
    let fields: &[&str] = match packet {
        "view" | "length" | "dirty" => &["value"],
        "iter_next" => &["key", "value"],
        "error" => &["error"],
        other => return Err(bad(format!("unknown packet {other:?}"))),
    };
    closed(object, &[&["schema_version", "packet"], fields].concat())?;
    let field = |name: &str| {
        object
            .get(name)
            .ok_or_else(|| bad(format!("{packet}: no {name}")))
    };
    Ok(match packet {
        "view" => Packet::View(view(field("value")?)?),
        "length" => Packet::Length(i64_text(field("value")?)?),
        "dirty" => Packet::Dirty(
            field("value")?
                .as_bool()
                .ok_or_else(|| bad("dirty: not a boolean"))?,
        ),
        "iter_next" => Packet::IterNext {
            key: scalar(field("key")?)?,
            value: match tag(field("value")?)? {
                "item" => {
                    closed(field("value")?.as_object().unwrap_or(&Map::new()), &["tag"])?;
                    IterValue::Item
                }
                _ => IterValue::Scalar(scalar(field("value")?)?),
            },
        },
        _ => Packet::Error(level(field("error")?, true)?),
    })
}

/// Every key of `object` is one of `allowed` (v1 objects are closed).
fn closed(object: &Map<String, Json>, allowed: &[&str]) -> Result<(), PacketError> {
    match object.keys().find(|key| !allowed.contains(&key.as_str())) {
        Some(key) => Err(bad(format!("unknown key {key:?}"))),
        None => Ok(()),
    }
}

fn tag(value: &Json) -> Result<&str, PacketError> {
    value
        .as_object()
        .and_then(|o| o.get("tag"))
        .and_then(Json::as_str)
        .ok_or_else(|| bad("a value without a tag"))
}

fn hex(text: &str) -> Result<Vec<u8>, PacketError> {
    if !text.len().is_multiple_of(2) {
        return Err(bad("odd hex length"));
    }
    (0..text.len() / 2)
        .map(|i| {
            let pair = &text[i * 2..i * 2 + 2];
            if pair
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            {
                u8::from_str_radix(pair, 16).map_err(|_| bad("bad hex"))
            } else {
                Err(bad("bad hex"))
            }
        })
        .collect()
}

/// `<prefix>:<hex>`: the bytes.
fn prefixed(value: &Json, prefix: &str) -> Result<Vec<u8>, PacketError> {
    let text = value
        .as_str()
        .ok_or_else(|| bad(format!("{prefix}: not a string")))?;
    let rest = text
        .strip_prefix(prefix)
        .and_then(|rest| rest.strip_prefix(':'))
        .ok_or_else(|| bad(format!("expected {prefix}: text")))?;
    hex(rest)
}

/// A `u64:`/`i64:`/`f64:` value: exactly 16 hex digits, big endian.
fn word(value: &Json, prefix: &str) -> Result<u64, PacketError> {
    let bytes = prefixed(value, prefix)?;
    let bytes: [u8; 8] = bytes
        .try_into()
        .map_err(|_| bad(format!("{prefix}: not 16 hex digits")))?;
    Ok(u64::from_be_bytes(bytes))
}

fn i64_text(value: &Json) -> Result<i64, PacketError> {
    word(value, "i64").map(|bits| bits as i64)
}

fn scalar(value: &Json) -> Result<Scalar, PacketError> {
    let object = value
        .as_object()
        .ok_or_else(|| bad("a value that is no object"))?;
    let tag = tag(value)?;
    let inner = || {
        object
            .get("value")
            .ok_or_else(|| bad(format!("{tag}: no value")))
    };
    let allowed: &[&str] = if tag == "nil" {
        &["tag"]
    } else {
        &["tag", "value"]
    };
    closed(object, allowed)?;
    Ok(match tag {
        "nil" => Scalar::Nil,
        "bool" => Scalar::Bool(
            inner()?
                .as_bool()
                .ok_or_else(|| bad("bool: not a boolean"))?,
        ),
        "i64" => Scalar::Int(i64_text(inner()?)?),
        "f64" => Scalar::Num(word(inner()?, "f64")?),
        "string" => Scalar::Str(prefixed(inner()?, "string")?),
        other => return Err(bad(format!("unknown scalar tag {other:?}"))),
    })
}

fn type_ref(value: &Json) -> Result<TypeRef, PacketError> {
    let object = value
        .as_object()
        .ok_or_else(|| bad("a type that is no object"))?;
    closed(object, &["index", "qualified_name"])?;
    Ok(TypeRef {
        index: word(
            object.get("index").ok_or_else(|| bad("type: no index"))?,
            "u64",
        )?,
        qualified_name: object
            .get("qualified_name")
            .and_then(Json::as_str)
            .ok_or_else(|| bad("type: no qualified_name"))?
            .to_owned(),
    })
}

fn view(value: &Json) -> Result<View, PacketError> {
    match tag(value)? {
        "typed" => {
            let object = value.as_object().unwrap_or_else(|| unreachable!());
            closed(object, &["tag", "class", "storage", "type", "variant_type"])?;
            let text = |name: &str| {
                object
                    .get(name)
                    .and_then(Json::as_str)
                    .map(str::to_owned)
                    .ok_or_else(|| bad(format!("typed: no {name}")))
            };
            let class = text("class")?;
            if !matches!(class.as_str(), "struct" | "array" | "variant") {
                return Err(bad(format!("unknown class {class:?}")));
            }
            let storage = text("storage")?;
            if !matches!(storage.as_str(), "owned" | "mapped") {
                return Err(bad(format!("unknown storage {storage:?}")));
            }
            let variant_type = object.get("variant_type").map(type_ref).transpose()?;
            if variant_type.is_some() != (class == "variant") {
                return Err(bad("variant_type belongs to a variant only"));
            }
            Ok(View::Typed {
                class,
                storage,
                r#type: type_ref(object.get("type").ok_or_else(|| bad("typed: no type"))?)?,
                variant_type,
            })
        }
        "iterator" => {
            closed(
                value.as_object().unwrap_or_else(|| unreachable!()),
                &["tag"],
            )?;
            Ok(View::Iterator)
        }
        _ => Ok(View::Scalar(scalar(value)?)),
    }
}

const CLASSES: &[&str] = &[
    "runtime",
    "position_out_of_bounds",
    "static_array",
    "invalid_property",
    "borrow",
    "validation",
    "conversion",
    "lua",
    "internal",
];
const KINDS: &[&str] = &[
    "incompatible_type",
    "missing_field",
    "invalid_type",
    "invalid_guid",
    "lua",
    "mapping",
];

fn level(value: &Json, root: bool) -> Result<ErrorLevel, PacketError> {
    let object = value
        .as_object()
        .ok_or_else(|| bad("an error level that is no object"))?;
    let class = object
        .get("class")
        .and_then(Json::as_str)
        .ok_or_else(|| bad("error: no class"))?;
    if !CLASSES.contains(&class) {
        return Err(bad(format!("unknown error class {class:?}")));
    }
    if class == "internal" && !root {
        return Err(bad("internal below the root"));
    }
    let message = prefixed(
        object
            .get("message")
            .ok_or_else(|| bad("error: no message"))?,
        "bytes",
    )?;
    let mut out = ErrorLevel {
        class: class.to_owned(),
        message,
        mode: None,
        host_error: None,
        kind: None,
        found_userdata: None,
        cause: None,
    };
    let mut allowed = vec!["class", "message"];
    match class {
        "position_out_of_bounds" => {
            allowed.extend(["index", "start", "end"]);
            for name in ["index", "start", "end"] {
                word(
                    object.get(name).ok_or_else(|| bad(format!("no {name}")))?,
                    "u64",
                )?;
            }
        }
        "invalid_property" => {
            allowed.extend(["key", "type"]);
            prefixed(object.get("key").ok_or_else(|| bad("no key"))?, "bytes")?;
        }
        "borrow" => {
            allowed.push("mode");
            let mode = object
                .get("mode")
                .and_then(Json::as_str)
                .ok_or_else(|| bad("borrow: no mode"))?;
            if !matches!(mode, "shared" | "mutable") {
                return Err(bad(format!("unknown borrow mode {mode:?}")));
            }
            out.mode = Some(mode.to_owned());
        }
        "lua" => {
            allowed.push("host_error");
            if let Some(token) = object.get("host_error") {
                let token = word(token, "u64")?;
                if token == 0 {
                    return Err(bad("host_error 0"));
                }
                out.host_error = Some(token);
            }
        }
        "validation" | "conversion" => {
            allowed.extend(["path", "kind"]);
            let kind = object
                .get("kind")
                .and_then(Json::as_str)
                .ok_or_else(|| bad("no kind"))?;
            if !KINDS.contains(&kind) {
                return Err(bad(format!("unknown kind {kind:?}")));
            }
            // conversion raises only these (convert_resource_values.rs:30-41)
            if class == "conversion"
                && !matches!(
                    kind,
                    "incompatible_type" | "invalid_guid" | "lua" | "mapping"
                )
            {
                return Err(bad(format!("conversion has no {kind}")));
            }
            match kind {
                "incompatible_type" => {
                    allowed.extend(["expected", "found", "found_userdata"]);
                    if object.get("expected").and_then(Json::as_str).is_none() {
                        return Err(bad("incompatible_type: no expected"));
                    }
                    match (object.get("found"), object.get("found_userdata")) {
                        (Some(found), None) => {
                            prefixed(found, "bytes")?;
                        }
                        (None, Some(found)) => {
                            let found = found
                                .as_object()
                                .ok_or_else(|| bad("found_userdata: no object"))?;
                            closed(found, &["lua_kind", "adapter", "value_kind"])?;
                            let text = |name: &str| {
                                found.get(name).and_then(Json::as_str).map(str::to_owned)
                            };
                            let descriptor = FoundUserdata {
                                lua_kind: text("lua_kind")
                                    .ok_or_else(|| bad("found_userdata: no lua_kind"))?,
                                adapter: text("adapter")
                                    .ok_or_else(|| bad("found_userdata: no adapter"))?,
                                value_kind: text("value_kind"),
                            };
                            if !matches!(
                                descriptor.lua_kind.as_str(),
                                "typed_value" | "known_userdata" | "other_userdata"
                            ) || !matches!(
                                descriptor.adapter.as_str(),
                                "none" | "guid" | "content" | "resource"
                            ) {
                                return Err(bad("found_userdata: unknown class"));
                            }
                            // value_kind names a typed value's class, and only a typed value's
                            if descriptor.value_kind.is_some()
                                != (descriptor.lua_kind == "typed_value")
                            {
                                return Err(bad(
                                    "found_userdata: value_kind belongs to a typed value",
                                ));
                            }
                            out.found_userdata = Some(descriptor);
                        }
                        _ => {
                            return Err(bad(
                                "incompatible_type: exactly one of found and found_userdata",
                            ));
                        }
                    }
                }
                "missing_field" => allowed.extend(["field", "type"]),
                "invalid_type" => allowed.push("name"),
                "invalid_guid" => allowed.push("guid"),
                "lua" => {
                    allowed.push("cause");
                    let cause = object
                        .get("cause")
                        .ok_or_else(|| bad("lua kind without a cause"))?;
                    out.cause = Some(Box::new(level(cause, false)?));
                }
                _ => {}
            }
            out.kind = Some(kind.to_owned());
        }
        _ => {}
    }
    closed(object, &allowed)?;
    Ok(out)
}
