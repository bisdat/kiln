// SPDX-License-Identifier: MIT
// the interpreter's runtime.world.voxel family tests.
//! `runtime.world.voxel.get_grid_spec` on the real service adapter, the real registrar, the world context and the
//! C-ABI fake kiln.build (`grid_for_session`): the denial first, the arity and the id's conversion, the session (none,
//! a dedicated server, a hosting client's server world, a client joined to another machine's world, a session that moves
//! during the call), every result of the member as ShroudForge's text, the metadata checks, the table's shape, and the
//! production catalog's and registrar's rows.
//!
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9:
//! modules/mod-runtime/lua/ingame/world/read_write_voxels.rs:3-40, game-runtime/bridge/windows/voxels/read_grid_spec.rs:6-56,
//! native/voxels/read_registered_voxel_grid.cpp:373-435, lua/ingame/check_ingame_access.rs:93-98,264-265.
#[allow(clippy::duplicate_mod)]
#[path = "support/fake_services/mod.rs"]
mod fake_services;
#[allow(clippy::duplicate_mod)]
mod support;

use kiln_abi::services::kiln_build::KilnBuildGridSpec;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::operation_catalog::{
    Backing, CATALOG, Catalog, Fact, FactRow, Need, OperationRow,
};
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::world::context::{self, WorldContext};
use kiln_sfi::runtime::world::voxels::{self, *};
use kiln_sfi::worker::*;
use mlua::{FromLuaMulti, Table, Value};
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

const BUILD: &str = "test-build";
const SERVER: i32 = KILN_WORLD_SERVER;
const CLIENT: i32 = KILN_WORLD_CLIENT;
const GRID_OP: &str = "runtime.world.voxel.grid_spec";
/// kiln-sfi's plugin owner as the family gets it (`RuntimeContext::owner`): the member's caller.
const PLUGIN_OWNER: KilnOwner = 0x5f1;
const INACTIVE: &str = "runtime API access is available only while this mod's lifecycle is active";

fn unverified(operation: &str) -> String {
    format!("operation {operation} has not been verified for game build {BUILD}")
}

static CONTEXT_NEEDS: [Need; 1] = [Need {
    member: context::probe,
    capability: context::CAPABILITY,
}];
static GRID_NEEDS: [Need; 1] = [Need {
    member: voxels::probe,
    capability: GRID_CAPABILITY,
}];
const fn row(operation: Op, backing: Backing) -> OperationRow {
    OperationRow { operation, backing }
}
const LATER: Backing = Backing::NotLanded("a later family");
/// The production rows of the world context and the voxel grid.
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
        row(Op::EcsQuery, LATER),
        row(Op::EcsResolve, LATER),
        row(Op::EcsRead, LATER),
        row(Op::EcsWrite, LATER),
        row(Op::AssetsWrite, LATER),
        row(Op::WorldContextActive, Backing::Kiln(&CONTEXT_NEEDS)),
        row(Op::WorldCursorGet, LATER),
        row(Op::WorldBuildingInput, LATER),
        row(Op::WorldVoxelRead, LATER),
        row(Op::WorldVoxelWrite, LATER),
        row(Op::WorldVoxelGridSpec, Backing::Kiln(&GRID_NEEDS)),
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
        FactRow {
            fact: Fact::WorldOperation(Op::WorldVoxelRead),
            backing: Backing::Kiln(&GRID_NEEDS),
            read: None,
        },
        FactRow {
            fact: Fact::WorldOperation(Op::WorldVoxelGridSpec),
            backing: Backing::Kiln(&GRID_NEEDS),
            read: None,
        },
    ],
};

// ---- the process-wide fakes, one test at a time ----

static SERIAL: Mutex<()> = Mutex::new(());
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
    fake_services::set_cap(context::CAPABILITY, None);
    fake_services::set_cap(GRID_CAPABILITY, None);
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
    } else {
        KILN_CAP_REASON_SYMBOL_MISSING
    };
    fake_services::set_cap(name, Some((state, reason)));
}
fn rows_on() {
    cap(context::CAPABILITY, KILN_CAP_AVAILABLE);
    cap(GRID_CAPABILITY, KILN_CAP_AVAILABLE);
}

// ---- one runtime-create's facts and context ----

struct Fixture {
    owner: KilnOwner,
    services: Rc<ServiceAdapter>,
    facts: Rc<RuntimeFacts>,
    world: Rc<WorldContext>,
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
    let world = Rc::new(WorldContext::new(services.clone(), side, Box::new(|_| 0)));
    facts.attach_world(world.clone());
    Fixture {
        owner,
        services,
        facts,
        world,
    }
}
impl Fixture {
    /// A live world of `kind` in count `session`, generation `generation`, taken by the worker's stream.
    fn live(&self, kind: i32, session: u32, generation: u64) {
        set_session(kind, session);
        fake_services::set_world_session(self.owner, kind as u32, generation);
        self.world.apply(WorldLifecycle {
            world: kind as u32,
            session,
            event: LifecycleEvent::Started,
        });
    }
    fn grid_calls(&self) -> Vec<(u64, Vec<u8>)> {
        fake_services::grid_calls(self.owner)
    }
}

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
fn package(fx: &Fixture, id: &str, phase: Phase, side: Side) -> Package {
    let context = HostContext::new(phase, side, SourceVersion::Sf179);
    let active = Rc::new(Active(Cell::new(true)));
    let host = Rc::new(KilnRuntimeHost::new(
        &manifest(id, &["runtime"]),
        &context,
        active.clone(),
        fx.facts.clone(),
    ));
    let vm = LuaVm::new(context.clone()).unwrap();
    let (facts, services) = (fx.facts.clone(), fx.services.clone());
    let env = vm
        .add_package(
            RuntimePackage::new(id.to_owned(), Rc::new(NoFiles)),
            |lua, env, owner| {
                let game: Table = env.raw_get("game")?;
                game.raw_set("types", lua.create_table()?)?;
                let runtime = register_runtime(lua, env, owner, vm.context(), &game, host.clone())?;
                let world = lua.create_table()?;
                runtime.raw_set("world", world.clone())?;
                let voxel = lua.create_table()?;
                world.raw_set("voxel", voxel.clone())?;
                context::install_into(
                    lua,
                    &world,
                    host.clone() as Rc<dyn RuntimeHost>,
                    facts.clone(),
                )?;
                voxels::install_into(
                    lua,
                    &voxel,
                    host.clone() as Rc<dyn RuntimeHost>,
                    facts.clone(),
                    services.clone(),
                    PLUGIN_OWNER,
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
            .set_name("=runtime_world_voxels")
            .set_environment(self.env.clone())
            .eval()
    }
    /// `get_grid_spec(args)` as (a table or nil, the reason).
    fn grid(&self, args: &str) -> (Value, Option<String>) {
        self.eval(&format!("return runtime.world.voxel.get_grid_spec({args})"))
            .unwrap()
    }
    fn reason(&self, args: &str) -> Option<String> {
        let (value, reason) = self.grid(args);
        assert!(value.is_nil(), "{args}: no table with a reason");
        reason
    }
}
fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        other => other.to_string(),
    }
}
fn spec_with(edit: impl FnOnce(&mut KilnBuildGridSpec)) -> KilnBuildGridSpec {
    let mut spec = fake_services::current_grid();
    edit(&mut spec);
    spec
}

// ---- the production catalog and registrar ----

#[test]
fn the_production_catalog_backs_the_grid_spec_and_keeps_reads_and_writes_refused() {
    assert_eq!(CATALOG.check(), Ok(()));
    assert_eq!(TEST_CATALOG.check(), Ok(()));
    let need = |backing: Backing| match backing {
        Backing::Kiln([need]) => need.capability,
        _ => panic!("one Kiln need"),
    };
    assert_eq!(
        need(CATALOG.row(Op::WorldVoxelGridSpec).unwrap().backing),
        GRID_CAPABILITY
    );
    assert_eq!(GRID_CAPABILITY, "build.voxel_grid_tasks");
    assert!(matches!(
        CATALOG.row(Op::WorldVoxelRead).unwrap().backing,
        Backing::NotLanded(_)
    ));
    assert!(matches!(
        CATALOG.row(Op::WorldVoxelWrite).unwrap().backing,
        Backing::NotLanded(_)
    ));
    for op in [Op::WorldVoxelRead, Op::WorldVoxelGridSpec] {
        let row = CATALOG
            .facts
            .iter()
            .find(|r| r.fact == Fact::WorldOperation(op))
            .expect("a fact row");
        assert_eq!(need(row.backing), GRID_CAPABILITY, "{op:?}");
        assert!(row.read.is_none());
    }
    // The registrar: the family claims get_grid_spec; read and write stay the unavailable family's.
    let families = kiln_sfi::runtime::registrar::production(kiln_sfi::runtime::registrar::Parts {
        types: None,
        ecs: None,
        attributes: None,
        assets: None,
        network: kiln_sfi::runtime::Family::new("test.network", &[], |_, _, _, _, _| Ok(())),
        ui: kiln_sfi::runtime::Family::new("test.ui", &[], |_, _, _, _, _| Ok(())),
        trackers: None,
    })
    .unwrap();
    assert!(
        families
            .iter()
            .any(|f| f.name() == FAMILY && f.claims() == ["runtime.world.voxel.get_grid_spec"])
    );
    let unavailable = families
        .iter()
        .find(|f| f.name() == kiln_sfi::runtime::unavailable::FAMILY)
        .expect("the unavailable family")
        .claims()
        .to_vec();
    assert!(!unavailable.contains(&"runtime.world.voxel.get_grid_spec"));
    assert!(
        unavailable.contains(&"runtime.world.voxel.read")
            && unavailable.contains(&"runtime.world.voxel.write")
    );
}

// ---- the denial, the arity, the id ----

#[test]
fn the_denial_comes_first_then_the_arity_then_the_id() {
    let _serial = serial();
    let fx = fixture(Side::Server);
    let startup = package(&fx, "early", Phase::Startup, Side::Server);
    assert_eq!(
        startup.reason("'voxel'").as_deref(),
        Some("runtime APIs are available only during the ingame phase")
    );
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Server);
    // The row is not there: outside the contract.
    assert_eq!(alpha.reason("'voxel'"), Some(unverified(GRID_OP)));
    cap(GRID_CAPABILITY, KILN_CAP_UNAVAILABLE);
    assert_eq!(alpha.reason("'voxel'"), Some(unverified(GRID_OP)));
    rows_on();
    alpha.active.0.set(false);
    assert_eq!(alpha.reason("'voxel'").as_deref(), Some(INACTIVE));
    alpha.active.0.set(true);
    // The arity, before the id converts.
    for args in ["", "'voxel', 1", "nil, nil"] {
        let error = alpha
            .eval::<Value>(&format!("return runtime.world.voxel.get_grid_spec({args})"))
            .unwrap_err();
        assert!(root(&error).contains(GRID_ARITY), "{args}: {error}");
    }
    let error = alpha
        .eval::<Value>("return runtime.world.voxel.get_grid_spec({})")
        .unwrap_err();
    assert!(
        !root(&error).contains(GRID_ARITY),
        "a table does not convert: {error}"
    );
    assert!(fx.grid_calls().is_empty(), "nothing reached the member");
}

// ---- the session ----

#[test]
fn without_a_live_world_the_session_has_changed_and_nothing_is_called() {
    let _serial = serial();
    rows_on();
    let fx = fixture(Side::Server);
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Server);
    assert_eq!(alpha.reason("'voxel'").as_deref(), Some(SESSION_CHANGED));
    assert!(fx.grid_calls().is_empty());
}

#[test]
fn a_dedicated_server_gets_shroudforge_s_table() {
    let _serial = serial();
    rows_on();
    let fx = fixture(Side::Server);
    fx.live(SERVER, 1, 9);
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Server);
    let shape: (String, f64, f64, f64, f64, f64, f64, i64, i64, i64, i64, String) = alpha
        .eval(
            "local s = runtime.world.voxel.get_grid_spec('voxel')
             return s.id, s.origin[1], s.origin.y, s.origin[3], s.cellSize[1], s.cellSize.y, s.cellSize.z,
                 s.maximum[1], s.maximum.x, s.maximum.y, s.maximum[3], math.type(s.maximum[1])",
        )
        .unwrap();
    assert_eq!(
        shape,
        (
            "voxel".into(),
            0.0,
            -4.0,
            0.0,
            0.5,
            0.5,
            0.5,
            20480,
            20480,
            0x7fff_ffff - 7,
            20480,
            "integer".into()
        )
    );
    let keys: Vec<String> = alpha
        .eval::<Table>("local k = {} for key in pairs(runtime.world.voxel.get_grid_spec('voxel')) do k[#k + 1] = key end table.sort(k) return k")
        .unwrap()
        .sequence_values::<String>()
        .map(Result::unwrap)
        .collect();
    assert_eq!(keys, ["cellSize", "id", "maximum", "origin"]);
    assert_eq!(
        fx.grid_calls()[0],
        (9, b"voxel".to_vec()),
        "the server world's session and the id"
    );
    // The caller is the interpreter's own owner (the core's ShroudForge-dialect owner): kiln.build's busy latch is
    // per caller, and that owner is never latched.
    let callers = fake_services::grid_callers(fx.owner);
    assert!(
        !callers.is_empty() && callers.iter().all(|c| *c == PLUGIN_OWNER),
        "{callers:?}"
    );
    let (has, available, read): (bool, bool, bool) = alpha
        .eval(
            "return runtime.has('runtime.world.voxel.grid_spec'), runtime.world.operation_available('runtime.world.voxel.grid_spec'),
                runtime.has('runtime.world.voxel.read')",
        )
        .unwrap();
    assert!(
        has && available && !read,
        "grid_spec is in the contract and ready, read is not"
    );
}

#[test]
fn a_hosting_client_asks_for_its_server_world_and_a_joined_client_is_refused() {
    let _serial = serial();
    set_session_side(false);
    rows_on();
    let fx = fixture(Side::Client);
    fx.live(SERVER, 1, 7);
    fx.live(CLIENT, 1, 3);
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Client);
    let (value, reason) = alpha.grid("'voxel'");
    assert!(value.is_table() && reason.is_none(), "{reason:?}");
    assert_eq!(
        fx.grid_calls().last().unwrap().0,
        7,
        "the hosted server world's session, not the client world's"
    );
    // Joined to another machine's world: only the client world runs here.
    set_session(SERVER, 0);
    fake_services::set_world_session(fx.owner, SERVER as u32, 0);
    let calls = fx.grid_calls().len();
    assert_eq!(alpha.reason("'voxel'").as_deref(), Some(GRID_UNAVAILABLE));
    // ...but an id ShroudForge's native grid does not know is rejected first there (its id check comes before its
    // session guard), and so here.
    for args in ["'fog'", "''", "'voxel-voxel-voxel'"] {
        assert_eq!(alpha.reason(args).as_deref(), Some(GRID_REJECTED), "{args}");
    }
    assert_eq!(
        fx.grid_calls().len(),
        calls,
        "no call without a local authority"
    );
}

#[test]
fn a_session_that_moves_during_the_call_has_changed() {
    let _serial = serial();
    rows_on();
    let fx = fixture(Side::Server);
    fx.live(SERVER, 1, 9);
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Server);
    // Before the call: the context's two samples and the server session; after it: the same three, moved on.
    fake_services::script_world_session(fx.owner, SERVER as u32, &[9, 9, 9, 10, 10, 10]);
    assert_eq!(alpha.reason("'voxel'").as_deref(), Some(SESSION_CHANGED));
    assert_eq!(
        fx.grid_calls().len(),
        1,
        "it was called once, with the old session"
    );
}

// ---- every result of the member, and the metadata ----

#[test]
fn every_result_of_the_member_is_shroudforge_s_text() {
    let _serial = serial();
    rows_on();
    let fx = fixture(Side::Server);
    fx.live(SERVER, 1, 9);
    let alpha = package(&fx, "alpha", Phase::Runtime, Side::Server);
    for (knob, text) in [
        (fake_services::Knob::Busy, GRID_PENDING),
        (fake_services::Knob::Unreadable, GRID_PENDING),
        (fake_services::Knob::Stale, SESSION_CHANGED),
        (fake_services::Knob::Unavailable, GRID_UNAVAILABLE),
    ] {
        fake_services::set_knob(fx.owner, knob);
        assert_eq!(alpha.reason("'voxel'").as_deref(), Some(text), "{knob:?}");
    }
    fake_services::set_knob(fx.owner, fake_services::Knob::Ok);
    // Another grid, a number that converts, too long an id: the member rejects them.
    for args in ["'fog'", "12", "'voxel-voxel-voxel'", "''"] {
        assert_eq!(alpha.reason(args).as_deref(), Some(GRID_REJECTED), "{args}");
    }
    assert_eq!(
        fx.grid_calls().iter().filter(|(_, id)| id == b"12").count(),
        1,
        "the number arrives as text"
    );
    // A NUL in the id never reaches the member.
    let calls = fx.grid_calls().len();
    assert_eq!(alpha.reason("'vox\\0el'").as_deref(), Some(NUL_ID));
    assert_eq!(fx.grid_calls().len(), calls);
    // The metadata, checked as ShroudForge checks it.
    for (what, spec) in [
        ("a zero cell", spec_with(|s| s.cell_size[1] = 0.0)),
        ("a negative cell", spec_with(|s| s.cell_size[0] = -0.5)),
        (
            "an infinite cell",
            spec_with(|s| s.cell_size[2] = f64::INFINITY),
        ),
        ("a NaN origin", spec_with(|s| s.origin[0] = f64::NAN)),
        ("a zero maximum", spec_with(|s| s.maximum[1] = 0)),
        (
            "another id",
            spec_with(|s| s.id[0] = b'b' as core::ffi::c_char),
        ),
        (
            "an id without its NUL",
            spec_with(|s| s.id = [b'v' as core::ffi::c_char; 16]),
        ),
    ] {
        fake_services::set_grid_spec(fx.owner, Some(spec));
        assert_eq!(
            alpha.reason("'voxel'").as_deref(),
            Some(GRID_INVALID),
            "{what}"
        );
    }
    fake_services::set_grid_spec(fx.owner, None);
    let (value, _) = alpha.grid("'voxel'");
    assert!(value.is_table());
}
