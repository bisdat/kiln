// SPDX-License-Identifier: MIT
// the interpreter's runtime.world session and context family tests.
//! `runtime.world.session_id`, `context_active`, `context_kind` and
//! `operation_available`.
//!
//! - The shared bodies (tests/shared/runtime_world_context.rs): the production
//!   family through the real worker, lifecycle host, installer and the registrar's production host.
//! - Here: the same functions on the real service adapter, the real registrar and a test catalog in which
//!   `runtime.ecs.query` is in the contract (so `session_id`'s live answer shows before the ECS queries land), the live
//!   context's every condition on its own, the argument rules, and the production catalog's rows.
//!
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), identical in both pins:
//! modules/mod-runtime/lua/ingame/world/read_world_context.rs:3-61, lua/ingame/check_ingame_access.rs:179-414,
//! game-runtime/native/engine-context/track_world_context.cpp:181-226.
#[path = "shared/runtime_world_context.rs"]
mod bodies;
#[allow(clippy::duplicate_mod)]
#[path = "support/fake_services/mod.rs"]
mod fake_services;
#[allow(clippy::duplicate_mod)]
mod support;

use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_world::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::Families;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::operation_catalog::{
    Backing, CATALOG, Catalog, Fact, FactRow, Need, OperationRow,
};
use kiln_sfi::runtime::overrides::{
    ClaimAuthority, HostAuthority, SESSION_REFUSAL, component_claim,
};
use kiln_sfi::runtime::services::{ServiceAdapter, ServiceError};
use kiln_sfi::runtime::world::context::{
    self, CAPABILITY, CLAIMS, ContextKind, NOT_ACTIVE, Snapshot, WorldContext,
};
use kiln_sfi::service_member;
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Table};
use serde_json::json;
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, register_runtime};
use sfi_core::host::{
    HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion,
};
use sfi_core::lua::LuaVm;
use sfi_core::package::{self, Manifest};
use std::cell::Cell;
use std::rc::Rc;
use std::sync::{Mutex, MutexGuard};
use support::*;

#[test]
fn on_a_dedicated_server_the_context_follows_the_worker_s_lifecycle_stream() {
    bodies::on_a_dedicated_server_the_context_follows_the_worker_s_lifecycle_stream()
}

#[test]
fn on_a_client_hosting_is_direct_and_joining_is_read_only() {
    bodies::on_a_client_hosting_is_direct_and_joining_is_read_only()
}

const BUILD: &str = "test-build";
const SERVER: i32 = KILN_WORLD_SERVER;
const CLIENT: i32 = KILN_WORLD_CLIENT;
const OPERATION: &str = "runtime.world.context.active";
/// The test catalog's `runtime.ecs.query` row (the production one waits for the ECS queries).
const QUERY: &str = "test.r07a.ecs.query";
/// ShroudForge's provider reason; the licence guard keeps the prefix out of MIT sources.
const NOT_READY: &str = concat!(
    "K",
    "FC Runtime world operation is not ready: runtime.world.context.active"
);
const INACTIVE: &str = "runtime API access is available only while this mod's lifecycle is active";

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {BUILD}")
}

fn query_member(a: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = a.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, ecs_query).map(drop)
}
static QUERY_NEEDS: [Need; 1] = [Need {
    member: query_member,
    capability: QUERY,
}];
/// The production context's need, as operation_catalog.rs declares it.
static CONTEXT_NEEDS: [Need; 1] = [Need {
    member: context::probe,
    capability: CAPABILITY,
}];
const fn row(operation: Op, backing: Backing) -> OperationRow {
    OperationRow { operation, backing }
}
const LATER: Backing = Backing::NotLanded("a later family");
/// The production rows of the world context, and `runtime.ecs.query` in the contract.
static TEST_CATALOG: Catalog = Catalog {
    operations: &[
        row(Op::Lifecycle, Backing::Interpreter),
        row(Op::IsMainMenu, LATER),
        row(Op::PlayerName, LATER),
        row(Op::NetworkStatus, LATER),
        row(Op::NetworkSend, LATER),
        row(Op::NetworkSendMod, LATER),
        row(Op::NetworkReceive, LATER),
        row(Op::NetworkReceiveMod, LATER),
        row(Op::NetworkAccept, LATER),
        row(Op::NetworkConnectedPeers, LATER),
        row(Op::EcsQuery, Backing::Kiln(&QUERY_NEEDS)),
        row(Op::EcsResolve, LATER),
        row(Op::EcsRead, LATER),
        row(Op::EcsWrite, LATER),
        row(Op::AssetsWrite, LATER),
        row(Op::WorldContextActive, Backing::Kiln(&CONTEXT_NEEDS)),
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
            fact: Fact::WorldContextActive,
            backing: Backing::Kiln(&CONTEXT_NEEDS),
            read: None,
        },
        FactRow {
            fact: Fact::WorldOperation(Op::WorldContextActive),
            backing: Backing::Kiln(&CONTEXT_NEEDS),
            read: None,
        },
    ],
};

// ---- the fakes this file changes, one test at a time ----

static SERIAL: Mutex<()> = Mutex::new(());
/// The core's session record and the caps rows are process-wide: the tests that change them take turns and put
/// them back (also after a failure).
struct Serial {
    _serial: MutexGuard<'static, ()>,
    _session: MutexGuard<'static, ()>,
}
impl Drop for Serial {
    fn drop(&mut self) {
        reset();
    }
}
fn reset() {
    set_session(SERVER, 0);
    set_session(CLIENT, 0);
    set_session_side(true);
    set_lifecycle(true);
    fake_services::set_cap(CAPABILITY, None);
    fake_services::set_cap(QUERY, None);
}
fn serial() -> Serial {
    let serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let session = session_lock();
    reset();
    Serial {
        _serial: serial,
        _session: session,
    }
}
fn cap(name: &str, state: i32) {
    let reason = if state == KILN_CAP_AVAILABLE {
        KILN_CAP_REASON_NONE
    } else if state == KILN_CAP_PENDING {
        KILN_CAP_REASON_SESSION
    } else {
        KILN_CAP_REASON_FAULTED
    };
    fake_services::set_cap(name, Some((state, reason)));
}

// ---- one runtime-create's facts and context, without a worker ----

struct Fixture {
    owner: KilnOwner,
    services: Rc<ServiceAdapter>,
    facts: Rc<RuntimeFacts>,
    world: Rc<WorldContext>,
    /// The lifecycle subscriber's stopping floor per world kind.
    floors: Rc<Cell<[u32; 3]>>,
}
fn fixture(side: Side) -> Fixture {
    let owner = next_owner();
    fake_services::bind_thread(owner);
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &TEST_CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: BUILD.into(),
        },
    ));
    let floors = Rc::new(Cell::new([0; 3]));
    let seen = floors.clone();
    let world = Rc::new(WorldContext::new(
        services.clone(),
        side,
        Box::new(move |kind: u32| seen.get()[kind as usize]),
    ));
    facts.attach_world(world.clone());
    Fixture {
        owner,
        services,
        facts,
        world,
        floors,
    }
}
impl Fixture {
    fn generation(&self, kind: i32, value: u64) {
        fake_services::set_world_session(self.owner, kind as u32, value);
    }
    fn started(&self, kind: i32, session: u32) {
        self.world.apply(WorldLifecycle {
            world: kind as u32,
            session,
            event: LifecycleEvent::Started,
        });
    }
    fn stopping(&self, kind: i32, session: u32) {
        self.world.apply(WorldLifecycle {
            world: kind as u32,
            session,
            event: LifecycleEvent::Stopping,
        });
    }
    fn floor(&self, kind: i32, session: u32) {
        let mut floors = self.floors.get();
        floors[kind as usize] = session;
        self.floors.set(floors);
    }
    /// A live world of `kind` in count `session`, generation `generation`, taken by the worker's stream.
    fn live(&self, kind: i32, session: u32, generation: u64) {
        set_session(kind, session);
        self.generation(kind, generation);
        self.started(kind, session);
    }
    fn reads(&self) -> u64 {
        fake_services::counters(self.owner).calls("game_world_session")
    }
}
fn direct(session: u64) -> Snapshot {
    Snapshot {
        session,
        kind: ContextKind::Direct,
    }
}

// ---- packages on the registrar with the four functions ----

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
struct Package {
    vm: LuaVm,
    env: Table,
    active: Rc<Active>,
}
fn package(fx: &Fixture, id: &str, capabilities: &[&str], phase: Phase, side: Side) -> Package {
    let context = HostContext::new(phase, side, SourceVersion::Sf179);
    let active = Rc::new(Active(Cell::new(true)));
    let host = Rc::new(KilnRuntimeHost::new(
        &manifest(id, capabilities),
        &context,
        active.clone(),
        fx.facts.clone(),
    ));
    let vm = LuaVm::new(context.clone()).unwrap();
    let facts = fx.facts.clone();
    let env = vm
        .add_package(
            RuntimePackage::new(id.to_owned(), Rc::new(NoFiles)),
            |lua, env, owner| {
                let game: Table = env.raw_get("game")?;
                game.raw_set("types", lua.create_table()?)?;
                let runtime = register_runtime(lua, env, owner, vm.context(), &game, host.clone())?;
                let world = lua.create_table()?;
                runtime.raw_set("world", world.clone())?;
                context::install_into(
                    lua,
                    &world,
                    host.clone() as Rc<dyn RuntimeHost>,
                    facts.clone(),
                )
            },
        )
        .unwrap();
    Package { vm, env, active }
}
impl Package {
    fn eval<T: FromLuaMulti>(&self, source: &str) -> mlua::Result<T> {
        self.vm
            .lua()
            .load(source)
            .set_name("=runtime_07a")
            .set_environment(self.env.clone())
            .eval()
    }
    fn session(&self) -> (u64, String) {
        self.eval("local s = runtime.world.session_id(); return s, math.type(s)")
            .unwrap()
    }
    fn active(&self) -> (bool, Option<String>) {
        self.eval("return runtime.world.context_active()").unwrap()
    }
    fn kind(&self) -> String {
        self.eval("return runtime.world.context_kind()").unwrap()
    }
    fn available(&self, name: &str) -> bool {
        self.eval(&format!("return runtime.world.operation_available({name})"))
            .unwrap()
    }
    fn status(&self, expression: &str) -> Status {
        self.eval(&format!(
            "local s = {expression}; return s.available, s.reason"
        ))
        .unwrap()
    }
    /// has, status and the catalog entry of the operation.
    fn r_reg(&self) -> (bool, Status, Status) {
        (
            self.eval(&format!("return runtime.has('{OPERATION}')"))
                .unwrap(),
            self.status(&format!("runtime.status('{OPERATION}')")),
            self.status(&format!("runtime.get_operations()['{OPERATION}']")),
        )
    }
}
/// (available, reason) of a status table.
type Status = (bool, Option<String>);
fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        other => other.to_string(),
    }
}

// ---- the production catalog ----

#[test]
fn the_production_catalog_backs_the_context_and_the_ecs_reads() {
    assert_eq!(CATALOG.check(), Ok(()));
    assert_eq!(TEST_CATALOG.check(), Ok(()));
    let need = |backing: Backing| match backing {
        Backing::Kiln([need]) => need.capability,
        _ => panic!("one Kiln need"),
    };
    let operation = CATALOG.row(Op::WorldContextActive).unwrap().backing;
    assert_eq!(need(operation), "world.lifecycle");
    assert_eq!(CAPABILITY, "world.lifecycle");
    let fact = |fact: Fact| {
        CATALOG
            .facts
            .iter()
            .find(|row| row.fact == fact)
            .unwrap_or_else(|| panic!("no row for {fact:?}"))
    };
    for decided in [
        Fact::WorldContextActive,
        Fact::WorldOperation(Op::WorldContextActive),
    ] {
        let row = fact(decided);
        assert_eq!(need(row.backing), "world.lifecycle", "{decided:?}");
        assert!(row.read.is_none(), "{decided:?}: the context decides it");
    }
    // ECS readiness and the queries are kiln.world's ECS reads (tests/runtime_ecs_reads.rs).
    assert_eq!(need(fact(Fact::Ready).backing), "world.ecs_read");
    assert_eq!(
        need(CATALOG.row(Op::EcsQuery).unwrap().backing),
        "world.ecs_read"
    );
    // The probe: kiln.world's game_world_session and the core's session record, copied, never called.
    let _serial = serial();
    let fx = fixture(Side::Server);
    assert_eq!(context::probe(&fx.services), Ok(()));
    assert_eq!(fx.reads(), 0);
    assert_eq!(fx.services.counters().calls, 0);
}

// ---- the live context, each condition on its own ----

#[test]
fn the_live_context_needs_the_session_world_the_stream_the_floor_and_a_steady_sample() {
    let _serial = serial();
    let fx = fixture(Side::Server);
    // The menu.
    assert_eq!(fx.world.read(), Snapshot::NONE);
    // A dedicated server's world, taken by the stream: direct, its generation.
    fx.live(SERVER, 1, 5);
    assert_eq!(fx.world.read(), direct(5));
    // One sample: two generation reads around two session records.
    let before = (fx.reads(), fx.services.counters().calls);
    fx.world.read();
    assert_eq!(fx.reads() - before.0, 2);
    assert_eq!(fx.services.counters().calls - before.1, 4);
    // The generation moved between its two reads: no session.
    fake_services::script_world_session(fx.owner, SERVER as u32, &[5, 6]);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    assert_eq!(fx.world.read(), direct(5), "the next steady sample");
    // kiln.world has no session for the world yet.
    fx.generation(SERVER, 0);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    fx.generation(SERVER, 5);
    // The core does not see stops.
    set_lifecycle(false);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    set_lifecycle(true);
    // The subscriber saw this session (or a newer one) stopping.
    fx.floor(SERVER, 1);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    fx.floor(SERVER, 2);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    fx.floor(SERVER, 0);
    fx.floor(CLIENT, 9);
    assert_eq!(
        fx.world.read(),
        direct(5),
        "another kind's floor is not this world's"
    );
    // The core lists a newer session the stream has not taken: none; then the stream takes it.
    fx.live(SERVER, 2, 7);
    assert_eq!(
        fx.world.read(),
        direct(7),
        "the same world pointer, a new session"
    );
    set_session(SERVER, 3);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    // A bare STOPPING ends the live session and every older one; a STARTED of an ended one is stale.
    set_session(SERVER, 2);
    fx.stopping(SERVER, 3);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    fx.started(SERVER, 2);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    // The core no longer lists a world.
    fx.live(SERVER, 4, 9);
    assert_eq!(fx.world.read(), direct(9));
    set_session(SERVER, 0);
    assert_eq!(fx.world.read(), Snapshot::NONE);
}

#[test]
fn a_client_s_session_world_is_its_client_world_hosting_or_joined() {
    let _serial = serial();
    set_session_side(false);
    let fx = fixture(Side::Client);
    // Hosting before the client world runs: the server world alone is not the session world.
    fx.live(SERVER, 1, 5);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    fx.live(CLIENT, 1, 3);
    assert_eq!(
        fx.world.read(),
        direct(3),
        "hosting: the client world's generation, direct"
    );
    // Joined to another machine's world: only the client world runs here.
    set_session(SERVER, 0);
    assert_eq!(
        fx.world.read(),
        Snapshot {
            session: 3,
            kind: ContextKind::ClientReadOnly
        }
    );
    // The client world's floor and stream count, not the server world's.
    fx.floor(SERVER, 5);
    fx.stopping(SERVER, 1);
    assert!(fx.world.read().active());
    fx.floor(CLIENT, 1);
    assert_eq!(fx.world.read(), Snapshot::NONE);
    // A server-side runtime never reads a client world.
    let server = fixture(Side::Server);
    server.live(CLIENT, 2, 4);
    assert_eq!(server.world.read(), Snapshot::NONE);
}

// ---- the four functions ----

#[test]
fn session_id_is_the_live_session_after_the_ecs_query_denial() {
    let _serial = serial();
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    cap(QUERY, KILN_CAP_AVAILABLE);
    let fx = fixture(Side::Server);
    let alpha = package(&fx, "alpha", &["runtime"], Phase::Runtime, Side::Server);
    assert_eq!(alpha.session(), (0, "integer".into()), "no world");
    fx.live(SERVER, 1, 5);
    assert_eq!(alpha.session(), (5, "integer".into()));
    // Arguments are ignored.
    assert_eq!(
        alpha
            .eval::<u64>("return runtime.world.session_id(1, {}, nil)")
            .unwrap(),
        5
    );
    // Each denial of runtime.ecs.query answers 0 with the world live.
    alpha.active.0.set(false);
    assert_eq!(alpha.session().0, 0, "inactive lifecycle");
    alpha.active.0.set(true);
    let beta = package(&fx, "beta", &[], Phase::Runtime, Side::Server);
    assert_eq!(beta.session().0, 0, "no runtime permission");
    let early = package(&fx, "early", &["runtime"], Phase::Startup, Side::Server);
    assert_eq!(early.session().0, 0, "startup phase");
    cap(QUERY, KILN_CAP_UNAVAILABLE);
    assert_eq!(
        alpha.session().0,
        0,
        "runtime.ecs.query outside the contract"
    );
    assert_eq!(alpha.active(), (true, None), "the context itself is live");
    cap(QUERY, KILN_CAP_AVAILABLE);
    // The context's row off: no session.
    cap(CAPABILITY, KILN_CAP_UNAVAILABLE);
    assert_eq!(alpha.session().0, 0);
    // Pending: the operation stays in the contract, but the context is not read.
    cap(CAPABILITY, KILN_CAP_PENDING);
    assert_eq!(alpha.session().0, 0);
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    assert_eq!(alpha.session().0, 5);
}

#[test]
fn context_active_and_context_kind_answer_shroudforge_s_shapes() {
    let _serial = serial();
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    let fx = fixture(Side::Server);
    let alpha = package(&fx, "alpha", &["runtime"], Phase::Runtime, Side::Server);
    assert_eq!(alpha.active(), (false, Some(NOT_ACTIVE.into())));
    assert_eq!(alpha.kind(), "unknown");
    fx.live(SERVER, 1, 5);
    assert_eq!(alpha.active(), (true, None));
    assert_eq!(alpha.kind(), "direct");
    assert_eq!(
        alpha
            .eval::<(bool, Option<String>)>("return runtime.world.context_active(1, 2)")
            .unwrap(),
        (true, None)
    );
    // The denials, in ShroudForge's order, each with kind "unknown".
    alpha.active.0.set(false);
    assert_eq!(alpha.active(), (false, Some(INACTIVE.into())));
    assert_eq!(alpha.kind(), "unknown");
    alpha.active.0.set(true);
    let beta = package(&fx, "beta", &[], Phase::Runtime, Side::Server);
    assert_eq!(
        beta.active(),
        (
            false,
            Some("mod 'beta' requires capabilities: [\"runtime\"]".into())
        )
    );
    assert_eq!(beta.kind(), "unknown");
    let early = package(&fx, "early", &["runtime"], Phase::Startup, Side::Server);
    assert_eq!(
        early.active(),
        (
            false,
            Some("runtime APIs are available only during the ingame phase".into())
        )
    );
    assert_eq!(early.kind(), "unknown");
    cap(CAPABILITY, KILN_CAP_UNAVAILABLE);
    assert_eq!(alpha.active(), (false, Some(unverified(OPERATION))));
    assert_eq!(alpha.kind(), "unknown");
    // A pending row keeps the operation in the contract; the context waits for the row.
    cap(CAPABILITY, KILN_CAP_PENDING);
    assert_eq!(alpha.active(), (false, Some(NOT_ACTIVE.into())));
    assert_eq!(alpha.kind(), "unknown");
    // A client joined to another machine's world.
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    set_session_side(false);
    set_session(SERVER, 0);
    let client = fixture(Side::Client);
    let gamma = package(&client, "gamma", &["runtime"], Phase::Runtime, Side::Client);
    client.live(CLIENT, 1, 3);
    assert_eq!(gamma.active(), (true, None));
    assert_eq!(gamma.kind(), "client-read-only");
}

#[test]
fn operation_available_converts_its_name_and_answers_only_world_operations() {
    let _serial = serial();
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    cap(QUERY, KILN_CAP_AVAILABLE);
    let fx = fixture(Side::Server);
    let alpha = package(&fx, "alpha", &["runtime"], Phase::Runtime, Side::Server);
    let op = format!("'{OPERATION}'");
    assert!(!alpha.available(&op), "no context");
    fx.live(SERVER, 1, 5);
    assert!(alpha.available(&op));
    // In the contract but not a world operation; outside the contract; not an operation; numbers coerce.
    for name in [
        "'runtime.lifecycle'",
        "'runtime.ecs.query'",
        "'runtime.world.voxel.read'",
        "'runtime.world.context_active'",
        "'made.up'",
        "''",
        "12",
        "1.5",
    ] {
        assert!(!alpha.available(name), "{name}");
    }
    // ShroudForge's argument errors, before any denial.
    for (call, kind) in [("", "nil"), ("{}", "table"), ("false", "boolean")] {
        let error = alpha
            .eval::<bool>(&format!("return runtime.world.operation_available({call})"))
            .unwrap_err();
        assert_eq!(
            root(&error),
            format!("bad argument #1 (string expected, got {kind})"),
            "{call}"
        );
    }
    let error = alpha
        .eval::<bool>("return runtime.world.operation_available('\\255')")
        .unwrap_err();
    assert!(root(&error).contains("invalid utf-8"), "{error}");
    let beta = package(&fx, "beta", &[], Phase::Runtime, Side::Server);
    assert!(
        beta.eval::<bool>("return runtime.world.operation_available(nil) == nil")
            .is_err()
    );
    // The denials.
    assert!(!beta.available(&op), "no runtime permission");
    alpha.active.0.set(false);
    assert!(!alpha.available(&op), "inactive lifecycle");
    alpha.active.0.set(true);
    cap(CAPABILITY, KILN_CAP_UNAVAILABLE);
    assert!(!alpha.available(&op), "outside the contract");
}

#[test]
fn r_reg_reads_both_facts_from_one_sample_of_the_context() {
    let _serial = serial();
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    let fx = fixture(Side::Server);
    let alpha = package(&fx, "alpha", &["runtime"], Phase::Runtime, Side::Server);
    assert_eq!(
        alpha.r_reg(),
        (
            false,
            (false, Some(NOT_READY.into())),
            (false, Some(NOT_READY.into()))
        )
    );
    fx.live(SERVER, 1, 5);
    let before = fx.reads();
    let facts = fx.facts.provider_facts();
    assert!(facts.world_context_active);
    assert!(facts.world_operation_available(Op::WorldContextActive));
    assert_eq!(fx.reads() - before, 2, "one sample for both facts");
    assert!(!facts.world_operation_available(Op::WorldVoxelRead));
    assert_eq!(alpha.r_reg(), (true, (true, None), (true, None)));
    // Pending: in the contract, the facts wait for the row (as the functions do).
    cap(CAPABILITY, KILN_CAP_PENDING);
    assert_eq!(
        alpha.r_reg(),
        (
            false,
            (false, Some(NOT_READY.into())),
            (false, Some(NOT_READY.into()))
        )
    );
    // Without an attached context (a runtime whose family never ran its hook) the facts are false.
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    let bare = Rc::new(RuntimeFacts::new(
        fx.services.clone(),
        &TEST_CATALOG,
        fx.facts.config().clone(),
    ));
    assert!(bare.world().is_none());
    assert!(!bare.provider_facts().world_context_active);
    assert_eq!(bare.world_snapshot(), Snapshot::NONE);
    // The first context attached stays.
    let other = Rc::new(WorldContext::new(
        fx.services.clone(),
        Side::Client,
        Box::new(|_: u32| 0),
    ));
    fx.facts.attach_world(other);
    assert_eq!(fx.facts.world().unwrap().side(), Side::Server);
}

// ---- the claims' session ----

/// ShroudForge's write claims bind to its world session marker, which is 0 once the world stops; kiln.world's raw
/// generation stays nonzero (advanced) after a stop. So the production claim authority reads the live context's
/// session: a claim made after the stop is refused, as in ShroudForge.
#[test]
fn the_claim_authority_reads_the_live_context_s_session_not_kiln_world_s_generation() {
    let _serial = serial();
    cap(CAPABILITY, KILN_CAP_AVAILABLE);
    let fx = fixture(Side::Server);
    let context = HostContext::new(Phase::Runtime, Side::Server, SourceVersion::Sf179);
    let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
        &manifest("alpha", &["runtime"]),
        &context,
        Rc::new(Active(Cell::new(true))),
        fx.facts.clone(),
    ));
    let server = Rc::new(HostAuthority::on_world(
        host.clone(),
        fx.facts.clone(),
        SERVER as u32,
    ));
    let client = HostAuthority::on_world(host, fx.facts.clone(), CLIENT as u32);
    let claim = |authority: &Rc<HostAuthority>| {
        component_claim(
            fx.facts.ownership(),
            authority.clone() as Rc<dyn ClaimAuthority>,
            7,
            "Health",
            None,
        )
        .map(|claim| claim.session())
    };
    assert_eq!(server.world_session(), 0, "no world");
    assert_eq!(claim(&server), Err(SESSION_REFUSAL.to_owned()));
    fx.live(SERVER, 1, 5);
    assert_eq!(server.world_session(), 5);
    assert_eq!(claim(&server), Ok(5));
    // A server-side runtime's session world is the server world.
    fx.generation(CLIENT, 4);
    assert_eq!(client.world_session(), 0);
    // The world stops: kiln.world advances its generation, the subscriber's floor passes the session.
    fx.generation(SERVER, 6);
    fx.floor(SERVER, 1);
    assert_eq!(
        fx.facts.world_session(SERVER as u32),
        6,
        "kiln.world's generation stays nonzero after the stop"
    );
    assert_eq!(server.world_session(), 0);
    assert_eq!(claim(&server), Err(SESSION_REFUSAL.to_owned()));
    // The worker takes the STOPPING and the core forgets the world: still none.
    fx.stopping(SERVER, 1);
    set_session(SERVER, 0);
    assert_eq!(server.world_session(), 0);
    assert_eq!(claim(&server), Err(SESSION_REFUSAL.to_owned()));
}

#[test]
fn the_family_claims_its_four_names_inside_the_world_container() {
    assert_eq!(
        CLAIMS,
        [
            "runtime.world.operation_available",
            "runtime.world.context_active",
            "runtime.world.context_kind",
            "runtime.world.session_id",
        ]
    );
    let family = context::family();
    assert_eq!(family.name(), context::FAMILY);
    assert_eq!(family.claims(), CLAIMS);
    assert!(Families::new(vec![family]).is_ok());
    // Its names belong to it alone.
    let error = Families::new(vec![
        context::family(),
        kiln_sfi::runtime::Family::new("thief", &["runtime.world.session_id"], |_, _, _, _, _| {
            Ok(())
        }),
    ])
    .err()
    .unwrap();
    assert!(error.contains("runtime.world.session_id"), "{error}");
}
