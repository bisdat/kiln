// SPDX-License-Identifier: MIT
// the interpreter's game.types / runtime.types, shared test bodies.
// Run by kiln-sfi's own tests (tests/runtime_05.rs) and by any other test package that
// includes them.
//! game.types / runtime.types and the Type userdata on the registry port,
//! with a fake archive registry that serves registry_metadata pages exactly as the archive
//! schema defines them (schema/sfi/eml-archive/registry.schema.json), a converter for
//! ShroudForge-format registries (the shape of ShroudForge's own registry tests), the fake
//! kiln.types for the cross-check, and the real worker for the wiring.
// Another test package may load these through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_archive.rs"]
mod fake_archive;
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_services/mod.rs"]
mod fake_services;
// The scripted kiln.eml.values provider, for game.types.of on typed values.
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_values/mod.rs"]
mod fake_values;
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::types::{
    self, ArchiveRegistryPort, CrossCheck, KilnTypes, LoadError, NativeType, NativeTypes,
    PortError, Recognizer, Registry, RegistryPin, TypeRegistryPort, TypesWiring,
};
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::worker::compose::{ComposePlan, RuntimeHosts};
use kiln_sfi::worker::*;
use mlua::{Lua, Table, Value};
use serde_json::{Value as Json, json};
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::host::{HostContext, Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry as Packages},
};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use support::*;

/// Must-happen waits (the worker's runtime-create, a record): never reached when passing.
const WAIT: Duration = Duration::from_secs(300);

fn fnv(text: &str) -> u32 {
    text.bytes().fold(0x811c_9dc5_u32, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

// ---- the fake archive registry (schema v1 pages) ----

#[derive(Clone, Debug)]
struct A {
    key: String,
    name: String,
    namespace: Vec<String>,
    ty: Option<u64>,
    value: String,
}
fn attr(key: &str, value: &str) -> A {
    A {
        key: key.into(),
        name: key.rsplit("::").next().unwrap().into(),
        namespace: vec!["m6".into(), "attr".into()],
        ty: None,
        value: value.into(),
    }
}

#[derive(Clone, Debug)]
struct F {
    name: String,
    ty: u64,
    offset: u64,
    attributes: Vec<A>,
}

#[derive(Clone, Debug)]
struct T {
    name: String,
    qualified: String,
    impact: String,
    namespace: Vec<String>,
    /// qualified, internal (None: 0xa6000000 + index), name, impact.
    qualified_hash: u32,
    internal_hash: Option<u32>,
    name_hash: u32,
    impact_hash: u32,
    inner: Option<u64>,
    size: u32,
    alignment: u32,
    element_alignment: u32,
    field_count: u32,
    primitive: &'static str,
    flags: u8,
    default: Option<Vec<u8>>,
    fields: Vec<F>,
    enums: Vec<(String, u64)>,
    attributes: Vec<A>,
}

fn ty(name: &str, primitive: &'static str, size: u32) -> T {
    let qualified = format!("m6.{name}");
    let impact = format!("M6_{name}");
    T {
        name: name.into(),
        qualified_hash: fnv(&qualified),
        internal_hash: None,
        name_hash: fnv(name),
        impact_hash: fnv(&impact),
        qualified,
        impact,
        namespace: vec!["m6".into()],
        inner: None,
        size,
        alignment: 4,
        element_alignment: 4,
        field_count: 0,
        primitive,
        flags: 0,
        default: None,
        fields: Vec::new(),
        enums: Vec::new(),
        attributes: Vec::new(),
    }
}
impl T {
    fn field(mut self, name: &str, ty: u64, offset: u64) -> Self {
        self.fields.push(F {
            name: name.into(),
            ty,
            offset,
            attributes: Vec::new(),
        });
        self.field_count = self.fields.len() as u32;
        self
    }
    fn inner(mut self, inner: u64) -> Self {
        self.inner = Some(inner);
        self
    }
    fn flags(mut self, flags: u8) -> Self {
        self.flags = flags;
        self
    }
}

const FLAG_NAMES: [&str; 8] = [
    "HAS_DS",
    "HAS_BLOB_ARRAY",
    "HAS_BLOB_STRING",
    "HAS_BLOB_OPTIONAL",
    "HAS_BLOB_VARIANT",
    "IS_GPU_UNIFORM",
    "IS_GPU_STORAGE",
    "IS_GPU_CONSTANT",
];

fn u64t(value: u64) -> String {
    format!("u64:{value:016x}")
}
fn attrs_json(attributes: &[A]) -> Json {
    attributes
        .iter()
        .map(|a| {
            json!({"key": a.key, "name": a.name, "namespace": a.namespace,
                   "type_index": a.ty.map(u64t), "value": a.value})
        })
        .collect()
}

/// The provider's resolved fields: base first through struct/typedef inner types, a
/// redeclared name keeps the base's slot. A cycle or a missing base gives [].
fn resolved(types: &[T], index: usize) -> Json {
    let mut chain = Vec::new();
    let mut current = Some(index as u64);
    while let Some(at) = current {
        if chain.contains(&at) {
            return json!([]);
        }
        let Some(t) = types.get(at as usize) else {
            return json!([]);
        };
        chain.push(at);
        current = if matches!(t.primitive, "Struct" | "Typedef") {
            t.inner
        } else {
            None
        };
    }
    let mut out: Vec<(u64, F)> = Vec::new();
    for owner in chain.into_iter().rev() {
        for f in &types[owner as usize].fields {
            match out.iter_mut().find(|(_, x)| x.name == f.name) {
                Some(slot) => *slot = (owner, f.clone()),
                None => out.push((owner, f.clone())),
            }
        }
    }
    out.iter()
        .map(|(owner, f)| {
            json!({"name": f.name, "type_index": u64t(f.ty), "data_offset": u64t(f.offset),
                   "attributes": attrs_json(&f.attributes), "declaring_type": u64t(*owner)})
        })
        .collect()
}

fn type_json(types: &[T], index: usize) -> Json {
    let t = &types[index];
    json!({
        "index": u64t(index as u64),
        "name": t.name,
        "qualified_name": t.qualified,
        "impact_name": t.impact,
        "namespace": t.namespace,
        "hashes": {"qualified": t.qualified_hash,
                   "internal": t.internal_hash.unwrap_or(0xa600_0000 + index as u32),
                   "name": t.name_hash, "impact": t.impact_hash},
        "inner_type": t.inner.map(u64t),
        "size": t.size,
        "alignment": t.alignment,
        "element_alignment": t.element_alignment,
        "field_count": t.field_count,
        "primitive_type": t.primitive,
        "flags_bits": t.flags,
        "flags": FLAG_NAMES.iter().enumerate().filter(|(bit, _)| t.flags & (1 << bit) != 0)
            .map(|(_, n)| *n).collect::<Vec<_>>(),
        "has_ds": t.flags & 1 != 0,
        "default_bytes": t.default.as_ref().map(|bytes| format!("bytes:{}",
            bytes.iter().map(|b| format!("{b:02x}")).collect::<String>())),
        "struct_fields": t.fields.iter().map(|f| json!({"name": f.name,
            "type_index": u64t(f.ty), "data_offset": u64t(f.offset),
            "attributes": attrs_json(&f.attributes)})).collect::<Vec<_>>(),
        "fields": resolved(types, index),
        "enum_fields": t.enums.iter().map(|(name, value)| json!({"name": name,
            "value": u64t(*value), "value_hex": format!("0x{value:016x}")})).collect::<Vec<_>>(),
        "attributes": attrs_json(&t.attributes),
    })
}

fn pin_json() -> Json {
    json!({
        "launch_id": "01234567-89ab-4cde-8fab-0123456789ab",
        "plan_id": "fedcba98-7654-4321-8fed-cba987654321",
        "generation": u64t(7),
        "reload_epoch": u64t(2),
        "plan_sha256": "1".repeat(64),
        "archive_sha256": "2".repeat(64),
        "registry_id": u64t(3),
        "registry_sha256": "3".repeat(64),
    })
}
fn pin() -> RegistryPin {
    RegistryPin {
        launch_id: "01234567-89ab-4cde-8fab-0123456789ab".into(),
        plan_id: "fedcba98-7654-4321-8fed-cba987654321".into(),
        generation: 7,
        reload_epoch: 2,
        plan_sha256: "1".repeat(64),
        archive_sha256: "2".repeat(64),
        registry_id: 3,
        registry_sha256: "3".repeat(64),
    }
}

const GAME_VERSION: &str = "2.0.0-preview+archive";
const REGISTRY_VERSION: &str = "registry/revision B";

type Tamper = Rc<dyn Fn(u64, &mut Json)>;

/// registry_metadata over a list of types; \`tamper(first, page)\` edits a page.
#[derive(Clone)]
struct FakePort {
    types: Rc<Vec<T>>,
    calls: Rc<RefCell<Vec<(u64, u32)>>>,
    tamper: Option<Tamper>,
    fail_at: Option<u64>,
    expected: Option<RegistryPin>,
}
impl FakePort {
    fn new(types: Vec<T>) -> Self {
        Self {
            types: Rc::new(types),
            calls: Rc::default(),
            tamper: None,
            fail_at: None,
            expected: None,
        }
    }
    fn tamper(mut self, tamper: impl Fn(u64, &mut Json) + 'static) -> Self {
        self.tamper = Some(Rc::new(tamper));
        self
    }
    fn page_json(&self, first: u64, limit: u32) -> Json {
        page_json(&self.types, first, limit)
    }
}

/// registry_metadata's page of `types` for (first, limit), pinned to [`pin_json`].
fn page_json(types: &[T], first: u64, limit: u32) -> Json {
    let count = types.len() as u64;
    let end = (first + limit as u64).min(count);
    let page: Vec<Json> = (first.min(count)..end)
        .map(|at| type_json(types, at as usize))
        .collect();
    json!({
        "schema_version": 1,
        "pin": pin_json(),
        "game_version": GAME_VERSION,
        "registry_version": REGISTRY_VERSION,
        "count": u64t(count),
        "first": u64t(first),
        "types": page,
    })
}
impl TypeRegistryPort for FakePort {
    fn page(&self, first: u64, limit: u32) -> Result<Vec<u8>, PortError> {
        self.calls.borrow_mut().push((first, limit));
        if self.fail_at == Some(first) && limit > 0 {
            return Err(PortError::Failed {
                member: "registry_metadata",
                result: KILN_ERR_UNAVAILABLE,
                message: Some("the generation was retired".into()),
            });
        }
        let mut page = self.page_json(first, limit);
        if let Some(tamper) = &self.tamper {
            tamper(first, &mut page);
        }
        Ok(serde_json::to_vec(&page).unwrap())
    }
    fn expected_pin(&self) -> Option<RegistryPin> {
        self.expected.clone()
    }
}

/// The synthetic archive's shape (13 synthetic types: scalar, base and child structs, an array, blob
/// containers, a root struct, DS containers) plus an enum and an attributed type.
fn synthetic() -> Vec<T> {
    let mut types = vec![
        ty("UInt32", "UInt32", 4),
        ty("Base", "Struct", 4).field("base", 0, 0),
        ty("Child", "Struct", 8).field("extra", 0, 4).inner(1),
        ty("Fixed", "StaticArray", 8).inner(0),
        ty("Children", "BlobArray", 8).inner(2).flags(0x02),
        ty("MaybeChild", "BlobOptional", 4).inner(2).flags(0x08),
        ty("Choice", "BlobVariant", 12).inner(1).flags(0x10),
        ty("Text", "BlobString", 8).flags(0x04),
        ty("Root", "Struct", 48)
            .field("child", 2, 0)
            .field("fixed", 3, 8)
            .field("children", 4, 16)
            .field("maybe", 5, 24)
            .field("choice", 6, 28)
            .field("text", 7, 40)
            .flags(0x1e),
        ty("DsChildren", "DsArray", 24).inner(2).flags(0x01),
        ty("DsMaybeChild", "DsOptional", 8).inner(2).flags(0x01),
        ty("DsChoice", "DsVariant", 24).inner(1).flags(0x01),
        ty("DsText", "DsString", 16).flags(0x01),
    ];
    types[3].field_count = 2;
    let mut mode = ty("Mode", "Enum", 8);
    mode.enums = vec![
        ("Off".into(), 0),
        ("High".into(), 0x8000_0000_0000_0000),
        ("All".into(), u64::MAX),
    ];
    types.push(mode); // 13
    let mut noted = ty("Noted", "Struct", 8).field("x", 0, 0);
    noted.alignment = 8;
    noted.element_alignment = 2;
    noted.default = Some(vec![17, 0, 0xff, 0, 0x80, 23, 0, 0]);
    noted.attributes = vec![
        A {
            ty: Some(0),
            ..attr("m6::attr::Note", "a\0b")
        },
        A {
            ty: Some(99),
            ..attr("m6::attr::Dangling", "x")
        },
        attr("m6::attr::Plain", ""),
    ];
    noted.fields[0].attributes = vec![attr("m6::attr::Range", "0..10")];
    noted.flags = 0xe0;
    types.push(noted); // 14
    types
}

fn wiring(port: FakePort) -> TypesWiring {
    types::wire(port, |_, _| {})
}

struct NoSource;
impl PackageSource for NoSource {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}
fn package(id: &str) -> RuntimePackage {
    RuntimePackage::new(id.to_owned(), Rc::new(NoSource))
}

/// A plain Lua state with package \`a\`'s \`game.types\` as the global \`types\`.
fn lua_with(wiring: &TypesWiring) -> Lua {
    let lua = Lua::new();
    let table = wiring.game_types.types(&lua, &package("a")).unwrap();
    lua.globals().set("types", table).unwrap();
    lua
}
fn eval<R: mlua::FromLuaMulti>(lua: &Lua, code: &str) -> R {
    lua.load(code)
        .eval::<R>()
        .unwrap_or_else(|e| panic!("{code}: {e}"))
}
/// The error a chunk raises, as text.
fn raised(lua: &Lua, code: &str) -> String {
    match lua.load(code).exec() {
        Ok(()) => panic!("{code}: no error"),
        Err(e) => e.to_string(),
    }
}
/// Asserts the chunk raises an error whose text contains `needle`.
#[track_caller]
fn raises(lua: &Lua, code: &str, needle: &str) {
    let text = raised(lua, code);
    assert!(text.contains(needle), "{code}: {needle:?} not in {text:?}");
}
fn synthetic_lua() -> (Lua, TypesWiring) {
    let wiring = wiring(FakePort::new(synthetic()));
    (lua_with(&wiring), wiring)
}

// ---- the loader ----

pub fn the_loader_reads_the_header_then_whole_pages_and_keeps_both_versions() {
    let mut types = Vec::new();
    for i in 0..600 {
        types.push(ty(&format!("T{i}"), "UInt32", 4));
    }
    let port = FakePort::new(types);
    let calls = port.calls.clone();
    let registry = Registry::load(&port).unwrap();
    assert_eq!(
        *calls.borrow(),
        [(0, 0), (0, 256), (256, 256), (512, 88)],
        "the header page, then pages of PAGE_LIMIT and the rest"
    );
    assert_eq!(registry.len(), 600);
    assert_eq!(registry.get(599).unwrap().qualified_name, "m6.T599");
    assert_eq!(registry.game_version(), GAME_VERSION);
    assert_eq!(registry.registry_version(), REGISTRY_VERSION);
    assert_eq!(*registry.pin(), pin());

    // An empty registry reads the header only.
    let port = FakePort::new(Vec::new());
    let calls = port.calls.clone();
    assert!(Registry::load(&port).unwrap().is_empty());
    assert_eq!(*calls.borrow(), [(0, 0)]);
}

fn refusal(port: FakePort, expected: &str) -> String {
    match Registry::load(&port) {
        Ok(_) => panic!("accepted: {expected}"),
        Err(error) => error.to_string(),
    }
}
fn set(first: Option<u64>, path: &'static str, value: Json) -> impl Fn(u64, &mut Json) {
    move |at, page| {
        // Some(f): the page of types from f (not the header page, which starts at 0 too).
        let data = page["types"].as_array().is_some_and(|t| !t.is_empty());
        if first.is_none_or(|f| f == at && data)
            && let Some(slot) = page.pointer_mut(path)
        {
            *slot = value.clone();
        }
    }
}

pub fn a_page_that_breaks_the_schema_or_the_registry_is_refused_whole() {
    let base = || FakePort::new(synthetic());
    let cases: Vec<(FakePort, &str)> = vec![
        (
            base().tamper(set(None, "/schema_version", json!(2))),
            "schema_version is not 1",
        ),
        (
            base().tamper(|_, p| {
                p["extra"] = json!(1);
            }),
            "page has an unknown property extra",
        ),
        (
            base().tamper(|_, p| {
                p.as_object_mut().unwrap().remove("registry_version");
            }),
            "page has no registry_version",
        ),
        (
            base().tamper(set(None, "/count", json!("u64:000000000000000F"))),
            "count is not a u64: text",
        ),
        (
            base().tamper(set(Some(0), "/pin/generation", json!(u64t(8)))),
            "belongs to another registry",
        ),
        (
            base().tamper(set(Some(0), "/registry_version", json!("other"))),
            "belongs to another registry",
        ),
        (
            base().tamper(set(Some(0), "/game_version", json!("other"))),
            "belongs to another registry",
        ),
        (
            base().tamper(|at, p| {
                if p["types"].as_array().unwrap().len() > 1 && at == 0 {
                    p["types"].as_array_mut().unwrap().pop();
                }
            }),
            "asked for 15 types from 0, got 14 from 0",
        ),
        (
            base().tamper(|_, p| {
                if !p["types"].as_array().unwrap().is_empty() {
                    p["first"] = json!(u64t(1));
                }
            }),
            "got 15 from 1",
        ),
        (
            base().tamper(set(None, "/types/2/index", json!(u64t(42)))),
            "has index 42 at registry ordinal 2",
        ),
        (
            base().tamper(set(
                None,
                "/types/2/hashes/qualified",
                json!(fnv("m6.Base")),
            )),
            "duplicate qualified hash",
        ),
        (
            base().tamper(set(None, "/types/2/hashes/impact", json!(fnv("M6_Base")))),
            "duplicate Impact hash",
        ),
        (
            base().tamper(set(None, "/types/2/hashes/internal", json!(4294967296u64))),
            "hashes.internal is not an integer in [0, 4294967295]",
        ),
        (
            base().tamper(set(None, "/types/2/size", json!(8.0))),
            "size is not an integer",
        ),
        (
            base().tamper(set(None, "/types/2/flags", json!(["HAS_DS"]))),
            "flags do not name its flags_bits",
        ),
        (
            base().tamper(set(None, "/types/2/has_ds", json!(true))),
            "has_ds is not its HAS_DS bit",
        ),
        (
            base().tamper(set(None, "/types/2/flags_bits", json!(256))),
            "flags_bits is more than one byte",
        ),
        (
            base().tamper(set(None, "/types/2/primitive_type", json!("STRUCT"))),
            "unknown primitive type STRUCT",
        ),
        (
            base().tamper(set(
                None,
                "/types/13/enum_fields/2/value_hex",
                json!("0x00000000ffffffff"),
            )),
            "value_hex that is not its value",
        ),
        (
            base().tamper(set(None, "/types/14/default_bytes", json!("bytes:0"))),
            "default_bytes is not a bytes: text",
        ),
        (
            base().tamper(set(
                None,
                "/types/14/attributes/1/key",
                json!("m6::attr::Note"),
            )),
            "lists attribute m6::attr::Note twice",
        ),
        (
            base().tamper(|_, p| {
                if let Some(fields) = p.pointer_mut("/types/8/struct_fields") {
                    let first = fields[0].clone();
                    fields.as_array_mut().unwrap().push(first);
                }
            }),
            "lists field child twice",
        ),
        (
            base().tamper(set(
                None,
                "/types/2/fields/0/declaring_type",
                json!(u64t(2)),
            )),
            "resolved fields of m6.Child do not follow",
        ),
        (
            base().tamper(set(None, "/pin/plan_sha256", json!("A".repeat(64)))),
            "pin.plan_sha256 is not a lowercase SHA-256",
        ),
        (
            base().tamper(|_, p| {
                p["types"] = json!({});
            }),
            "types is not an array",
        ),
        (
            FakePort {
                fail_at: Some(0),
                ..base()
            },
            "registry_metadata failed (3): the generation was retired",
        ),
        (
            FakePort {
                expected: Some(RegistryPin {
                    registry_id: 4,
                    ..pin()
                }),
                ..base()
            },
            "pinned to another launch identity",
        ),
    ];
    for (port, expected) in cases {
        let text = refusal(port, expected);
        assert!(text.contains(expected), "{expected:?} not in {text:?}");
    }
    // The same registry with the launch's own pin, and an Impact hash shared by a DS type
    // (DS types have no Impact entry), is accepted.
    let mut types = synthetic();
    types[12].impact_hash = types[0].impact_hash;
    let port = FakePort {
        expected: Some(pin()),
        ..FakePort::new(types)
    };
    assert_eq!(Registry::load(&port).unwrap().len(), 15);
}

pub fn a_registry_that_cannot_be_read_is_a_port_error_with_the_member_s_text() {
    let port = FakePort {
        fail_at: Some(0),
        ..FakePort::new(synthetic())
    };
    assert_eq!(
        Registry::load(&port).err().unwrap(),
        LoadError::Port(PortError::Failed {
            member: "registry_metadata",
            result: KILN_ERR_UNAVAILABLE,
            message: Some("the generation was retired".into()),
        })
    );
}

// ---- game.types ----

pub fn the_table_has_shroudforge_s_members_and_count_and_version_are_the_registry_s() {
    let (lua, _w) = synthetic_lua();
    let keys: Vec<String> = eval(
        &lua,
        "local k = {} for name in pairs(types) do k[#k + 1] = name end table.sort(k) return k",
    );
    assert_eq!(
        keys,
        [
            "count",
            "find",
            "find_by_hash",
            "get",
            "get_all",
            "get_by_impact_name",
            "get_by_index",
            "get_by_internal_hash",
            "get_by_qualified_hash",
            "get_by_qualified_name",
            "resolve",
            "of",
            "version",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect::<Vec<_>>()
        .tap_sorted()
    );
    let (count, version): (i64, String) = eval(&lua, "return types.count, types.version");
    assert_eq!((count, version.as_str()), (15, REGISTRY_VERSION));
    // get and get_by_qualified_name are two functions over one body, as in ShroudForge.
    assert!(eval::<bool>(
        &lua,
        "return types.get ~= types.get_by_qualified_name and types.get_by_qualified_name('m6.Child') == types.get('m6.Child')"
    ));
}

trait TapSorted {
    fn tap_sorted(self) -> Self;
}
impl TapSorted for Vec<String> {
    fn tap_sorted(mut self) -> Self {
        self.sort();
        self
    }
}

pub fn get_is_string_only_and_coerces_numbers_to_their_text() {
    let mut types = synthetic();
    let mut numeric = ty("x", "UInt32", 4);
    numeric.qualified = "4242".into();
    numeric.qualified_hash = fnv("4242");
    types.push(numeric); // 15
    let wiring = wiring(FakePort::new(types));
    let lua = lua_with(&wiring);
    assert!(eval::<bool>(
        &lua,
        "return types.get('m6.Child').index == 2"
    ));
    // ShroudForge's string cast: an integer or a float becomes its decimal text.
    assert!(eval::<bool>(&lua, "return types.get(4242).index == 15"));
    assert!(eval::<bool>(&lua, "return types.get(4242.0).index == 15"));
    // A name is never a hash: the qualified hash of m6.Child as a number is a missing name.
    assert!(eval::<bool>(
        &lua,
        &format!("return types.get({}) == nil", fnv("m6.Child"))
    ));
    raises(
        &lua,
        "types.get({})",
        "bad argument #1 (string expected, got table)",
    );
    raises(
        &lua,
        "types.get()",
        "bad argument #1 (string expected, got nil)",
    );
    // resolve takes the qualified hash (and tables and Types) where get does not.
    assert!(eval::<bool>(
        &lua,
        &format!("return types.resolve({}).index == 2", fnv("m6.Child"))
    ));
}

pub fn names_resolve_through_their_hash_index_and_must_match_exactly() {
    let mut types = synthetic();
    // GHOST is stored under the qualified hash of "Aliased", and its Impact hash is that
    // of "AliasedImpact": the hash finds it, the exact-name check refuses it.
    let mut ghost = ty("GHOST", "UInt32", 4);
    ghost.qualified_hash = fnv("Aliased");
    ghost.impact_hash = fnv("AliasedImpact");
    types.push(ghost); // 15
    let wiring = wiring(FakePort::new(types));
    let lua = lua_with(&wiring);
    assert!(eval::<bool>(&lua, "return types.get('Aliased') == nil"));
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_impact_name('AliasedImpact') == nil"
    ));
    // GHOST's own name hashes elsewhere: its stored hash is the index, so it has no name.
    assert!(eval::<bool>(&lua, "return types.get('m6.GHOST') == nil"));
    // But a scan by name (resolve) and by stored hash still find it.
    assert!(eval::<bool>(
        &lua,
        "return types.resolve('m6.GHOST').index == 15"
    ));
    assert!(eval::<bool>(
        &lua,
        &format!(
            "return types.get_by_qualified_hash({}).index == 15",
            fnv("Aliased")
        )
    ));
    // Impact names: found for a type without DS, never for a DS type.
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_impact_name('M6_Child').index == 2"
    ));
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_impact_name('M6_DsText') == nil"
    ));
}

pub fn every_hash_domain_refuses_a_collision_and_reports_a_missing_hash() {
    let mut types = synthetic();
    for (at, name) in [(15usize, "One"), (16, "Two")] {
        let mut t = ty(name, "UInt32", 4);
        t.internal_hash = Some(777);
        t.name_hash = 888;
        t.impact_hash = 900 + at as u32;
        types.push(t);
    }
    types[16].flags = 0x01; // DS: may share an Impact hash
    types[16].impact_hash = 915;
    let wiring = wiring(FakePort::new(types));
    let lua = lua_with(&wiring);
    let (value, reason): (Value, String) = eval(&lua, "return types.get_by_internal_hash(777)");
    assert!(value.is_nil());
    assert_eq!(
        reason,
        "Ambiguous type hash 0x00000309. Use a qualified name or qualified hash"
    );
    let (value, reason): (Value, String) = eval(&lua, "return types.get_by_qualified_hash(0xbeef)");
    assert!(value.is_nil());
    assert_eq!(reason, "type hash not found: 0x0000beef");
    let (index, reason): (i64, Value) = eval(
        &lua,
        &format!(
            "local t, r = types.get_by_qualified_hash({}) return t.index, r",
            fnv("m6.Two")
        ),
    );
    assert_eq!((index, reason), (16, Value::Nil));
    let (index, reason): (i64, Value) = eval(
        &lua,
        "local t, r = types.get_by_internal_hash(0xa6000002) return t.index, r",
    );
    assert_eq!((index, reason), (2, Value::Nil));
    for (selector, expected) in [
        (
            "{hash = 777, domain = 'internal'}",
            "Ambiguous type hash 0x00000309",
        ),
        (
            "{hash = 888, domain = 'name'}",
            "Ambiguous type hash 0x00000378",
        ),
        (
            "{hash = 915, domain = 'impact'}",
            "Ambiguous type hash 0x00000393",
        ),
        (
            "{hash = 1, domain = 'address'}",
            "Unknown hash domain 'address'. Expected qualified, internal, name or impact",
        ),
        (
            "{hash = 0xbeef, domain = 'qualified'}",
            "type hash not found: 0x0000beef",
        ),
        ("'m6.Nothing'", "type not found: m6.Nothing"),
        (
            "-1",
            "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}",
        ),
        (
            "4294967296",
            "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}",
        ),
        (
            "1.5",
            "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}",
        ),
        (
            "true",
            "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}",
        ),
    ] {
        let (value, reason): (Value, String) =
            eval(&lua, &format!("return types.resolve({selector})"));
        assert!(value.is_nil(), "{selector}");
        assert!(reason.starts_with(expected), "{selector}: {reason}");
    }
    // A table selector's fields go through Lua's conversions; their errors are the reason.
    let (_, reason): (Value, String) = eval(&lua, "return types.resolve({domain = 'name'})");
    let lua2 = Lua::new();
    let expected = lua2
        .create_table()
        .unwrap()
        .get::<u32>("hash")
        .unwrap_err()
        .to_string();
    assert_eq!(reason, expected);
    // find_by_hash lists collisions instead of refusing them, in registry order.
    let found: Vec<i64> = eval(
        &lua,
        "local r = {} for _, t in ipairs(types.find_by_hash(777, 'internal')) do r[#r+1] = t.index end return r",
    );
    assert_eq!(found, [15, 16]);
    let found: Vec<i64> = eval(
        &lua,
        &format!(
            "local r = {{}} for _, t in ipairs(types.find_by_hash({})) do r[#r+1] = t.index end return r",
            fnv("m6.Base")
        ),
    );
    assert_eq!(found, [1], "the default domain is qualified");
    raises(
        &lua,
        "types.find_by_hash(1, 'address')",
        "Unknown hash domain 'address'. Expected qualified, internal, name or impact",
    );
    raises(
        &lua,
        "types.find_by_hash(-1)",
        "bad argument #1 (32-bit unsigned integer expected, got integer)",
    );
}

pub fn index_bounds_follow_shroudforge_s_unsigned_cast() {
    let (lua, _w) = synthetic_lua();
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_index(0).qualified_name == 'm6.UInt32'"
    ));
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_index(14).qualified_name == 'm6.Noted'"
    ));
    assert!(eval::<bool>(&lua, "return types.get_by_index(15) == nil"));
    // -1 is a huge unsigned index (nil); -1.0 saturates to 0; a numeric string parses.
    assert!(eval::<bool>(&lua, "return types.get_by_index(-1) == nil"));
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_index(-1.0).index == 0"
    ));
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_index('2').index == 2"
    ));
    raises(
        &lua,
        "types.get_by_index(1.5)",
        "bad argument #1 (64-bit unsigned integer expected, got number)",
    );
    raises(
        &lua,
        "types.get_by_index()",
        "bad argument #1 (64-bit unsigned integer expected, got nil)",
    );
}

pub fn get_all_includes_ds_types_unless_told_not_to() {
    let (lua, _w) = synthetic_lua();
    let (all, explicit, without): (i64, i64, i64) = eval(
        &lua,
        "return #types.get_all(), #types.get_all(true), #types.get_all(false)",
    );
    assert_eq!((all, explicit, without), (15, 15, 11));
    let order: Vec<i64> = eval(
        &lua,
        "local r = {} for _, t in ipairs(types.get_all(false)) do r[#r+1] = t.index end return r",
    );
    assert_eq!(order, [0, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14]);
    raises(
        &lua,
        "types.get_all(1)",
        "bad argument #1 (boolean? expected, got integer)",
    );
    // A fresh table each call.
    assert!(eval::<bool>(
        &lua,
        "return types.get_all() ~= types.get_all()"
    ));
}

pub fn find_filters_on_prefix_attribute_primitive_and_ds() {
    let (lua, _w) = synthetic_lua();
    let list = |code: &str| -> Vec<i64> {
        eval(
            &lua,
            &format!(
                "local r = {{}} for _, t in ipairs(types.find({code})) do r[#r+1] = t.index end return r"
            ),
        )
    };
    assert_eq!(list("").len(), 15);
    assert_eq!(list("{}").len(), 15);
    assert_eq!(list("{prefix = 'm6.Ds'}"), [9, 10, 11, 12]);
    assert_eq!(list("{primitive_type = 'Struct'}"), [1, 2, 8, 14]);
    assert_eq!(list("{attribute = 'm6::attr::Note'}"), [14]);
    // A field's attribute is not the type's.
    assert!(list("{attribute = 'm6::attr::Range'}").is_empty());
    assert_eq!(list("{has_ds = true}"), [9, 10, 11, 12]);
    assert_eq!(list("{has_ds = false}").len(), 11);
    // Lua truthiness: any non-nil, non-false value is true.
    assert_eq!(list("{has_ds = 0}"), [9, 10, 11, 12]);
    assert_eq!(list("{prefix = 'm6.D', primitive_type = 'DsString'}"), [12]);
    raises(
        &lua,
        "types.find(5)",
        "bad argument #1 (table? expected, got integer)",
    );
    // Option fields go through Lua's own conversions.
    let lua2 = Lua::new();
    let expected = lua2
        .load("return {prefix = {}}")
        .eval::<Table>()
        .unwrap()
        .get::<Option<String>>("prefix")
        .unwrap_err()
        .to_string();
    raises(&lua, "types.find({prefix = {}})", &expected);
}

pub fn invalid_utf8_names_are_conversion_errors() {
    let (lua, _w) = synthetic_lua();
    let lua2 = Lua::new();
    let expected = lua2
        .create_string(b"\xff")
        .unwrap()
        .to_str()
        .err()
        .unwrap()
        .to_string();
    // get raises (the string cast); resolve answers the same text as its reason.
    raises(&lua, "types.get('\\255')", &expected);
    raises(&lua, "types.get_by_impact_name('\\255')", &expected);
    let (value, reason): (Value, String) = eval(&lua, "return types.resolve('\\255')");
    assert!(value.is_nil());
    assert_eq!(reason, expected);
}

pub fn one_type_per_index_per_vm_shared_by_every_package_s_table() {
    let wiring = wiring(FakePort::new(synthetic()));
    let lua = lua_with(&wiring);
    let other = wiring.game_types.types(&lua, &package("b")).unwrap();
    lua.globals().set("other", other).unwrap();
    assert!(eval::<bool>(
        &lua,
        "local t = types.get('m6.Child') return t == types.get_by_index(2) and t == types.resolve(2 * 0 + types.get('m6.Child').qualified_hash) \
         and t == other.get('m6.Child') and t == types.get_by_internal_hash(0xa6000002) \
         and t == types.get_all()[3] and t == types.get('m6.Children').inner_type"
    ));
    // Each package has its own table: a write to one is invisible in the other.
    assert!(eval::<bool>(
        &lua,
        "types.count = 0 return other.count == 15 and types ~= other"
    ));
    // Another VM gets its own session and its own Types.
    let lua2 = lua_with(&wiring);
    let a: mlua::AnyUserData = eval(&lua, "return types.get('m6.Child')");
    let b: mlua::AnyUserData = eval(&lua2, "return types.get('m6.Child')");
    assert!(!a.to_pointer().eq(&b.to_pointer()));
    // The registry was read once for both.
    assert_eq!(wiring.handle.registry().unwrap().len(), 15);
}

pub fn a_finalizer_asking_for_the_type_being_made_keeps_one_type_per_index() {
    // Lua runs finalizers from inside allocations: with the collector always in debt, a
    // Type's own allocation runs finalizers that ask for that same index. Each round is a
    // fresh session; the window is reached when a finalizer ran during the outer call.
    let lua = Lua::new();
    let (mut reached, mut split) = (0i64, 0i64);
    for _ in 0..40 {
        let wiring = wiring(FakePort::new(synthetic()));
        let table = wiring.game_types.types(&lua, &package("a")).unwrap();
        lua.globals().set("types", table).unwrap();
        let (hits, bad): (i64, i64) = eval(
            &lua,
            "collectgarbage('incremental', 1, 1000) \
             local hits, bad = 0, 0 \
             for i = 0, 14 do \
               local seen = {} \
               for _ = 1, 16 do setmetatable({}, {__gc = function() seen[#seen + 1] = types.get_by_index(i) end}) end \
               local before = #seen \
               local t = types.get_by_index(i) \
               if #seen > before then hits = hits + 1 end \
               collectgarbage() \
               for _, s in ipairs(seen) do if s ~= t then bad = bad + 1 end end \
               if types.get_by_index(i) ~= t then bad = bad + 1 end \
             end \
             collectgarbage('incremental', 200, 100) \
             return hits, bad",
        );
        reached += hits;
        split += bad;
    }
    assert!(reached > 0, "no finalizer ran inside a Type's construction");
    assert_eq!(split, 0, "two Types for one index");
}

pub fn of_answers_through_registered_recognizers_only() {
    let wiring = wiring(FakePort::new(synthetic()));
    let lua = lua_with(&wiring);
    assert!(eval::<bool>(
        &lua,
        "return types.of({}) == nil and types.of(7) == nil and types.of() == nil"
    ));
    let recognize: Recognizer =
        Rc::new(|value: &Value| Ok(value.as_integer().filter(|v| *v == 7).map(|_| 2)));
    wiring.handle.add_recognizer(recognize);
    let second: Recognizer = Rc::new(|value: &Value| Ok(value.as_integer().map(|_| 1)));
    wiring.handle.add_recognizer(second);
    assert!(eval::<bool>(
        &lua,
        "return types.of(7) == types.get('m6.Child') and types.of(8) == types.get('m6.Base')"
    ));
}

/// `of` answers a typed value with its Type (ShroudForge's `type_of` over its value classes,
/// resource_values/mod.rs:203-252, both pins): the recognizer the production wiring registers reads the value's
/// registry index from the provider (an identity-only `value_info`, no logical borrow), the same index `get` finds;
/// anything else, a Type included, is nil; once the arena closed, Kiln's closed error.
pub fn of_answers_a_typed_value_with_its_type_through_the_provider() {
    use kiln_abi::services::kiln_eml_values::{KILN_LUA_LEASE_ADOPTED, KILN_VALUE_STRUCT_OWNED};
    let wiring = wiring(FakePort::new(synthetic()));
    wiring
        .handle
        .add_recognizer(Rc::new(kiln_sfi::runtime::values::type_index_of));
    let t = fake_values::vm();
    let arena = t
        .arena(kiln_sfi::runtime::values::ArenaOptions::default())
        .unwrap();
    let table = wiring.game_types.types(t.lua(), &package("a")).unwrap();
    t.set("types", table);
    // a struct the provider makes (MAKE_TYPED_PROXY inside a value_open) on node 41, of registry index 2
    fake_values::with(|f| f.type_indexes.insert(41, 2));
    fake_values::on("value_open", |_| {
        let (_, code, reply) = fake_values::make_proxy(KILN_VALUE_STRUCT_OWNED, 41);
        assert_eq!(code, KILN_OK);
        assert_eq!(reply.lease_disposition, KILN_LUA_LEASE_ADOPTED as u32);
        fake_values::handled(1, fake_values::lease(KILN_VALUE_STRUCT_OWNED, 41), 0)
    });
    let value = arena.open_default(t.lua(), "m6.Child").unwrap();
    t.set("v", value);
    assert!(
        t.run::<bool>(
            "return types.of(v) == types.get('m6.Child') and types.of(v).index == 2 \
             and types.of(types.get('m6.Child')) == nil and types.of(1) == nil and types.of({}) == nil"
        )
        .unwrap()
    );
    // the index is the provider's, asked again on each call
    fake_values::with(|f| f.type_indexes.insert(41, 1));
    assert!(
        t.run::<bool>("return types.of(v) == types.get('m6.Base')")
            .unwrap()
    );
    // the arena closed (the VM's host at its stop): Kiln's own error, never a guess
    assert!(arena.close());
    let error = t
        .run::<bool>("return types.of(v) ~= nil")
        .unwrap_err()
        .to_string();
    assert!(
        error.contains("kiln: kiln.eml.values: the arena is closed"),
        "{error}"
    );
}

// ---- the Type userdata ----

pub fn a_type_has_shroudforge_s_metadata_fields() {
    let (lua, _w) = synthetic_lua();
    lua.load("t = types.get('m6.Noted') c = types.get('m6.Child') d = types.get('m6.DsChildren')")
        .exec()
        .unwrap();
    let (index, name, impact, qualified): (i64, String, String, String) = eval(
        &lua,
        "return t.index, t.name, t.impact_name, t.qualified_name",
    );
    assert_eq!(
        (index, name.as_str(), impact.as_str(), qualified.as_str()),
        (14, "Noted", "M6_Noted", "m6.Noted")
    );
    let hashes: (u32, u32, u32, u32) = eval(
        &lua,
        "return t.qualified_hash, t.internal_hash, t.name_hash, t.impact_hash",
    );
    assert_eq!(
        hashes,
        (fnv("m6.Noted"), 0xa600_000e, fnv("Noted"), fnv("M6_Noted"))
    );
    let layout: (i64, i64, i64, i64) = eval(
        &lua,
        "return t.size, t.alignment, t.element_alignment, t.field_count",
    );
    assert_eq!(layout, (8, 8, 2, 1), "element_alignment is its own field");
    let (primitive, bits, has_ds): (String, i64, bool) =
        eval(&lua, "return t.primitive_type, t.flags_bits, t.has_ds");
    assert_eq!((primitive.as_str(), bits, has_ds), ("Struct", 0xe0, false));
    let flags: Vec<String> = eval(&lua, "return t.flags");
    assert_eq!(
        flags,
        ["IS_GPU_UNIFORM", "IS_GPU_STORAGE", "IS_GPU_CONSTANT"]
    );
    let ds_flags: Vec<String> = eval(&lua, "return d.flags");
    assert_eq!(ds_flags, ["HAS_DS"]);
    assert!(eval::<bool>(&lua, "return d.has_ds and d.flags_bits == 1"));
    // The default bytes exactly, NUL and high bytes included; nil when the type has none.
    let bytes: mlua::String = eval(&lua, "return t.default_bytes");
    assert_eq!(bytes.as_bytes().as_ref(), [17, 0, 0xff, 0, 0x80, 23, 0, 0]);
    assert!(eval::<bool>(&lua, "return c.default_bytes == nil"));
    // namespace and flags: a fresh, mutable table on every read.
    assert!(eval::<bool>(
        &lua,
        "local n = t.namespace n[1] = 'changed' return t.namespace[1] == 'm6' and t.namespace ~= t.namespace and t.flags ~= t.flags"
    ));
    // inner_type: the base's Type, nil without one.
    assert!(eval::<bool>(
        &lua,
        "return c.inner_type == types.get('m6.Base') and t.inner_type == nil"
    ));
    // tostring names the userdata as ShroudForge's does.
    assert!(eval::<String>(&lua, "return tostring(t)").starts_with("Type"));
    // No setters: a write is an error, the metadata does not change.
    assert!(!raised(&lua, "t.size = 1").is_empty());
    assert!(eval::<bool>(&lua, "return t.size == 8"));
}

pub fn enum_values_keep_their_high_bits() {
    let (lua, _w) = synthetic_lua();
    let rows: Vec<(String, i64, String)> = eval::<Vec<Vec<Value>>>(
        &lua,
        "local r = {} for name, f in pairs(types.get('m6.Mode').enum_fields) do r[#r+1] = {name, f.value, f.value_hex} end \
         return r",
    )
    .into_iter()
    .map(|row: Vec<Value>| {
        (
            row[0].to_string().unwrap(),
            row[1].as_integer().unwrap(),
            row[2].to_string().unwrap(),
        )
    })
    .collect::<Vec<_>>();
    assert_eq!(
        rows,
        [
            ("Off".to_string(), 0, "0x0000000000000000".to_string()),
            (
                "High".to_string(),
                i64::MIN,
                "0x8000000000000000".to_string()
            ),
            ("All".to_string(), -1, "0xffffffffffffffff".to_string()),
        ]
    );
    let name: String = eval(&lua, "return types.get('m6.Mode').enum_fields.All.name");
    assert_eq!(name, "All");
}

pub fn attributes_are_text_with_their_type_or_nil() {
    let (lua, _w) = synthetic_lua();
    lua.load("t = types.get('m6.Noted') a = t.attributes")
        .exec()
        .unwrap();
    let keys: Vec<String> = eval(
        &lua,
        "local r = {} for k in pairs(a) do r[#r+1] = k end return r",
    );
    assert_eq!(
        keys,
        ["m6::attr::Note", "m6::attr::Dangling", "m6::attr::Plain"]
    );
    let value: mlua::String = eval(&lua, "return a['m6::attr::Note'].value");
    assert_eq!(value.as_bytes().as_ref(), b"a\0b");
    assert!(eval::<bool>(
        &lua,
        "local n = a['m6::attr::Note'] return n.name == 'Note' and n.type == types.get_by_index(0) \
         and n.namespace[1] == 'm6' and n.namespace[2] == 'attr' and #n.namespace == 2 \
         and a['m6::attr::Dangling'].type == nil and a['m6::attr::Plain'].type == nil and a['m6::attr::Plain'].value == ''"
    ));
    // A struct field's attributes are its own.
    assert!(eval::<bool>(
        &lua,
        "return t.struct_fields.x.attributes['m6::attr::Range'].value == '0..10' and t.attributes['m6::attr::Range'] == nil"
    ));
}

pub fn fields_are_base_first_and_a_redeclared_field_keeps_the_base_s_slot() {
    let mut types = synthetic();
    types.push(ty("Shape", "Struct", 12).field("x", 0, 0).field("y", 0, 4)); // 15
    types.push(
        ty("Square", "Struct", 16)
            .field("y", 0, 8)
            .field("z", 0, 12)
            .inner(15),
    ); // 16
    types.push(ty("Alias", "Typedef", 16).inner(16)); // 17
    let wiring = wiring(FakePort::new(types));
    let lua = lua_with(&wiring);
    let rows: Vec<(String, i64, i64)> = eval::<Vec<Table>>(
        &lua,
        "local r = {} for i, f in ipairs(types.get('m6.Square').fields) do r[i] = {f.name, f.declaring_type.index, f.data_offset} end return r",
    )
    .into_iter()
    .map(|row| (row.get(1).unwrap(), row.get(2).unwrap(), row.get(3).unwrap()))
    .collect();
    assert_eq!(
        rows,
        [
            ("x".to_string(), 15, 0),
            ("y".to_string(), 16, 8),
            ("z".to_string(), 16, 12)
        ]
    );
    // struct_fields are the type's own fields only.
    let own: Vec<String> = eval(
        &lua,
        "local r = {} for k in pairs(types.get('m6.Square').struct_fields) do r[#r+1] = k end return r",
    );
    assert_eq!(own, ["y", "z"]);
    // A typedef inherits through its inner type too.
    let alias: i64 = eval(&lua, "return #types.get('m6.Alias').fields");
    assert_eq!(alias, 3);
    // fields is a fresh, mutable table of fresh entries; each entry's type is the Type.
    assert!(eval::<bool>(
        &lua,
        "local t = types.get('m6.Square') local f = t.fields f[1].name = 'changed' \
         return t.fields ~= f and t.fields[1].name == 'x' and t.fields[1].type == types.get_by_index(0) \
         and t.fields[1].attributes ~= nil"
    ));
}

pub fn is_a_follows_real_inheritance_only() {
    let (lua, _w) = synthetic_lua();
    assert!(eval::<bool>(
        &lua,
        "local c, b = types.get('m6.Child'), types.get('m6.Base') \
         return c:is_a(b) and c:is_a('m6.Base') and c:is_a(c) and not b:is_a(c) \
         and c:is_a(b.qualified_hash) and c:is_a({hash = b.internal_hash, domain = 'internal'})"
    ));
    // An array's, optional's or variant's inner type is its element, not a base.
    assert!(eval::<bool>(
        &lua,
        "return not types.get('m6.Fixed'):is_a('m6.UInt32') and not types.get('m6.Children'):is_a('m6.Child') \
         and not types.get('m6.Choice'):is_a('m6.Base')"
    ));
    // A selector that selects nothing raises its reason.
    raises(
        &lua,
        "types.get('m6.Child'):is_a('m6.Nothing')",
        "type not found: m6.Nothing",
    );
    raises(
        &lua,
        "types.get('m6.Child'):is_a(true)",
        "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}",
    );
}

pub fn cyclic_and_unresolved_bases_are_errors_at_use() {
    let mut types = synthetic();
    types.push(ty("Loop1", "Struct", 4).inner(16)); // 15
    types.push(ty("Loop2", "Struct", 4).inner(15)); // 16
    types.push(ty("Orphan", "Struct", 4).inner(99).field("o", 0, 0)); // 17
    let wiring = wiring(FakePort::new(types));
    let lua = lua_with(&wiring);
    raises(
        &lua,
        "local _ = types.get('m6.Loop1').fields",
        "cyclic type inheritance",
    );
    raises(
        &lua,
        "types.get('m6.Loop1'):is_a('m6.Base')",
        "cyclic type inheritance",
    );
    raises(
        &lua,
        "local _ = types.get('m6.Orphan').fields",
        "unresolved base type",
    );
    raises(
        &lua,
        "types.get('m6.Orphan'):is_a('m6.Base')",
        "unresolved base type",
    );
    // The type itself still answers its own metadata; a missing inner type is nil.
    assert!(eval::<bool>(
        &lua,
        "local o = types.get('m6.Orphan') return o.inner_type == nil and o.struct_fields.o.data_offset == 0"
    ));
}

pub fn read_only_containers_index_count_and_walk_in_order() {
    let (lua, _w) = synthetic_lua();
    lua.load("r = types.get('m6.Root') s = r.struct_fields")
        .exec()
        .unwrap();
    // Built once per Type.
    assert!(eval::<bool>(
        &lua,
        "return r.struct_fields == s and r.enum_fields == r.enum_fields and r.attributes == r.attributes"
    ));
    assert_eq!(eval::<i64>(&lua, "return #s"), 6);
    let names: Vec<String> = eval(
        &lua,
        "local r = {} for k, v in pairs(s) do r[#r+1] = k .. '=' .. v.name end return r",
    );
    assert_eq!(
        names,
        [
            "child=child",
            "fixed=fixed",
            "children=children",
            "maybe=maybe",
            "choice=choice",
            "text=text"
        ]
    );
    assert!(eval::<bool>(
        &lua,
        "return s.text.data_offset == 40 and s.text.type == types.get('m6.Text') and s.nothing == nil and #s.text.attributes == 0"
    ));
    assert!(eval::<String>(&lua, "return tostring(s)").starts_with("ReadOnlyMap"));
    // A key must be a string; a write is an error and changes nothing.
    raises(
        &lua,
        "local _ = s[1]",
        "bad argument #1 (string expected, got integer)",
    );
    assert!(!raised(&lua, "s.text = 1").is_empty());
    assert!(eval::<bool>(&lua, "return s.text.name == 'text'"));
    // Arrays: 1-based, 0 and past the end are nil, numeric strings cast.
    lua.load("n = types.get('m6.Noted').attributes['m6::attr::Note'].namespace")
        .exec()
        .unwrap();
    assert!(eval::<bool>(
        &lua,
        "return n[0] == nil and n[1] == 'm6' and n['2'] == 'attr' and n[3] == nil and #n == 2"
    ));
    assert!(eval::<String>(&lua, "return tostring(n)").starts_with("ReadOnlyArray"));
    // -1 is a huge unsigned position (nil); a fraction is a cast error.
    assert!(eval::<bool>(&lua, "return n[-1] == nil"));
    raises(
        &lua,
        "local _ = n[1.5]",
        "bad argument #1 (64-bit unsigned integer expected, got number)",
    );
    let walked: Vec<String> = eval(
        &lua,
        "local r = {} for i, v in pairs(n) do r[#r+1] = i .. v end return r",
    );
    assert_eq!(walked, ["1m6", "2attr"]);
}

pub fn an_iterator_keeps_its_own_position_and_refuses_another_state() {
    let (lua, _w) = synthetic_lua();
    let walked: Vec<String> = eval(
        &lua,
        "local s = types.get('m6.Root').struct_fields \
         local step, state = pairs(s) \
         local out = {} \
         local k1 = step(state) out[#out+1] = k1 \
         for k in pairs(s) do end \
         local step2, state2 = pairs(s) step2(state2) step2(state2) \
         local k2 = step(state) out[#out+1] = k2 \
         local k3 = step2(state2) out[#out+1] = k3 \
         for _ = 1, 10 do local k = step(state) if k == nil then out[#out+1] = 'end' break end end \
         out[#out+1] = tostring(step(state)) \
         return out",
    );
    assert_eq!(walked, ["child", "fixed", "children", "end", "nil"]);
    // The step function takes its own state type only.
    raises(
        &lua,
        "local s = types.get('m6.Root').struct_fields local step = pairs(s) step(types.get('m6.Root').attributes)",
        "bad argument #1 (ReadOnlyMapIter expected, got ",
    );
    raises(
        &lua,
        "local n = types.get('m6.Noted').attributes['m6::attr::Note'].namespace local step = pairs(n) step(nil)",
        "bad argument #1 (ReadOnlyArrayIter expected, got nil)",
    );
}

pub fn a_type_from_another_registry_resolves_only_to_the_same_type() {
    let wiring = wiring(FakePort::new(synthetic()));
    let lua = lua_with(&wiring);
    // A second registry in the same VM: same first types, then a different type 2.
    let mut changed = synthetic();
    changed[2].qualified = "m6.Other".into();
    changed[2].qualified_hash = fnv("m6.Other");
    let other = types::wire(FakePort::new(changed), |_, _| {});
    let table = other.game_types.types(&lua, &package("x")).unwrap();
    lua.globals().set("foreign", table).unwrap();
    assert!(eval::<bool>(
        &lua,
        "local f = foreign.get_by_index(1) local t = types.resolve(f) return t == types.get('m6.Base') and t ~= f"
    ));
    let (value, reason): (Value, String) =
        eval(&lua, "return types.resolve(foreign.get('m6.Other'))");
    assert!(value.is_nil());
    assert_eq!(reason, "type belongs to another registry");
    raises(
        &lua,
        "types.get('m6.Child'):is_a(foreign.get('m6.Other'))",
        "type belongs to another registry",
    );
}

pub fn a_type_outlives_its_session_as_metadata_only() {
    let wiring = wiring(FakePort::new(synthetic()));
    let lua = lua_with(&wiring);
    lua.load("kept = types.get('m6.Child') types = nil")
        .exec()
        .unwrap();
    lua.gc_collect().unwrap();
    lua.gc_collect().unwrap();
    // The package's table was the session's only owner here.
    assert!(eval::<bool>(
        &lua,
        "return kept.qualified_name == 'm6.Child' and kept.size == 8"
    ));
    raises(
        &lua,
        "local _ = kept.inner_type",
        "the type registry session of this Type has ended",
    );
}

// ---- ShroudForge-format registries ----

/// A registry in ShroudForge's own serialized form (camelCase keys, SCREAMING primitive
/// names, flags as "A | B" text, struct fields as a map), as its registry tests write
/// it, turned into schema pages.
fn from_shroudforge(registry: Json) -> Vec<T> {
    let lua_name = |screaming: &str| -> &'static str {
        let squash = |s: &str| s.replace('_', "").to_ascii_uppercase();
        let wanted = squash(screaming);
        [
            "None",
            "Bool",
            "UInt8",
            "SInt8",
            "UInt16",
            "SInt16",
            "UInt32",
            "SInt32",
            "UInt64",
            "SInt64",
            "Float32",
            "Float64",
            "Enum",
            "Bitmask8",
            "Bitmask16",
            "Bitmask32",
            "Bitmask64",
            "Typedef",
            "Struct",
            "StaticArray",
            "DsArray",
            "DsString",
            "DsOptional",
            "DsVariant",
            "BlobArray",
            "BlobString",
            "BlobOptional",
            "BlobVariant",
            "ObjectReference",
            "Guid",
        ]
        .into_iter()
        .find(|name| squash(name) == wanted)
        .unwrap_or_else(|| panic!("{screaming}"))
    };
    registry["types"]
        .as_array()
        .unwrap()
        .iter()
        .map(|t| {
            let flags = t["flags"]
                .as_str()
                .unwrap_or("")
                .split('|')
                .map(str::trim)
                .filter(|f| !f.is_empty())
                .map(|f| 1u8 << FLAG_NAMES.iter().position(|n| *n == f).unwrap())
                .fold(0, |a, b| a | b);
            let mut fields = Vec::new();
            if let Some(map) = t.get("structFields").and_then(Json::as_object) {
                for f in map.values() {
                    fields.push(F {
                        name: f["name"].as_str().unwrap().into(),
                        ty: f["type"].as_u64().unwrap(),
                        offset: f["dataOffset"].as_u64().unwrap(),
                        attributes: Vec::new(),
                    });
                }
            }
            T {
                name: t["name"].as_str().unwrap().into(),
                qualified: t["qualifiedName"].as_str().unwrap().into(),
                impact: t["impactName"].as_str().unwrap().into(),
                namespace: Vec::new(),
                qualified_hash: t["qualifiedHash"].as_u64().unwrap() as u32,
                internal_hash: Some(t["internalHash"].as_u64().unwrap() as u32),
                name_hash: t["nameHash"].as_u64().unwrap() as u32,
                impact_hash: t["impactHash"].as_u64().unwrap() as u32,
                inner: t.get("innerType").and_then(Json::as_u64),
                size: t["size"].as_u64().unwrap() as u32,
                alignment: t["alignment"].as_u64().unwrap() as u32,
                element_alignment: t["elementAlignment"].as_u64().unwrap() as u32,
                field_count: t["fieldCount"].as_u64().unwrap() as u32,
                primitive: lua_name(t["primitiveType"].as_str().unwrap()),
                flags,
                default: t
                    .get("defaultValue")
                    .and_then(Json::as_array)
                    .map(|b| b.iter().map(|x| x.as_u64().unwrap() as u8).collect()),
                fields,
                enums: Vec::new(),
                attributes: Vec::new(),
            }
        })
        .collect()
}

/// ShroudForge's registry test fixture (resolve_type.rs \`tests::fixture\`, both pins).
fn shroudforge_fixture() -> Json {
    let mut types = Vec::new();
    for (i, (name, kind, size, inner, flags)) in [
        ("U32", "UINT32", 4, None, ""),
        ("Base", "STRUCT", 4, None, ""),
        ("Child", "STRUCT", 8, Some(1), ""),
        ("Array", "STATIC_ARRAY", 8, Some(0), ""),
        ("DsArray", "DS_ARRAY", 24, Some(0), "HAS_DS"),
        ("HasDs", "STRUCT", 24, None, "HAS_DS"),
    ]
    .into_iter()
    .enumerate()
    {
        let qualified = format!("test::{name}");
        types.push(json!({
            "index": i, "name": name, "qualifiedName": qualified, "impactName": name,
            "size": size, "alignment": 4, "elementAlignment": 4,
            "fieldCount": if i == 3 { 2 } else { 0 },
            "primitiveType": kind, "innerType": inner, "flags": flags,
            "nameHash": fnv(name), "qualifiedHash": fnv(&qualified),
            "impactHash": fnv(name), "internalHash": 99,
            "defaultValue": vec![0; size]
        }));
    }
    types[1]["structFields"] = json!({"x": {"name": "x", "type": 0, "dataOffset": 0}});
    types[2]["structFields"] = json!({"y": {"name": "y", "type": 0, "dataOffset": 4}});
    types[5]["structFields"] = json!({"items": {"name": "items", "type": 4, "dataOffset": 0}});
    types[2]["defaultValue"] = json!([17, 0, 0, 0, 23, 0, 0, 0]);
    json!({"version": "fixture", "types": types})
}

pub fn shroudforge_s_own_registry_cases_hold_through_lua() {
    // inheritance_is_not_element_containment_and_fields_keep_owners
    let wiring = wiring(FakePort::new(from_shroudforge(shroudforge_fixture())));
    let lua = lua_with(&wiring);
    assert!(eval::<bool>(
        &lua,
        "local c = types.get('test::Child') local f = c.fields \
         return c:is_a('test::Base') and not types.get('test::Array'):is_a('test::U32') \
         and #f == 2 and f[1].declaring_type.index == 1 and f[1].name == 'x' and f[1].data_offset == 0 \
         and f[2].declaring_type.index == 2 and f[2].name == 'y' and f[2].data_offset == 4"
    ));
    let bytes: mlua::String = eval(&lua, "return types.get('test::Child').default_bytes");
    assert_eq!(bytes.as_bytes().as_ref(), [17, 0, 0, 0, 23, 0, 0, 0]);
    // selectors_reject_collisions_wrong_domains_and_invalid_numbers
    let (_, ambiguous): (Value, String) = eval(
        &lua,
        "return types.resolve({hash = 99, domain = 'internal'})",
    );
    assert!(ambiguous.to_ascii_lowercase().contains("ambiguous"));
    let (value, _): (Value, String) = eval(
        &lua,
        "return types.resolve({hash = 99, domain = 'address'})",
    );
    assert!(value.is_nil());
    assert!(eval::<bool>(
        &lua,
        "return types.resolve(-1) == nil and types.resolve(4294967296) == nil"
    ));
    assert!(eval::<bool>(
        &lua,
        &format!("return types.resolve({}).index == 2", fnv("test::Child"))
    ));
    // DS types have no Impact entry (U32's Impact name is unique, DsArray's is skipped).
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_impact_name('U32').index == 0 and types.get_by_impact_name('DsArray') == nil \
         and #types.get_all(false) == 4"
    ));

    // cycles_are_reported_instead_of_looping
    let mut cyclic = shroudforge_fixture();
    cyclic["types"][1]["innerType"] = json!(2);
    let wiring = types::wire(FakePort::new(from_shroudforge(cyclic)), |_, _| {});
    let lua = lua_with(&wiring);
    raises(&lua, "local _ = types.get('test::Child').fields", "cyclic");

    // qualified_hash_resolves_while_shared_internal_hash_is_rejected
    let shared: Vec<Json> = (0..2)
        .map(|index| {
            json!({
                "index": index, "name": format!("Type{index}"),
                "impactName": format!("ecs.Type{index}"),
                "qualifiedName": format!("keen::ecs::Type{index}"),
                "size": 4, "alignment": 4, "elementAlignment": 4,
                "fieldCount": 0, "primitiveType": "STRUCT", "flags": "",
                "nameHash": 100 + index, "impactHash": 200 + index,
                "qualifiedHash": 300 + index, "internalHash": 777
            })
        })
        .collect();
    let wiring = types::wire(
        FakePort::new(from_shroudforge(
            json!({"version": "test", "types": shared}),
        )),
        |_, _| {},
    );
    let lua = lua_with(&wiring);
    assert!(eval::<bool>(
        &lua,
        "return types.get_by_qualified_hash(301).qualified_name == 'keen::ecs::Type1' \
         and types.get_by_qualified_hash(302) == nil and select(2, types.get_by_internal_hash(777)):lower():find('ambiguous') ~= nil"
    ));
}

// ---- the kiln.types cross-check ----

struct Natives(Result<Vec<(&'static str, u32)>, &'static str>);
impl NativeTypes for Natives {
    fn types(&self) -> Result<Vec<NativeType>, String> {
        self.0
            .clone()
            .map(|list| {
                list.into_iter()
                    .map(|(name, size)| NativeType {
                        qualified_name: name.as_bytes().to_vec(),
                        size,
                    })
                    .collect()
            })
            .map_err(str::to_owned)
    }
}

/// An archive registry with the fake kiln.types' three types.
fn fake_native_registry() -> Vec<T> {
    fake_services::FAKE_TYPES
        .iter()
        .map(|(qualified, size)| {
            let name = qualified.rsplit("::").next().unwrap();
            let mut t = ty(name, "UInt32", *size);
            t.qualified = (*qualified).into();
            t.qualified_hash = fnv(qualified);
            t
        })
        .collect()
}

pub fn the_cross_check_agrees_skips_or_refuses() {
    let registry = Registry::load(&FakePort::new(fake_native_registry())).unwrap();
    let all = fake_services::FAKE_TYPES.to_vec();
    assert_eq!(
        registry.cross_check(&Natives(Ok(all.clone()))),
        Ok(CrossCheck::Agreed { count: 3 })
    );
    // Order is not compared: names and sizes are.
    let mut reversed = all.clone();
    reversed.reverse();
    assert_eq!(
        registry.cross_check(&Natives(Ok(reversed))),
        Ok(CrossCheck::Agreed { count: 3 })
    );
    assert_eq!(
        registry.cross_check(&Natives(Err("kiln.types: not provided"))),
        Ok(CrossCheck::Skipped("kiln.types: not provided".into()))
    );
    for (natives, expected) in [
        (
            all[..2].to_vec(),
            "kiln.types has 2 types, the archive registry 3",
        ),
        (
            vec![all[0], all[1], ("fake::Other", 8)],
            "the archive registry has no type fake::Other",
        ),
        (
            vec![all[0], all[1], ("fake::Child", 12)],
            "fake::Child is 12 bytes in kiln.types and 8 in the archive registry",
        ),
        (
            vec![all[0], all[1], all[1]],
            "kiln.types lists fake::Base twice",
        ),
    ] {
        match registry.cross_check(&Natives(Ok(natives))) {
            Err(LoadError::Disagrees(why)) => assert_eq!(why, expected),
            other => panic!("{expected}: {other:?}"),
        }
    }
}

fn adapter() -> (ServiceAdapter, Box<KilnApi>) {
    let api = fake_services::api(true);
    let host = unsafe { Host::read(&*api) };
    (
        ServiceAdapter::new(host, Rc::new(Watchdog::new(process_clock()))),
        api,
    )
}

pub fn kiln_types_is_read_through_the_adapter_with_one_call_per_member() {
    let (services, _api) = adapter();
    let owner = next_owner();
    fake_services::bind_thread(owner);
    let natives = KilnTypes::new(&services).types().unwrap();
    assert_eq!(
        natives
            .iter()
            .map(|n| (String::from_utf8(n.qualified_name.clone()).unwrap(), n.size))
            .collect::<Vec<_>>(),
        fake_services::FAKE_TYPES
            .iter()
            .map(|(n, s)| (n.to_string(), *s))
            .collect::<Vec<_>>()
    );
    let counters = fake_services::counters(owner);
    assert_eq!(
        (
            counters.calls("types.status"),
            counters.calls("types.type_count"),
            counters.calls("types.type_at"),
            counters.calls("types.type_info"),
            counters.retries,
            counters.hops,
        ),
        (1, 1, 3, 3, 0, 0)
    );
    let registry = Registry::load(&FakePort::new(fake_native_registry())).unwrap();
    assert_eq!(
        registry.cross_check(&KilnTypes::new(&services)),
        Ok(CrossCheck::Agreed { count: 3 })
    );
    // kiln.types missing, too old, without type_info, or unavailable: skipped, never
    // agreed, with no further call.
    for (shape, expected) in [
        (
            Some(fake_services::Shape::Missing),
            "kiln.types: not provided",
        ),
        (
            Some(fake_services::Shape::Version(0)),
            "kiln.types: version 0, 1 needed",
        ),
        (
            Some(fake_services::Shape::NullAt(std::mem::offset_of!(
                kiln_abi::services::kiln_types::KilnTypesApi,
                type_info
            ))),
            "kiln.types.type_info: not provided",
        ),
    ] {
        fake_services::set_shape("kiln.types", shape);
        match registry.cross_check(&KilnTypes::new(&services)) {
            Ok(CrossCheck::Skipped(why)) => assert_eq!(why, expected),
            other => panic!("{expected}: {other:?}"),
        }
    }
    fake_services::set_shape("kiln.types", None);
    fake_services::set_knob(owner, fake_services::Knob::Unavailable);
    let before = fake_services::counters(owner).total_calls();
    match registry.cross_check(&KilnTypes::new(&services)) {
        Ok(CrossCheck::Skipped(why)) => {
            assert!(why.contains("the type table is not available"), "{why}")
        }
        other => panic!("{other:?}"),
    }
    assert_eq!(
        fake_services::counters(owner).total_calls(),
        before + 1,
        "status only"
    );
    // A registry that disagrees with the fake kiln.types is refused.
    fake_services::set_knob(owner, fake_services::Knob::Ok);
    let mut wrong = fake_native_registry();
    wrong[2].size = 12;
    let registry = Registry::load(&FakePort::new(wrong)).unwrap();
    assert!(matches!(
        registry.cross_check(&KilnTypes::new(&services)),
        Err(LoadError::Disagrees(_))
    ));
}

// ---- the wiring, through the worker ----

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
fn note(owner: KilnOwner, text: String) {
    RECORDS.lock().unwrap().entry(owner).or_default().push(text);
}
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

#[derive(Clone, Copy)]
enum Archive {
    /// The fake kiln.types' three types: the cross-check agrees.
    Agreeing,
    /// Child is 12 bytes: kiln.types disagrees.
    Disagreeing,
    /// The registry cannot be read.
    Unreadable,
    /// kiln.types answers no type table: the cross-check is skipped.
    NoNativeTable,
}
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, Archive>>> = LazyLock::new(Default::default);
thread_local! {
    /// The types family of the wiring the plan made (plan and families run on the worker
    /// thread at one runtime-create, in that order).
    static FAMILY: RefCell<Option<Family>> = const { RefCell::new(None) };
}

struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(
                b"record(table.concat({tostring(game.types == runtime.types), game.version, \
                  game.types.version, game.types.count, game.types.get('fake::Child').size}, '|')) \
                  return {}"
                    .to_vec(),
            )
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct NoCleanup;
impl Cleanup for NoCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}
struct Runtime(RuntimeOwnerContext);
impl RuntimeHost for Runtime {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.0
    }
    fn static_support(&self, _: RuntimeOperation) -> Availability {
        Availability::Available
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        RuntimeProviderFacts::default()
    }
    fn lifecycle_active(&self) -> bool {
        true
    }
    fn write_fault(&self) -> Option<String> {
        None
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}

const IDS: [&str; 2] = ["a", "b"];
struct TypesPlan;
impl PlanSource for TypesPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let archive = SCENARIOS.lock().unwrap()[&cx.owner()];
        let mut types = fake_native_registry();
        let mut port = FakePort::new(Vec::new());
        match archive {
            Archive::Agreeing => {}
            Archive::Disagreeing => types[2].size = 12,
            Archive::Unreadable => port.fail_at = Some(0),
            Archive::NoNativeTable => {
                fake_services::bind_thread(cx.owner());
                fake_services::set_knob(cx.owner(), fake_services::Knob::Unavailable);
            }
        }
        port.types = Rc::new(types);
        // KilnApi.log under kiln-sfi's owner, as the production plan source wires it.
        let (host, owner) = (*cx.shared().host(), cx.owner());
        let wiring = types::wire(port, move |level, text| host.log(owner, level, text));
        FAMILY.with_borrow_mut(|slot| *slot = Some(wiring.family));
        let mut registry = Packages::default();
        for id in IDS {
            let source: Rc<dyn PackageSource> = Rc::new(Source);
            let mut parsed = package::Package::parse(
                json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                    .to_string()
                    .as_bytes(),
                None,
                package::ParseOptions::default(),
            )
            .unwrap();
            parsed.manifest.enabled = true;
            registry
                .insert(
                    PackageRecord::new(
                        id.into(),
                        parsed,
                        RuntimePackage::new(id.to_owned(), source.clone()),
                        source,
                    )
                    .unwrap(),
                )
                .unwrap();
        }
        let runtime: RuntimeHosts =
            Rc::new(|manifest: &package::Manifest, context: &HostContext| {
                Rc::new(Runtime(RuntimeOwnerContext::new(
                    manifest, context, false, false, "r05",
                ))) as Rc<dyn RuntimeHost>
            });
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry,
            plan: IDS.map(String::from).to_vec(),
            admitted: IDS.map(String::from).into(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: ComposePlan {
                game_types: Some(wiring.game_types),
                runtime: Some(runtime),
                withdrawn: Vec::new(),
            },
        })
    }
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        let types = FAMILY
            .with_borrow_mut(Option::take)
            .expect("the plan made the wiring");
        let record = Family::new("record", &[], move |lua, env, _, _, _| {
            env.raw_set(
                "record",
                lua.create_function(move |_, text: String| {
                    note(owner, text);
                    Ok(())
                })?,
            )
        });
        Families::new(vec![types, record])
    }
}

struct Run {
    owner: KilnOwner,
    pool: std::sync::Arc<FakePool>,
    shared: std::sync::Arc<WorkerShared>,
}
fn start(archive: Archive) -> Run {
    let owner = next_owner();
    SCENARIOS.lock().unwrap().insert(owner, archive);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<TypesPlan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    Run {
        owner,
        pool,
        shared,
    }
}

pub fn the_wiring_gives_every_package_game_types_runtime_types_and_game_version() {
    let run = start(Archive::Agreeing);
    assert!(
        eventually(WAIT, || records(run.owner).len() == 2),
        "{:?} {:?}",
        records(run.owner),
        run.shared.status()
    );
    let line = format!("true|{GAME_VERSION}|{REGISTRY_VERSION}|3|8");
    assert_eq!(records(run.owner), [line.clone(), line]);
    assert!(
        !logs(run.owner)
            .iter()
            .any(|(_, text)| text.starts_with(types::SKIPPED)),
        "the cross-check ran: no skip line"
    );
    run.shared.request_stop();
    assert!(run.pool.task(0).wait_finished(WAIT));
}

pub fn an_unreadable_kiln_types_skips_the_cross_check_with_one_info_line() {
    let run = start(Archive::NoNativeTable);
    assert!(
        eventually(WAIT, || records(run.owner).len() == 2),
        "{:?} {:?}",
        records(run.owner),
        run.shared.status()
    );
    // The archive registry is the authority: both packages run on it.
    let line = format!("true|{GAME_VERSION}|{REGISTRY_VERSION}|3|8");
    assert_eq!(records(run.owner), [line.clone(), line]);
    let skipped: Vec<(i32, String)> = logs(run.owner)
        .into_iter()
        .filter(|(_, text)| text.starts_with(types::SKIPPED))
        .collect();
    assert_eq!(
        skipped,
        [(
            KILN_LOG_INFO,
            format!(
                "{}: kiln.types: the type table is not available (status {}, types 0)",
                types::SKIPPED,
                KILN_ERR_UNAVAILABLE
            )
        )],
        "one INFO line for the runtime-create, not one per package"
    );
    run.shared.request_stop();
    assert!(run.pool.task(0).wait_finished(WAIT));
}

pub fn a_refused_registry_stops_runtime_create_before_any_package_code() {
    for (archive, reason) in [
        (
            Archive::Disagreeing,
            "kiln.types disagrees with the archive registry: fake::Child is 8 bytes in kiln.types and 12 in the archive registry",
        ),
        (
            Archive::Unreadable,
            "registry_metadata failed (3): the generation was retired",
        ),
    ] {
        let run = start(archive);
        assert!(run.pool.task(0).wait_finished(WAIT));
        let status = run.shared.status();
        assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
        let detail = status.detail.unwrap_or_default();
        assert!(
            detail.contains(&format!("{}: {reason}", types::REGISTRY_FAILED)),
            "{detail}"
        );
        assert!(records(run.owner).is_empty());
    }
}

// ---- part B: the port on kiln.eml.archive ----

/// An adapter whose lookup also serves the fake kiln.eml.archive and kiln.caps `find`.
fn archive_adapter() -> (Rc<ServiceAdapter>, Box<KilnApi>) {
    let mut api = fake_services::api(true);
    fake_archive::wrap(&mut api);
    let host = unsafe { Host::read(&*api) };
    (
        Rc::new(ServiceAdapter::new(
            host,
            Rc::new(Watchdog::new(process_clock())),
        )),
        api,
    )
}

fn pages_of(types: Vec<T>) -> fake_archive::Pages {
    let types = std::sync::Arc::new(types);
    std::sync::Arc::new(move |first, limit| {
        serde_json::to_vec(&page_json(&types, first, limit)).unwrap()
    })
}

/// A fresh owner granted the synthetic registry, and a port on it expecting `expected`.
fn archive_port(
    grant: impl FnOnce(&mut fake_archive::Grant),
    expected: kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1,
) -> (KilnOwner, ArchiveRegistryPort, Box<KilnApi>) {
    let owner = next_owner();
    let mut granted =
        fake_archive::Grant::new(fake_archive::fixture_identity(), pages_of(synthetic()));
    grant(&mut granted);
    fake_archive::grant(owner, granted);
    let (services, api) = archive_adapter();
    (
        owner,
        ArchiveRegistryPort::new(services, owner, expected),
        api,
    )
}

fn page_len(first: u64, limit: u32) -> usize {
    serde_json::to_vec(&page_json(&synthetic(), first, limit))
        .unwrap()
        .len()
}

pub fn the_archive_port_acquires_with_the_grant_checks_identity_and_sizes_every_page() {
    let (owner, port, _api) = archive_port(|_| {}, fake_archive::fixture_identity());
    assert_eq!(
        port.expected_pin(),
        Some(pin()),
        "the grant is the expected pin"
    );
    assert_eq!(
        port.view(),
        None,
        "nothing is acquired before the first page"
    );
    let wiring = types::wire(port, |_, _| {});
    let lua = lua_with(&wiring);
    assert_eq!(eval::<i64>(&lua, "return types.count"), 15);
    assert_eq!(
        eval::<String>(&lua, "return types.get('m6.Child').name"),
        "Child"
    );
    let seen = fake_archive::seen(owner);
    assert_eq!(seen.views.len(), 1);
    let view = seen.views[0];
    // Every page: a sizing call with no buffer, then exactly the complete length.
    assert_eq!(
        seen.calls,
        [
            "acquire".to_owned(),
            "identity".to_owned(),
            "registry_metadata(0,0,0,record)".to_owned(),
            format!("registry_metadata(0,0,{},record)", page_len(0, 0)),
            // the loader asks for the 15 remaining types
            "registry_metadata(0,15,0,record)".to_owned(),
            format!("registry_metadata(0,15,{},record)", page_len(0, 15)),
        ]
    );
    assert!(seen.released.is_empty(), "held while the wiring lives");
    drop(lua);
    drop(wiring);
    let seen = fake_archive::seen(owner);
    assert_eq!(seen.released, [view], "released once, when the port drops");
    assert_eq!(seen.calls.last().unwrap(), &format!("release({view})"));
    assert!(seen.tokens.is_empty());
}

pub fn an_identity_the_archive_did_not_grant_is_refused_and_a_lying_view_released() {
    use kiln_abi::services::kiln_eml_archive::KILN_EML_ARCHIVE_PREPARED;
    // Every pin counts: another generation, epoch, digest or phase is not the grant.
    let edits: [fn(&mut kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1); 5] = [
        |i| i.generation = 8,
        |i| i.reload_epoch = 3,
        |i| i.registry_sha256[31] ^= 1,
        |i| i.launch_id[0] ^= 1,
        |i| i.phase = KILN_EML_ARCHIVE_PREPARED as u32,
    ];
    for edit in edits {
        let mut expected = fake_archive::fixture_identity();
        edit(&mut expected);
        let (owner, port, _api) = archive_port(|_| {}, expected);
        let refused = PortError::Failed {
            member: "acquire",
            result: KILN_ERR_UNAVAILABLE,
            message: None,
        };
        assert_eq!(port.page(0, 0), Err(refused.clone()));
        // remembered, never tried twice
        assert_eq!(port.page(0, 256), Err(refused));
        assert_eq!(fake_archive::seen(owner).calls, ["acquire"]);
        drop(port);
        assert!(fake_archive::seen(owner).released.is_empty());
    }
    // The archive grants a view whose identity is not the one asked for: released at once.
    let (owner, port, _api) = archive_port(
        |grant| {
            let mut other = grant.identity;
            other.registry_id = 4;
            grant.lie = Some(other);
        },
        fake_archive::fixture_identity(),
    );
    assert_eq!(port.page(0, 0), Err(PortError::IdentityMismatch));
    assert_eq!(
        Registry::load(&port).err().unwrap().to_string(),
        "kiln.eml.archive.identity: the view is not pinned to this start's launch identity"
    );
    let seen = fake_archive::seen(owner);
    assert_eq!(seen.released, seen.views, "the one view, released once");
    assert!(
        !seen
            .calls
            .iter()
            .any(|c| c.starts_with("registry_metadata"))
    );
    drop(port);
    assert_eq!(
        fake_archive::seen(owner).released.len(),
        1,
        "not again on drop"
    );
}

pub fn a_page_that_grows_after_its_sizing_call_is_refused() {
    let (owner, port, _api) = archive_port(|g| g.grow = 5, fake_archive::fixture_identity());
    let len = page_len(0, 0);
    assert_eq!(
        port.page(0, 0),
        Err(PortError::Failed {
            member: "registry_metadata",
            result: KILN_ERR_BUFFER_TOO_SMALL,
            message: Some(format!(
                "the page needs {} bytes after the sizing call said {len}",
                len + 5
            )),
        })
    );
    // two calls for the page, no third
    assert_eq!(
        fake_archive::seen(owner)
            .calls
            .iter()
            .filter(|c| c.starts_with("registry_metadata"))
            .count(),
        2
    );
}

pub fn a_failed_page_carries_the_provider_s_text_copied_and_released_exactly_once() {
    let text = b"generation 7 was retired\0 by the provider".to_vec();
    let (owner, port, _api) = archive_port(
        |g| g.fail_at = Some((0, KILN_ERR_REVOKED, text.clone())),
        fake_archive::fixture_identity(),
    );
    assert!(port.page(0, 0).is_ok(), "the header page reads");
    assert_eq!(
        port.page(0, 256),
        Err(PortError::Failed {
            member: "registry_metadata",
            result: KILN_ERR_REVOKED,
            message: Some(String::from_utf8_lossy(&text).into_owned()),
        })
    );
    let seen = fake_archive::seen(owner);
    assert_eq!(seen.tokens.len(), 1);
    let token = seen.tokens[0];
    assert_eq!(
        seen.token_releases.get(&token),
        Some(&1),
        "released exactly once"
    );
    assert_eq!(
        seen.calls[seen.calls.len() - 3..],
        [
            format!("error_copy({token},0)"),
            format!("error_copy({token},{})", text.len()),
            format!("error_release({token})"),
        ]
    );
    // Through the loader the refusal says the member, its result and the text.
    let (_, port, _api) = archive_port(
        |g| g.fail_at = Some((0, KILN_ERR_REVOKED, b"retired".to_vec())),
        fake_archive::fixture_identity(),
    );
    assert_eq!(
        Registry::load(&port).err().unwrap().to_string(),
        format!("registry_metadata failed ({KILN_ERR_REVOKED}): retired")
    );

    // Without the errors channel (a member row withheld) the record is NULL: no token is
    // issued, so none can leak, and the failure has no text.
    fake_archive::set_row(
        "eml.archive.errors.error_release",
        Some(fake_archive::Row::Unreported),
    );
    let (owner, port, _api) = archive_port(
        |g| g.fail_at = Some((0, KILN_ERR_REVOKED, text.clone())),
        fake_archive::fixture_identity(),
    );
    assert!(port.page(0, 0).is_ok());
    assert_eq!(
        port.page(0, 256),
        Err(PortError::Failed {
            member: "registry_metadata",
            result: KILN_ERR_REVOKED,
            message: None,
        })
    );
    let seen = fake_archive::seen(owner);
    assert!(seen.tokens.is_empty());
    assert!(seen.calls.iter().all(|c| !c.starts_with("error_")));
    assert!(
        seen.calls
            .iter()
            .filter(|c| c.starts_with("registry_metadata"))
            .all(|c| c.ends_with(",null)")),
        "{:?}",
        seen.calls
    );
    fake_archive::set_row("eml.archive.errors.error_release", None);
}

pub fn missing_rows_or_slots_are_unavailable_before_anything_is_acquired() {
    use fake_archive::{Row, Table};
    use kiln_sfi::runtime::services::{ServiceError, Unavailable};
    let refused = |expected: PortError| {
        let (owner, port, _api) = archive_port(|_| {}, fake_archive::fixture_identity());
        assert_eq!(port.page(0, 0), Err(expected.clone()));
        assert!(
            fake_archive::seen(owner).calls.is_empty(),
            "{expected}: no call before the checks pass"
        );
    };
    for row in fake_archive::ROWS[..5].iter().copied() {
        fake_archive::set_row(row, Some(Row::Unreported));
        refused(PortError::Withheld {
            row,
            text: format!("nobody reported it ({KILN_ERR_NOT_FOUND})"),
        });
        fake_archive::set_row(row, None);
    }
    fake_archive::set_row(
        "eml.archive.metadata",
        Some(Row::State(
            kiln_abi::services::kiln_caps::KILN_CAP_PENDING,
            "game test pending: archive reads",
        )),
    );
    refused(PortError::Withheld {
        row: "eml.archive.metadata",
        text: "it is pending: game test pending: archive reads".into(),
    });
    fake_archive::set_row(
        "eml.archive.metadata.acquire",
        Some(Row::State(
            kiln_abi::services::kiln_caps::KILN_CAP_UNAVAILABLE,
            "",
        )),
    );
    fake_archive::set_row("eml.archive.metadata", None);
    refused(PortError::Withheld {
        row: "eml.archive.metadata.acquire",
        text: "it is unavailable".into(),
    });
    fake_archive::set_row("eml.archive.metadata.acquire", None);

    let unavailable = |why| {
        PortError::Unavailable(ServiceError {
            service: "kiln.eml.archive",
            why,
        })
    };
    fake_archive::set_table(Some(Table::Missing));
    refused(unavailable(Unavailable::Missing));
    fake_archive::set_table(Some(Table::Edited(|t| t.registry_metadata = None)));
    refused(unavailable(Unavailable::Null {
        member: "registry_metadata",
    }));
    fake_archive::set_table(Some(Table::Edited(|t| t.header.version = 0)));
    refused(unavailable(Unavailable::Version { have: 0, need: 1 }));
    fake_archive::set_table(None);
}

pub fn a_page_beyond_page_max_or_of_zero_bytes_is_refused_before_it_is_read() {
    use kiln_sfi::runtime::types::archive::PAGE_MAX;
    let metadata_calls = |owner| -> Vec<String> {
        fake_archive::seen(owner)
            .calls
            .into_iter()
            .filter(|c| c.starts_with("registry_metadata"))
            .collect()
    };
    for claimed in [PAGE_MAX + 1, usize::MAX, 0] {
        let (owner, port, _api) = archive_port(
            |g| g.sizing = Some(claimed),
            fake_archive::fixture_identity(),
        );
        assert_eq!(
            port.page(0, 0),
            Err(PortError::Failed {
                member: "registry_metadata",
                result: KILN_ERR_BUFFER_TOO_SMALL,
                message: Some(format!(
                    "a page of {claimed} bytes is outside what this reader takes (1 to {PAGE_MAX})"
                )),
            })
        );
        assert_eq!(
            metadata_calls(owner),
            ["registry_metadata(0,0,0,record)"],
            "{claimed}: nothing allocated or read after the sizing call"
        );
    }
    // The cap is inclusive: a page of exactly PAGE_MAX bytes is read (this fake then
    // writes its real, shorter page, which the exact-length check refuses).
    let (owner, port, _api) = archive_port(
        |g| g.sizing = Some(PAGE_MAX),
        fake_archive::fixture_identity(),
    );
    let len = page_len(0, 0);
    assert_eq!(
        port.page(0, 0),
        Err(PortError::Failed {
            member: "registry_metadata",
            result: KILN_ERR_INTERNAL,
            message: Some(format!(
                "the page is {len} bytes after the sizing call said {PAGE_MAX}"
            )),
        })
    );
    assert_eq!(
        metadata_calls(owner),
        [
            "registry_metadata(0,0,0,record)".to_owned(),
            format!("registry_metadata(0,0,{PAGE_MAX},record)"),
        ]
    );
}
