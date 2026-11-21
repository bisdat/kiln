// SPDX-License-Identifier: MIT
// Shared runtime-facts bodies: run by kiln-sfi's own tests (tests/runtime_facts.rs) and by any
// other test package that includes them. Reason texts and check
// order are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), both pins:
// modules/mod-runtime/lua/ingame/check_ingame_access.rs:12-414, game-runtime/compatibility/
// lib.rs:30-44.
//! The production runtime host through the real lifecycle host, worker and Lua VM: a plan
//! with the `game.types` port and no runtime host of its own gets the registrar's production host,
//! whose catalog, permissions, lifecycle state and effect records the packages see.
// Another test package may load support through the lifecycle bodies too; each copy belongs
// to its own bodies.
#[allow(clippy::duplicate_mod)]
#[path = "../support/mod.rs"]
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::bindings::runtime::RuntimeOperation;
use sfi_core::compose::GameTypes;
use sfi_core::host::{Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use support::*;

/// A liveness bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);
/// What this plan source says the game build is.
pub const GAME_BUILD: &str = "1.0.0.0-r02";

static VERSIONS: LazyLock<Mutex<HashMap<KilnOwner, SourceVersion>>> =
    LazyLock::new(Default::default);
static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);

fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

/// Both packages run this: what the top level (lifecycle not active yet) and on_load
/// (active) see of the registrar on the production host.
const SOURCE: &str = r#"
local function row(t) return tostring(t.available) .. '|' .. tostring(t.reason) end
local function pack(...)
  local t = table.pack(...)
  local out = {}
  for i = 1, t.n do out[i] = tostring(t[i]) end
  return t.n .. ':' .. table.concat(out, ',')
end
local function catalog(ops)
  local names = {}
  for name, status in pairs(ops) do names[#names + 1] = name .. '=' .. row(status) end
  table.sort(names)
  return #names .. ';' .. table.concat(names, ';')
end
record('top.has', tostring(runtime.has('runtime.lifecycle')))
record('top.lifecycle', row(runtime.get_operations()['runtime.lifecycle']))
record('top.effect', pack(runtime.report_effect('waiting')))
record('top.effect_args', pack(runtime.report_effect({}, {})))
return {
  on_load = function()
    record('load.catalog', catalog(runtime.get_operations(nil, false, {})))
    record('load.functions', catalog(runtime.functions.get_operations()))
    record('load.has', tostring(runtime.has('runtime.lifecycle')), tostring(runtime.has('runtime.ecs.query')),
      tostring(runtime.has(12)), tostring(runtime.has('game.assets.write')), tostring(runtime.has('export')))
    record('load.require', pack(runtime.require('runtime.lifecycle')),
      tostring(select(2, pcall(runtime.require, 'runtime.ecs.query'))))
    record('load.status', row(runtime.status('runtime.lifecycle')), row(runtime.status('runtime.world.context.active')),
      row(runtime.status('runtime.nothing')), row(runtime.status(12)))
    record('load.effect', pack(runtime.report_effect('write-confirmed', 'done')),
      tostring(select(2, pcall(runtime.report_effect, 'bad'))))
    record('load.values', pack(runtime.player.name()), pack(runtime.is_main_menu()), type(runtime.player),
      type(runtime.player.name), tostring(runtime.player))
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

/// alpha may patch and export; beta has only the runtime permission.
const PACKAGES: [(&str, &[&str]); 2] = [
    ("alpha", &["runtime", "patch", "export"]),
    ("beta", &["runtime"]),
];

struct FactsPlan;
impl PlanSource for FactsPlan {
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
            side: Side::Server,
            version,
            registry,
            plan: ids.clone(),
            admitted: ids.into_iter().collect(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            // Permissions and the phase's switches are separate facts: alpha may patch but
            // patching is off; export is on but beta may not.
            loader_patch: false,
            loader_export: true,
            attachments: None,
            register: Some(register),
            compose: kiln_sfi::worker::compose::ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    fn game_build(_: &WorkerContext<'_>) -> String {
        GAME_BUILD.to_owned()
    }
}

/// Runs both packages on the production host with `version` and returns each package's
/// records (label and values, tab-separated).
fn run(version: SourceVersion) -> HashMap<String, Vec<Vec<String>>> {
    let owner = next_owner();
    VERSIONS.lock().unwrap().insert(owner, version);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<FactsPlan>)));
    let api: &'static KilnApi = Box::leak(api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let done = |id: &str| records(owner).iter().any(|r| *r == format!("{id}\tdone"));
    assert!(
        eventually(WAIT, || done("alpha") && done("beta")),
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

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {GAME_BUILD}")
}

/// The catalog every active runtime package sees today: `runtime.lifecycle` available; the placement and patch
/// refusal-only operations in the contract but not ready, with ShroudForge's provider reasons
/// on a current profile; every other operation outside the contract until its family lands.
fn expected_catalog() -> String {
    let mut rows: Vec<String> = RuntimeOperation::ALL
        .iter()
        .map(|op| {
            if *op == RuntimeOperation::Lifecycle {
                format!("{}=true|nil", op.as_str())
            } else if matches!(
                op,
                RuntimeOperation::WorldEntityPlace | RuntimeOperation::WorldEntityFinishBuilding
            ) {
                format!(
                    "{0}=false|{1}{0}",
                    op.as_str(),
                    concat!("K", "FC Runtime world operation is not ready: ")
                )
            } else if *op == RuntimeOperation::GameplayPatch {
                format!(
                    "{}=false|runtime provider is not ready for this operation",
                    op.as_str()
                )
            } else {
                format!("{}=false|{}", op.as_str(), unverified(op.as_str()))
            }
        })
        .collect();
    rows.sort();
    format!("31;{}", rows.join(";"))
}

fn check(version: SourceVersion) {
    let seen = run(version);
    for (id, _) in PACKAGES {
        let r = &seen[id];
        // Top level: the lifecycle is not active yet. has() never reads it; the catalog's
        // denial and report_effect do, report_effect before converting its arguments.
        assert_eq!(values(r, "top.has"), ["true"], "{id}");
        assert_eq!(
            values(r, "top.lifecycle"),
            ["false|runtime API access is available only while this mod's lifecycle is active"],
            "{id}"
        );
        assert_eq!(
            values(r, "top.effect"),
            ["2:false,runtime mod is not active"],
            "{id}"
        );
        assert_eq!(
            values(r, "top.effect_args"),
            ["2:false,runtime mod is not active"],
            "{id}"
        );
        // on_load: active.
        assert_eq!(values(r, "load.catalog"), [expected_catalog()], "{id}");
        assert_eq!(values(r, "load.functions"), [expected_catalog()], "{id}");
        let (patch, export) = (false, id == "alpha");
        let has = ["true", "false", "false"].map(String::from);
        assert_eq!(
            values(r, "load.has"),
            [&has[..], &[patch.to_string(), export.to_string()]].concat(),
            "{id}"
        );
        let require = values(r, "load.require");
        assert_eq!(require[0], "0:", "{id}");
        assert!(
            require[1].starts_with("ShroudForge feature is unavailable: runtime.ecs.query"),
            "{id}: {require:?}"
        );
        assert_eq!(
            values(r, "load.status"),
            [
                "true|nil".to_owned(),
                format!("false|{}", unverified("runtime.world.context.active")),
                format!("false|{}", unverified("runtime.nothing")),
                format!("false|{}", unverified("12")),
            ],
            "{id}"
        );
        let effect = values(r, "load.effect");
        assert_eq!(effect[0], "2:true,nil", "{id}");
        assert!(
            effect[1].starts_with(
                "runtime.report_effect state must be waiting, no-target, no-change, write-confirmed, or write-failed"
            ),
            "{id}: {effect:?}"
        );
        let found = values(r, "load.values");
        assert_eq!(
            found[..4],
            [
                format!("2:nil,{}", unverified("runtime.player.name")),
                format!("2:nil,{}", unverified("runtime.is_main_menu")),
                "table".to_owned(),
                "function".to_owned(),
            ],
            "{id}"
        );
    }
    // runtime.player is each package's own table.
    assert_ne!(
        values(&seen["alpha"], "load.values")[4],
        values(&seen["beta"], "load.values")[4]
    );
}

/// The production host on ShroudForge 1.7.9's catalog and texts.
pub fn the_production_runtime_host_answers_through_the_lifecycle_sf179() {
    check(SourceVersion::Sf179);
}

/// The same on 1.8.0's (identical sources; nothing 1.8-only is switched on).
pub fn the_production_runtime_host_answers_through_the_lifecycle_sf180() {
    check(SourceVersion::Sf180);
}

/// Both pins' embedded catalogs list exactly the production catalog's operations, in order.
pub fn the_catalog_is_both_pins_runtime_operations_in_order() {
    let catalog = &kiln_sfi::runtime::operation_catalog::CATALOG;
    assert_eq!(catalog.check(), Ok(()));
    for text in [
        include_str!("../../../sfi-core/tests/fixtures/runtime/sf179-operations.json"),
        include_str!("../../../sfi-core/tests/fixtures/runtime/sf180-operations.json"),
    ] {
        let document: serde_json::Value = serde_json::from_str(text).unwrap();
        let pinned: Vec<&str> = document["runtimeOperations"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        let ours: Vec<&str> = catalog
            .operations
            .iter()
            .map(|r| r.operation.as_str())
            .collect();
        assert_eq!(pinned, ours);
        assert_eq!(ours.len(), 31);
    }
}
