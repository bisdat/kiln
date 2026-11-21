// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs registry family, shared bodies.
//! The ECS registry's shared bodies: run by kiln-sfi's own tests (tests/runtime_10.rs) and by
//! any other test package that includes them.
//! runtime.ecs.get_catalog / get_registry / get_component / get_components on
//! kiln.types' ecs_registry (the C-ABI fake, from kiln-abi's signatures) joined with an archive
//! registry (registry_metadata pages as the archive schema defines them), kiln.world's
//! ecs_write_policy through the real service adapter, and a scripted registrar host for every fact;
//! one case on the production host (KilnRuntimeHost).
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), identical in
//! both pins: modules/mod-runtime/lua/ingame/ecs/read_component_catalog.rs:2-193,
//! ecs/read_component_registry.rs:3-180, lua/values/validate_owned_type.rs:5-38,
//! game-runtime/native/ecs/registry/refresh_component_registry.cpp:63-77,148-170,243-258,
//! game-runtime/bridge/windows/ecs/read_component_registry.rs:3-13,
//! bridge/windows/provider/read_native_catalog.rs:3-7.
// Another test package may load support through other shared bodies too; each copy belongs to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_services/mod.rs"]
mod fake_services;
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use fake_services::{ECS_TYPE_BASE, EcsCatalog, Shape};
use kiln_abi::services::kiln_types::KilnTypesApi;
use kiln_abi::services::kiln_world::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::Families;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::ecs::catalog::text;
use kiln_sfi::runtime::ecs::registry::text as provider;
use kiln_sfi::runtime::ecs::{self, CLAIMS, Ecs, install_into};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::types::{self, PortError, TypeRegistryPort, TypesWiring};
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Lua, Table, Value};
use serde_json::{Value as Json, json};
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation as Op, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::package::{self, Manifest};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use support::*;

const BUILD: &str = "test-build";
const REGISTRY_VERSION: &str = "registry rev 10";
const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";

fn fnv(text: &str) -> u32 {
    text.bytes().fold(0x811c_9dc5_u32, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

// ---- the archive registry: registry_metadata pages (schema v1) ----

#[derive(Clone)]
struct T {
    qualified: &'static str,
    primitive: &'static str,
    size: u32,
    inner: Option<u64>,
    fields: Vec<(&'static str, u64, u64)>,
    flags: u8,
}
fn t(qualified: &'static str, primitive: &'static str, size: u32) -> T {
    T {
        qualified,
        primitive,
        size,
        inner: None,
        fields: Vec::new(),
        flags: 0,
    }
}
impl T {
    fn field(mut self, name: &'static str, ty: u64, offset: u64) -> Self {
        self.fields.push((name, ty, offset));
        self
    }
}

const UINT32: usize = 0;
const HEALTH: &str = "keen::ecs::Health";
const OWNER: &str = "keen::ecs::Owner";
const DYNAMIC_OWNER: &str = "keen::ecs::DynamicOwner";
const HELPER: &str = "keen::ecs::Helper";
const MISMATCH: &str = "keen::ecs::Mismatch";
const TAGGED: &str = "keen::ecs::Tagged";
const LOCKED: &str = "keen::ecs::Locked";
const DS_LIST: usize = 7;
const DS_REASON: &str = concat!(
    "keen::DsList requires engine allocation and ownership. The K",
    "FC value codec cannot construct it"
);

/// The reflected types: a scalar, seven keen::ecs types (an owner type and the dynamic type it
/// would store, a helper, a DS-holding type), a DS array and a keen::Health whose short name
/// collides with keen::ecs::Health's.
fn archive_types() -> Vec<T> {
    vec![
        t("keen::UInt32", "UInt32", 4),
        t(HEALTH, "Struct", 8)
            .field("hp", UINT32 as u64, 0)
            .field("max", UINT32 as u64, 4),
        t(OWNER, "Struct", 4).field("id", UINT32 as u64, 0),
        t(DYNAMIC_OWNER, "Struct", 24).field("list", DS_LIST as u64, 0),
        t(HELPER, "Struct", 4),
        t(MISMATCH, "Struct", 12),
        t(TAGGED, "Struct", 24).field("list", DS_LIST as u64, 0),
        T {
            inner: Some(UINT32 as u64),
            flags: 0x01,
            ..t("keen::DsList", "DsArray", 24)
        },
        t(LOCKED, "Struct", 4),
        t("keen::Health", "Struct", 8),
    ]
}
fn index_of(name: &str) -> usize {
    archive_types()
        .iter()
        .position(|t| t.qualified == name)
        .unwrap()
}
fn size_of_type(name: &str) -> u32 {
    archive_types()[index_of(name)].size
}

fn u64t(value: u64) -> String {
    format!("u64:{value:016x}")
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
    let resolved: Vec<Json> = t
        .fields
        .iter()
        .map(|(name, ty, offset)| {
            json!({"name": name, "type_index": u64t(*ty), "data_offset": u64t(*offset), "attributes": [],
                   "declaring_type": u64t(index as u64)})
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
        "field_count": t.fields.len(),
        "primitive_type": t.primitive,
        "flags_bits": t.flags,
        "flags": if t.flags & 1 != 0 { vec!["HAS_DS"] } else { vec![] },
        "has_ds": t.flags & 1 != 0,
        "default_bytes": null,
        "struct_fields": fields,
        "fields": resolved,
        "enum_fields": [],
        "attributes": [],
    })
}
struct Port(Vec<T>);
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
            "registry_version": REGISTRY_VERSION,
            "count": tagged(count),
            "first": tagged(first),
            "types": (first.min(count)..end).map(|at| type_json(&self.0, at as usize)).collect::<Vec<_>>(),
        }))
        .unwrap())
    }
}

// ---- kiln.types' catalog ----

/// A catalog row for `name` as kiln.types reports it: (row size, type_info size, component hash).
#[derive(Clone, Copy)]
struct Row {
    name: &'static str,
    row_size: u32,
    native_size: u32,
    hash: u32,
    flags: u32,
}
fn row(name: &'static str) -> Row {
    Row {
        name,
        row_size: size_of_type(name),
        native_size: size_of_type(name),
        hash: fnv(name),
        flags: 0,
    }
}
fn catalog(rows: &[Row]) -> EcsCatalog {
    let mut catalog = EcsCatalog {
        stamp: 0x1234_0000_0001,
        ..EcsCatalog::default()
    };
    for r in rows {
        let identity = ECS_TYPE_BASE as u64 + catalog.types.len() as u64;
        catalog
            .types
            .push((r.name.to_owned(), r.native_size, r.hash));
        catalog.rows.push((r.hash, r.row_size, r.flags, identity));
    }
    catalog
}
/// Health (flags with bit 31), Owner, Tagged and Locked: every row proven.
fn standard() -> EcsCatalog {
    catalog(&[
        Row {
            flags: 0x8000_0007,
            ..row(HEALTH)
        },
        row(OWNER),
        row(TAGGED),
        row(LOCKED),
    ])
}

// ---- the host ----

struct Script {
    owner: RuntimeOwnerContext,
    facts: RefCell<RuntimeProviderFacts>,
    active: Cell<bool>,
    contract: Cell<bool>,
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
        None
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}
fn manifest(capabilities: &[&str]) -> Manifest {
    package::Package::parse(
        json!({"id": "alpha", "name": "alpha", "version": "1.0.0", "capabilities": capabilities})
            .to_string()
            .as_bytes(),
        None,
        package::ParseOptions::default(),
    )
    .unwrap()
    .manifest
}
struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}

// ---- one Lua state with the four functions on this thread ----

struct Setup {
    owner: KilnOwner,
    lua: Lua,
    host: Rc<Script>,
    _wiring: TypesWiring,
}
fn context() -> HostContext {
    HostContext::new(Phase::Runtime, Side::Server, SourceVersion::Sf179)
}
/// Every fact on: ready, a write context, the contract, an active lifecycle, the runtime
/// capability.
fn setup(kiln: Option<EcsCatalog>) -> Setup {
    setup_with(kiln, &["runtime"], |_| {})
}
fn setup_with(
    kiln: Option<EcsCatalog>,
    capabilities: &[&str],
    shape: impl FnOnce(KilnOwner),
) -> Setup {
    let owner = next_owner();
    fake_services::bind_thread(owner);
    fake_services::set_ecs_catalog(owner, kiln);
    shape(owner);
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let host = Rc::new(Script {
        owner: RuntimeOwnerContext::new(&manifest(capabilities), &context(), false, false, BUILD),
        facts: RefCell::new(RuntimeProviderFacts {
            ready: true,
            can_write: true,
            ..RuntimeProviderFacts::default()
        }),
        active: Cell::new(true),
        contract: Cell::new(true),
    });
    let wiring = types::wire(Port(archive_types()), |_, _| {});
    let lua = Lua::new();
    let ecs = Rc::new(Ecs::new(
        wiring.handle.session(&lua).unwrap(),
        services,
        KILN_WORLD_SERVER as u32,
    ));
    let table = lua.create_table().unwrap();
    install_into(&lua, &table, host.clone(), ecs).unwrap();
    lua.globals().set("ecs", table).unwrap();
    let game_types = wiring
        .game_types
        .types(&lua, &RuntimePackage::new("alpha", Rc::new(NoFiles)))
        .unwrap();
    lua.globals().set("types", game_types).unwrap();
    lua.load(
        "function entry(c, name)
           for _, e in ipairs(c.entries) do if e.qualified_name == name then return e end end
         end",
    )
    .exec()
    .unwrap();
    Setup {
        owner,
        lua,
        host,
        _wiring: wiring,
    }
}
impl Setup {
    fn eval<R: FromLuaMulti>(&self, code: &str) -> R {
        self.lua
            .load(code)
            .eval::<R>()
            .unwrap_or_else(|e| panic!("{code}: {e}"))
    }
    fn calls(&self, member: &str) -> u64 {
        fake_services::counters(self.owner).calls(member)
    }
    fn facts(&self, f: impl FnOnce(&mut RuntimeProviderFacts)) {
        f(&mut self.host.facts.borrow_mut());
    }
    /// `(nil, reason)` of a call that fails.
    fn reason(&self, call: &str) -> String {
        let (value, reason): (Value, Option<String>) = self.eval(&format!("return {call}"));
        assert!(value.is_nil(), "{call}: {value:?}");
        reason.unwrap_or_else(|| panic!("{call}: no reason"))
    }
    fn catalog_entry(&self, name: &str) -> Table {
        self.eval(&format!("return entry(ecs.get_catalog(), '{name}')"))
    }
}
fn get<V: mlua::FromLua>(table: &Table, key: &str) -> V {
    table.raw_get(key).unwrap_or_else(|e| panic!("{key}: {e}"))
}

// ---- the join ----

pub fn the_join_proves_each_row_by_hash_name_and_size_and_is_read_once_per_stamp() {
    let s = setup(Some(standard()));
    let (registry, reason): (Table, Option<String>) = s.eval("return ecs.get_registry()");
    assert_eq!(reason, None);
    assert_eq!(get::<i64>(&registry, "schema_version"), 1);
    assert_eq!(get::<i64>(&registry, "layout_version"), 1);
    assert_eq!(
        get::<String>(&registry, "source"),
        "live-engine-registration"
    );
    assert_eq!(get::<i64>(&registry, "count"), 4);
    assert_eq!(get::<i64>(&registry, "runtime_type_count"), 4);
    let names: Vec<String> = s.eval(
        "local r = ecs.get_registry(); local out = {}
         for i, e in ipairs(r.entries) do out[i] = e.qualified_name end; return out",
    );
    assert_eq!(names, [HEALTH, OWNER, TAGGED, LOCKED]);
    // one sizing call and one read, then one sizing call for the second get_registry; each row
    // named once through type_at + type_info
    assert_eq!(s.calls("types.ecs_registry"), 3);
    assert_eq!(s.calls("types.type_at"), 4);
    assert_eq!(s.calls("types.type_info"), 4);
    // the same stamp: one sizing call per metadata call, no row read again
    let _: Table = s.eval("return ecs.get_catalog()");
    let _: Table = s.eval("return (ecs.get_component('keen::ecs::Owner'))");
    assert_eq!(s.calls("types.ecs_registry"), 5);
    assert_eq!(s.calls("types.type_at"), 4);
    // a replaced catalog of the same count (another stamp) is read and joined again
    fake_services::edit_ecs_catalog(s.owner, |c| {
        c.stamp += 1;
        c.rows[1].0 ^= 1;
    });
    assert_eq!(
        s.reason("ecs.get_registry()"),
        format!("{}{OWNER}", provider::DISAGREES)
    );
    assert_eq!(s.calls("types.type_at"), 8);
    // ... and one with fewer rows
    fake_services::edit_ecs_catalog(s.owner, |c| {
        c.stamp += 1;
        c.rows.truncate(2);
        c.rows[1].0 ^= 1;
    });
    assert_eq!(s.eval::<i64>("return ecs.get_registry().count"), 2);
    assert_eq!(s.calls("types.type_at"), 10);
}

pub fn a_registration_entry_has_shroudforge_s_fields_with_unsigned_hash_and_flag_bits() {
    let s = setup(Some(standard()));
    let entry: Table = s.eval("return ecs.get_registry().entries[1]");
    assert_eq!(get::<i64>(&entry, "index"), 0);
    assert_eq!(get::<String>(&entry, "qualified_name"), HEALTH);
    // both are u32 with bit 31 set: never sign-extended
    assert_eq!(fnv(HEALTH), 0xdc10_1091);
    assert_eq!(get::<i64>(&entry, "qualified_hash"), 0xdc10_1091);
    assert_eq!(get::<i64>(&entry, "flags_bits"), 0x8000_0007);
    assert_eq!(get::<i64>(&entry, "runtime_size"), 8);
    assert!(s.eval::<bool>(
        "return ecs.get_registry().entries[1].runtime_type == types.get('keen::ecs::Health')"
    ));
    assert!(get::<Value>(&entry, "template_type").is_nil());
    assert_eq!(get::<String>(&entry, "storage"), "entity");
    // the detail `ecs_registration` gives stays partial
    assert!(get::<Value>(&entry, "storage_flags_bits").is_nil());
    assert_eq!(get::<Table>(&entry, "callbacks").raw_len(), 0);
    assert!(get::<Value>(&entry, "reason").is_nil());
    for key in [
        "read_available",
        "write_available",
        "read_fields_available",
        "write_fields_available",
    ] {
        assert!(get::<bool>(&entry, key), "{key}");
    }
    assert!(!get::<bool>(&entry, "partial_value"));
    for key in ["value_reason", "write_fields_reason", "write_reason"] {
        assert!(get::<Value>(&entry, key).is_nil(), "{key}");
    }
    // the DS-holding type: whole-value writes refused by the codec, field writes allowed
    let tagged: Table = s.eval("return (ecs.get_component('keen::ecs::Tagged'))");
    assert!(get::<bool>(&tagged, "partial_value"));
    assert_eq!(get::<String>(&tagged, "value_reason"), DS_REASON);
    assert_eq!(get::<String>(&tagged, "write_reason"), DS_REASON);
    assert!(!get::<bool>(&tagged, "write_available"));
    assert!(get::<bool>(&tagged, "write_fields_available"));
    assert!(get::<Value>(&tagged, "write_fields_reason").is_nil());
    // a component the write policy refuses
    fake_services::set_write_policy(
        s.owner,
        fnv(LOCKED),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    let locked: Table = s.eval("return (ecs.get_component('keen::ecs::Locked'))");
    assert!(!get::<bool>(&locked, "write_available"));
    assert!(!get::<bool>(&locked, "write_fields_available"));
    assert!(get::<bool>(&locked, "read_available"));
    assert_eq!(
        get::<String>(&locked, "write_fields_reason"),
        text::PHASE_UNAVAILABLE
    );
    assert_eq!(
        get::<String>(&locked, "write_reason"),
        text::PHASE_UNAVAILABLE
    );
    // no write context: the policy alone does not make it writable
    s.facts(|f| f.can_write = false);
    let health: Table = s.eval("return (ecs.get_component('keen::ecs::Health'))");
    assert!(!get::<bool>(&health, "write_available"));
    assert!(!get::<bool>(&health, "write_fields_available"));
    assert!(get::<bool>(&health, "read_available"));
}

pub fn a_size_hash_or_name_mismatch_refuses_the_report_and_resolves_only_proven_rows() {
    let cases: [(&str, Row); 4] = [
        (
            "reflected size",
            Row {
                row_size: 8,
                native_size: 8,
                ..row(MISMATCH)
            },
        ),
        (
            "kiln.types size",
            Row {
                native_size: 16,
                ..row(MISMATCH)
            },
        ),
        (
            "component hash",
            Row {
                hash: fnv(MISMATCH) ^ 1,
                ..row(MISMATCH)
            },
        ),
        (
            "name",
            Row {
                name: "keen::ecs::Unknown",
                ..row(MISMATCH)
            },
        ),
    ];
    for (what, bad) in cases {
        let s = setup(Some(catalog(&[row(HEALTH), bad, row(OWNER)])));
        let expected = format!("{}{}", provider::DISAGREES, bad.name);
        assert_eq!(s.reason("ecs.get_registry()"), expected, "{what}");
        let c: Table = s.eval("return ecs.get_catalog()");
        assert!(!get::<bool>(&c, "registry_available"), "{what}");
        assert_eq!(get::<i64>(&c, "registered_type_count"), 0, "{what}");
        // the proven rows still resolve their storage, as the provider's known layouts do
        assert_eq!(get::<i64>(&c, "resolved_count"), 2, "{what}");
        let health = s.catalog_entry(HEALTH);
        assert!(get::<bool>(&health, "runtime_resolved"), "{what}");
        assert!(get::<bool>(&health, "read_available"), "{what}");
        assert_eq!(get::<String>(&health, "storage"), "unregistered", "{what}");
        let mismatch = s.catalog_entry(MISMATCH);
        assert!(!get::<bool>(&mismatch, "runtime_resolved"), "{what}");
        assert!(!get::<bool>(&mismatch, "read_available"), "{what}");
        assert_eq!(
            get::<String>(&mismatch, "reason"),
            text::NOT_REGISTERED,
            "{what}"
        );
        // an unavailable report has no entries for get_component
        assert_eq!(
            s.reason("ecs.get_component('keen::ecs::Health')"),
            format!("{}{HEALTH}", text::NO_REGISTRATION),
            "{what}"
        );
        let listed: Vec<String> = s.eval(
            "local out = {}; for i, t in ipairs(ecs.get_components()) do out[i] = t.qualified_name end; return out",
        );
        assert_eq!(listed, [HEALTH, OWNER], "{what}");
    }
}

pub fn a_registered_owner_type_is_not_its_stored_dynamic_type() {
    let s = setup(Some(catalog(&[row(OWNER)])));
    let owner = s.catalog_entry(OWNER);
    assert!(get::<bool>(&owner, "registered"));
    assert!(get::<bool>(&owner, "runtime_resolved"));
    assert_eq!(get::<String>(&owner, "storage"), "entity");
    let roles: Table = get(&owner, "registrations");
    assert_eq!(roles.raw_len(), 1);
    let role: Table = roles.raw_get(1).unwrap();
    assert_eq!(get::<String>(&role, "role"), "runtime");
    assert_eq!(get::<i64>(&role, "index"), 0);
    assert_eq!(get::<String>(&role, "qualified_name"), OWNER);
    let dynamic = s.catalog_entry(DYNAMIC_OWNER);
    assert!(!get::<bool>(&dynamic, "registered"));
    assert!(!get::<bool>(&dynamic, "runtime_resolved"));
    assert_eq!(get::<String>(&dynamic, "storage"), "unregistered");
    assert_eq!(get::<String>(&dynamic, "reason"), text::NOT_REGISTERED);
    assert_eq!(
        s.reason("ecs.get_component('keen::ecs::DynamicOwner')"),
        format!("{}{DYNAMIC_OWNER}", text::NO_REGISTRATION)
    );
    assert!(s.eval::<bool>(
        "local e = ecs.get_component('keen::ecs::Owner')
         return e.runtime_type == types.get('keen::ecs::Owner') and e.runtime_type ~= types.get('keen::ecs::DynamicOwner')"
    ));
    // a Type selector finds the same registration
    assert_eq!(
        s.eval::<i64>("return ecs.get_component(types.get('keen::ecs::Owner')).index"),
        0
    );
}

pub fn part_a_has_no_template_role_and_a_type_without_a_column_resolves_nothing() {
    let s = setup(Some(standard()));
    let c: Table = s.eval("return ecs.get_catalog()");
    assert_eq!(get::<i64>(&c, "template_type_count"), 0);
    let roles: Vec<String> = s.eval(
        "local out = {}
         for _, e in ipairs(ecs.get_catalog().entries) do
           for _, r in ipairs(e.registrations) do out[#out + 1] = r.role end
           if e.storage == 'template-only' then out[#out + 1] = 'template-only' end
         end
         for _, e in ipairs(ecs.get_registry().entries) do
           if e.template_type ~= nil or e.storage ~= 'entity' then out[#out + 1] = 'bad ' .. e.qualified_name end
         end
         return out",
    );
    assert_eq!(roles, ["runtime"; 4]);
    // a reflected type with no registration row: no column, nothing readable even with every fact on
    let helper = s.catalog_entry(HELPER);
    assert!(!get::<bool>(&helper, "resolved"));
    assert!(!get::<bool>(&helper, "read_available"));
    assert!(!get::<bool>(&helper, "read_fields_available"));
    assert_eq!(
        s.reason("ecs.get_component('keen::ecs::Helper')"),
        format!("{}{HELPER}", text::NO_REGISTRATION)
    );
    // a row that names the type but has no storage bytes for it fails the size proof
    let s = setup(Some(catalog(&[
        row(HEALTH),
        Row {
            row_size: 0,
            ..row(HELPER)
        },
    ])));
    assert_eq!(
        s.reason("ecs.get_registry()"),
        format!("{}{HELPER}", provider::DISAGREES)
    );
    assert!(!get::<bool>(&s.catalog_entry(HELPER), "runtime_resolved"));
}

pub fn a_missing_selector_and_an_ambiguous_one_answer_shroudforge_s_reasons() {
    let s = setup(Some(standard()));
    assert_eq!(
        s.reason("ecs.get_component('keen::ecs::Nope')"),
        "type not found: keen::ecs::Nope"
    );
    assert_eq!(
        s.reason("ecs.get_component(12345)"),
        "type hash not found: 0x00003039"
    );
    // keen::Health and keen::ecs::Health share their name hash
    assert_eq!(
        s.reason(&format!(
            "ecs.get_component({{hash = {}, domain = 'name'}})",
            fnv("Health")
        )),
        format!(
            "Ambiguous type hash 0x{:08x}. Use a qualified name or qualified hash",
            fnv("Health")
        )
    );
    assert_eq!(
        s.reason("ecs.get_component()"),
        "expected a qualified name, unsigned qualified hash, Type, or {hash, domain}"
    );
    // the qualified hash domain is unique: it selects keen::ecs::Health
    assert_eq!(
        s.eval::<String>(&format!(
            "return ecs.get_component({}).qualified_name",
            fnv(HEALTH)
        )),
        HEALTH
    );
    // a reflected type the selector finds, not registered: the registry's own reason
    assert_eq!(
        s.reason("ecs.get_component('keen::Health')"),
        "no engine component registration for keen::Health"
    );
    // nothing about the catalog is asked before the selector resolves
    let before = s.calls("types.ecs_registry");
    let _ = s.reason("ecs.get_component('keen::ecs::Nope')");
    assert_eq!(s.calls("types.ecs_registry"), before);
}

pub fn get_component_and_get_registry_give_the_provider_s_reason_when_there_is_no_registry() {
    // kiln.types without ecs_registry (an older core), or no kiln.types at all
    let null_slot = |_| {
        fake_services::set_shape(
            "kiln.types",
            Some(Shape::NullAt(std::mem::offset_of!(
                KilnTypesApi,
                ecs_registry
            ))),
        )
    };
    let missing = |_| fake_services::set_shape("kiln.types", Some(Shape::Missing));
    for shape in [&null_slot as &dyn Fn(KilnOwner), &missing] {
        let s = setup_with(Some(standard()), &["runtime"], shape);
        assert_eq!(s.reason("ecs.get_registry()"), provider::LACKS);
        assert_eq!(
            s.reason("ecs.get_component('keen::ecs::Health')"),
            provider::LACKS
        );
        let c: Table = s.eval("return ecs.get_catalog()");
        assert!(!get::<bool>(&c, "registry_available"));
        assert_eq!(get::<i64>(&c, "resolved_count"), 0);
        assert_eq!(get::<i64>(&c, "reflected_only_count"), 7);
        assert_eq!(s.eval::<i64>("return #ecs.get_components()"), 0);
        fake_services::set_shape("kiln.types", None);
    }
    // a build without a component table
    let s = setup(Some(EcsCatalog {
        unavailable: true,
        ..standard()
    }));
    assert_eq!(s.reason("ecs.get_registry()"), provider::UNAVAILABLE);
    assert_eq!(
        s.reason("ecs.get_component('keen::ecs::Health')"),
        format!("{}{HEALTH}", text::NO_REGISTRATION)
    );
    // no catalog at all for this owner answers the same
    let s = setup(None);
    assert_eq!(s.reason("ecs.get_registry()"), provider::UNAVAILABLE);
    // a catalog replaced during every read: four attempts, then ShroudForge's text
    let s = setup(Some(EcsCatalog {
        unreadable: 9,
        ..standard()
    }));
    assert_eq!(s.reason("ecs.get_registry()"), provider::CHANGED);
    assert_eq!(s.calls("types.ecs_registry"), 4);
    // a fetch that failed is the provider's error, not an empty report: get_component gives its text
    // (ShroudForge's fetch_json errors reach get_component through registry()'s Err)
    assert_eq!(
        s.reason("ecs.get_component('keen::ecs::Health')"),
        provider::CHANGED
    );
    let c: Table = s.eval("return ecs.get_catalog()");
    assert!(!get::<bool>(&c, "registry_available"));
    // ... replaced twice: read on the next attempts
    let s = setup(Some(EcsCatalog {
        unreadable: 2,
        ..standard()
    }));
    assert_eq!(s.eval::<i64>("return ecs.get_registry().count"), 4);
}

// ---- the catalog, field by field ----

pub fn every_catalog_field_on_its_own() {
    let s = setup(Some(standard()));
    fake_services::set_write_policy(
        s.owner,
        fnv(LOCKED),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    let c: Table = s.eval("return ecs.get_catalog()");
    assert_eq!(get::<String>(&c, "version"), REGISTRY_VERSION);
    assert!(get::<bool>(&c, "registry_available"));
    assert_eq!(get::<i64>(&c, "registered_type_count"), 4);
    assert_eq!(get::<i64>(&c, "runtime_type_count"), 4);
    assert_eq!(get::<i64>(&c, "template_type_count"), 0);
    assert_eq!(get::<i64>(&c, "resolved_count"), 4);
    assert_eq!(get::<i64>(&c, "reflected_only_count"), 3);
    assert!(get::<bool>(&c, "entity_access_ready"));
    assert!(get::<bool>(&c, "entity_writes_ready"));
    // every keen::ecs type, in registry order, and nothing else
    let names: Vec<String> = s.eval(
        "local out = {}; for i, e in ipairs(ecs.get_catalog().entries) do out[i] = e.qualified_name end; return out",
    );
    assert_eq!(
        names,
        [
            HEALTH,
            OWNER,
            DYNAMIC_OWNER,
            HELPER,
            MISMATCH,
            TAGGED,
            LOCKED
        ]
    );
    assert!(s.eval::<bool>("return entry(ecs.get_catalog(), 'keen::ecs::Health').type == types.get('keen::ecs::Health')"));

    // (name, size, registered, runtime_resolved, resolved, partial, value_reason, read, write,
    //  read_fields, write_fields, write_fields_reason, write_reason, reason)
    type Expect<'a> = (
        &'a str,
        i64,
        bool,
        bool,
        bool,
        bool,
        Option<&'a str>,
        bool,
        bool,
        bool,
        bool,
        Option<&'a str>,
        Option<&'a str>,
        Option<&'a str>,
    );
    let rows: [Expect; 7] = [
        (
            HEALTH, 8, true, true, true, false, None, true, true, true, true, None, None, None,
        ),
        (
            OWNER, 4, true, true, true, false, None, true, true, true, true, None, None, None,
        ),
        (
            DYNAMIC_OWNER,
            24,
            false,
            false,
            false,
            true,
            Some(DS_REASON),
            false,
            false,
            false,
            false,
            None,
            Some(DS_REASON),
            Some(text::NOT_REGISTERED),
        ),
        (
            HELPER,
            4,
            false,
            false,
            false,
            false,
            None,
            false,
            false,
            false,
            false,
            None,
            None,
            Some(text::NOT_REGISTERED),
        ),
        (
            MISMATCH,
            12,
            false,
            false,
            false,
            false,
            None,
            false,
            false,
            false,
            false,
            None,
            None,
            Some(text::NOT_REGISTERED),
        ),
        (
            TAGGED,
            24,
            true,
            true,
            true,
            true,
            Some(DS_REASON),
            true,
            false,
            true,
            true,
            None,
            Some(DS_REASON),
            None,
        ),
        (
            LOCKED,
            4,
            true,
            true,
            true,
            false,
            None,
            true,
            false,
            true,
            false,
            Some(text::PHASE_NOT_VALIDATED),
            Some(text::PHASE_NOT_VALIDATED),
            None,
        ),
    ];
    for (
        name,
        size,
        registered,
        runtime_resolved,
        resolved,
        partial,
        value_reason,
        read,
        write,
        read_fields,
        write_fields,
        write_fields_reason,
        write_reason,
        reason,
    ) in rows
    {
        let e = s.catalog_entry(name);
        assert_eq!(get::<String>(&e, "qualified_name"), name);
        assert_eq!(get::<i64>(&e, "size"), size, "{name} size");
        assert_eq!(
            get::<bool>(&e, "registered"),
            registered,
            "{name} registered"
        );
        assert_eq!(
            get::<bool>(&e, "runtime_resolved"),
            runtime_resolved,
            "{name} runtime_resolved"
        );
        assert_eq!(get::<bool>(&e, "resolved"), resolved, "{name} resolved");
        assert_eq!(
            get::<String>(&e, "registration_state"),
            if registered {
                "registered"
            } else {
                "reflected-only"
            },
            "{name} registration_state"
        );
        assert_eq!(
            get::<String>(&e, "storage"),
            if registered { "entity" } else { "unregistered" },
            "{name} storage"
        );
        assert_eq!(
            get::<Table>(&e, "registrations").raw_len(),
            usize::from(registered),
            "{name} registrations"
        );
        assert_eq!(
            get::<bool>(&e, "partial_value"),
            partial,
            "{name} partial_value"
        );
        assert_eq!(
            get::<Option<String>>(&e, "value_reason").as_deref(),
            value_reason,
            "{name} value_reason"
        );
        assert_eq!(
            get::<bool>(&e, "read_available"),
            read,
            "{name} read_available"
        );
        assert_eq!(
            get::<bool>(&e, "write_available"),
            write,
            "{name} write_available"
        );
        assert_eq!(
            get::<bool>(&e, "read_fields_available"),
            read_fields,
            "{name} read_fields_available"
        );
        assert_eq!(
            get::<bool>(&e, "write_fields_available"),
            write_fields,
            "{name} write_fields_available"
        );
        assert_eq!(
            get::<Option<String>>(&e, "read_reason"),
            None,
            "{name} read_reason"
        );
        assert_eq!(
            get::<Option<String>>(&e, "write_fields_reason").as_deref(),
            write_fields_reason,
            "{name} write_fields_reason"
        );
        assert_eq!(
            get::<Option<String>>(&e, "write_reason").as_deref(),
            write_reason,
            "{name} write_reason"
        );
        assert_eq!(
            get::<Option<String>>(&e, "reason").as_deref(),
            reason,
            "{name} reason"
        );
    }
}

pub fn the_catalog_s_read_and_write_facts_follow_readiness_the_write_context_and_the_denials() {
    let s = setup(Some(standard()));
    // not ready: no reads, writes untouched
    s.facts(|f| f.ready = false);
    let c: Table = s.eval("return ecs.get_catalog()");
    assert!(!get::<bool>(&c, "entity_access_ready"));
    let health = s.catalog_entry(HEALTH);
    assert!(!get::<bool>(&health, "read_available"));
    assert!(!get::<bool>(&health, "read_fields_available"));
    assert!(get::<bool>(&health, "write_available"));
    assert!(get::<Value>(&health, "read_reason").is_nil());
    // no write context: the policy is not asked, and every resolved type says why
    s.facts(|f| {
        f.ready = true;
        f.can_write = false;
    });
    let before = s.calls("ecs_write_policy");
    let c: Table = s.eval("return ecs.get_catalog()");
    assert_eq!(s.calls("ecs_write_policy"), before);
    assert!(!get::<bool>(&c, "entity_writes_ready"));
    for name in [HEALTH, HELPER] {
        let e = s.catalog_entry(name);
        assert!(!get::<bool>(&e, "write_available"), "{name}");
        assert!(!get::<bool>(&e, "write_fields_available"), "{name}");
        assert_eq!(
            get::<String>(&e, "write_fields_reason"),
            text::NO_WRITE_CONTEXT,
            "{name}"
        );
        assert_eq!(
            get::<String>(&e, "write_reason"),
            text::NO_WRITE_CONTEXT,
            "{name}"
        );
    }
    // the codec's reason comes before the write context's
    assert_eq!(
        get::<String>(&s.catalog_entry(TAGGED), "write_reason"),
        DS_REASON
    );
    // a write context: the policy is asked once per resolved type
    s.facts(|f| f.can_write = true);
    let before = s.calls("ecs_write_policy");
    let _: Table = s.eval("return ecs.get_catalog()");
    assert_eq!(s.calls("ecs_write_policy") - before, 4);
    // an inactive lifecycle: the denial is the reason for reads and writes alike
    s.host.active.set(false);
    let health = s.catalog_entry(HEALTH);
    assert!(!get::<bool>(&health, "read_available"));
    assert!(!get::<bool>(&health, "write_available"));
    assert_eq!(get::<String>(&health, "read_reason"), NOT_ACTIVE);
    assert_eq!(get::<String>(&health, "write_fields_reason"), NOT_ACTIVE);
    assert_eq!(get::<String>(&health, "write_reason"), NOT_ACTIVE);
    assert_eq!(
        get::<String>(&s.catalog_entry(TAGGED), "write_reason"),
        NOT_ACTIVE
    );
    // ... and the same on a registration entry
    let entry: Table = s.eval("return (ecs.get_component('keen::ecs::Health'))");
    assert!(!get::<bool>(&entry, "read_available"));
    assert!(!get::<bool>(&entry, "write_available"));
    s.host.active.set(true);
    // outside the contract: the contract's reason
    s.host.contract.set(false);
    let health = s.catalog_entry(HEALTH);
    assert!(!get::<bool>(&health, "read_available"));
    assert_eq!(
        get::<String>(&health, "read_reason"),
        format!("operation runtime.ecs.read has not been verified for game build {BUILD}")
    );
    assert_eq!(
        get::<String>(&health, "write_reason"),
        format!("operation runtime.ecs.write has not been verified for game build {BUILD}")
    );
}

pub fn the_metadata_calls_answer_without_an_active_lifecycle_or_the_runtime_capability() {
    let s = setup_with(Some(standard()), &[], |_| {});
    s.host.active.set(false);
    let c: Table = s.eval("return ecs.get_catalog()");
    assert!(get::<bool>(&c, "registry_available"));
    assert_eq!(get::<i64>(&c, "resolved_count"), 4);
    assert_eq!(s.eval::<i64>("return ecs.get_registry().count"), 4);
    assert_eq!(
        s.eval::<i64>("return ecs.get_component('keen::ecs::Owner').index"),
        1
    );
    let read_reason = get::<String>(&s.catalog_entry(HEALTH), "read_reason");
    assert_eq!(
        read_reason,
        "mod 'alpha' requires capabilities: [\"runtime\"]"
    );
    // get_components keeps ShroudForge's query denial
    assert_eq!(s.reason("ecs.get_components()"), read_reason);
}

pub fn get_components_lists_resolved_storage_types_once_ready() {
    let s = setup(Some(catalog(&[
        row(OWNER),
        row(HEALTH),
        Row {
            row_size: 16,
            native_size: 16,
            ..row(MISMATCH)
        },
    ])));
    let listed: Vec<String> = s.eval(
        "local out = {}; for i, t in ipairs(ecs.get_components()) do out[i] = t.qualified_name end; return out",
    );
    // registry order, the size-proof failure left out
    assert_eq!(listed, [HEALTH, OWNER]);
    assert!(s.eval::<bool>("return ecs.get_components()[1] == types.get('keen::ecs::Health')"));
    s.facts(|f| f.ready = false);
    assert_eq!(s.reason("ecs.get_components()"), text::NOT_READY);
    s.host.active.set(false);
    assert_eq!(s.reason("ecs.get_components()"), NOT_ACTIVE);
}

// ---- production pieces ----

struct Active;
impl OwnerActivity for Active {
    fn active(&self) -> bool {
        true
    }
}

pub fn on_the_production_host_reads_and_writes_wait_for_their_slices_and_the_metadata_answers() {
    let owner = next_owner();
    fake_services::bind_thread(owner);
    fake_services::set_ecs_catalog(owner, Some(standard()));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: BUILD.into(),
        },
    ));
    let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
        &manifest(&["runtime"]),
        &context(),
        Rc::new(Active),
        facts,
    ));
    let wiring = types::wire(Port(archive_types()), |_, _| {});
    let lua = Lua::new();
    let ecs = Rc::new(Ecs::new(
        wiring.handle.session(&lua).unwrap(),
        services,
        KILN_WORLD_SERVER as u32,
    ));
    let table = lua.create_table().unwrap();
    install_into(&lua, &table, host, ecs).unwrap();
    lua.globals().set("ecs", table).unwrap();
    let (available, ready, read_reason, query): (bool, bool, String, String) = lua
        .load(
            "local c = ecs.get_catalog()
             local e; for _, x in ipairs(c.entries) do if x.qualified_name == 'keen::ecs::Health' then e = x end end
             return c.registry_available, c.entity_access_ready, e.read_reason, select(2, ecs.get_components())",
        )
        .eval()
        .unwrap();
    assert!(available);
    assert!(!ready);
    assert_eq!(
        read_reason,
        format!("operation runtime.ecs.read has not been verified for game build {BUILD}")
    );
    assert_eq!(
        query,
        format!("operation runtime.ecs.query has not been verified for game build {BUILD}")
    );
    // with kiln.world's ECS reads available on this build, the reads are in the contract and the world is ready
    fake_services::set_cap(
        "world.ecs_read",
        Some((
            kiln_abi::services::kiln_caps::KILN_CAP_AVAILABLE,
            kiln_abi::services::kiln_caps::KILN_CAP_REASON_NONE,
        )),
    );
    let (ready, read_reason, listed): (bool, Option<String>, i64) = lua
        .load(
            "local c = ecs.get_catalog()
             local e; for _, x in ipairs(c.entries) do if x.qualified_name == 'keen::ecs::Health' then e = x end end
             return c.entity_access_ready, e.read_reason, #ecs.get_components()",
        )
        .eval()
        .unwrap();
    fake_services::set_cap("world.ecs_read", None);
    assert!(ready);
    assert_eq!(read_reason, None);
    assert_eq!(listed, 4);
}

pub fn the_family_claims_its_four_names_inside_the_ecs_container() {
    // the four registry names, then the entity queries and reads (tests/runtime_ecs_reads.rs)
    assert_eq!(
        CLAIMS[..4],
        [
            "runtime.ecs.get_components",
            "runtime.ecs.get_catalog",
            "runtime.ecs.get_registry",
            "runtime.ecs.get_component",
        ]
    );
    assert!(
        CLAIMS
            .iter()
            .all(|name| name.starts_with("runtime.ecs.") || name.starts_with("runtime.overrides."))
    );
    let wiring = types::wire(Port(archive_types()), |_, _| {});
    let families = Families::new(vec![ecs::family(
        wiring.handle.clone(),
        KILN_WORLD_SERVER as u32,
    )]);
    assert!(families.is_ok());
}
