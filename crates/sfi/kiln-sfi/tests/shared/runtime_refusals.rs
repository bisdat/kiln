// SPDX-License-Identifier: MIT
// Shared refusal bodies: run by kiln-sfi's own tests (tests/runtime_refusals.rs) and by any
// other test package that includes them. Reason texts, argument
// errors and check order are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), byte-identical
// in both pins: modules/mod-runtime/lua/ingame/world/change_building_entities.rs:36-139,
// set_profile_patch_enabled.rs:3-55, check_ingame_access.rs:12-414, lua/ingame/mod.rs:116-127,
// game-runtime/bridge/windows/building/format_operation_error.rs:1-23, native/building/
// place_building_prop.cpp:34-38, finish_building.cpp:23-24, remove_building_prop.cpp:131-134,
// native/patches/set_runtime_patch.cpp:204-211; the profiles of builds from 25966634 on have
// no worldOperations / runtimePatches (tests/fixtures/runtime/d7-profiles.toml).
//! The refusal-only shapes of `runtime.world.entity.place` / `.finish_building` / the spatial
//! `.destroy` and of `runtime.patch`: the functions on hosts that walk every denial in order,
//! and the families through the real lifecycle host, worker and Lua VM on the production host.
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::patch;
use kiln_sfi::runtime::world::placement::{self, HandleDestroy};
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation as Op, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::compose::GameTypes;
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::lua::FunctionArgs;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);
pub const GAME_BUILD: &str = "1.0.0.0-v3r1";

pub const INGAME: &str = "runtime APIs are available only during the ingame phase";
pub const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";
pub const PLACE_ARITY: &str = "runtime.world.entity.place/destroy expects position[3], rotation[4], bounds[6], trackingId, feedbackId";
pub const FINISH_ARITY: &str = "runtime.world.entity.finish_building expects complete:boolean";
pub const PATCH_ARITY: &str =
    "runtime.patch.set_enabled expects profile patch name and enabled:boolean";
pub const PLACE_REJECTED: &str = "runtime.world.entity.place was rejected before dispatch (profile, arguments, or live context unavailable)";
pub const DESTROY_REJECTED: &str = "runtime.world.entity.destroy was rejected before dispatch (profile, arguments, or live context unavailable)";
pub const FINISH_REJECTED: &str = "runtime.world.entity.finish_building was rejected before dispatch (profile, arguments, or live context unavailable)";
/// The licence guard keeps the prefix out of MIT sources.
const WORLD_NOT_READY: &str = concat!("K", "FC Runtime world operation is not ready: ");
const PROVIDER_NOT_READY: &str = "runtime provider is not ready for this operation";

fn unverified(operation: &str, build: &str) -> String {
    format!("operation {operation} has not been verified for game build {build}")
}
fn requires(id: &str) -> String {
    format!("mod '{id}' requires capabilities: [\"runtime\"]")
}

// ---- a host that walks every denial ----

/// A registrar host with a chosen phase, permissions, activity and static support.
pub struct TestHost {
    owner: RuntimeOwnerContext,
    active: bool,
    unavailable: Vec<(Op, String)>,
}
impl RuntimeHost for TestHost {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.owner
    }
    fn static_support(&self, operation: Op) -> Availability {
        match self.unavailable.iter().find(|(op, _)| *op == operation) {
            Some((_, reason)) => Availability::Unavailable {
                reason: reason.clone(),
            },
            None => Availability::Available,
        }
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        RuntimeProviderFacts::default()
    }
    fn lifecycle_active(&self) -> bool {
        self.active
    }
    fn write_fault(&self) -> Option<String> {
        // Never consulted by these operations (only ECS and asset writes are latched).
        Some("a write fault these operations never read".into())
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}

fn manifest(id: &str, capabilities: &[&str]) -> package::Manifest {
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

pub fn host(
    version: SourceVersion,
    phase: Phase,
    capabilities: &[&str],
    active: bool,
    unavailable: &[(Op, &str)],
) -> Rc<dyn RuntimeHost> {
    let context = HostContext::new(phase, Side::Server, version);
    Rc::new(TestHost {
        owner: RuntimeOwnerContext::new(
            &manifest("alpha", capabilities),
            &context,
            true,
            true,
            GAME_BUILD,
        ),
        active,
        unavailable: unavailable
            .iter()
            .map(|(op, reason)| (*op, (*reason).to_owned()))
            .collect(),
    })
}

/// A VM whose `entity` and `patch` tables call the functions over `host`, and a log of
/// the handles a handle-destroy backend received.
pub struct Harness {
    lua: Lua,
    pub handles: Rc<RefCell<Vec<u32>>>,
}
pub fn harness(host: Rc<dyn RuntimeHost>, with_backend: bool) -> Harness {
    let lua = Lua::new();
    let handles = Rc::new(RefCell::new(Vec::new()));
    let backend: Option<HandleDestroy> = with_backend.then(|| {
        let handles = handles.clone();
        Rc::new(move |_: &Lua, handle: u32| {
            handles.borrow_mut().push(handle);
            Ok((true, None))
        }) as HandleDestroy
    });
    let entity = lua.create_table().unwrap();
    let h = host.clone();
    entity
        .raw_set(
            "place",
            lua.create_function(move |_, a: FunctionArgs| placement::place(h.as_ref(), &a))
                .unwrap(),
        )
        .unwrap();
    let h = host.clone();
    entity
        .raw_set(
            "destroy",
            lua.create_function(move |lua, a: FunctionArgs| {
                placement::destroy(lua, h.as_ref(), &a, backend.as_ref())
            })
            .unwrap(),
        )
        .unwrap();
    let h = host.clone();
    entity
        .raw_set(
            "finish_building",
            lua.create_function(move |_, a: FunctionArgs| {
                placement::finish_building(h.as_ref(), &a)
            })
            .unwrap(),
        )
        .unwrap();
    let p = lua.create_table().unwrap();
    let h = host.clone();
    p.raw_set(
        "available",
        lua.create_function(move |_, a: FunctionArgs| patch::available(h.as_ref(), &a))
            .unwrap(),
    )
    .unwrap();
    p.raw_set(
        "set_enabled",
        lua.create_function(move |_, a: FunctionArgs| patch::set_enabled(host.as_ref(), &a))
            .unwrap(),
    )
    .unwrap();
    lua.globals().raw_set("entity", entity).unwrap();
    lua.globals().raw_set("patch", p).unwrap();
    Harness { lua, handles }
}

/// Every value an expression returns: "<count>:v1,v2,...".
const PACK: &str = r#"
function pack(...)
  local t = table.pack(...)
  local out = {}
  for i = 1, t.n do out[i] = tostring(t[i]) end
  return t.n .. ':' .. table.concat(out, ',')
end
"#;
impl Harness {
    pub fn call(&self, expression: &str) -> Result<String, String> {
        self.lua.load(PACK).exec().unwrap();
        self.lua
            .load(format!("return pack({expression})"))
            .eval::<String>()
            .map_err(|e| root(&e))
    }
    pub fn ok(&self, expression: &str) -> String {
        self.call(expression)
            .unwrap_or_else(|e| panic!("{expression}: {e}"))
    }
    pub fn err(&self, expression: &str) -> String {
        match self.call(expression) {
            Err(e) => e,
            Ok(v) => panic!("{expression}: returned {v}"),
        }
    }
}
pub fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        other => other.to_string(),
    }
}
fn refused(reason: &str) -> String {
    format!("2:false,{reason}")
}

const P: &str = "{1, 2, 3}";
const R: &str = "{0, 0, 0, 1}";
const B: &str = "{0, 0, 0, 1, 1, 1}";
fn args(position: &str, rotation: &str, bounds: &str, tracking: &str, feedback: &str) -> String {
    format!("{position}, {rotation}, {bounds}, {tracking}, {feedback}")
}

// ---- place, the spatial destroy, finish_building ----

fn placement_order(version: SourceVersion) {
    let good = args(P, R, B, "7", "9");
    for (name, op, rejected) in [
        ("place", Op::WorldEntityPlace, PLACE_REJECTED),
        ("destroy", Op::WorldEntityDestroy, DESTROY_REJECTED),
    ] {
        let call = |h: &Harness, a: &str| h.ok(&format!("entity.{name}({a})"));
        // Each denial comes before the arity and the arguments, in ShroudForge's order.
        let startup = harness(
            host(version, Phase::Startup, &[], false, &[(op, "x")]),
            false,
        );
        assert_eq!(call(&startup, ""), refused(INGAME), "{name}");
        let lacking = harness(host(version, Phase::Runtime, &["patch"], false, &[]), false);
        assert_eq!(call(&lacking, ""), refused(&requires("alpha")), "{name}");
        let inactive = harness(
            host(version, Phase::Runtime, &["runtime"], false, &[]),
            false,
        );
        assert_eq!(call(&inactive, ""), refused(NOT_ACTIVE), "{name}");
        let finish = (Op::WorldEntityFinishBuilding, "finish is out");
        let both = harness(
            host(
                version,
                Phase::Runtime,
                &["runtime"],
                true,
                &[(op, "op is out"), finish],
            ),
            false,
        );
        assert_eq!(
            call(&both, ""),
            refused("op is out"),
            "{name}: its own row first"
        );
        let only_finish = harness(
            host(version, Phase::Runtime, &["runtime"], true, &[finish]),
            false,
        );
        assert_eq!(
            call(&only_finish, &good),
            refused("finish is out"),
            "{name}"
        );
        // Allowed: the arity, then each argument in order, then the native refusal.
        let h = harness(
            host(version, Phase::Runtime, &["runtime"], true, &[]),
            false,
        );
        let e = |a: &str| h.err(&format!("entity.{name}({a})"));
        for wrong in [
            "",
            "{1,2,3}, {0,0,0,1}, {0,0,0,1,1,1}, 7",
            &format!("{good}, 1"),
        ] {
            if name == "destroy" && wrong.is_empty() {
                // No arguments is the spatial form too (only exactly one is a handle).
                assert_eq!(e(wrong), PLACE_ARITY);
                continue;
            }
            assert_eq!(e(wrong), PLACE_ARITY, "{name}({wrong})");
        }
        assert_eq!(
            e(&args("nil", "nil", "nil", "nil", "nil")),
            "bad argument #1 (table expected, got nil)"
        );
        assert_eq!(
            e(&args("{1, 2}", "false", B, "7", "9")),
            "position must contain exactly 3 numbers",
            "position is checked whole before rotation is read"
        );
        assert_eq!(
            e(&args(P, "false", B, "7", "9")),
            "bad argument #2 (table expected, got boolean)"
        );
        assert_eq!(
            e(&args(P, "{0, 0, 1}", B, "7", "9")),
            "rotation must contain exactly 4 numbers"
        );
        assert_eq!(
            e(&args(P, R, "'b'", "7", "9")),
            "bad argument #3 (table expected, got string)"
        );
        assert_eq!(
            e(&args(P, R, "{0, 0, 0, 1, 1}", "7", "9")),
            "bounds must contain exactly 6 numbers"
        );
        let element = e(&args("{1, {}, 3}", R, B, "7", "9"));
        assert!(
            element.starts_with("error converting Lua table to f64"),
            "{name}: {element}"
        );
        assert_eq!(
            e(&args(P, R, B, "-1", "9")),
            "bad argument #4 (32-bit unsigned integer expected, got integer)"
        );
        assert_eq!(
            e(&args(P, R, B, "7", "1.5")),
            "bad argument #5 (32-bit unsigned integer expected, got number)"
        );
        // Every well-formed call is refused before dispatch, as a current profile answers;
        // numeric strings and a zero id are converted as ShroudForge converts them.
        assert_eq!(call(&h, &good), refused(rejected), "{name}");
        assert_eq!(
            call(&h, &args("{'1', 2.5, -3}", R, B, "'7'", "0")),
            refused(rejected),
            "{name}"
        );
    }
}

fn finish_order(version: SourceVersion) {
    let finish = |h: &Harness, a: &str| h.ok(&format!("entity.finish_building({a})"));
    let startup = harness(
        host(version, Phase::Startup, &["runtime"], true, &[]),
        false,
    );
    assert_eq!(finish(&startup, ""), refused(INGAME));
    let lacking = harness(host(version, Phase::Runtime, &[], true, &[]), false);
    assert_eq!(finish(&lacking, "1, 2"), refused(&requires("alpha")));
    let inactive = harness(
        host(version, Phase::Runtime, &["runtime"], false, &[]),
        false,
    );
    assert_eq!(finish(&inactive, "'x'"), refused(NOT_ACTIVE));
    let out = harness(
        host(
            version,
            Phase::Runtime,
            &["runtime"],
            true,
            &[(Op::WorldEntityFinishBuilding, "finish is out")],
        ),
        false,
    );
    assert_eq!(finish(&out, ""), refused("finish is out"));
    let h = harness(
        host(version, Phase::Runtime, &["runtime"], true, &[]),
        false,
    );
    for wrong in ["", "true, false"] {
        assert_eq!(
            h.err(&format!("entity.finish_building({wrong})")),
            FINISH_ARITY
        );
    }
    assert_eq!(
        h.err("entity.finish_building(1)"),
        "bad argument #1 (boolean expected, got integer)"
    );
    assert_eq!(
        h.err("entity.finish_building(nil)"),
        "bad argument #1 (boolean expected, got nil)"
    );
    assert_eq!(finish(&h, "true"), refused(FINISH_REJECTED));
    assert_eq!(finish(&h, "false"), refused(FINISH_REJECTED));
}

fn handle_destroy(version: SourceVersion) {
    // The handle form: destroy's denial, then the handle, then the backend.
    let inactive = harness(
        host(version, Phase::Runtime, &["runtime"], false, &[]),
        true,
    );
    assert_eq!(inactive.ok("entity.destroy('x')"), refused(NOT_ACTIVE));
    let out = harness(
        host(
            version,
            Phase::Runtime,
            &["runtime"],
            true,
            &[(Op::WorldEntityDestroy, "destroy is out")],
        ),
        true,
    );
    assert_eq!(out.ok("entity.destroy({})"), refused("destroy is out"));
    // finish_building's row is not consulted by the handle form.
    let finish_out = harness(
        host(
            version,
            Phase::Runtime,
            &["runtime"],
            true,
            &[(Op::WorldEntityFinishBuilding, "finish is out")],
        ),
        true,
    );
    assert_eq!(finish_out.ok("entity.destroy('7')"), "2:true,nil");
    assert_eq!(*finish_out.handles.borrow(), [7]);
    let h = harness(host(version, Phase::Runtime, &["runtime"], true, &[]), true);
    assert_eq!(
        h.err("entity.destroy(-1)"),
        "bad argument #1 (32-bit unsigned integer expected, got integer)"
    );
    assert_eq!(h.ok("entity.destroy(4294967295)"), "2:true,nil");
    assert_eq!(*h.handles.borrow(), [u32::MAX]);
    assert_eq!(*inactive.handles.borrow(), [] as [u32; 0]);
    assert_eq!(*out.handles.borrow(), [] as [u32; 0]);
    // Without a backend: an operation outside the contract.
    let bare = harness(
        host(version, Phase::Runtime, &["runtime"], true, &[]),
        false,
    );
    assert_eq!(
        bare.ok("entity.destroy(7)"),
        refused(&unverified("runtime.world.entity.destroy", GAME_BUILD))
    );
    // Two arguments, or nil plus one, are the spatial form.
    assert_eq!(bare.err("entity.destroy(7, 8)"), PLACE_ARITY);
    assert_eq!(bare.err("entity.destroy(7, nil)"), PLACE_ARITY);
}

/// place / spatial destroy / finish_building / handle destroy: denial order, arity,
/// arguments and the refusal on ShroudForge `version`.
pub fn placement_shapes(version: SourceVersion) {
    placement_order(version);
    finish_order(version);
    handle_destroy(version);
}

/// runtime.patch.available / set_enabled on ShroudForge `version`.
pub fn patch_shapes(version: SourceVersion) {
    let gameplay = (Op::GameplayPatch, "gameplay patch is out");
    for (label, host) in [
        (
            "startup",
            host(version, Phase::Startup, &["runtime"], true, &[]),
        ),
        ("lacking", host(version, Phase::Runtime, &[], true, &[])),
        (
            "inactive",
            host(version, Phase::Runtime, &["runtime"], false, &[]),
        ),
        (
            "unsupported",
            host(version, Phase::Runtime, &["runtime"], true, &[gameplay]),
        ),
        (
            "allowed",
            host(version, Phase::Runtime, &["runtime"], true, &[]),
        ),
    ] {
        let h = harness(host, false);
        // available converts its name first, then answers one value: false.
        assert_eq!(
            h.err("patch.available()"),
            "bad argument #1 (string expected, got nil)",
            "{label}"
        );
        assert_eq!(
            h.err("patch.available({})"),
            "bad argument #1 (string expected, got table)",
            "{label}"
        );
        assert!(
            h.err("patch.available('\\255')").contains("invalid utf-8"),
            "{label}"
        );
        for name in [
            "'runtime.patch.example'",
            "'runtime.gameplay.patch'",
            "12",
            "''",
        ] {
            assert_eq!(
                h.ok(&format!("patch.available({name})")),
                "1:false",
                "{label}"
            );
        }
        let denial = match label {
            "startup" => Some(INGAME.to_owned()),
            "lacking" => Some(requires("alpha")),
            "inactive" => Some(NOT_ACTIVE.to_owned()),
            "unsupported" => Some(gameplay.1.to_owned()),
            _ => None,
        };
        if let Some(reason) = denial {
            // set_enabled: the denial before the arity and the arguments.
            for a in ["", "{}, 1, 2", "'runtime.patch.x', true"] {
                assert_eq!(
                    h.ok(&format!("patch.set_enabled({a})")),
                    refused(&reason),
                    "{label}({a})"
                );
            }
            continue;
        }
        for a in ["", "'runtime.patch.x'", "'runtime.patch.x', true, 3"] {
            assert_eq!(
                h.err(&format!("patch.set_enabled({a})")),
                PATCH_ARITY,
                "{a}"
            );
        }
        assert_eq!(
            h.err("patch.set_enabled(nil, true)"),
            "bad argument #1 (string expected, got nil)"
        );
        assert_eq!(
            h.err("patch.set_enabled({}, 'not a boolean')"),
            "bad argument #1 (string expected, got table)",
            "the name before enabled"
        );
        assert_eq!(
            h.err("patch.set_enabled('runtime.patch.x', 1)"),
            "bad argument #2 (boolean expected, got integer)"
        );
        assert_eq!(
            h.err("patch.set_enabled('runtime.patch.x', nil)"),
            "bad argument #2 (boolean expected, got nil)"
        );
        for (name, shown) in [
            ("'runtime.patch.example'", "runtime.patch.example"),
            ("'runtime.gameplay.patch'", "runtime.gameplay.patch"),
            ("12", "12"),
            ("1.5", "1.5"),
            ("''", ""),
        ] {
            for enabled in ["true", "false"] {
                assert_eq!(
                    h.ok(&format!("patch.set_enabled({name}, {enabled})")),
                    refused(&format!("build profile does not resolve {shown}")),
                    "{name}, {enabled}"
                );
            }
        }
    }
}

pub fn placement_shapes_sf179() {
    placement_shapes(SourceVersion::Sf179);
}
pub fn placement_shapes_sf180() {
    placement_shapes(SourceVersion::Sf180);
}
pub fn patch_shapes_sf179() {
    patch_shapes(SourceVersion::Sf179);
}
pub fn patch_shapes_sf180() {
    patch_shapes(SourceVersion::Sf180);
}

// ---- the families through the lifecycle host, on the production host ----

static VERSIONS: LazyLock<Mutex<HashMap<KilnOwner, SourceVersion>>> =
    LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);

const SOURCE: &str = r#"
local function pack(...)
  local t = table.pack(...)
  local out = {}
  for i = 1, t.n do out[i] = tostring(t[i]) end
  return t.n .. ':' .. table.concat(out, ',')
end
local function row(t) return tostring(t.available) .. '|' .. tostring(t.reason) end
local function err(f, ...) return (tostring(select(2, pcall(f, ...))):match('^[^\n]*')) end
local e, p = runtime.world.entity, runtime.patch
local P, R, B = {1, 2, 3}, {0, 0, 0, 1}, {0, 0, 0, 1, 1, 1}
local function shapes(at)
  record(at .. '.place', pack(e.place(P, R, B, 7, 9)))
  record(at .. '.destroy', pack(e.destroy(P, R, B, 7, 9)))
  record(at .. '.handle', pack(e.destroy(7)))
  record(at .. '.finish', pack(e.finish_building(true)))
  record(at .. '.available', pack(p.available('runtime.patch.example')))
  record(at .. '.set_enabled', pack(p.set_enabled('runtime.patch.example', true)))
end
local names = {}
for name in pairs(e) do names[#names + 1] = name end
table.sort(names)
record('top.entity', table.concat(names, ','))
local patch_names = {}
for name in pairs(p) do patch_names[#patch_names + 1] = name end
table.sort(patch_names)
record('top.patch', table.concat(patch_names, ','), type(runtime.gameplay))
shapes('top')
return {
  on_load = function()
    shapes('load')
    record('load.arity', err(e.place), err(e.finish_building), err(p.set_enabled, 'x'))
    local ops = runtime.get_operations()
    record('load.status', row(runtime.status('runtime.world.entity.place')),
      row(runtime.status('runtime.world.entity.finish_building')), row(runtime.status('runtime.gameplay.patch')))
    record('load.has', tostring(runtime.has('runtime.world.entity.place')),
      tostring(runtime.has('runtime.world.entity.finish_building')), tostring(runtime.has('runtime.gameplay.patch')))
    record('load.catalog', row(ops['runtime.world.entity.place']), row(ops['runtime.world.entity.finish_building']),
      row(ops['runtime.gameplay.patch']))
    record('done')
  end,
}
"#;

struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(SOURCE.as_bytes().to_vec())
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
/// The `game.types` port: the production runtime host comes with it.
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, _: &RuntimePackage) -> mlua::Result<Table> {
        lua.create_table()
    }
}

/// Two runtime packages, each with its own host (a package without the runtime permission
/// never runs in this phase; that denial is in the host matrix above).
const PACKAGES: [(&str, &[&str]); 2] = [("alpha", &["runtime"]), ("gamma", &["runtime"])];

struct RefusalsPlan;
impl PlanSource for RefusalsPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        let version = VERSIONS.lock().unwrap()[&owner];
        let mut registry = Registry::default();
        for (id, capabilities) in PACKAGES {
            let source: Rc<dyn PackageSource> = Rc::new(Source);
            let mut parsed = package::Package::parse(
                json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities})
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
        let register: Register = Box::new(move |lua, env, package, _| {
            let id = package.id().to_owned();
            env.set(
                "record",
                lua.create_function(move |_, parts: mlua::Variadic<String>| {
                    let text = std::iter::once(id.clone())
                        .chain(parts.iter().cloned())
                        .collect::<Vec<_>>()
                        .join("\t");
                    RECORDS.lock().unwrap().entry(owner).or_default().push(text);
                    Ok(())
                })?,
            )
        });
        let ids: Vec<String> = PACKAGES.iter().map(|(id, _)| id.to_string()).collect();
        Ok(RuntimePlan {
            side: Side::Client,
            version,
            registry,
            plan: ids.clone(),
            admitted: ids.into_iter().collect(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: Some(register),
            compose: kiln_sfi::worker::compose::ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    /// The production families, then a probe: whose host the installer handed this
    /// package's families (`RuntimeContext::package_host`).
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        let mut all = families();
        all.push(Family::new("probe", &[], move |_, _, package, _, cx| {
            let host = cx
                .package_host()
                .map(|h| h.owner_context().id().to_owned())
                .unwrap_or_default();
            RECORDS
                .lock()
                .unwrap()
                .entry(owner)
                .or_default()
                .push(format!("{}\thost\t{host}", package.id()));
            Ok(())
        }));
        Families::new(all)
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        GAME_BUILD.to_owned()
    }
}

/// These production families: the placement refusals (no handle backend yet) and
/// runtime.patch.
pub fn families() -> Vec<Family> {
    vec![placement::family(None), patch::family()]
}

fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

/// Runs both packages with `version` on `api` and returns each package's records.
pub fn run(version: SourceVersion, api: &'static KilnApi) -> HashMap<String, Vec<Vec<String>>> {
    let owner = next_owner();
    VERSIONS.lock().unwrap().insert(owner, version);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<RefusalsPlan>)));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let done = |id: &str| records(owner).iter().any(|r| *r == format!("{id}\tdone"));
    assert!(
        eventually(WAIT, || done("alpha") && done("gamma")),
        "never finished: {:?} / {:?}",
        records(owner),
        shared.status()
    );
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
    let mut by_package: HashMap<String, Vec<Vec<String>>> = HashMap::new();
    for record in records(owner) {
        let mut parts: Vec<String> = record.split('\t').map(str::to_owned).collect();
        let id = parts.remove(0);
        by_package.entry(id).or_default().push(parts);
    }
    by_package
}

fn values<'a>(records: &'a [Vec<String>], label: &str) -> &'a [String] {
    &records
        .iter()
        .find(|r| r[0] == label)
        .unwrap_or_else(|| panic!("no {label} in {records:?}"))[1..]
}

/// What the families answer through the lifecycle host on the production host.
pub fn check(seen: &HashMap<String, Vec<Vec<String>>>) {
    let destroy_unverified = unverified("runtime.world.entity.destroy", GAME_BUILD);
    for (id, _) in PACKAGES {
        let r = &seen[id];
        assert_eq!(
            values(r, "host"),
            [id],
            "each package's families get its own host"
        );
        assert_eq!(
            values(r, "top.entity"),
            ["destroy,finish_building,place"],
            "{id}: the family adds exactly its claims to the installer's container"
        );
        assert_eq!(
            values(r, "top.patch"),
            ["available,set_enabled", "nil"],
            "{id}"
        );
        for at in ["top", "load"] {
            // At the top level the lifecycle is not active yet.
            let denial = (at == "top").then(|| NOT_ACTIVE.to_owned());
            let expect = |op: &str, allowed: &str| {
                assert_eq!(
                    values(r, &format!("{at}.{op}")),
                    [refused(denial.as_deref().unwrap_or(allowed))],
                    "{id} {at}.{op}"
                );
            };
            expect("place", PLACE_REJECTED);
            // destroy's own row has not landed: both forms answer its reason.
            expect("destroy", &destroy_unverified);
            expect("handle", &destroy_unverified);
            expect("finish", FINISH_REJECTED);
            expect(
                "set_enabled",
                "build profile does not resolve runtime.patch.example",
            );
            assert_eq!(
                values(r, &format!("{at}.available")),
                ["1:false"],
                "{id} {at}"
            );
        }
        // The first line of pcall's message (mlua's traceback follows it).
        assert_eq!(
            values(r, "load.arity"),
            [PLACE_ARITY, FINISH_ARITY, PATCH_ARITY]
        );
        // In the contract, not ready on a current profile: ShroudForge's provider reasons.
        let place = format!("false|{WORLD_NOT_READY}runtime.world.entity.place");
        let finish = format!("false|{WORLD_NOT_READY}runtime.world.entity.finish_building");
        assert_eq!(
            values(r, "load.status"),
            [place.clone(), finish.clone(), "true|nil".to_owned()]
        );
        assert_eq!(values(r, "load.has"), ["false", "false", "false"]);
        assert_eq!(
            values(r, "load.catalog"),
            [place, finish, format!("false|{PROVIDER_NOT_READY}")]
        );
    }
}

pub fn the_families_answer_through_the_lifecycle_sf179() {
    check(&run(SourceVersion::Sf179, Box::leak(api(true))));
}
pub fn the_families_answer_through_the_lifecycle_sf180() {
    check(&run(SourceVersion::Sf180, Box::leak(api(true))));
}

/// On both pins: every profile of a build from 25966634 on has neither section, and the
/// builds the interpreter targets are among them.
pub fn current_profiles_have_no_world_operations_or_runtime_patches() {
    let text = include_str!("../fixtures/runtime/d7-profiles.toml");
    let mut rows = 0;
    let mut pins = std::collections::BTreeSet::new();
    let mut builds = std::collections::BTreeSet::new();
    for line in text.lines().filter(|l| l.starts_with('"')) {
        // "<pin>/<side>/<build>.json" = "<sha256> worldOperations=<n> runtimePatches=<n>"
        let (key, value) = line.split_once(" = ").unwrap();
        let key = key.trim_matches('"');
        let mut parts = key.split('/');
        let (pin, side, file) = (
            parts.next().unwrap(),
            parts.next().unwrap(),
            parts.next().unwrap(),
        );
        let build: u64 = file.trim_end_matches(".json").parse().unwrap();
        let fields: Vec<&str> = value.trim_matches('"').split(' ').collect();
        assert_eq!(fields[0].len(), 64, "{key}");
        let world = fields[1].strip_prefix("worldOperations=").unwrap();
        let patches = fields[2].strip_prefix("runtimePatches=").unwrap();
        if build >= 25966634 {
            assert_eq!((world, patches), ("0", "0"), "{key}");
            rows += 1;
            builds.insert((side.to_owned(), build));
        } else {
            assert_ne!(
                world, "0",
                "{key}: the old profiles are the ones that had them"
            );
        }
        pins.insert(pin.to_owned());
    }
    assert_eq!(pins, ["sf-1.7.9", "sf-1.8.0"].map(String::from).into());
    assert_eq!(rows, 22, "11 current profiles on each pin");
    assert!(builds.contains(&("client".to_owned(), 26430625)));
    assert!(builds.contains(&("server".to_owned(), 26430253)));
}
