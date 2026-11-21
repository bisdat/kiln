// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs entity calls, the tests' shared fixture.
//! What the runtime.ecs entity tests (tests/runtime_ecs_reads.rs, tests/runtime_ecs_writes.rs) share:
//! - an archive registry of test types (registry_metadata pages, schema v1) and kiln.types' catalog over its
//!   keen::ecs types (every row proven), so the registry join resolves them;
//! - kiln.world's ECS world fake (support/fake_services [`EcsWorld`]): entities, components, templates, sessions and
//!   the compare-exchange steps, behind the real service adapter;
//! - a scripted registrar host (every fact on, the lifecycle active, the contract) and the production runtime facts
//!   (the session write latch and the ownership store);
//! - the VM's typed values on the scripted kiln.eml.values provider (support/fake_values) with a small codec: BYTES of
//!   keen::UInt32, keen::SInt64 and keen::Float32 open as Lua scalars, of a keen::ecs:: type as a mapped struct whose
//!   SOURCE_BYTES are the bytes it was opened with; LUA_INPUT of a Lua number as a scalar node that value_export
//!   serializes as the opened type (little-endian), any other input refused as Source refuses it.
#![allow(dead_code, unused_imports)]

#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_services/mod.rs"]
pub mod fake_services;
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_values/mod.rs"]
pub mod fake_values;
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
pub mod support;

pub use fake_services::{
    CasStep, ECS_TYPE_BASE, EcsCatalog, EcsEntity, EcsWorld, FAKE_SESSION, Knob,
};
pub use kiln_abi::services::kiln_eml_values::*;
pub use kiln_abi::services::kiln_world::*;
pub use kiln_abi::*;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::availability::{FactsConfig, RuntimeFacts};
use kiln_sfi::runtime::ecs::{Ecs, EcsRuntime, install_into};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::types::{self, PortError, TypeRegistryPort, TypesWiring};
use kiln_sfi::runtime::values::{ArenaOptions, ValuesArena};
use kiln_sfi::worker::Host;
pub use mlua::{FromLuaMulti, Table, Value};
use serde_json::{Value as Json, json};
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation as Op, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::host::{HostContext, Phase, Side, SourceVersion};
use sfi_core::package::{self, Manifest};
pub use std::cell::{Cell, RefCell};
use std::collections::HashMap;
pub use std::rc::Rc;

pub const BUILD: &str = "test-build";
pub const SERVER: u32 = KILN_WORLD_SERVER as u32;
pub const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";
/// ShroudForge's readiness text; the licence guard keeps the prefix out of MIT sources.
pub const NOT_READY: &str = concat!("K", "FC Runtime is not ready");

pub fn fnv(text: &str) -> u32 {
    text.bytes().fold(0x811c_9dc5_u32, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

// ---- the archive registry ----

#[derive(Clone)]
pub struct T {
    pub qualified: &'static str,
    pub primitive: &'static str,
    pub size: u32,
    pub inner: Option<u64>,
    pub count: u32,
    pub fields: Vec<(&'static str, u64, u64)>,
    pub flags: u8,
}
fn t(qualified: &'static str, primitive: &'static str, size: u32) -> T {
    T {
        qualified,
        primitive,
        size,
        inner: None,
        count: 0,
        fields: Vec::new(),
        flags: 0,
    }
}
impl T {
    fn field(mut self, name: &'static str, ty: usize, offset: u64) -> Self {
        self.fields.push((name, ty as u64, offset));
        self
    }
    fn array(mut self, inner: usize, count: u32) -> Self {
        self.inner = Some(inner as u64);
        self.count = count;
        self
    }
}

pub const UINT32: usize = 0;
pub const FLOAT32: usize = 1;
pub const SINT64: usize = 2;
pub const HEALTH: &str = "keen::ecs::Health";
pub const TRANSFORM: &str = "keen::ecs::CurrentTransform";
pub const TAGGED: &str = "keen::ecs::Tagged";
pub const COUNTERS: &str = "keen::ecs::Counters";
pub const TEMPLATE: &str = "keen::ecs::TemplateResource";
pub const OTHER: &str = "keen::Other";
pub const UNREGISTERED: &str = "keen::ecs::Unregistered";
pub const ATTRIBUTE_BASE: &str = "keen::ecs::AttributeComponent";
pub const HUNGER: &str = "keen::ecs::Hunger";
pub const LOCKED: &str = "keen::ecs::Locked";
pub const BIG: &str = "keen::ecs::Big";
pub const COMPONENT_BASE: &str = "keen::tpl::Component";
pub const FALL_DAMAGE: &str = "keen::tpl::FallDamage";

/// The reflected types (module docs), in index order.
pub fn archive_types() -> Vec<T> {
    vec![
        t("keen::UInt32", "UInt32", 4),
        t("keen::Float32", "Float32", 4),
        t("keen::SInt64", "SInt64", 8),
        t("keen::Float32x4", "StaticArray", 16).array(FLOAT32, 4),
        t("keen::Float32x3", "StaticArray", 12).array(FLOAT32, 3),
        t("keen::UInt32x3", "StaticArray", 12).array(UINT32, 3),
        T {
            flags: 0x01,
            ..t("keen::DsList", "DsArray", 24).array(UINT32, 0)
        },
        t(HEALTH, "Struct", 8)
            .field("hp", UINT32, 0)
            .field("max", UINT32, 4),
        t(TRANSFORM, "Struct", 56)
            .field("x", SINT64, 0)
            .field("y", SINT64, 8)
            .field("z", SINT64, 16)
            .field("rotation", 3, 24)
            .field("scale", 4, 40)
            .field("pad", UINT32, 52),
        t(TAGGED, "Struct", 28)
            .field("list", 6, 0)
            .field("count", UINT32, 24),
        t(COUNTERS, "Struct", 16)
            .field("values", 5, 0)
            .field("speed", FLOAT32, 12),
        // the template's components: a BlobArray (22) of BlobVariants (21) over the component base (19)
        t(TEMPLATE, "Struct", 8).field("components", 22, 0),
        t(OTHER, "Struct", 4).field("v", UINT32, 0),
        t(UNREGISTERED, "Struct", 4).field("v", UINT32, 0),
        t(ATTRIBUTE_BASE, "Struct", 4).field("header", UINT32, 0),
        T {
            inner: Some(14),
            ..t(HUNGER, "Struct", 8).field("value", FLOAT32, 4)
        },
        t(LOCKED, "Struct", 4).field("v", UINT32, 0),
        t("keen::UInt32x1100", "StaticArray", 4400).array(UINT32, 1100),
        t(BIG, "Struct", 4400).field("v", 17, 0),
        // 19..22: template components (outside keen::ecs, so kiln.types' catalog and the ECS catalog leave them out)
        t(COMPONENT_BASE, "Struct", 0),
        T {
            inner: Some(19),
            ..t(FALL_DAMAGE, "Struct", 8)
                .field("start", FLOAT32, 0)
                .field("lethal", FLOAT32, 4)
        },
        t("keen::tpl::ComponentVariant", "BlobVariant", 12).array(19, 0),
        t("keen::tpl::Components", "BlobArray", 8).array(21, 0),
    ]
}
pub fn index_of(name: &str) -> usize {
    archive_types()
        .iter()
        .position(|t| t.qualified == name)
        .unwrap()
}

fn u64t(value: u64) -> String {
    format!("u64:{value:016x}")
}
/// The effective fields (base first) with the type that declares each.
fn resolved(types: &[T], index: usize) -> Vec<(&'static str, u64, u64, usize)> {
    let ty = &types[index];
    let mut out = match (ty.primitive, ty.inner) {
        ("Struct", Some(base)) => resolved(types, base as usize),
        _ => Vec::new(),
    };
    for (name, field, offset) in &ty.fields {
        match out.iter_mut().find(|f| f.0 == *name) {
            Some(slot) => *slot = (name, *field, *offset, index),
            None => out.push((name, *field, *offset, index)),
        }
    }
    out
}
fn type_json(types: &[T], index: usize) -> Json {
    let t = &types[index];
    let short = t.qualified.rsplit("::").next().unwrap();
    let namespace: Vec<&str> = t.qualified.split("::").filter(|p| *p != short).collect();
    let impact = t.qualified.replace("::", "_");
    let fields: Vec<Json> = t
        .fields
        .iter()
        .map(|(name, ty, offset)| {
            json!({"name": name, "type_index": u64t(*ty), "data_offset": u64t(*offset), "attributes": []})
        })
        .collect();
    let effective: Vec<Json> = resolved(types, index)
        .into_iter()
        .map(|(name, ty, offset, owner)| {
            json!({"name": name, "type_index": u64t(ty), "data_offset": u64t(offset), "attributes": [],
                   "declaring_type": u64t(owner as u64)})
        })
        .collect();
    json!({
        "index": u64t(index as u64),
        "name": short,
        "qualified_name": t.qualified,
        "impact_name": impact,
        "namespace": namespace,
        "hashes": {"qualified": fnv(t.qualified), "internal": 0xa600_0000u32 + index as u32,
                   "name": fnv(short), "impact": fnv(&impact)},
        "inner_type": t.inner.map(u64t),
        "size": t.size,
        "alignment": 4,
        "element_alignment": 4,
        "field_count": if t.primitive == "StaticArray" { t.count as usize } else { t.fields.len() },
        "primitive_type": t.primitive,
        "flags_bits": t.flags,
        "flags": if t.flags & 1 != 0 { vec!["HAS_DS"] } else { vec![] },
        "has_ds": t.flags & 1 != 0,
        "default_bytes": null,
        "struct_fields": fields,
        "fields": effective,
        "enum_fields": [],
        "attributes": [],
    })
}
pub struct Port(pub Vec<T>);
impl TypeRegistryPort for Port {
    fn page(&self, first: u64, limit: u32) -> Result<Vec<u8>, PortError> {
        let count = self.0.len() as u64;
        let end = (first + limit as u64).min(count);
        let tagged = |v: u64| format!("u64:{v:016x}");
        Ok(serde_json::to_vec(&json!({
            "schema_version": 1,
            "pin": {
                "launch_id": "01234567-89ab-4cde-8fab-0123456789ab",
                "plan_id": "fedcba98-7654-4321-8fed-cba987654321",
                "generation": tagged(7),
                "reload_epoch": tagged(2),
                "plan_sha256": "1".repeat(64),
                "archive_sha256": "2".repeat(64),
                "registry_id": tagged(3),
                "registry_sha256": "3".repeat(64),
            },
            "game_version": "1.0.4-archive",
            "registry_version": "registry rev 303",
            "count": tagged(count),
            "first": tagged(first),
            "types": (first.min(count)..end).map(|at| type_json(&self.0, at as usize)).collect::<Vec<_>>(),
        }))
        .unwrap())
    }
}

/// kiln.types' catalog: every keen::ecs type but keen::ecs::Unregistered, each row proven.
pub fn ecs_catalog() -> EcsCatalog {
    let mut catalog = EcsCatalog {
        stamp: 0x303e_c500_0001,
        ..EcsCatalog::default()
    };
    for t in archive_types()
        .into_iter()
        .filter(|t| t.qualified.starts_with("keen::ecs::") && t.qualified != UNREGISTERED)
    {
        let identity = ECS_TYPE_BASE as u64 + catalog.types.len() as u64;
        let hash = fnv(t.qualified);
        catalog.types.push((t.qualified.to_owned(), t.size, hash));
        catalog.rows.push((hash, t.size, 0, identity));
    }
    catalog
}

// ---- the host ----

pub struct Script {
    pub owner: RuntimeOwnerContext,
    pub facts: RefCell<RuntimeProviderFacts>,
    pub active: Cell<bool>,
    pub contract: Cell<bool>,
    pub fault: RefCell<Option<String>>,
}
impl RuntimeHost for Script {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.owner
    }
    fn static_support(&self, operation: Op) -> Availability {
        if self.contract.get() {
            Availability::Available
        } else {
            Availability::Unavailable {
                reason: format!(
                    "operation {} has not been verified for game build {BUILD}",
                    operation.as_str()
                ),
            }
        }
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        self.facts.borrow().clone()
    }
    fn lifecycle_active(&self) -> bool {
        self.active.get()
    }
    fn write_fault(&self) -> Option<String> {
        self.fault.borrow().clone()
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}
pub fn manifest(id: &str, capabilities: &[&str]) -> Manifest {
    package::Package::parse(
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities})
            .to_string()
            .as_bytes(),
        None,
        package::ParseOptions::default(),
    )
    .unwrap()
    .manifest
}
pub fn context() -> HostContext {
    HostContext::new(Phase::Runtime, Side::Server, SourceVersion::Sf179)
}
pub fn script(id: &str) -> Rc<Script> {
    Rc::new(Script {
        owner: RuntimeOwnerContext::new(
            &manifest(id, &["runtime"]),
            &context(),
            false,
            false,
            BUILD,
        ),
        facts: RefCell::new(RuntimeProviderFacts {
            ready: true,
            can_write: true,
            ..RuntimeProviderFacts::default()
        }),
        active: Cell::new(true),
        contract: Cell::new(true),
        fault: RefCell::new(None),
    })
}

// ---- the typed values' codec ----

/// What the codec saw and holds.
#[derive(Default)]
pub struct Codec {
    /// Every value_open: (origin, type, bytes).
    pub opened: Vec<(u32, String, Vec<u8>)>,
    /// A mapped struct's source bytes, by its lease.
    pub sources: HashMap<u64, Vec<u8>>,
    /// What value_export of a lease answers.
    pub exports: HashMap<u64, Vec<u8>>,
    /// Every value_export: (lease, selected type).
    pub exported: Vec<(u64, String)>,
    next_node: u64,
}
fn view(value: Json) -> u64 {
    fake_values::json(json!({"schema_version": 1, "packet": "view", "value": value}))
}
fn scalar_view(qualified: &str, bytes: &[u8]) -> Option<Json> {
    Some(match qualified {
        "keen::UInt32" => {
            json!({"tag": "i64", "value": format!("i64:{:016x}", u32::from_le_bytes(bytes.try_into().ok()?) as u64)})
        }
        "keen::SInt64" => {
            json!({"tag": "i64", "value": format!("i64:{:016x}", i64::from_le_bytes(bytes.try_into().ok()?) as u64)})
        }
        "keen::Float32" => {
            json!({"tag": "f64", "value": format!("f64:{:016x}", (f32::from_le_bytes(bytes.try_into().ok()?) as f64).to_bits())})
        }
        _ => return None,
    })
}
/// The codec on this thread's fake provider (module docs).
pub fn codec() -> Rc<RefCell<Codec>> {
    let codec = Rc::new(RefCell::new(Codec {
        next_node: 0x900,
        ..Codec::default()
    }));
    let c = codec.clone();
    fake_values::on("value_open", move |call| {
        let mut codec = c.borrow_mut();
        codec
            .opened
            .push((call.origin, call.qualified_type.clone(), call.bytes.clone()));
        codec.next_node += 1;
        let node = codec.next_node;
        match call.origin as i32 {
            KILN_VALUE_OPEN_BYTES => {
                if let Some(scalar) = scalar_view(&call.qualified_type, &call.bytes) {
                    let item = fake_values::lease(KILN_VALUE_INTEGER, node);
                    return fake_values::handled(1, item, view(scalar));
                }
                if call.qualified_type.starts_with("keen::ecs::") {
                    let item = fake_values::lease(KILN_VALUE_STRUCT_MAPPED, node);
                    codec.sources.insert(item, call.bytes.clone());
                    return fake_values::handled(1, item, 0);
                }
                fake_values::Answer::Refuse(KILN_ERR_INVALID_ARGUMENT)
            }
            KILN_VALUE_OPEN_LUA_INPUT => {
                drop(codec);
                let (code, described) = fake_values::access(KILN_LUA_DESCRIBE, call.object, b"", 0);
                let mut codec = c.borrow_mut();
                if code != KILN_OK {
                    return fake_values::Answer::Refuse(code);
                }
                let number = match described.kind as i32 {
                    KILN_LUA_INTEGER => described.bits as i64 as f64,
                    KILN_LUA_NUMBER => f64::from_bits(described.bits),
                    _ => {
                        return fake_values::semantic(fake_values::json(json!({
                        "schema_version": 1, "packet": "error",
                        "error": {"level": "source", "class": "runtime", "message": "a number expected"}})));
                    }
                };
                let bytes = match call.qualified_type.as_str() {
                    "keen::UInt32" => (number as u32).to_le_bytes().to_vec(),
                    "keen::SInt64" => (number as i64).to_le_bytes().to_vec(),
                    "keen::Float32" => (number as f32).to_le_bytes().to_vec(),
                    _ => return fake_values::Answer::Refuse(KILN_ERR_INVALID_ARGUMENT),
                };
                let item = fake_values::lease(KILN_VALUE_NUMBER, node);
                codec.exports.insert(item, bytes);
                fake_values::handled(1, item, 0)
            }
            _ => fake_values::Answer::Refuse(KILN_ERR_INVALID_ARGUMENT),
        }
    });
    let c = codec.clone();
    fake_values::on("value_read", move |call| {
        let codec = c.borrow();
        if call.query as i32 == KILN_VALUE_READ_SOURCE_BYTES
            && let Some(bytes) = codec.sources.get(&call.lease)
        {
            return fake_values::handled(1, 0, fake_values::packet(bytes.clone()));
        }
        fake_values::handled(1, 0, 0)
    });
    let c = codec.clone();
    fake_values::on("value_export", move |call| {
        let mut codec = c.borrow_mut();
        codec
            .exported
            .push((call.lease, call.qualified_type.clone()));
        match codec.exports.get(&call.lease) {
            Some(bytes) => fake_values::handled(1, 0, fake_values::packet(bytes.clone())),
            None => fake_values::Answer::Refuse(KILN_ERR_INVALID_ARGUMENT),
        }
    });
    codec
}

// ---- one VM with the family's functions ----

pub struct Setup {
    pub owner: KilnOwner,
    pub vm: fake_values::TestVm,
    pub arena: ValuesArena,
    pub codec: Rc<RefCell<Codec>>,
    pub host: Rc<Script>,
    pub ecs: Rc<Ecs>,
    pub facts: Rc<RuntimeFacts>,
    pub services: Rc<ServiceAdapter>,
    /// ShroudForge's world session marker the family sees.
    pub session: Rc<Cell<u64>>,
    /// The world kind the family binds.
    pub kind: Rc<Cell<u32>>,
    /// The family follows the production facts' choice (`RuntimeFacts::ecs_world`) instead of `kind` and `session`.
    pub follow_facts: Rc<Cell<bool>>,
    _wiring: TypesWiring,
}

/// A server VM of package "alpha" (every fact on) over `world`, the catalog and the codec.
pub fn setup(world: EcsWorld) -> Setup {
    let owner = support::next_owner();
    fake_services::bind_thread(owner);
    fake_services::set_ecs_catalog(owner, Some(ecs_catalog()));
    fake_services::set_ecs_world(owner, Some(world));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let vm = fake_values::vm();
    let arena = vm.arena(ArenaOptions::default()).unwrap();
    let codec = codec();
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: BUILD.into(),
        },
    ));
    let session = Rc::new(Cell::new(FAKE_SESSION));
    let kind = Rc::new(Cell::new(SERVER));
    let follow_facts = Rc::new(Cell::new(false));
    let (live, bound, follow, chooser) = (
        session.clone(),
        kind.clone(),
        follow_facts.clone(),
        facts.clone(),
    );
    let wiring = types::wire(Port(archive_types()), |_, _| {});
    let ecs = Rc::new(
        Ecs::new(
            wiring.handle.session(vm.lua()).unwrap(),
            services.clone(),
            SERVER,
        )
        .with_runtime(EcsRuntime {
            owner,
            values: Some(arena.values()),
            facts: facts.clone(),
            world: Rc::new(move || {
                if follow.get() {
                    chooser.ecs_world()
                } else {
                    (bound.get(), live.get())
                }
            }),
        }),
    );
    let host = script("alpha");
    let table = vm.lua().create_table().unwrap();
    install_into(vm.lua(), &table, host.clone(), ecs.clone()).unwrap();
    vm.set("ecs", table);
    vm.run::<()>(
        "function perr(f, ...)
           local ok, e = pcall(f, ...)
           assert(not ok, 'expected an error')
           return (tostring(e):match('^[^\\n]*'))
         end",
    )
    .unwrap();
    Setup {
        owner,
        vm,
        arena,
        codec,
        host,
        ecs,
        facts,
        services,
        session,
        kind,
        follow_facts,
        _wiring: wiring,
    }
}

impl Setup {
    pub fn eval<R: FromLuaMulti>(&self, code: &str) -> R {
        self.vm
            .run::<R>(code)
            .unwrap_or_else(|e| panic!("{code}: {e}"))
    }
    /// `(nil, reason)` (or `(false, reason)`) of a call that refuses.
    pub fn reason(&self, call: &str) -> String {
        let (value, reason): (Value, Option<String>) = self.eval(&format!("return {call}"));
        assert!(
            matches!(value, Value::Nil | Value::Boolean(false)),
            "{call}: {value:?}"
        );
        reason.unwrap_or_else(|| panic!("{call}: no reason"))
    }
    /// The first line of the error a call raises.
    pub fn raised(&self, call: &str) -> String {
        self.eval(&format!("return perr(function() return {call} end)"))
    }
    pub fn calls(&self, member: &str) -> u64 {
        fake_services::counters(self.owner).calls(member)
    }
    pub fn facts(&self, f: impl FnOnce(&mut RuntimeProviderFacts)) {
        f(&mut self.host.facts.borrow_mut());
    }
    pub fn world<R>(&self, f: impl FnOnce(&mut EcsWorld) -> R) -> R {
        fake_services::ecs_world(self.owner, f).expect("an ECS world")
    }
    pub fn knob(&self, knob: Knob) {
        fake_services::set_knob(self.owner, knob);
    }
    /// The handle of entity `id` (through resolve).
    pub fn handle(&self, id: u32) -> i64 {
        self.eval(&format!("return (ecs.resolve({id}))"))
    }
}

/// A component's bytes from u32 words.
pub fn words(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}
/// A CurrentTransform at `position` (metres) with `scale`.
pub fn transform(position: [f64; 3], scale: [f32; 3]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(56);
    for axis in position {
        bytes.extend(((axis * 4_294_967_296.0) as i64).to_le_bytes());
    }
    for q in [0.0f32, 0.0, 0.0, 1.0] {
        bytes.extend(q.to_le_bytes());
    }
    for s in scale {
        bytes.extend(s.to_le_bytes());
    }
    bytes.extend(0u32.to_le_bytes());
    bytes
}
pub fn hash(name: &str) -> u32 {
    fnv(name)
}
