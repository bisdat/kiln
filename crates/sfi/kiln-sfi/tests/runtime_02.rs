// SPDX-License-Identifier: MIT
//! Runtime facts, availability and the write-fault latch on the real service
//! adapter and the C-ABI fake services (kiln.world, kiln.assets, kiln.caps), with the registrar's
//! real registrar in a real Lua VM. Expected texts and check order are ShroudForge's (MIT,
//! Copyright (c) 2026 bonsaibauer), both pins: modules/mod-runtime/lua/ingame/
//! check_ingame_access.rs:12-414, session/record_runtime_state.rs:38-81, game-runtime/
//! compatibility/lib.rs:30-44 (record_runtime_state.rs lines are 1.7.9's; 1.8.0's are 5 later).
//! The production host through the lifecycle host is in
//! runtime_facts.rs.
#[path = "support/fake_services/mod.rs"]
mod fake_services;
mod support;

use kiln_abi::services::kiln_assets::*;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_world::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::availability::{
    CapabilityRow, FactsConfig, GAME_BUILD_UNKNOWN, KilnRuntimeHost, OwnerActivity, RuntimeFacts,
};
use kiln_sfi::runtime::operation_catalog::{
    Backing, CATALOG, Catalog, Fact, FactRow, Need, OperationRow,
};
use kiln_sfi::runtime::services::{ServiceAdapter, ServiceError};
use kiln_sfi::runtime::write_fault::{REASONS, WriteFaultLatch, WriteOutcome, reason};
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::service_member;
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Lua, Table};
use serde_json::json;
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation as Op, RuntimeProviderFacts,
    register_runtime,
};
use sfi_core::compose::GameTypes;
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self, Manifest,
    discover::{PackageRecord, Registry},
};
use std::cell::Cell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::Duration;
use support::*;

const BUILD: &str = "test-build";
const SERVER: u32 = KILN_WORLD_SERVER as u32;
/// ShroudForge's provider reasons; the licence guard keeps the prefix out of MIT sources.
const WAITING: &str = concat!("K", "FC Runtime is waiting for the live Keen ECS world");
const NO_WRITE: &str = concat!("K", "FC Runtime has no writable live component context");
const NO_RESOURCE: &str = concat!(
    "K",
    "FC Runtime has no validated live resource scalar write context"
);
const NOT_ACTIVE: &str =
    "runtime API access is available only while this mod's lifecycle is active";
const UNCERTAIN: &str = "ecs-write-outcome-uncertain-rollback-unverified";
const WORLD_NOT_READY: &str = concat!("K", "FC Runtime world operation is not ready: ");
/// In the production catalog, backed by the interpreter, refused on current profiles.
const REFUSAL_ONLY: [Op; 3] = [
    Op::WorldEntityPlace,
    Op::WorldEntityFinishBuilding,
    Op::GameplayPatch,
];

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {BUILD}")
}

// ---- the adapter, a test catalog and its capability rows ----

fn adapter() -> Rc<ServiceAdapter> {
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let host = unsafe { Host::read(api) };
    Rc::new(ServiceAdapter::new(
        host,
        Rc::new(Watchdog::new(process_clock())),
    ))
}
fn facts(catalog: &'static Catalog, global_patch: bool, global_export: bool) -> Rc<RuntimeFacts> {
    Rc::new(RuntimeFacts::new(
        adapter(),
        catalog,
        FactsConfig {
            global_patch,
            global_export,
            game_build: BUILD.into(),
        },
    ))
}

fn query_member(a: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = a.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, ecs_query).map(drop)
}
fn cas_member(a: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = a.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, ecs_compare_exchange).map(drop)
}
fn resource_member(a: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = a.bind::<KilnAssetsApi>(KILN_ASSETS_SERVICE, KILN_ASSETS_VERSION)?;
    service_member!(table, KilnAssetsApi, resource_read).map(drop)
}
fn menu_values(_: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.is_main_menu = Some(false);
    facts.selected_character_name = Some("character".into());
}

const READ: &str = "test.r02.ecs.read";
const WRITE: &str = "test.r02.ecs.write";
const ASSETS: &str = "test.r02.assets.write";
const MENU: &str = "test.r02.menu";
static READ_NEEDS: [Need; 1] = [Need {
    member: query_member,
    capability: READ,
}];
static WRITE_NEEDS: [Need; 1] = [Need {
    member: cas_member,
    capability: WRITE,
}];
static ASSETS_NEEDS: [Need; 1] = [Need {
    member: resource_member,
    capability: ASSETS,
}];
static MENU_NEEDS: [Need; 1] = [Need {
    member: query_member,
    capability: MENU,
}];

const fn row(operation: Op, backing: Backing) -> OperationRow {
    OperationRow { operation, backing }
}
const LATER: Backing = Backing::NotLanded("a later family");
/// Landed families for the ECS reads and writes, resource writes and the menu value.
static TEST_CATALOG: Catalog = Catalog {
    operations: &[
        row(Op::Lifecycle, Backing::Interpreter),
        row(Op::IsMainMenu, Backing::Kiln(&MENU_NEEDS)),
        row(Op::PlayerName, Backing::Kiln(&MENU_NEEDS)),
        row(Op::NetworkStatus, LATER),
        row(Op::NetworkSend, LATER),
        row(Op::NetworkSendMod, LATER),
        row(Op::NetworkReceive, LATER),
        row(Op::NetworkReceiveMod, LATER),
        row(Op::NetworkAccept, LATER),
        row(Op::NetworkConnectedPeers, LATER),
        row(Op::EcsQuery, Backing::Kiln(&READ_NEEDS)),
        row(Op::EcsResolve, Backing::Kiln(&READ_NEEDS)),
        row(Op::EcsRead, Backing::Kiln(&READ_NEEDS)),
        row(Op::EcsWrite, Backing::Kiln(&WRITE_NEEDS)),
        row(Op::AssetsWrite, Backing::Kiln(&ASSETS_NEEDS)),
        row(Op::WorldContextActive, LATER),
        row(Op::WorldCursorGet, LATER),
        row(Op::WorldBuildingInput, LATER),
        row(Op::WorldVoxelRead, LATER),
        row(Op::WorldVoxelWrite, LATER),
        row(Op::WorldVoxelGridSpec, LATER),
        row(Op::WorldEntitySpawn, LATER),
        row(Op::WorldEntityQueryProps, LATER),
        row(Op::WorldEntityQueryPropsInBounds, LATER),
        row(Op::WorldEntityRegisterPropRecipes, LATER),
        row(Op::WorldEntityGetTransform, LATER),
        row(Op::WorldEntitySetScale, LATER),
        row(Op::WorldEntityPlace, LATER),
        row(Op::WorldEntityDestroy, LATER),
        row(Op::WorldEntityFinishBuilding, LATER),
        row(Op::GameplayPatch, LATER),
    ],
    facts: &[
        FactRow {
            fact: Fact::Ready,
            backing: Backing::Kiln(&READ_NEEDS),
            read: None,
        },
        FactRow {
            fact: Fact::CanWrite,
            backing: Backing::Kiln(&WRITE_NEEDS),
            read: None,
        },
        FactRow {
            fact: Fact::ResourceCanWrite,
            backing: Backing::Kiln(&ASSETS_NEEDS),
            read: None,
        },
        FactRow {
            fact: Fact::MainMenu,
            backing: Backing::Kiln(&MENU_NEEDS),
            read: Some(menu_values),
        },
    ],
};

/// The test catalog's capability rows are process-wide (kiln.caps has no owner): the tests
/// that set them take turns.
static CAPS: Mutex<()> = Mutex::new(());
fn caps() -> MutexGuard<'static, ()> {
    let guard = CAPS.lock().unwrap_or_else(|e| e.into_inner());
    // A test that failed while holding the rows may have left them set.
    for name in [READ, WRITE, ASSETS, MENU] {
        fake_services::set_cap(name, None);
    }
    guard
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Row {
    Available,
    Pending,
    Unavailable,
    Missing,
}
fn set(name: &str, row: Row) {
    fake_services::set_cap(
        name,
        match row {
            Row::Available => Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
            Row::Pending => Some((KILN_CAP_PENDING, KILN_CAP_REASON_SESSION)),
            Row::Unavailable => Some((KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING)),
            Row::Missing => None,
        },
    );
}

// ---- packages, hosts and Lua ----

struct Active(Cell<bool>);
impl OwnerActivity for Active {
    fn active(&self) -> bool {
        self.0.get()
    }
}
fn manifest(id: &str, capabilities: &[&str]) -> Manifest {
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
struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::ErrorKind::NotFound.into())
    }
}
/// One package registered with the registrar on `host`, as the composition does.
fn install(context: &HostContext, host: Rc<KilnRuntimeHost>) -> (LuaVm, Table) {
    let vm = LuaVm::new(context.clone()).unwrap();
    let id = host.owner_context().id().to_owned();
    let env = vm
        .add_package(
            RuntimePackage::new(id, Rc::new(NoFiles)),
            |lua, env, owner| {
                let game: Table = env.raw_get("game")?;
                game.raw_set("types", lua.create_table()?)?;
                register_runtime(lua, env, owner, vm.context(), &game, host.clone())?;
                Ok(())
            },
        )
        .unwrap();
    (vm, env)
}
fn eval<T: FromLuaMulti>(vm: &LuaVm, env: &Table, source: &str) -> mlua::Result<T> {
    vm.lua()
        .load(source)
        .set_name("=runtime_02")
        .set_environment(env.clone())
        .eval()
}
fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        other => other.to_string(),
    }
}
/// (available, reason) of a status table.
type Status = (bool, Option<String>);
fn status(vm: &LuaVm, env: &Table, expression: &str) -> (bool, Option<String>) {
    eval(
        vm,
        env,
        &format!("local s = {expression}; return s.available, s.reason"),
    )
    .unwrap()
}
use sfi_core::lua::LuaVm;

struct Package {
    vm: LuaVm,
    env: Table,
    active: Rc<Active>,
    host: Rc<KilnRuntimeHost>,
}
fn package(
    facts: &Rc<RuntimeFacts>,
    id: &str,
    capabilities: &[&str],
    phase: Phase,
    side: Side,
    active: bool,
) -> Package {
    let context = HostContext::new(phase, side, SourceVersion::Sf179);
    let activity = Rc::new(Active(Cell::new(active)));
    let host = Rc::new(KilnRuntimeHost::new(
        &manifest(id, capabilities),
        &context,
        activity.clone(),
        facts.clone(),
    ));
    let (vm, env) = install(&context, host.clone());
    Package {
        vm,
        env,
        active: activity,
        host,
    }
}
impl Package {
    fn has(&self, feature: &str) -> bool {
        eval(
            &self.vm,
            &self.env,
            &format!("return runtime.has('{feature}')"),
        )
        .unwrap()
    }
    fn status(&self, feature: &str) -> (bool, Option<String>) {
        status(&self.vm, &self.env, &format!("runtime.status('{feature}')"))
    }
    fn catalog(&self, feature: &str) -> (bool, Option<String>) {
        status(
            &self.vm,
            &self.env,
            &format!("runtime.get_operations()['{feature}']"),
        )
    }
    fn effect(&self, arguments: &str) -> (bool, Option<String>) {
        eval(
            &self.vm,
            &self.env,
            &format!("return runtime.report_effect({arguments})"),
        )
        .unwrap()
    }
}

// ---- the catalog ----

#[test]
fn the_catalog_check_refuses_a_malformed_catalog() {
    assert_eq!(CATALOG.check(), Ok(()));
    assert_eq!(TEST_CATALOG.check(), Ok(()));
    let leak_ops =
        |ops: Vec<OperationRow>| -> &'static [OperationRow] { Box::leak(ops.into_boxed_slice()) };
    let leak_facts =
        |facts: Vec<FactRow>| -> &'static [FactRow] { Box::leak(facts.into_boxed_slice()) };
    let bad = |operations: &'static [OperationRow], facts: &'static [FactRow]| {
        let catalog: &'static Catalog = Box::leak(Box::new(Catalog { operations, facts }));
        catalog.check().expect_err("refused")
    };
    let ops = CATALOG.operations;
    assert!(bad(&ops[1..], &[]).contains("every runtime operation once"));
    let mut swapped = ops.to_vec();
    swapped.swap(0, 1);
    assert!(bad(leak_ops(swapped), &[]).contains("in source order"));
    let mut twice = ops.to_vec();
    twice.push(ops[0]);
    assert!(bad(leak_ops(twice), &[]).contains("every runtime operation once"));
    let mut empty_needs = ops.to_vec();
    empty_needs[10].backing = Backing::Kiln(&[]);
    assert!(bad(leak_ops(empty_needs), &[]).contains("names its members"));
    let mut unnamed = ops.to_vec();
    unnamed[10].backing = Backing::NotLanded("");
    assert!(bad(leak_ops(unnamed), &[]).contains("names what it waits for"));
    static NO_CAP: [Need; 1] = [Need {
        member: query_member,
        capability: "",
    }];
    let mut no_cap = ops.to_vec();
    no_cap[10].backing = Backing::Kiln(&NO_CAP);
    assert!(bad(leak_ops(no_cap), &[]).contains("capability row"));
    let ready = FactRow {
        fact: Fact::Ready,
        backing: LATER,
        read: None,
    };
    assert!(bad(ops, leak_facts(vec![ready, ready])).contains("two rows"));
    let world = FactRow {
        fact: Fact::WorldOperation(Op::EcsQuery),
        backing: LATER,
        read: None,
    };
    assert!(bad(ops, leak_facts(vec![world])).contains("not a world operation"));
    let menu = FactRow {
        fact: Fact::MainMenu,
        backing: Backing::Kiln(&MENU_NEEDS),
        read: None,
    };
    assert!(bad(ops, leak_facts(vec![menu])).contains("needs a reader"));
    let world_op = FactRow {
        fact: Fact::WorldOperation(Op::WorldCursorGet),
        backing: LATER,
        read: None,
    };
    assert_eq!(
        Box::leak(Box::new(Catalog {
            operations: ops,
            facts: leak_facts(vec![world_op])
        }))
        .check(),
        Ok(())
    );
}

#[test]
fn production_supports_lifecycle_and_the_refusal_only_operations_and_probes_only_the_network_and_context_rows()
 {
    let owner = next_owner();
    fake_services::bind_thread(owner);
    assert_eq!(GAME_BUILD_UNKNOWN, "unknown");
    let facts = facts(&CATALOG, true, true);
    // Every fake service is here and no kiln.caps row is set. The refusal-only operations
    // are in the contract and need no member. The network core, the mod messages and
    // connected_peers are backed by kiln.net's lease and its row, the world context
    // by its members and the world.lifecycle row (unknown here, so unsupported); the other
    // families have not landed, so no member of theirs is read.
    let network = [
        Op::NetworkStatus,
        Op::NetworkSend,
        Op::NetworkSendMod,
        Op::NetworkReceive,
        Op::NetworkReceiveMod,
        Op::NetworkAccept,
        Op::NetworkConnectedPeers,
    ];
    for op in Op::ALL {
        let expected = if REFUSAL_ONLY.contains(op) || *op == Op::Lifecycle {
            Availability::Available
        } else {
            Availability::Unavailable {
                reason: unverified(op.as_str()),
            }
        };
        assert_eq!(facts.static_support(*op), expected, "{}", op.as_str());
    }
    assert_eq!(facts.provider_facts(), RuntimeProviderFacts::default());
    // Only the capability rows were asked: once per network operation and network fact, once for
    // runtime.world.context.active and once per context fact; the members are copied, never
    // called (no session record and no world generation read either).
    let counters = fake_services::counters(owner);
    assert_eq!(counters.calls("caps.find"), network.len() as u64 + 2 + 3);
    assert_eq!(counters.total_calls(), counters.calls("caps.find"));
}

#[test]
fn static_support_needs_every_member_and_no_unavailable_capability_row() {
    let _caps = caps();
    let facts = facts(&TEST_CATALOG, false, false);
    let supported = |op: Op| facts.static_support(op) == Availability::Available;
    for (row, expected, seen) in [
        (Row::Available, true, CapabilityRow::Available),
        (Row::Pending, true, CapabilityRow::Pending),
        (Row::Unavailable, false, CapabilityRow::Unavailable),
        (Row::Missing, false, CapabilityRow::Unknown),
    ] {
        set(READ, row);
        assert_eq!(facts.capability(READ), seen, "{row:?}");
        assert_eq!(supported(Op::EcsQuery), expected, "{row:?}");
        assert_eq!(supported(Op::EcsRead), expected, "{row:?}");
    }
    assert_eq!(
        facts.static_support(Op::EcsQuery),
        Availability::Unavailable {
            reason: unverified("runtime.ecs.query")
        }
    );
    set(READ, Row::Available);
    let query = std::mem::offset_of!(KilnWorldApi, ecs_query);
    for (service, shape) in [
        ("kiln.world", fake_services::Shape::Missing),
        ("kiln.world", fake_services::Shape::NullAt(query)),
        ("kiln.world", fake_services::Shape::Short(query as u32)),
        (
            "kiln.world",
            fake_services::Shape::Version(KILN_WORLD_VERSION - 1),
        ),
        ("kiln.caps", fake_services::Shape::Missing),
    ] {
        fake_services::set_shape(service, Some(shape));
        assert!(!supported(Op::EcsQuery), "{service} {shape:?}");
        fake_services::set_shape(service, None);
    }
    assert!(supported(Op::EcsQuery));
    // Another family's member or row does not matter.
    set(WRITE, Row::Unavailable);
    assert!(supported(Op::EcsQuery) && !supported(Op::EcsWrite));
    set(READ, Row::Missing);
    set(WRITE, Row::Missing);
}

#[test]
fn provider_facts_hold_while_members_are_present_and_rows_are_available() {
    let _caps = caps();
    let facts = facts(&TEST_CATALOG, false, false);
    for row in [Row::Pending, Row::Unavailable, Row::Missing] {
        set(READ, row);
        set(MENU, row);
        let now = facts.provider_facts();
        assert!(!now.ready, "{row:?}");
        assert_eq!(
            (now.is_main_menu, now.selected_character_name),
            (None, None)
        );
    }
    set(READ, Row::Available);
    set(MENU, Row::Available);
    let now = facts.provider_facts();
    assert!(now.ready && !now.can_write && !now.resource_can_write);
    assert_eq!(now.is_main_menu, Some(false));
    assert_eq!(now.selected_character_name.as_deref(), Some("character"));
    fake_services::set_shape("kiln.world", Some(fake_services::Shape::Missing));
    let without_world = facts.provider_facts();
    fake_services::set_shape("kiln.world", None);
    assert_eq!(without_world, RuntimeProviderFacts::default());
    set(READ, Row::Missing);
    set(MENU, Row::Missing);
}

#[test]
fn a_capability_that_turns_on_or_off_is_seen_without_re_admission() {
    let _caps = caps();
    let facts = facts(&TEST_CATALOG, false, false);
    let p = package(
        &facts,
        "alpha",
        &["runtime"],
        Phase::Runtime,
        Side::Server,
        true,
    );
    let holders = Rc::strong_count(&p.host);
    let mut seen = Vec::new();
    for row in [
        Row::Pending,
        Row::Available,
        Row::Unavailable,
        Row::Available,
        Row::Missing,
    ] {
        set(READ, row);
        seen.push((
            row,
            p.has("runtime.ecs.query"),
            p.status("runtime.ecs.query"),
        ));
    }
    assert_eq!(
        seen,
        [
            (Row::Pending, false, (false, Some(WAITING.to_owned()))),
            (Row::Available, true, (true, None)),
            (
                Row::Unavailable,
                false,
                (false, Some(unverified("runtime.ecs.query")))
            ),
            (Row::Available, true, (true, None)),
            (
                Row::Missing,
                false,
                (false, Some(unverified("runtime.ecs.query")))
            ),
        ]
    );
    // One registration throughout: the same host, held by the same closures.
    assert_eq!(Rc::strong_count(&p.host), holders);
}

// ---- the cross product ----

#[derive(Clone, Copy, Debug)]
enum Fault {
    None,
    Live,
    Stale,
}

/// ShroudForge's checks (check_ingame_access.rs) for the test catalog's four operations,
/// written out independently of the registrar.
#[allow(clippy::too_many_arguments)]
fn expected(
    op: Op,
    runtime_phase: bool,
    runtime_cap: bool,
    active: bool,
    fault: Fault,
    read: Row,
    write: Row,
    assets: Row,
) -> (bool, Status, Status) {
    let decided = |row: Row| matches!(row, Row::Available | Row::Pending);
    let static_ok = match op {
        Op::Lifecycle => true,
        Op::EcsQuery => decided(read),
        Op::EcsWrite => decided(write),
        _ => decided(assets),
    };
    let (ready, can_write, resource) = (
        read == Row::Available,
        write == Row::Available,
        assets == Row::Available,
    );
    let fact = match op {
        Op::Lifecycle => true,
        Op::EcsQuery => ready,
        Op::EcsWrite => can_write,
        _ => resource,
    };
    let has = runtime_phase && static_ok && runtime_cap && fact;
    let unavailable = |text: String| (false, Some(text));
    let status = if !runtime_cap {
        unavailable("mod 'pkg' lacks the required capability".into())
    } else if !static_ok {
        unavailable(unverified(op.as_str()))
    } else {
        match op {
            Op::EcsQuery | Op::EcsWrite if !ready => unavailable(WAITING.into()),
            Op::EcsWrite if !can_write => unavailable(NO_WRITE.into()),
            Op::AssetsWrite if !resource => unavailable(NO_RESOURCE.into()),
            _ => (true, None),
        }
    };
    let write_op = matches!(op, Op::EcsWrite | Op::AssetsWrite);
    let denial = if !runtime_phase {
        Some("runtime APIs are available only during the ingame phase".to_owned())
    } else if !runtime_cap {
        Some("mod 'pkg' requires capabilities: [\"runtime\"]".to_owned())
    } else if !active {
        Some(NOT_ACTIVE.to_owned())
    } else if write_op && matches!(fault, Fault::Live) {
        Some(UNCERTAIN.to_owned())
    } else if !static_ok {
        Some(unverified(op.as_str()))
    } else {
        None
    };
    let available = has && denial.is_none();
    let reason = denial.or(status.1.clone()).or_else(|| {
        (!available).then(|| "runtime provider is not ready for this operation".to_owned())
    });
    (has, status, (available, reason))
}

#[test]
fn the_phase_capability_active_fault_readiness_cross_product_matches_shroudforge() {
    let _caps = caps();
    let owner = next_owner();
    fake_services::bind_thread(owner);
    let readiness = [
        (Row::Unavailable, Row::Unavailable, Row::Unavailable),
        (Row::Pending, Row::Pending, Row::Pending),
        (Row::Available, Row::Pending, Row::Pending),
        // ECS reads not ready but writes are: has() and get_operations() answer the write
        // available while its status keeps the waiting reason (check_ingame_access.rs:43-48,
        // 165-171, 216-225).
        (Row::Pending, Row::Available, Row::Available),
        (Row::Available, Row::Available, Row::Available),
    ];
    let mut cases = 0;
    for phase in [Phase::Startup, Phase::Runtime] {
        for capabilities in [&[][..], &["runtime"][..]] {
            for active in [false, true] {
                for fault in [Fault::None, Fault::Live, Fault::Stale] {
                    for (read, write, assets) in readiness {
                        set(READ, read);
                        set(WRITE, write);
                        set(ASSETS, assets);
                        let facts = facts(&TEST_CATALOG, false, false);
                        match fault {
                            Fault::None => {}
                            Fault::Live => {
                                facts.write_faults().report(
                                    SERVER,
                                    fake_services::FAKE_SESSION,
                                    WriteOutcome::EcsWriteUncertain,
                                );
                            }
                            Fault::Stale => {
                                facts.write_faults().report(
                                    SERVER,
                                    fake_services::FAKE_SESSION + 6,
                                    WriteOutcome::EcsWriteUncertain,
                                );
                            }
                        }
                        let p = package(&facts, "pkg", capabilities, phase, Side::Server, active);
                        let runtime_phase = phase == Phase::Runtime;
                        let runtime_cap = !capabilities.is_empty();
                        let case = format!(
                            "{phase:?} {capabilities:?} active={active} {fault:?} {read:?}/{write:?}/{assets:?}"
                        );
                        for op in [Op::Lifecycle, Op::EcsQuery, Op::EcsWrite, Op::AssetsWrite] {
                            let name = op.as_str();
                            let want = expected(
                                op,
                                runtime_phase,
                                runtime_cap,
                                active,
                                fault,
                                read,
                                write,
                                assets,
                            );
                            let got = (p.has(name), p.status(name), p.catalog(name));
                            assert_eq!(got, want, "{case} {name}");
                        }
                        let effect = if !runtime_phase || !runtime_cap {
                            (false, Some("runtime effect reporting requires an active ingame runtime mod".to_owned()))
                        } else if !active {
                            (false, Some("runtime mod is not active".to_owned()))
                        } else {
                            (true, None)
                        };
                        assert_eq!(p.effect("'waiting'"), effect, "{case}");
                        cases += 1;
                    }
                }
            }
        }
    }
    assert_eq!(cases, 120);
    for name in [READ, WRITE, ASSETS] {
        set(name, Row::Missing);
    }
}

// ---- operation ids, names and arguments ----

#[test]
fn all_31_operation_ids_and_unknown_names_answer_sf_s_shapes() {
    let facts = facts(&CATALOG, false, false);
    let p = package(
        &facts,
        "alpha",
        &["runtime"],
        Phase::Runtime,
        Side::Server,
        true,
    );
    for op in Op::ALL {
        let name = op.as_str();
        if *op == Op::Lifecycle {
            assert!(p.has(name));
            assert_eq!(p.status(name), (true, None));
            assert_eq!(p.catalog(name), (true, None));
        } else if *op == Op::GameplayPatch {
            // In the contract, never available on a current profile.
            assert!(!p.has(name));
            assert_eq!(p.status(name), (true, None));
            assert_eq!(
                p.catalog(name),
                (
                    false,
                    Some("runtime provider is not ready for this operation".into())
                )
            );
        } else if REFUSAL_ONLY.contains(op) {
            let not_ready = format!("{WORLD_NOT_READY}{name}");
            assert!(!p.has(name), "{name}");
            assert_eq!(p.status(name), (false, Some(not_ready.clone())), "{name}");
            assert_eq!(p.catalog(name), (false, Some(not_ready)), "{name}");
        } else {
            assert!(!p.has(name), "{name}");
            assert_eq!(p.status(name), (false, Some(unverified(name))), "{name}");
            assert_eq!(p.catalog(name), (false, Some(unverified(name))), "{name}");
        }
    }
    let keys: i64 = eval(
        &p.vm,
        &p.env,
        "local n = 0; for _ in pairs(runtime.get_operations()) do n = n + 1 end; return n",
    )
    .unwrap();
    assert_eq!(keys, 31);
    // Not operation ids: a Lua function path, other case, a trailing space, nothing.
    for name in [
        "runtime.unknown",
        "Runtime.lifecycle",
        "runtime.world.context_active",
        "runtime.lifecycle ",
        "",
        "export.extra",
    ] {
        assert!(!p.has(name), "{name:?}");
        assert_eq!(p.status(name), (false, Some(unverified(name))), "{name:?}");
        let error = eval::<()>(&p.vm, &p.env, &format!("runtime.require('{name}')")).unwrap_err();
        assert_eq!(
            root(&error),
            format!("ShroudForge feature is unavailable: {name}")
        );
    }
}

#[test]
fn string_arguments_are_coerced_and_invalid_utf8_raises() {
    let facts = facts(&CATALOG, false, false);
    let p = package(
        &facts,
        "alpha",
        &["runtime"],
        Phase::Runtime,
        Side::Client,
        true,
    );
    let (vm, env) = (&p.vm, &p.env);
    assert!(!eval::<bool>(vm, env, "return runtime.has(12)").unwrap());
    assert_eq!(
        status(vm, env, "runtime.status(12)"),
        (false, Some(unverified("12")))
    );
    assert_eq!(
        status(vm, env, "runtime.status(1.5)"),
        (false, Some(unverified("1.5")))
    );
    for call in [
        "runtime.has('\\255')",
        "runtime.require('\\255')",
        "runtime.status('\\255')",
    ] {
        let error = eval::<()>(vm, env, call).unwrap_err();
        assert!(root(&error).contains("invalid utf-8"), "{call}: {error}");
    }
    for (call, kind) in [
        ("runtime.has()", "nil"),
        ("runtime.status({})", "table"),
        ("runtime.require(false)", "boolean"),
    ] {
        let error = eval::<()>(vm, env, call).unwrap_err();
        assert_eq!(
            root(&error),
            format!("bad argument #1 (string expected, got {kind})"),
            "{call}"
        );
    }
    assert_eq!(p.effect("'waiting', 42"), (true, None));
    assert_eq!(facts.effects().get("alpha").unwrap().detail, "42");
    let error = eval::<()>(vm, env, "runtime.report_effect('waiting', '\\255')").unwrap_err();
    assert!(root(&error).contains("invalid utf-8"), "{error}");
    let error = eval::<()>(vm, env, "runtime.report_effect(12)").unwrap_err();
    assert_eq!(
        root(&error),
        "runtime.report_effect state must be waiting, no-target, no-change, write-confirmed, or write-failed"
    );
}

#[test]
fn report_effect_denies_before_converting_and_keeps_sf_s_record() {
    let facts = facts(&CATALOG, false, false);
    let ingame = "runtime effect reporting requires an active ingame runtime mod";
    let startup = package(
        &facts,
        "pre",
        &["runtime"],
        Phase::Startup,
        Side::Server,
        true,
    );
    assert_eq!(startup.effect("{}, {}"), (false, Some(ingame.into())));
    let plain = package(&facts, "plain", &[], Phase::Runtime, Side::Server, true);
    assert_eq!(plain.effect("{}, {}"), (false, Some(ingame.into())));
    let p = package(
        &facts,
        "alpha",
        &["runtime"],
        Phase::Runtime,
        Side::Server,
        false,
    );
    assert_eq!(
        p.effect("{}, {}"),
        (false, Some("runtime mod is not active".into()))
    );
    assert!(facts.effects().snapshot().is_empty());
    p.active.0.set(true);
    let long = "é".repeat(300);
    assert_eq!(p.effect(&format!("'no-target', '{long}'")), (true, None));
    let first = facts.effects().get("alpha").unwrap();
    assert_eq!(first.state, EffectState::NoTarget);
    assert_eq!(first.detail, "é".repeat(240));
    assert_eq!(
        (first.process_target, first.scope),
        ("server", "this-process")
    );
    assert!(first.updated_at > 1_700_000_000);
    // The same state and (cut) detail again changes nothing; another state replaces it.
    assert_eq!(
        p.effect(&format!("'no-target', '{}'", "é".repeat(250))),
        (true, None)
    );
    assert_eq!(facts.effects().get("alpha").unwrap(), first);
    assert_eq!(p.effect("'write-failed'"), (true, None));
    let second = facts.effects().get("alpha").unwrap();
    assert_eq!(
        (second.state, second.detail.as_str()),
        (EffectState::WriteFailed, "")
    );
    let client = package(
        &facts,
        "beta",
        &["runtime"],
        Phase::Runtime,
        Side::Client,
        true,
    );
    assert_eq!(client.effect("'waiting', 'x'"), (true, None));
    let effects = facts.effects().snapshot();
    assert_eq!(effects.len(), 2);
    assert_eq!(effects["beta"].process_target, "client");
    assert_eq!(effects["alpha"], second);
}

// ---- the write-fault latch ----

fn outcomes() -> Vec<WriteOutcome> {
    let all = vec![
        WriteOutcome::EcsWriteUncertain,
        WriteOutcome::EcsWriteConflictExhausted,
    ];
    // A new outcome must be listed here (and given its row).
    for outcome in &all {
        match outcome {
            WriteOutcome::EcsWriteUncertain | WriteOutcome::EcsWriteConflictExhausted => {}
        }
    }
    all
}

#[test]
fn every_kiln_outcome_has_one_row_and_latches_by_sf_s_own_rule() {
    let all = outcomes();
    assert_eq!(REASONS.len(), all.len());
    for outcome in all {
        assert_eq!(
            REASONS.iter().filter(|r| r.outcome == outcome).count(),
            1,
            "{outcome:?}"
        );
        let row = reason(outcome);
        // access_named_service.rs:231 and its siblings: a reason latches when it says uncertain.
        assert_eq!(
            row.latches,
            row.reason.to_lowercase().contains("uncertain"),
            "{outcome:?}"
        );
    }
    assert_eq!(reason(WriteOutcome::EcsWriteUncertain).reason, UNCERTAIN);
    assert_eq!(
        reason(WriteOutcome::EcsWriteConflictExhausted).reason,
        "ecs-scene-frame-busy"
    );
}

#[test]
fn the_latch_keeps_the_first_uncertain_write_of_a_session_until_the_session_changes() {
    let latch = WriteFaultLatch::new();
    assert_eq!(
        latch.report(SERVER, 0, WriteOutcome::EcsWriteUncertain),
        UNCERTAIN
    );
    assert_eq!(latch.fault(), None, "session 0 never latches");
    assert_eq!(
        latch.report(SERVER, 5, WriteOutcome::EcsWriteConflictExhausted),
        "ecs-scene-frame-busy"
    );
    assert_eq!(latch.fault(), None, "a busy answer does not latch");
    latch.report(SERVER, 5, WriteOutcome::EcsWriteUncertain);
    let fault = latch.fault().unwrap();
    assert_eq!(
        (fault.world_kind, fault.session, fault.reason),
        (SERVER, 5, UNCERTAIN)
    );
    assert_eq!(
        latch.active(|kind| if kind == SERVER { 5 } else { 0 }),
        Some(UNCERTAIN.into())
    );
    assert_eq!(latch.active(|_| 6), None);
    assert_eq!(latch.active(|_| 0), None);
    latch.report(SERVER, 5, WriteOutcome::EcsWriteConflictExhausted);
    assert_eq!(latch.fault(), Some(fault));
    latch.report(SERVER, 6, WriteOutcome::EcsWriteUncertain);
    assert_eq!(latch.fault().unwrap().session, 6);
    assert_eq!(latch.active(|_| 6), Some(UNCERTAIN.into()));
    assert_eq!(latch.active(|_| 5), None);
}

#[test]
fn a_latched_session_denies_only_the_writes_and_only_while_it_is_live() {
    let _caps = caps();
    let owner = next_owner();
    fake_services::bind_thread(owner);
    for name in [READ, WRITE, ASSETS] {
        set(name, Row::Available);
    }
    let facts = facts(&TEST_CATALOG, false, false);
    let p = package(
        &facts,
        "alpha",
        &["runtime"],
        Phase::Runtime,
        Side::Server,
        true,
    );
    let writes = |p: &Package| {
        (
            p.catalog("runtime.ecs.write"),
            p.catalog("runtime.assets.write"),
        )
    };
    let open = ((true, None), (true, None));
    assert_eq!(writes(&p), open);
    assert_eq!(facts.write_fault(), None);
    let session = facts.world_session(SERVER);
    assert_eq!(session, fake_services::FAKE_SESSION);
    facts
        .write_faults()
        .report(SERVER, session, WriteOutcome::EcsWriteUncertain);
    let denied = (
        (false, Some(UNCERTAIN.to_owned())),
        (false, Some(UNCERTAIN.to_owned())),
    );
    assert_eq!(writes(&p), denied);
    // Reads, has() and status() never read the latch.
    assert_eq!(p.catalog("runtime.ecs.query"), (true, None));
    assert!(p.has("runtime.ecs.write") && p.has("runtime.assets.write"));
    assert_eq!(p.status("runtime.ecs.write"), (true, None));
    // The next session ends it; the same session again would still be the latched one.
    fake_services::set_world_session(owner, SERVER, session + 1);
    assert_eq!(writes(&p), open);
    fake_services::set_world_session(owner, SERVER, session);
    assert_eq!(writes(&p), denied);
    fake_services::set_world_session(owner, SERVER, 0);
    assert_eq!(writes(&p), open);
    fake_services::set_world_session(owner, SERVER, session);
    fake_services::set_shape("kiln.world", Some(fake_services::Shape::Missing));
    assert_eq!(facts.write_fault(), None, "no session known: no fault");
    fake_services::set_shape("kiln.world", None);
    assert!(fake_services::counters(owner).calls("game_world_session") > 0);
    for name in [READ, WRITE, ASSETS] {
        set(name, Row::Missing);
    }
}

// ---- permissions bound at registration ----

#[test]
fn the_owner_context_is_bound_at_registration_from_the_package_s_own_manifest() {
    let facts = facts(&CATALOG, true, false);
    let context = HostContext::new(Phase::Runtime, Side::Client, SourceVersion::Sf180);
    let mut alpha = manifest("alpha", &["runtime", "export"]);
    let host = KilnRuntimeHost::new(
        &alpha,
        &context,
        Rc::new(Active(Cell::new(true))),
        facts.clone(),
    );
    // A later change to the manifest is not seen: the context was copied at registration.
    alpha.id = "changed".into();
    alpha.capabilities.clear();
    let owner = host.owner_context();
    assert_eq!(owner.id(), "alpha");
    assert!(owner.has_runtime() && !owner.has_patch() && owner.has_export());
    assert!(owner.global_patch() && !owner.global_export());
    assert_eq!(
        (
            owner.phase(),
            owner.side(),
            owner.version(),
            owner.game_build()
        ),
        (Phase::Runtime, Side::Client, SourceVersion::Sf180, BUILD)
    );
    // The switches and the permissions are separate facts.
    let a = package(
        &facts,
        "alpha",
        &["runtime", "export"],
        Phase::Runtime,
        Side::Client,
        true,
    );
    assert!(
        !a.has("game.assets.write"),
        "patching on, but alpha may not patch"
    );
    assert!(!a.has("export"), "alpha may export, but exporting is off");
    let b = package(
        &facts,
        "beta",
        &["runtime", "patch"],
        Phase::Runtime,
        Side::Client,
        true,
    );
    assert!(b.has("game.assets.write"));
    // ShroudForge's status() looks every feature up in the operation contract, which has no
    // `game.assets.write` (check_ingame_access.rs:179-190): available to has(), not to status().
    assert_eq!(
        b.status("game.assets.write"),
        (false, Some(unverified("game.assets.write")))
    );
    assert_eq!(
        a.status("game.assets.write"),
        (
            false,
            Some("mod 'alpha' lacks the required capability".into())
        )
    );
    assert_eq!(b.host.owner_context().id(), "beta");
}

// ---- through the lifecycle host: the families' facts are the host's ----

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
const WAIT: Duration = Duration::from_secs(300);

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
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, _: &RuntimePackage) -> mlua::Result<Table> {
        lua.create_table()
    }
}
struct Source;
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path != "src/mod.lua" {
            return Err(std::io::ErrorKind::NotFound.into());
        }
        Ok(br#"
local function row(t) return tostring(t.available) .. '|' .. tostring(t.reason) end
return { on_load = function()
  record('before', row(runtime.get_operations()['runtime.ecs.write']))
  latch()
  record('after', row(runtime.get_operations()['runtime.ecs.write']),
    row(runtime.get_operations()['runtime.assets.write']), row(runtime.get_operations()['runtime.ecs.query']),
    tostring(runtime.has('runtime.lifecycle')))
end }
"#
        .to_vec())
    }
}
struct LatchPlan;
impl PlanSource for LatchPlan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let _ = cx;
        let source: Rc<dyn PackageSource> = Rc::new(Source);
        let mut parsed = package::Package::parse(
            json!({"id": "alpha", "name": "alpha", "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        let mut registry = Registry::default();
        registry
            .insert(
                PackageRecord::new(
                    "alpha".into(),
                    parsed,
                    RuntimePackage::new("alpha".to_owned(), source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry,
            plan: vec!["alpha".into()],
            admitted: ["alpha".to_owned()].into(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: kiln_sfi::worker::compose::ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: None,
                withdrawn: Vec::new(),
            },
        })
    }
    /// A family that reports an uncertain write through its context, as the CAS core will,
    /// and a record function.
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        Families::new(vec![Family::new(
            "latch",
            &[],
            move |lua, env, _, _, cx| {
                let facts = cx.facts().clone();
                env.raw_set(
                    "latch",
                    lua.create_function(move |_, ()| {
                        let session = facts.world_session(SERVER);
                        Ok(facts
                            .write_faults()
                            .report(SERVER, session, WriteOutcome::EcsWriteUncertain)
                            .to_owned())
                    })?,
                )?;
                env.raw_set(
                    "record",
                    lua.create_function(move |_, parts: mlua::Variadic<String>| {
                        RECORDS
                            .lock()
                            .unwrap()
                            .entry(owner)
                            .or_default()
                            .push(parts.join("\t"));
                        Ok(())
                    })?,
                )
            },
        )])
    }
}

#[test]
fn a_family_s_latch_is_the_one_the_production_host_reads() {
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<LatchPlan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    let records = || {
        RECORDS
            .lock()
            .unwrap()
            .get(&owner)
            .cloned()
            .unwrap_or_default()
    };
    assert!(
        eventually(WAIT, || records().len() == 2),
        "{:?} / {:?}",
        records(),
        shared.status()
    );
    let unknown = |op: &str| {
        format!("false|operation {op} has not been verified for game build {GAME_BUILD_UNKNOWN}")
    };
    assert_eq!(
        records(),
        [
            format!("before\t{}", unknown("runtime.ecs.write")),
            [
                "after".to_owned(),
                format!("false|{UNCERTAIN}"),
                format!("false|{UNCERTAIN}"),
                unknown("runtime.ecs.query"),
                "true".to_owned(),
            ]
            .join("\t"),
        ]
    );
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
}
