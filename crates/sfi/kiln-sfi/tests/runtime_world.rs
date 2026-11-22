// SPDX-License-Identifier: MIT
// the interpreter's runtime.world cursor, building input and props tests.
//! `runtime.world.cursor.get`, `runtime.world.building.submit / status / cancel` and `runtime.world.entity.query_props`,
//! `query_props_in_bounds`, `register_prop_recipes`, `get_transform` and `set_scale` on the C-ABI fakes
//! (support/fake_services: kiln.world's `EcsWorld` with `ecs_snapshot_many`, kiln.client's `ClientModel`) through the
//! real service adapter and registry join, on a scripted registrar host; the catalog's rows and facts on the production
//! runtime facts.
//! Expected texts and shapes are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 d8a394bc:
//! modules/mod-runtime/lua/ingame/player_input/{read_building_cursor,submit_building_input}.rs,
//! lua/ingame/world/{query_props,register_prop_recipes,access_prop_transform,mod}.rs, game-runtime/bridge/windows/
//! {building/read_cursor.rs,props/*.rs} and game-runtime/native/ecs/{query_building_props,read_building_prop}.cpp.
#[path = "shared/ecs_fixture.rs"]
mod fixture;

use fixture::fake_services::{CasStep, ClientModel, Knob, set_client};
use fixture::*;
use kiln_abi::services::kiln_caps::{
    KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, KILN_CAP_UNAVAILABLE,
};
use kiln_abi::services::kiln_client::*;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::availability::{FactsConfig, RuntimeFacts};
use kiln_sfi::runtime::ecs::{self, Ecs, EcsRuntime};
use kiln_sfi::runtime::operation_catalog::{Backing, CATALOG, Fact};
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::types::{self, TypesWiring};
use kiln_sfi::runtime::values::{ArenaOptions, ValuesArena};
use kiln_sfi::runtime::world::EcsSource;
use kiln_sfi::runtime::world::building_input::{self, Sequence, text as bt};
use kiln_sfi::runtime::world::cursor::{self, text as ct};
use kiln_sfi::runtime::world::props::{self, text as pt};
use kiln_sfi::runtime::world::recipes::{self, Recipes, text as rt};
use kiln_sfi::worker::Host;
use sfi_core::bindings::runtime::{
    RuntimeOperation as Op, RuntimeOwnerContext, RuntimeProviderFacts,
};
use sfi_core::host::{HostContext, Phase, Side, SourceVersion};
use std::collections::VecDeque;

const CLIENT: u32 = KILN_WORLD_CLIENT as u32;
const PLAYER: u32 = 10;
const INPUT: &str = "keen::ecs::ClientPlayerInput";
const ITEM: &str = "keen::ecs::UsedItem";

// ---- the archive registry: the fixture's types and the cursor's and props' ----

fn ty(qualified: &'static str, primitive: &'static str, size: u32) -> T {
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
fn fields(mut t: T, fields: &[(&'static str, usize, u64)]) -> T {
    t.fields
        .extend(fields.iter().map(|(n, i, o)| (*n, *i as u64, *o)));
    t
}
fn inner(mut t: T, of: usize) -> T {
    t.inner = Some(of as u64);
    t
}
/// The fixture's types, then (from index `base`) the cursor's record and `UsedItem`.
fn all_types() -> Vec<T> {
    let mut types = archive_types();
    let b = types.len();
    let at = |k: usize| b + k;
    types.extend([
        ty("keen::UInt8", "UInt8", 1),         // 0
        ty("keen::UInt64", "UInt64", 8),       // 1
        ty("keen::Bool", "Bool", 1),           // 2
        ty("keen::Bitmask32", "Bitmask32", 4), // 3
        fields(
            ty("keen::Vec3i64", "Struct", 24),
            &[("x", SINT64, 0), ("y", SINT64, 8), ("z", SINT64, 16)],
        ), // 4
        fields(
            ty("keen::Quat", "Struct", 16),
            &[
                ("x", FLOAT32, 0),
                ("y", FLOAT32, 4),
                ("z", FLOAT32, 8),
                ("w", FLOAT32, 12),
            ],
        ), // 5
        fields(
            ty("keen::Vec3f", "Struct", 12),
            &[("x", FLOAT32, 0), ("y", FLOAT32, 4), ("z", FLOAT32, 8)],
        ), // 6
        fields(
            ty("keen::CursorTransform", "Struct", 56),
            &[
                ("position", at(4), 0),
                ("orientation", at(5), 24),
                ("scale", at(6), 40),
                ("pad", UINT32, 52),
            ],
        ), // 7
        inner(ty("keen::VoxelMaterialId", "Typedef", 1), at(0)), // 8
        inner(ty("keen::ObjectType", "Enum", 4), UINT32), // 9
        fields(
            ty("keen::VersionData", "Struct", 8),
            &[("version", at(1), 0)],
        ), // 10
        fields(
            ty("keen::ObjectId", "Struct", 16),
            &[("value", at(1), 0), ("type", at(9), 8), ("pad", UINT32, 12)],
        ), // 11
        fields(
            ty("keen::SelectObjectAction", "Struct", 24),
            &[("versionData", at(10), 0), ("selectedObjectId", at(11), 8)],
        ), // 12
        fields(
            ty("keen::ClientCursorInput", "Struct", 152),
            &[
                ("primaryTransform", at(7), 0),
                ("secondaryTransform", at(7), 56),
                ("primaryFlags", at(3), 112),
                ("secondaryFlags", at(3), 116),
                ("hoveredVoxelMaterialId", at(8), 120),
                ("isCursorAttachedToProp", at(2), 121),
                ("selectObjectAction", at(12), 128),
            ],
        ), // 13
        fields(
            ty("keen::ClientInputData", "Struct", 168),
            &[("cursorInput", at(13), 16)],
        ), // 14
        fields(ty(INPUT, "Struct", 176), &[("data", at(14), 8)]), // 15
        fields(ty(ITEM, "Struct", 4), &[("itemId", UINT32, 0)]), // 16
    ]);
    types
}
/// kiln.types' catalog over every keen::ecs type of [`all_types`] but Unregistered (and `skip`), each row proven.
fn catalog_without(skip: &[&str]) -> EcsCatalog {
    let mut catalog = EcsCatalog {
        stamp: 0x303e_c500_0002,
        ..EcsCatalog::default()
    };
    for t in all_types().into_iter().filter(|t| {
        t.qualified.starts_with("keen::ecs::")
            && t.qualified != UNREGISTERED
            && !skip.contains(&t.qualified)
    }) {
        let identity = ECS_TYPE_BASE as u64 + catalog.types.len() as u64;
        let hash = fnv(t.qualified);
        catalog.types.push((t.qualified.to_owned(), t.size, hash));
        catalog.rows.push((hash, t.size, 0, identity));
    }
    catalog
}

// ---- one VM with the world families ----

struct W {
    owner: KilnOwner,
    vm: fake_values::TestVm,
    _arena: ValuesArena,
    host: Rc<Script>,
    _ecs: Rc<Ecs>,
    facts: Rc<RuntimeFacts>,
    services: Rc<ServiceAdapter>,
    session: Rc<Cell<u64>>,
    /// The world kind the shared [`Ecs`] binds (production: `RuntimeFacts::ecs_world`).
    bound: Rc<Cell<u32>>,
    scripted: Rc<RefCell<VecDeque<u64>>>,
    sequence: Rc<Sequence>,
    recipes: Rc<Recipes>,
    _wiring: TypesWiring,
}

fn host(id: &str, side: Side) -> Rc<Script> {
    Rc::new(Script {
        owner: RuntimeOwnerContext::new(
            &manifest(id, &["runtime"]),
            &HostContext::new(Phase::Runtime, side, SourceVersion::Sf179),
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

/// A VM of package "alpha" on `side` over `world` (and its kiln.types catalog), the world families installed in
/// `world.cursor`, `world.building`, `world.entity` and the ECS family's functions in `ecs`, all on one [`Ecs`].
fn setup_with(world: EcsWorld, side: Side, catalog: EcsCatalog) -> W {
    let kind = world.world_kind;
    setup_on(world, side, catalog, kind)
}
/// [`setup_with`] with the shared [`Ecs`] on world kind `kind` (a host's: the server world, while the fake world is
/// the client's).
fn setup_on(world: EcsWorld, side: Side, catalog: EcsCatalog, kind: u32) -> W {
    let owner = support::next_owner();
    fake_services::bind_thread(owner);
    fake_services::set_ecs_catalog(owner, Some(catalog));
    fake_services::set_ecs_world(owner, Some(world));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let vm = fake_values::vm();
    let arena = vm.arena(ArenaOptions::default()).unwrap();
    let _ = codec();
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
    let scripted = Rc::new(RefCell::new(VecDeque::new()));
    let bound = Rc::new(Cell::new(kind));
    let (live, queue, now) = (session.clone(), scripted.clone(), bound.clone());
    let wiring = types::wire(Port(all_types()), |_, _| {});
    let ecs = Rc::new(
        Ecs::new(
            wiring.handle.session(vm.lua()).unwrap(),
            services.clone(),
            kind,
        )
        .with_runtime(EcsRuntime {
            owner,
            values: Some(arena.values()),
            facts: facts.clone(),
            world: Rc::new(move || {
                (
                    now.get(),
                    queue.borrow_mut().pop_front().unwrap_or(live.get()),
                )
            }),
        }),
    );
    let host = host("alpha", side);
    let lua = vm.lua();
    let ecs_table = lua.create_table().unwrap();
    ecs::install_into(lua, &ecs_table, host.clone(), ecs.clone()).unwrap();
    let world = lua.create_table().unwrap();
    let (cursor_table, building, entity) = (
        lua.create_table().unwrap(),
        lua.create_table().unwrap(),
        lua.create_table().unwrap(),
    );
    let shared = ecs.clone();
    let source: EcsSource = Rc::new(move |_| Some(shared.clone()));
    cursor::install_into(lua, &cursor_table, host.clone(), source.clone()).unwrap();
    let sequence = Rc::new(Sequence::default());
    building_input::install_into(
        lua,
        &building,
        host.clone(),
        sequence.clone(),
        services.clone(),
        owner,
    )
    .unwrap();
    let recipes = Rc::new(Recipes::new());
    props::install_into(lua, &entity, host.clone(), source, recipes.clone()).unwrap();
    world.raw_set("cursor", cursor_table).unwrap();
    world.raw_set("building", building).unwrap();
    world.raw_set("entity", entity).unwrap();
    vm.set("world", world);
    vm.set("ecs", ecs_table);
    vm.run::<()>(
        "function perr(f, ...)
           local ok, e = pcall(f, ...)
           assert(not ok, 'expected an error')
           return (tostring(e):match('^[^\\n]*'))
         end",
    )
    .unwrap();
    W {
        owner,
        vm,
        _arena: arena,
        host,
        _ecs: ecs,
        facts,
        services,
        session,
        bound,
        scripted,
        sequence,
        recipes,
        _wiring: wiring,
    }
}
fn setup(world: EcsWorld, side: Side) -> W {
    setup_with(world, side, catalog_without(&[]))
}

impl W {
    fn eval<R: FromLuaMulti>(&self, code: &str) -> R {
        self.vm
            .run::<R>(code)
            .unwrap_or_else(|e| panic!("{code}: {e}"))
    }
    fn reason(&self, call: &str) -> String {
        let (value, reason): (Value, Option<String>) = self.eval(&format!("return {call}"));
        assert!(
            matches!(value, Value::Nil | Value::Boolean(false)),
            "{call}: {value:?}"
        );
        reason.unwrap_or_else(|| panic!("{call}: no reason"))
    }
    fn raised(&self, call: &str) -> String {
        self.eval(&format!("return perr(function() return {call} end)"))
    }
    fn world<R>(&self, f: impl FnOnce(&mut EcsWorld) -> R) -> R {
        fake_services::ecs_world(self.owner, f).expect("an ECS world")
    }
    fn client<R>(&self, f: impl FnOnce(&mut ClientModel) -> R) -> R {
        fake_services::client(self.owner, f).expect("a client model")
    }
}

// ---- the cursor ----

/// One cursor as the test writes it into `ClientPlayerInput`.
#[derive(Clone)]
struct Cursor {
    position: [i64; 3],
    rotation: [f32; 4],
    scale: [f32; 3],
    secondary_x: i64,
    flags: (u32, u32),
    material: u8,
    attached: bool,
    version: u64,
    object: (u64, u32),
}
fn sample() -> Cursor {
    Cursor {
        position: [100, -200, 300],
        rotation: [0.0, 0.5, 0.0, 0.75],
        scale: [1.0, 2.0, 0.5],
        secondary_x: -7,
        flags: (0x8000_0001, 0x40),
        material: 9,
        attached: true,
        version: 1_234_567_890_123,
        object: (0xdead_beef_0000_0042, 3),
    }
}
/// The 176 bytes of a `ClientPlayerInput` holding `c` (cursorInput at 24).
fn input_bytes(c: &Cursor) -> Vec<u8> {
    let mut b = vec![0u8; 176];
    let put =
        |b: &mut Vec<u8>, at: usize, bytes: &[u8]| b[at..at + bytes.len()].copy_from_slice(bytes);
    let base = 24;
    for (k, v) in c.position.iter().enumerate() {
        put(&mut b, base + k * 8, &v.to_le_bytes());
    }
    for (k, v) in c.rotation.iter().enumerate() {
        put(&mut b, base + 24 + k * 4, &v.to_le_bytes());
    }
    for (k, v) in c.scale.iter().enumerate() {
        put(&mut b, base + 40 + k * 4, &v.to_le_bytes());
    }
    put(&mut b, base + 56, &c.secondary_x.to_le_bytes());
    put(&mut b, base + 56 + 24 + 12, &1.0f32.to_le_bytes()); // secondary orientation w
    for k in 0..3 {
        put(&mut b, base + 56 + 40 + k * 4, &1.0f32.to_le_bytes());
    }
    put(&mut b, base + 112, &c.flags.0.to_le_bytes());
    put(&mut b, base + 116, &c.flags.1.to_le_bytes());
    b[base + 120] = c.material;
    b[base + 121] = u8::from(c.attached);
    put(&mut b, base + 128, &c.version.to_le_bytes());
    put(&mut b, base + 136, &c.object.0.to_le_bytes());
    put(&mut b, base + 144, &c.object.1.to_le_bytes());
    b
}
fn health(hp: u32) -> (u32, Vec<u8>) {
    (hash(HEALTH), words(&[hp, 100]))
}
fn client_world(c: &Cursor) -> EcsWorld {
    let mut world = EcsWorld::server().with(PLAYER, &[(hash(INPUT), input_bytes(c))]);
    world.world_kind = CLIENT;
    world
}
fn cursor_setup(c: &Cursor) -> W {
    let w = setup(client_world(c), Side::Client);
    set_client(w.owner, Some(ClientModel::local(PLAYER)));
    w
}

const DESCRIBE: &str = "local function describe(c)
  local v = c.value
  local function t(x) return table.concat({x.position.x, x.position.y, x.position.z, x.rotation[1], x.rotation[2],
    x.rotation[3], x.rotation[4], x.scale[1], x.scale[2], x.scale[3]}, ',') end
  return table.concat({t(v.primaryTransform), t(v.secondaryTransform), v.primaryFlags, v.secondaryFlags, v.material,
    v.selectionVersion, v.selectedObject.value, v.selectedObject.type, tostring(v.attachedToProp),
    math.type(v.primaryTransform.position.x), math.type(v.primaryFlags)}, '|')
end";

fn cursor_text(w: &W) -> (String, i64) {
    w.eval(&format!("{DESCRIBE}\nlocal c, why = world.cursor.get(); assert(c, why); return describe(c), c.sequence"))
}

#[test]
fn cursor_get_decodes_the_local_players_client_player_input_by_its_reflected_fields() {
    let w = cursor_setup(&sample());
    let (text, first) = cursor_text(&w);
    assert_eq!(
        text,
        "100,-200,300,0.0,0.5,0.0,0.75,1.0,2.0,0.5|-7,0,0,0.0,0.0,0.0,1.0,1.0,1.0,1.0|2147483649|64|9|\
         1234567890123|deadbeef00000042|3|true|integer|integer"
    );
    assert!(first > 0);
    // no arguments; the denial first
    assert_eq!(w.raised("world.cursor.get(1)"), ct::ARGUMENTS);
    w.host.contract.set(false);
    assert_eq!(
        w.reason("world.cursor.get(1)"),
        format!("operation runtime.world.cursor.get has not been verified for game build {BUILD}")
    );
}

#[test]
fn cursor_get_reads_a_fresh_copy_each_call_and_numbers_each_published_copy() {
    let w = cursor_setup(&sample());
    let (before, first) = cursor_text(&w);
    let changed = Cursor {
        position: [5, 6, 7],
        attached: false,
        flags: (2, 3),
        ..sample()
    };
    w.world(|world| {
        world
            .entities
            .get_mut(&PLAYER)
            .unwrap()
            .components
            .insert(hash(INPUT), input_bytes(&changed));
    });
    let (after, second) = cursor_text(&w);
    assert_ne!(before, after);
    assert!(after.starts_with("5,6,7,"), "{after}");
    assert!(
        after.contains("|2|3|9|") && after.contains("|false|"),
        "{after}"
    );
    assert!(second > first, "{second} after {first}");
    // one snapshot per call, nothing cached
    let snapshots = fake_services::counters(w.owner).calls("ecs_snapshot");
    cursor_text(&w);
    assert_eq!(
        fake_services::counters(w.owner).calls("ecs_snapshot"),
        snapshots + 1
    );
}

#[test]
fn cursor_get_refuses_copies_that_changed_under_the_read_and_a_moving_local_player() {
    let w = cursor_setup(&sample());
    // kiln.world's stable copy saw two different copies: unreadable
    w.world(|world| world.unreadable = true);
    assert_eq!(w.reason("world.cursor.get()"), ct::RETRY);
    w.world(|world| world.unreadable = false);
    // the local player's state moved during the copy (a new world generation)
    let mut moved = ClientModel::local(PLAYER).state.unwrap();
    w.client(|model| {
        model.scripted.push_back(model.state.unwrap());
        moved.generation = 2;
        model.scripted.push_back(moved);
    });
    assert_eq!(w.reason("world.cursor.get()"), ct::RETRY);
    cursor_text(&w);
}

#[test]
fn cursor_get_follows_the_world_session_marker() {
    let w = cursor_setup(&sample());
    // no session yet
    w.session.set(0);
    assert_eq!(w.reason("world.cursor.get()"), ct::WAITING);
    // the session changes between the open and the end of the copy (the marker is sampled at the start, by the
    // registry join of the shared Ecs's world, then after the copy)
    w.session.set(FAKE_SESSION);
    w.scripted
        .borrow_mut()
        .extend([FAKE_SESSION, FAKE_SESSION, FAKE_SESSION + 1]);
    assert_eq!(w.reason("world.cursor.get()"), ct::SESSION);
    cursor_text(&w);
    // the local input is the client world's live one, whatever the marker's world (on a host: the server world's)
    w.world(|world| world.session = FAKE_SESSION + 1);
    cursor_text(&w);
    w.session.set(FAKE_SESSION + 7);
    cursor_text(&w);
}

#[test]
fn cursor_get_reads_the_client_world_on_a_host_whose_shared_ecs_reads_the_server_world() {
    // a host: the shared Ecs is on the server world, and the server world's registry join has no player input
    let w = setup_on(
        client_world(&sample()),
        Side::Client,
        catalog_without(&[INPUT]),
        SERVER,
    );
    set_client(w.owner, Some(ClientModel::local(PLAYER)));
    let (text, _) = cursor_text(&w);
    assert!(text.starts_with("100,-200,300,"), "{text}");
    // the client world's component is not the reflected size: kiln.world's copy says so
    w.world(|world| {
        world
            .entities
            .get_mut(&PLAYER)
            .unwrap()
            .components
            .insert(hash(INPUT), vec![0; 170]);
    });
    assert_eq!(w.reason("world.cursor.get()"), ct::BUFFER);
    // larger than the reflected size: kiln.world reports the short buffer, which is the same mismatch
    w.world(|world| {
        world
            .entities
            .get_mut(&PLAYER)
            .unwrap()
            .components
            .insert(hash(INPUT), vec![0; 180]);
    });
    assert_eq!(w.reason("world.cursor.get()"), ct::BUFFER);
}

#[test]
fn cursor_get_answers_shroudforge_s_reasons_without_a_local_input() {
    // no known local player
    let w = cursor_setup(&sample());
    w.client(|model| model.state.as_mut().unwrap().flags = 0);
    assert_eq!(w.reason("world.cursor.get()"), ct::SESSION);
    w.client(|model| *model = ClientModel::local(PLAYER + 1));
    assert_eq!(w.reason("world.cursor.get()"), ct::SESSION);
    // a server process: its world is not the client world
    let mut server = EcsWorld::server().with(PLAYER, &[(hash(INPUT), input_bytes(&sample()))]);
    server.world_kind = SERVER;
    let w = setup(server, Side::Server);
    set_client(w.owner, Some(ClientModel::local(PLAYER)));
    assert_eq!(w.reason("world.cursor.get()"), ct::SESSION);
    // the local player has no ClientPlayerInput
    let w = setup(
        EcsWorld {
            world_kind: CLIENT,
            ..EcsWorld::server().with(PLAYER, &[health(1)])
        },
        Side::Client,
    );
    set_client(w.owner, Some(ClientModel::local(PLAYER)));
    assert_eq!(w.reason("world.cursor.get()"), ct::SESSION);
    // a VM without the ECS family
    let w = cursor_setup(&sample());
    let lua = w.vm.lua();
    let table = lua.create_table().unwrap();
    cursor::install_into(lua, &table, w.host.clone(), Rc::new(|_| None)).unwrap();
    w.vm.set("bare", table);
    assert_eq!(w.reason("bare.get()"), ct::REFLECTION);
}

// ---- building input ----

fn submit(w: &W, table: &str, input: &str) -> (Option<i64>, Option<String>) {
    w.eval(&format!("return {table}.submit({input})"))
}
const SELECT: &str = "{action = 'select', player = 10, itemId = 77, materialItemId = 5, slot = 2}";
const PLACE: &str = "{action = 'place', player = 10, position = {1.5, 2, 3}, rotation = {0, 0, 0.5, 0.5}, scale = {2, 2, 2}}";

/// A second package's building table on the same sequence.
fn second_package(w: &W) {
    let lua = w.vm.lua();
    let table = lua.create_table().unwrap();
    building_input::install_into(
        lua,
        &table,
        host("beta", Side::Client),
        w.sequence.clone(),
        w.services.clone(),
        w.owner,
    )
    .unwrap();
    w.vm.set("beta", table);
}

#[test]
fn building_submit_hands_the_request_to_kiln_client_and_one_package_owns_the_sequence() {
    let w = cursor_setup(&sample());
    second_package(&w);
    let (id, why) = submit(&w, "world.building", SELECT);
    assert_eq!(why, None);
    let id = id.unwrap();
    let sent = w.client(|model| model.submitted.clone());
    assert_eq!(sent.len(), 1);
    let s = sent[0];
    assert_eq!(
        (
            s.action,
            s.player,
            s.item_id,
            s.material_item_id,
            s.slot,
            s.target_entity_id,
            s.reserved
        ),
        (KILN_BUILDING_ACTION_SELECT as u32, PLAYER, 77, 5, 2, 0, 0)
    );
    assert_eq!(
        (s.position, s.rotation, s.scale),
        ([0.0; 3], [0.0, 0.0, 0.0, 1.0], [1.0; 3])
    );
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({id})")),
        "queued"
    );
    // another package: the sequence is owned, nothing reaches kiln.client
    assert_eq!(w.reason(&format!("beta.submit({SELECT})")), bt::OWNED);
    assert_eq!(w.client(|model| model.submitted.len()), 1);
    // its status and cancel of the owner's id know nothing
    assert_eq!(
        w.eval::<String>(&format!("return beta.status({id})")),
        "unknown"
    );
    assert!(!w.eval::<bool>(&format!("return beta.cancel({id})")));
    // the owner cancels: the sequence is free for the other package
    assert!(w.eval::<bool>(&format!("return world.building.cancel({id})")));
    assert_eq!(w.sequence.active(), 0);
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({id})")),
        "unknown"
    );
    let (next, why) = submit(&w, "beta", PLACE);
    assert_eq!(why, None);
    let p = *w
        .client(|model| model.submitted.last().cloned())
        .as_ref()
        .unwrap();
    assert_eq!(
        (p.action, p.position, p.rotation, p.scale),
        (
            KILN_BUILDING_ACTION_PLACE as u32,
            [1.5, 2.0, 3.0],
            [0.0, 0.0, 0.5, 0.5],
            [2.0; 3]
        )
    );
    assert!(next.unwrap() > id);
}

#[test]
fn building_status_and_cancel_answer_only_the_package_s_latest_id() {
    let w = cursor_setup(&sample());
    let first = submit(&w, "world.building", SELECT).0.unwrap();
    w.client(|model| {
        model
            .states
            .insert(first as u32, KILN_BUILDING_INPUT_DISPATCHED as u32)
    });
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({first})")),
        "dispatched"
    );
    let second = submit(&w, "world.building", SELECT).0.unwrap();
    assert_ne!(first, second);
    // the earlier id is no longer the package's request
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({first})")),
        "unknown"
    );
    assert!(!w.eval::<bool>(&format!("return world.building.cancel({first})")));
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({second})")),
        "queued"
    );
    // id 0 is never a request
    assert_eq!(
        w.eval::<String>("return world.building.status(0)"),
        "unknown"
    );
    assert!(!w.eval::<bool>("return world.building.cancel(0)"));
    // each state's text
    for (state, text) in [
        (KILN_BUILDING_INPUT_PRESSED, "pressed"),
        (KILN_BUILDING_INPUT_TIMEOUT, "timeout"),
        (KILN_BUILDING_INPUT_CANCELLED, "cancelled"),
        (KILN_BUILDING_INPUT_UNKNOWN, "unknown"),
    ] {
        w.client(|model| model.states.insert(second as u32, state as u32));
        assert_eq!(
            w.eval::<String>(&format!("return world.building.status({second})")),
            text
        );
    }
    assert_eq!(w.client(|model| model.cancelled.clone()), Vec::<u32>::new());
}

#[test]
fn building_cancel_after_a_press_reaches_kiln_client_and_forgets_the_id() {
    let w = cursor_setup(&sample());
    second_package(&w);
    let id = submit(&w, "world.building", PLACE).0.unwrap();
    w.client(|model| {
        model
            .states
            .insert(id as u32, KILN_BUILDING_INPUT_PRESSED as u32)
    });
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({id})")),
        "pressed"
    );
    // while it is pressed the client's one slot is busy: kiln.client refuses, ShroudForge's text
    assert_eq!(
        w.reason(&format!("world.building.submit({SELECT})")),
        bt::REFUSED
    );
    // the package still owns the sequence (it has a request)
    assert_eq!(w.reason(&format!("beta.submit({SELECT})")), bt::OWNED);
    assert!(w.eval::<bool>(&format!("return world.building.cancel({id})")));
    assert_eq!(w.client(|model| model.cancelled.clone()), vec![id as u32]);
    assert_eq!(
        w.eval::<String>(&format!("return world.building.status({id})")),
        "unknown"
    );
    assert!(!w.eval::<bool>(&format!("return world.building.cancel({id})")));
    assert_eq!(w.sequence.active(), 0);
}

#[test]
fn building_submit_checks_its_input_in_shroudforge_s_order() {
    let w = cursor_setup(&sample());
    second_package(&w);
    // the table is converted before anything else
    assert!(
        w.raised("world.building.submit('select')")
            .contains("table")
    );
    assert_eq!(
        w.raised("world.building.submit({action = 'jump', player = 10})"),
        format!("runtime error: {}", bt::ACTION)
    );
    assert_eq!(
        w.raised("world.building.submit({action = 'remove', player = 10})"),
        format!("runtime error: {}", bt::POSITION)
    );
    assert_eq!(
        w.reason(
            "world.building.submit({action = 'dismantle', player = 10, position = {0, 0, 0}})"
        ),
        bt::DISMANTLE
    );
    assert_eq!(
        w.reason("world.building.submit({action = 'undo', player = 10, targetEntityId = 4})"),
        bt::DISMANTLE
    );
    assert_eq!(
        w.raised("world.building.submit({action = 'place', player = 10, position = {0, 0/0, 0}})"),
        format!("runtime error: {}", bt::FINITE)
    );
    assert_eq!(w.client(|model| model.submitted.len()), 0);
    assert_eq!(w.sequence.active(), 0);
    // kiln.client refuses (no producer bound): ShroudForge's text, and the sequence is released
    w.client(|model| model.refuse = Some(KILN_ERR_UNAVAILABLE));
    assert_eq!(
        w.reason(&format!("world.building.submit({SELECT})")),
        bt::REFUSED
    );
    assert_eq!(w.sequence.active(), 0);
    w.client(|model| model.refuse = None);
    assert!(submit(&w, "beta", SELECT).0.is_some());
    // a dismantle carries its target
    w.eval::<bool>("return beta.cancel(1)");
    let (id, _) = submit(
        &w,
        "world.building",
        "{action = 'dismantle', player = 10, targetEntityId = 33, position = {1, 1, 1}}",
    );
    assert!(id.is_some());
    let last = w.client(|model| *model.submitted.last().unwrap());
    assert_eq!(
        (last.action, last.target_entity_id),
        (KILN_BUILDING_ACTION_DISMANTLE as u32, 33)
    );
    // a server process: building input belongs to a client
    let s = setup(EcsWorld::server(), Side::Server);
    set_client(s.owner, Some(ClientModel::local(PLAYER)));
    assert_eq!(
        s.reason(&format!("world.building.submit({SELECT})")),
        bt::SERVER
    );
    // the denial
    w.host.contract.set(false);
    assert_eq!(
        w.reason(&format!("world.building.submit({SELECT})")),
        format!(
            "operation runtime.world.building.input has not been verified for game build {BUILD}"
        )
    );
}

#[test]
fn building_requests_of_a_package_whose_vm_goes_are_cancelled_and_the_sequence_released() {
    let w = cursor_setup(&sample());
    let lua = fake_values::vm();
    let table = lua.lua().create_table().unwrap();
    building_input::install_into(
        lua.lua(),
        &table,
        host("gamma", Side::Client),
        w.sequence.clone(),
        w.services.clone(),
        w.owner,
    )
    .unwrap();
    lua.set("gamma", table);
    let id: i64 = lua
        .run(&format!("return (gamma.submit({SELECT}))"))
        .unwrap();
    assert_ne!(w.sequence.active(), 0);
    drop(lua);
    assert_eq!(w.sequence.active(), 0);
    assert_eq!(w.client(|model| model.cancelled.clone()), vec![id as u32]);
}

// ---- props ----

const TRANSFORM_HASH: fn() -> u32 = || hash(TRANSFORM);

fn prop_transform(position: [f64; 3], rotation: [f32; 4], scale: [f32; 3]) -> Vec<u8> {
    let mut bytes = transform(position, scale);
    for (k, q) in rotation.iter().enumerate() {
        bytes[24 + k * 4..28 + k * 4].copy_from_slice(&q.to_le_bytes());
    }
    bytes
}
fn prop(
    world: EcsWorld,
    id: u32,
    item: u32,
    bytes: Vec<u8>,
    template: Option<[u8; 16]>,
) -> EcsWorld {
    let mut world = world.with(
        id,
        &[(hash(TRANSFORM), bytes), (hash(ITEM), words(&[item]))],
    );
    world.entities.get_mut(&id).unwrap().template = template;
    world
}
const IDENTITY: [f32; 4] = [0.0, 0.0, 0.0, 1.0];
const UUID: [u8; 16] = [1, 0, 0, 0, 0, 0, 0, 0x10, 2, 0, 0, 0, 0, 0, 0, 0x20];
/// Props 1 (item 5 at 1,1,1), 2 (item 6 at 50,0,0, scale 4), 3 (item 0), 4 (a transform only), 5 (item 5 at 3,3,3).
fn props_world() -> EcsWorld {
    let w = prop(
        EcsWorld::server(),
        1,
        5,
        prop_transform([1.0, 1.0, 1.0], IDENTITY, [1.0; 3]),
        Some(UUID),
    );
    let w = prop(
        w,
        2,
        6,
        prop_transform([50.0, 0.0, 0.0], IDENTITY, [4.0, 1.0, 1.0]),
        Some(UUID),
    );
    let w = prop(
        w,
        3,
        0,
        prop_transform([1.0, 1.0, 1.0], IDENTITY, [1.0; 3]),
        Some(UUID),
    );
    let w = w.with(
        4,
        &[(
            hash(TRANSFORM),
            prop_transform([1.0, 1.0, 1.0], IDENTITY, [1.0; 3]),
        )],
    );
    prop(
        w,
        5,
        5,
        prop_transform([3.0, 3.0, 3.0], IDENTITY, [1.0; 3]),
        None,
    )
}
fn ids(w: &W, call: &str) -> Vec<i64> {
    w.eval(&format!(
        "local r, why = {call}; assert(r, why); local out = {{}}
         for i, p in ipairs(r) do out[i] = p.entityId end
         return out"
    ))
}

#[test]
fn query_props_keeps_the_props_inside_the_padded_bounds_as_shared_entity_handles() {
    let w = setup(props_world(), Side::Server);
    assert_eq!(
        ids(&w, "world.entity.query_props({0, 0, 0, 4, 4, 4}, 0)"),
        vec![1, 5]
    );
    // the margin is padding times the largest scale: prop 2 at x 50 with scale 4 and padding 2 reaches 42
    assert_eq!(
        ids(&w, "world.entity.query_props({40, -1, -1, 42.5, 1, 1}, 2)"),
        vec![2]
    );
    assert_eq!(
        ids(&w, "world.entity.query_props({40, -1, -1, 41.9, 1, 1}, 2)"),
        Vec::<i64>::new()
    );
    // the maximum is exclusive
    assert_eq!(
        ids(&w, "world.entity.query_props({0, 0, 0, 3, 4, 4}, 0)"),
        vec![1]
    );
    // the record: handle, ids, zero template hex (not read here), the transform in metres
    let text: String = w.eval(
        "local p = world.entity.query_props({0, 0, 0, 2, 2, 2}, 0)[1]
         local t = p.transform
         return table.concat({p.entityId, p.itemId, p.templateUuidHighHex, p.templateUuidLowHex, t.position.x,
           t.position[2], t.orientation.w, t.orientation[4], t.scale.x, t.scale[3]}, ',')",
    );
    assert_eq!(
        text,
        "1,5,0000000000000000,0000000000000000,1.0,1.0,1.0,1.0,1.0,1.0"
    );
    // its handle is the ECS family's: runtime.ecs reads it, and resolve gives the same number
    let same: bool = w.eval(
        "local p = world.entity.query_props({0, 0, 0, 2, 2, 2}, 0)[1]
         local bytes = ecs.read_bytes(p.handle, 'keen::ecs::CurrentTransform')
         return #bytes == 56 and ecs.resolve(1) == p.handle",
    );
    assert!(same);
    // argument rules and texts
    assert_eq!(
        w.raised("world.entity.query_props({0, 0, 0, 1, 1, 1})"),
        pt::QUERY_ARGUMENTS
    );
    assert_eq!(
        w.reason("world.entity.query_props({0, 0, 0/0, 1, 1, 1}, 0)"),
        pt::BOUNDS
    );
    assert_eq!(
        w.reason("world.entity.query_props({0, 0, 0, 1, 1, 1}, -1)"),
        pt::PADDING
    );
    assert_eq!(
        w.reason("world.entity.query_props({0, 0, 0, 0, 1, 1}, 0)"),
        pt::QUERY_FAILED
    );
    // a world kiln.world does not read, and a join without UsedItem: the provider's failure
    w.world(|world| world.world_kind = CLIENT);
    assert_eq!(
        w.reason("world.entity.query_props({0, 0, 0, 4, 4, 4}, 0)"),
        pt::QUERY_FAILED
    );
    let w = setup_with(props_world(), Side::Server, catalog_without(&[ITEM]));
    assert_eq!(
        w.reason("world.entity.query_props({0, 0, 0, 4, 4, 4}, 0)"),
        pt::QUERY_FAILED
    );
    // a copy kiln.world refuses is skipped, not a failure
    let w = setup(props_world(), Side::Server);
    w.world(|world| world.unreadable = true);
    assert_eq!(
        ids(&w, "world.entity.query_props({0, 0, 0, 4, 4, 4}, 0)"),
        Vec::<i64>::new()
    );
}

const RECIPES: &str = "{{itemId = 5, bounds = {-0.5, -0.5, -0.5, 0.5, 0.5, 0.5}, feedback = 1},
                       {itemId = 6, bounds = {0, -0.5, -0.5, 1, 0.5, 0.5}, feedback = 2}}";

#[test]
fn register_prop_recipes_replaces_the_whole_catalog_or_nothing() {
    let w = setup(props_world(), Side::Server);
    assert!(w.recipes.snapshot().is_none());
    assert_eq!(
        w.reason("world.entity.query_props_in_bounds({0, 0, 0, 4, 4, 4})"),
        pt::RECIPE_QUERY_FAILED
    );
    assert!(w.eval::<bool>(&format!(
        "return (world.entity.register_prop_recipes({RECIPES}))"
    )));
    let catalog = w.recipes.snapshot().unwrap();
    assert_eq!(catalog.len(), 2);
    assert_eq!(catalog[&6].feedback, 2);
    // a bad recipe anywhere: its index, and the catalog stays as it was
    for (bad, index) in [
        (
            "{{itemId = 7, bounds = {0, 0, 0, 1, 1, 1}, feedback = 0}, {itemId = 0, bounds = {0, 0, 0, 1, 1, 1}, feedback = 0}}",
            2,
        ),
        (
            "{{itemId = 7, bounds = {0, 0, 0, 1, 1, 1}, feedback = 0}, {itemId = 8, bounds = {2, 0, 0, 1, 1, 1}, feedback = 0}}",
            2,
        ),
        (
            "{{itemId = 7, bounds = {0, 0, 0, 1e300, 1, 1}, feedback = 0}}",
            1,
        ),
        (
            "{{itemId = 7, bounds = {0, 0, 0/0, 1, 1, 1}, feedback = 0}}",
            1,
        ),
    ] {
        assert_eq!(
            w.reason(&format!("world.entity.register_prop_recipes({bad})")),
            format!("invalid placement recipe at index {index}")
        );
        assert_eq!(w.recipes.snapshot().unwrap(), catalog);
    }
    // two recipes of one item: the native refusal, nothing kept
    assert_eq!(
        w.reason("world.entity.register_prop_recipes({{itemId = 7, bounds = {0, 0, 0, 1, 1, 1}, feedback = 0},
                  {itemId = 7, bounds = {0, 0, 0, 1, 1, 1}, feedback = 1}})"),
        rt::REJECTED
    );
    assert_eq!(w.recipes.snapshot().unwrap(), catalog);
    // raised conversions, and the arity
    assert!(
        w.raised("world.entity.register_prop_recipes({{itemId = 7, bounds = {0, 0, 0, 1, 1, 1}}})")
            .contains("nil")
    );
    assert_eq!(
        w.raised("world.entity.register_prop_recipes({{itemId = 7, bounds = {0, 0, 0, 1, 1}, feedback = 0}})"),
        "recipe bounds must contain exactly 6 numbers"
    );
    assert_eq!(
        w.raised("world.entity.register_prop_recipes()"),
        rt::ARGUMENTS
    );
    // an empty catalog is a ready catalog
    assert!(w.eval::<bool>("return (world.entity.register_prop_recipes({}))"));
    assert_eq!(
        ids(&w, "world.entity.query_props_in_bounds({0, 0, 0, 4, 4, 4})"),
        Vec::<i64>::new()
    );
}

#[test]
fn query_props_in_bounds_tests_each_prop_s_rotated_and_scaled_recipe_box_edges() {
    // prop 1: item 5 at (10, 0, 0) turned 90 degrees about z (local x is world y): recipe x 0..4, so world y 0..4
    let half = std::f32::consts::FRAC_1_SQRT_2;
    let turned = prop_transform([10.0, 0.0, 0.0], [0.0, 0.0, half, half], [1.0; 3]);
    let w = prop(EcsWorld::server(), 1, 5, turned, Some(UUID));
    // prop 2: item 6 at the origin scaled 2 on x: recipe x 0..1, so world x 0..2
    let w = prop(
        w,
        2,
        6,
        prop_transform([0.0; 3], IDENTITY, [2.0, 1.0, 1.0]),
        Some(UUID),
    );
    // prop 3: item 6 inside the bounds but naming no template
    let w = prop(
        w,
        3,
        6,
        prop_transform([0.5, 0.0, 0.0], IDENTITY, [1.0; 3]),
        None,
    );
    // prop 4: item 9 has no recipe
    let w = prop(
        w,
        4,
        9,
        prop_transform([0.5, 0.0, 0.0], IDENTITY, [1.0; 3]),
        Some(UUID),
    );
    let w = setup(w, Side::Server);
    assert!(w.eval::<bool>(
        "return (world.entity.register_prop_recipes({{itemId = 5, bounds = {0, -0.5, -0.5, 4, 0.5, 0.5}, feedback = 0},
                 {itemId = 6, bounds = {0, -0.5, -0.5, 1, 0.5, 0.5}, feedback = 0}}))"
    ));
    let q = |b: &str| ids(&w, &format!("world.entity.query_props_in_bounds({{{b}}})"));
    // the turned box: x 9.5..10.5, y 0..4
    assert_eq!(q("9, 3.999, -1, 11, 5, 1"), vec![1]);
    assert_eq!(q("9, 4.001, -1, 11, 5, 1"), Vec::<i64>::new());
    assert_eq!(q("9, -1, -1, 11, -0.001, 1"), Vec::<i64>::new());
    assert_eq!(q("9, -1, -1, 11, 0.001, 1"), vec![1]);
    assert_eq!(
        q("10.6, 0, -1, 12, 4, 1"),
        Vec::<i64>::new(),
        "its x extent is the recipe's y"
    );
    // the scaled box (exact arithmetic): x 0..2; its maximum meets a query minimum, the query maximum is exclusive
    assert_eq!(q("1.9, -1, -1, 3, 1, 1"), vec![2]);
    assert_eq!(
        q("2, -1, -1, 3, 1, 1"),
        vec![2],
        "the box's maximum meets the query's minimum"
    );
    assert_eq!(q("2.01, -1, -1, 3, 1, 1"), Vec::<i64>::new());
    assert_eq!(
        q("-1, -1, -1, 0, 1, 1"),
        Vec::<i64>::new(),
        "the query's maximum is exclusive"
    );
    // the template is read for a kept prop only
    let text: String = w.eval(
        "local p = world.entity.query_props_in_bounds({1.9, -1, -1, 3, 1, 1})[1]
         return p.templateUuidHighHex .. ':' .. p.templateUuidLowHex",
    );
    assert_eq!(text, "1000000000000001:2000000000000002");
    // bounds rules
    assert_eq!(
        w.raised("world.entity.query_props_in_bounds({0, 0, 0, 1, 1})"),
        "bounds must contain exactly 6 numbers"
    );
    assert_eq!(
        w.reason("world.entity.query_props_in_bounds({0, 0, 0, 1, 0, 1})"),
        pt::MIN_MAX
    );
    assert_eq!(
        w.raised("world.entity.query_props_in_bounds({0, 0, 0, 1, 1, 1}, 2)"),
        pt::IN_BOUNDS_ARGUMENTS
    );
    // the box arithmetic on its own: a zero-sized box counts a quarter metre each way
    let at = [(5.0f64 * 4_294_967_296.0) as i64, 0, 0];
    assert!(recipes::intersects(
        &at,
        &IDENTITY,
        &[1.0; 3],
        &[0.0; 6],
        &[5.2, -1.0, -1.0, 6.0, 1.0, 1.0]
    ));
    assert!(!recipes::intersects(
        &at,
        &IDENTITY,
        &[1.0; 3],
        &[0.0; 6],
        &[5.3, -1.0, -1.0, 6.0, 1.0, 1.0]
    ));
    assert!(!recipes::intersects(
        &at,
        &[0.0; 4],
        &[1.0; 3],
        &[0.0; 6],
        &[-9.0, -9.0, -9.0, 9.0, 9.0, 9.0]
    ));
}

#[test]
fn get_transform_reads_a_live_handle_and_refuses_stale_ones_with_shroudforge_s_reasons() {
    let w = setup(props_world(), Side::Server);
    let text: String = w.eval(
        "local h = ecs.resolve(1)
         local p, why = world.entity.get_transform(h)
         assert(p, why)
         return table.concat({p.handle == h and 'same' or 'other', p.entityId, p.itemId, p.templateUuidHighHex,
           p.transform.position.z}, ',')",
    );
    assert_eq!(text, "same,1,5,1000000000000001,1.0");
    let h1: i64 = w.eval("return (ecs.resolve(1))");
    // unknown and zero handles
    assert_eq!(
        w.reason("world.entity.get_transform(4000000000)"),
        pt::STALE
    );
    assert_eq!(
        w.reason("world.entity.get_transform(0)"),
        pt::INVALID_HANDLE
    );
    // a prop naming no template, an entity that is not a prop: not readable now
    let h5: i64 = w.eval("return (ecs.resolve(5))");
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h5})")),
        pt::RETRY
    );
    let h4: i64 = w.eval("return (ecs.resolve(4))");
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h4})")),
        pt::RETRY
    );
    // the id now names a newer entity: stale
    w.world(|world| world.entities.get_mut(&1).unwrap().generation = 2);
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h1})")),
        pt::STALE
    );
    // the entity is gone
    let h2: i64 = w.eval("return (ecs.resolve(2))");
    w.world(|world| world.entities.remove(&2));
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h2})")),
        pt::ABSENT
    );
    // no live session, another session, the session latch
    let h3: i64 = w.eval("return (ecs.resolve(5))");
    w.session.set(0);
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h3})")),
        pt::NO_SESSION
    );
    w.session.set(FAKE_SESSION + 1);
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h3})")),
        pt::STALE
    );
    w.session.set(FAKE_SESSION);
    *w.host.fault.borrow_mut() = Some("latched".into());
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h3})")),
        pt::UNCERTAIN
    );
    *w.host.fault.borrow_mut() = None;
    assert_eq!(
        w.raised("world.entity.get_transform()"),
        pt::TRANSFORM_ARGUMENTS
    );
    // a VM without the ECS family
    let lua = w.vm.lua();
    let table = lua.create_table().unwrap();
    props::install_into(
        lua,
        &table,
        w.host.clone(),
        Rc::new(|_| None),
        w.recipes.clone(),
    )
    .unwrap();
    w.vm.set("bare", table);
    assert_eq!(w.reason("bare.get_transform(1)"), pt::PROVIDER_READ);
    assert_eq!(
        w.reason("bare.query_props({0, 0, 0, 1, 1, 1}, 0)"),
        pt::PROVIDER
    );
}

#[test]
fn set_scale_writes_the_three_scale_floats_through_the_compare_exchange() {
    let w = setup(props_world(), Side::Server);
    let h: i64 = w.eval("return (ecs.resolve(1))");
    let (ok, why): (bool, Option<String>) =
        w.eval(&format!("return world.entity.set_scale({h}, {{2, 3, 4}})"));
    assert_eq!((ok, why), (true, None));
    let (entity, component, bytes) = w.world(|world| world.writes.last().cloned()).unwrap();
    assert_eq!((entity, component), (1, TRANSFORM_HASH()));
    let floats: Vec<f32> = (0..3)
        .map(|k| f32::from_le_bytes(bytes[40 + k * 4..44 + k * 4].try_into().unwrap()))
        .collect();
    assert_eq!(floats, [2.0, 3.0, 4.0]);
    assert_eq!(
        &bytes[..40],
        &prop_transform([1.0; 3], IDENTITY, [1.0; 3])[..40]
    );
    // only the scale bytes are under the mask
    let mask = w
        .world(|world| world.masks.last().cloned().flatten())
        .unwrap();
    assert!(
        mask.iter()
            .enumerate()
            .all(|(i, m)| (*m != 0) == (40..52).contains(&i))
    );
    // one compare-exchange against the bytes just read: a frame that moved the prop in between fails the call and
    // nothing is written (ShroudForge's native update gives up at the first conflict)
    let writes = w.world(|world| world.writes.len());
    w.world(|world| {
        world.cas.push_back(CasStep::Foreign {
            entity: 1,
            component: TRANSFORM_HASH(),
            offset: 0,
            bytes: (7i64 << 32).to_le_bytes().to_vec(),
        })
    });
    assert_eq!(
        w.reason(&format!("world.entity.set_scale({h}, {{5, 5, 5}})")),
        pt::SCALE_FAILED
    );
    assert_eq!(w.world(|world| world.writes.len()), writes);
    // every other refusal of the update is ShroudForge's one text too
    w.world(|world| world.cas.push_back(CasStep::Answer(Knob::Busy)));
    assert_eq!(
        w.reason(&format!("world.entity.set_scale({h}, {{5, 5, 5}})")),
        pt::SCALE_FAILED
    );
    assert_eq!(w.world(|world| world.writes.len()), writes);
    // the rules
    assert_eq!(
        w.raised(&format!("world.entity.set_scale({h})")),
        pt::SCALE_ARGUMENTS
    );
    assert_eq!(
        w.raised(&format!("world.entity.set_scale({h}, {{1, 2}})")),
        "scale must contain exactly 3 numbers"
    );
    let reason = |call: &str| w.reason(call);
    assert_eq!(
        reason(&format!("world.entity.set_scale({h}, {{1, math.huge, 1}})")),
        pt::SCALE_FINITE
    );
    // finite, but beyond a float: the native update refuses it
    assert_eq!(
        reason(&format!("world.entity.set_scale({h}, {{1, 1e300, 1}})")),
        pt::SCALE_FAILED
    );
    *w.host.fault.borrow_mut() = Some("latched".into());
    assert_eq!(
        reason(&format!("world.entity.set_scale({h}, {{1, 1, 1}})")),
        "latched"
    );
    *w.host.fault.borrow_mut() = None;
    // CurrentTransform tracked: no write phase
    fake_services::set_write_policy(
        w.owner,
        TRANSFORM_HASH(),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    assert_eq!(
        reason(&format!("world.entity.set_scale({h}, {{1, 1, 1}})")),
        pt::NO_WRITE_PHASE
    );
    fake_services::set_write_policy(w.owner, TRANSFORM_HASH(), None);
    // a stale handle: the snapshot is refused
    assert_eq!(
        reason("world.entity.set_scale(4000000000, {1, 1, 1})"),
        pt::SCALE_FAILED
    );
    w.host.contract.set(false);
    assert_eq!(
        reason(&format!("world.entity.set_scale({h}, {{1, 1, 1}})")),
        format!(
            "operation runtime.world.entity.set_scale has not been verified for game build {BUILD}"
        )
    );
}

#[test]
fn props_follow_the_world_the_shared_ecs_binds() {
    let w = setup(props_world(), Side::Server);
    let h: i64 = w.eval(
        "local r = assert(world.entity.query_props({0, 0, 0, 4, 4, 4}, 0)); return r[1].handle",
    );
    let (t, why): (Value, Option<String>) =
        w.eval(&format!("return world.entity.get_transform({h})"));
    assert!(matches!(t, Value::Table(_)), "{why:?}");
    // the shared Ecs now binds another world (a client that stopped hosting and joined one): the handle reads nothing
    w.bound.set(CLIENT);
    assert_eq!(
        w.reason(&format!("world.entity.get_transform({h})")),
        pt::STALE
    );
    assert_eq!(
        w.reason(&format!("world.entity.set_scale({h}, {{2, 2, 2}})")),
        pt::SCALE_FAILED
    );
    assert!(w.world(|world| world.writes.is_empty()));
}

// ---- the catalog ----

fn capabilities(backing: Backing) -> Vec<&'static str> {
    match backing {
        Backing::Kiln(needs) => needs.iter().map(|n| n.capability).collect(),
        _ => panic!("Kiln needs"),
    }
}
fn fact_row(fact: Fact) -> &'static kiln_sfi::runtime::operation_catalog::FactRow {
    CATALOG.facts.iter().find(|r| r.fact == fact).unwrap()
}

/// Every capability row this binary sets is set here only (kiln.caps is process-wide).
#[test]
fn the_catalog_backs_cursor_building_input_and_props_and_their_facts_follow_kiln() {
    assert_eq!(CATALOG.check(), Ok(()));
    assert_eq!(
        capabilities(CATALOG.row(Op::WorldCursorGet).unwrap().backing),
        ["world.ecs_read"]
    );
    assert_eq!(
        capabilities(CATALOG.row(Op::WorldBuildingInput).unwrap().backing),
        ["client.building_input"]
    );
    for op in [
        Op::WorldEntityQueryProps,
        Op::WorldEntityQueryPropsInBounds,
        Op::WorldEntityRegisterPropRecipes,
        Op::WorldEntityGetTransform,
    ] {
        assert_eq!(
            capabilities(CATALOG.row(op).unwrap().backing),
            ["world.ecs_read"],
            "{op:?}"
        );
    }
    assert_eq!(
        capabilities(CATALOG.row(Op::WorldEntitySetScale).unwrap().backing),
        ["world.ecs_read", "world.ecs_write"]
    );
    assert!(
        fact_row(Fact::WorldOperation(Op::WorldCursorGet))
            .read
            .is_some()
    );
    assert!(
        fact_row(Fact::WorldOperation(Op::WorldBuildingInput))
            .read
            .is_none()
    );
    assert!(fact_row(Fact::WorldEntityQueryPropsReady).read.is_some());
    assert!(fact_row(Fact::CanWriteCurrentTransform).read.is_some());

    // building input: refused while the producer's row is unavailable, lit when it turns available
    let w = cursor_setup(&sample());
    assert_eq!(building_input::probe(&w.services), Ok(()));
    let building = || {
        w.facts
            .provider_facts()
            .world_operation_available(Op::WorldBuildingInput)
    };
    fake_services::set_cap(
        "client.building_input",
        Some((KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(
        !w.facts
            .static_support(Op::WorldBuildingInput)
            .is_available()
    );
    assert!(!building());
    fake_services::set_cap(
        "client.building_input",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(
        w.facts
            .static_support(Op::WorldBuildingInput)
            .is_available()
    );
    assert!(building());
    fake_services::set_cap("client.building_input", None);
    assert!(!building());

    // the cursor: a live client world with an exact profile and a known local player
    assert_eq!(cursor::probe(&w.services), Ok(()));
    let cursor = || {
        w.facts
            .provider_facts()
            .world_operation_available(Op::WorldCursorGet)
    };
    assert!(!cursor() && !w.facts.static_support(Op::WorldCursorGet).is_available());
    fake_services::set_cap(
        "world.ecs_read",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(w.facts.static_support(Op::WorldCursorGet).is_available());
    assert!(cursor());
    w.client(|model| model.state.as_mut().unwrap().local_player = 0);
    assert!(!cursor());
    w.client(|model| *model = ClientModel::local(PLAYER));
    w.world(|world| world.world_kind = SERVER);
    assert!(!cursor());
    w.world(|world| world.world_kind = CLIENT);
    w.world(|world| world.exact_profile = false);
    assert!(!cursor());

    // props: ready with the ECS reads; set_scale's fact also needs the writes and CurrentTransform's policy
    let s = setup(props_world(), Side::Server);
    assert_eq!(props::probe(&s.services), Ok(()));
    let facts = || s.facts.provider_facts();
    assert!(facts().world_entity_query_props_ready);
    assert!(
        s.facts
            .static_support(Op::WorldEntityQueryProps)
            .is_available()
    );
    assert!(!facts().can_write_current_transform);
    fake_services::set_cap(
        "world.ecs_write",
        Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)),
    );
    assert!(facts().can_write_current_transform);
    assert!(
        s.facts
            .static_support(Op::WorldEntitySetScale)
            .is_available()
    );
    fake_services::set_write_policy(
        s.owner,
        TRANSFORM_HASH(),
        Some((
            KILN_ECS_NOT_WRITABLE as u32,
            KILN_ECS_WRITE_WHY_TRACKED as u32,
        )),
    );
    assert!(!facts().can_write_current_transform);
    fake_services::set_write_policy(s.owner, TRANSFORM_HASH(), None);
    s.world(|world| world.exact_profile = false);
    assert!(!facts().world_entity_query_props_ready && !facts().can_write_current_transform);
    fake_services::set_cap("world.ecs_write", None);
    fake_services::set_cap("world.ecs_read", None);
    assert!(!facts().world_entity_query_props_ready);
}
