// SPDX-License-Identifier: MIT
//! The kiln-mode probe of the client pass (mods/tests/sf-kiln-probe): its package parses as a 1.7.9 package with its
//! settings and button, and its Lua writes one line per probe (prefix "sf-kiln-probe: <probe> run=<n> ok=..."), with
//! every refusal and every error caught:
//! - on the production start (the production plan source and registrar over the fake core, the probe's own folder
//!   copied into <game>/mods): each probe of the first run logs its line, the refusals carry their reason, nothing
//!   calls a missing member, and the write is skipped with its setting off;
//! - in a plain VM on scripted members that answer: the values the guided checks read (56 bytes, the cursor in
//!   metres, the props count before and after the recipes, the transform inside the box, the resolve, the write and
//!   its restore only on a run started by the shortcut with the setting on);
//! - in a plain VM where every member raises, or is missing: every probe still logs, ok=false with the reason.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::lifecycle;
use kiln_sfi::plan_source::KilnPlanSource;
use kiln_sfi::worker::{Worker, WorkerPhase};
use kiln_sfi::{Boundary, InactiveReason, Phase};
use mlua::{Lua, Table};
use serde_json::{Value, json};
use sfi_core::package::{self, ParseOptions};
use sfi_policy::digest::package_content_sha256;
use std::path::{Path, PathBuf};
use std::time::Duration;
use support::*;

/// A liveness bound, as plan_source.rs's.
const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
const ID: &str = "kiln-sf-kiln-probe";

/// The probes every run logs a line for (world.props.before only on the first run of a world session).
const EVERY_RUN: &[&str] = &[
    "run",
    "ecs.catalog",
    "ecs.query",
    "ecs.players",
    "ecs.read_bytes",
    "ecs.read_fields",
    "ecs.get_template",
    "world.cursor",
    "resources.by_type",
    "resources.get",
    "world.props.nearby",
    "world.props.recipes",
    "world.props.query",
    "world.props.transform",
    "ecs.resolve",
    "attributes.list",
    "attributes.read",
    "services.get",
    "services.read_fields",
    "voxel.grid_spec",
    "building.status",
    "ecs.write_fields",
    "done",
];

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../mods/tests/sf-kiln-probe")
}
fn read(name: &str) -> Vec<u8> {
    std::fs::read(root().join(name)).unwrap()
}
fn source() -> String {
    String::from_utf8(read("src/mod.lua")).unwrap()
}

/// The line of `probe` in `run`, from lines that may carry the log's "[id] " prefix.
fn line<'a>(lines: &'a [String], probe: &str, run: u32) -> Option<&'a str> {
    let head = format!("sf-kiln-probe: {probe} run={run} ok=");
    lines
        .iter()
        .map(|l| l.split_once("] ").map_or(l.as_str(), |(_, t)| t))
        .find(|l| l.starts_with(&head))
}
fn must<'a>(lines: &'a [String], probe: &str, run: u32) -> &'a str {
    line(lines, probe, run).unwrap_or_else(|| panic!("no {probe} line in run {run}: {lines:#?}"))
}
/// Every line of the probe names its probe, says ok, and a refusal or a skip says why.
fn well_formed(lines: &[String]) {
    for l in lines {
        let Some((_, text)) = l.split_once("sf-kiln-probe: ") else {
            continue;
        };
        assert!(text.contains(" ok="), "{l}");
        if text.contains(" ok=false") || text.contains(" ok=skip") {
            assert!(text.contains(" reason="), "{l}");
        }
        assert!(!text.contains("attempt to call a nil value"), "{l}");
        assert!(!text.contains("probe error:"), "{l}");
    }
}

#[test]
fn the_probe_parses_as_a_1_7_9_package_with_its_settings_and_button() {
    let parsed = package::Package::parse(
        &read("mod.json"),
        Some(&read("extended.mod.json")),
        ParseOptions {
            version: package::SourceVersion::Sf179,
            ..ParseOptions::default()
        },
    )
    .unwrap();
    let m = parsed.manifest;
    assert_eq!(m.id, ID);
    let keys: Vec<&str> = m.settings.iter().map(|s| s.key.as_str()).collect();
    for key in [
        "probeKey",
        "intervalSeconds",
        "cursorSeconds",
        "boxHalfSize",
        "allowWrites",
        "writeComponent",
    ] {
        assert!(keys.contains(&key), "{key}: {keys:?}");
    }
    assert_eq!(
        m.setting_values["allowWrites"],
        json!(false),
        "writes are off by default"
    );
    assert!(
        m.groups
            .iter()
            .flat_map(|g| &g.actions)
            .any(|a| a.id == "runProbes"),
        "{:?}",
        m.groups
    );
}

// ---- the production start ----

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn copy(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).unwrap();
    for entry in std::fs::read_dir(from).unwrap() {
        let entry = entry.unwrap();
        let target = to.join(entry.file_name());
        if entry.file_type().unwrap().is_dir() {
            copy(&entry.path(), &target);
        } else {
            std::fs::copy(entry.path(), target).unwrap();
        }
    }
}

/// The probe's folder in <game>/mods, a client plan admitting its Lua, the early and normal phases with the
/// production worker factory (as plan_source.rs's boot), then ticks until its first run ended.
#[test]
fn on_the_production_start_every_probe_logs_and_every_refusal_is_caught() {
    let game = game_dir();
    let folder = game.join("mods").join("sf-kiln-probe");
    copy(&root(), &folder);
    let content = package_content_sha256(&folder).unwrap();
    let candidate = json!({"id":ID,"version":"0.1.0","kind":"shroudforge","root":"mods/sf-kiln-probe",
      "content_sha256":hex(&content),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client"],
      "capabilities":["runtime"],"dependencies":[],"requiredApis":[],"services":[],"requires180":false,
      "saveSensitive":false,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]});
    let mut document = plan_document("client");
    document["packages"] = Value::Array(vec![candidate]);
    document["loadOrder"] = json!([ID]);
    set_plan(Some(serde_json::to_vec(&document).unwrap()));

    set_launch(Some(KILN));
    set_state_dir(fresh_dir("probe-state"));
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_startup(fake_startup::resolve),
    ));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let api = Box::leak(api(true));
    fake_archive::wrap(api);
    fake_startup::install(fake_startup::Config::granting_current());
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
        KILN_OK
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut info) },
        KILN_OK
    );
    fake_startup::grant_archive(normal);
    if let Some(ready) = info.on_ready {
        unsafe { ready(info.user) };
    }
    let lines = || -> Vec<String> { logs(normal).into_iter().map(|(_, t)| t).collect() };
    assert!(
        eventually(WAIT, || worker
            .shared()
            .is_some_and(|s| s.phase() == WorkerPhase::Running)),
        "{:#?}",
        lines()
    );
    assert!(
        eventually(WAIT, || {
            fire_tick(normal);
            line(&lines(), "done", 1).is_some()
        }),
        "{:#?}",
        lines()
    );
    let all = lines();
    let mine: Vec<String> = all
        .iter()
        .filter(|l| l.starts_with(&format!("[{ID}] ")))
        .cloned()
        .collect();
    println!("{}", mine.join("\n"));
    must(&mine, "loaded", 0);
    for probe in EVERY_RUN.iter().chain(&["world.props.before"]) {
        must(&mine, probe, 1);
    }
    well_formed(&mine);
    // this fake core has no verified world operations: each member answers its own refusal, caught and logged
    for probe in [
        "ecs.query",
        "ecs.players",
        "world.cursor",
        "world.props.before",
        "world.props.query",
        "services.get",
        "voxel.grid_spec",
    ] {
        let text = must(&mine, probe, 1);
        assert!(
            text.contains(" ok=false ")
                && text.contains(" reason=operation runtime.")
                && text.contains(" has not been verified for game build "),
            "{text}"
        );
    }
    // a member that raises (the loaded resources of a type this fake core's game.types does not know): its error's
    // message, no traceback
    assert!(
        must(&mine, "resources.by_type", 1)
            .ends_with(" reason=error: type not found: keen::ItemInfo"),
        "{mine:#?}"
    );
    // ShroudForge's own answer without a request, and the write held back by its setting
    assert!(
        must(&mine, "building.status", 1).contains(" ok=true ")
            && must(&mine, "building.status", 1).ends_with(" status=unknown")
    );
    assert!(
        must(&mine, "ecs.write_fields", 1).ends_with(" reason=Allow writes is off"),
        "{mine:#?}"
    );
    assert!(must(&mine, "done", 1).contains(" probes=16"));
    // the package runs on: no warning that it stopped
    assert!(
        !all.iter()
            .any(|l| l.contains("will not run") || l.contains(&format!("'{ID}' stopped"))),
        "{all:#?}"
    );
    if let Some(shared) = worker.shared() {
        shared.request_stop();
        assert!(pool.task(0).wait_finished(WAIT));
    }
}

// ---- a plain VM on scripted members ----

/// Members that answer as a hosted world would: a catalog with CurrentTransform and one writable component, 56-byte
/// transforms, a cursor, props that need their recipes, loaded ItemInfo resources, a named service, attributes.
const ANSWERING: &str = r#"
local Q = 4294967296
lines, writes = {}, {}
local function ty(prim, fields, inner) return { primitive_type = prim, struct_fields = fields, inner_type = inner } end
local f32, i64 = ty("Float32"), ty("SInt64")
local vec3 = ty("Struct", { x = {name="x", type=i64, data_offset=0}, y = {name="y", type=i64, data_offset=8},
  z = {name="z", type=i64, data_offset=16} })
local transform_t = ty("Struct", { position = {name="position", type=vec3, data_offset=0},
  scale = {name="scale", type=f32, data_offset=40} })
local writable_t = ty("Struct", { flag = {name="flag", type=ty("Bool"), data_offset=4},
  speed = {name="speed", type=ty("Typedef", nil, f32), data_offset=0} })
store = { speed = 2.5, flag = false }
local resources = {}
for i = 1, 3 do
  resources[i] = { guid = "guid-" .. i, part = 0, source = "loaded-runtime", loaded = true, session_id = 5,
    data = { itemId = 100 + i, equipment = { placementAABBmin = {x=-1,y=0,z=-1}, placementAABBmax = {x=1,y=2,z=1},
      placementColliders = { { dataArray = { { value = { materialFeedbackId = 9 } } } } } } } }
end
local registered, sequence = nil, 41
runtime = {
  has = function(f) return true end,
  world = {
    session_id = function() return 5 end,
    context_kind = function() return "direct" end,
    cursor = { get = function() sequence = sequence + 1
      return { sequence = sequence, value = { primaryTransform = { position = { x = 3*Q, y = 4*Q, z = 5*Q } },
        attachedToProp = false, material = 2 } } end },
    entity = {
      query_props_in_bounds = function(b)
        if not registered then return nil, "native recipe-bounds prop query failed" end
        return { { handle = 77, entityId = 900, itemId = 101 } }
      end,
      query_props = function(b, p) return { { handle = 77, entityId = 900, itemId = 101 },
        { handle = 78, entityId = 901, itemId = 555 } } end,
      register_prop_recipes = function(r) registered = #r return true end,
      get_transform = function(h) return { handle = h, entityId = 900, itemId = 101,
        transform = { position = { x = 11.5, y = 2, z = -3 } } } end,
    },
    voxel = { get_grid_spec = function(id) return { id = id, origin = {0,0,0}, cellSize = {0.5,0.5,0.5},
      maximum = {100,100,100} } end },
    building = { status = function(id) return "unknown" end },
  },
  ecs = {
    get_catalog = function() return { registry_available = true, entity_access_ready = true,
      entity_writes_ready = true, entries = {
        { qualified_name = "keen::ecs::CurrentTransform", type = transform_t, read_fields_available = true,
          write_fields_available = false },
        { qualified_name = "keen::ecs::ProbeWritable", type = writable_t, read_fields_available = true,
          write_fields_available = true } } } end,
    query = function(name) if name == "keen::ecs::PlayerInput" then return { 8 } end return { 7, 8 } end,
    read_bytes = function(e, c) return string.pack("<i8i8i8", 10*Q, 2*Q, -3*Q) .. string.rep("\0", 32) end,
    read_fields = function(e, c, paths) local out = {}
      for _, p in ipairs(paths) do if store[p] ~= nil then out[p] = store[p] else out[p] = 1 end end return out end,
    get_template = function(e) return nil, "loaded resources are not available yet" end,
    resolve = function(id) if id == 900 then return 77 end return nil, "no entity" end,
    get_attributes = function() return { version = "v1", count = 2, entries = { { name = "Missing" }, { name = "Health" } } } end,
    read_attribute = function(e, n) if n == "Health" then return 100.0 end return nil, "no root" end,
    write_fields = function(e, c, changes, expected)
      for k, v in pairs(changes) do
        if expected and expected[k] ~= nil and math.abs(expected[k] - store[k]) > 1e-6 then return false, "conflict" end
        store[k] = v
      end
      writes[#writes + 1] = c .. ":" .. tostring(store.speed)
      return true, nil, { speed = store.speed }
    end,
  },
  services = { get = function(k) return { key = k, type = { qualified_name = "keen::ecs::GameSettings" },
    session_id = 5, write_supported = true,
    read_fields = function(self, p) return { enableDurability = true } end } end },
}
game = { assets = {
  get_loaded_resources_by_type = function(t) return resources end,
  get_loaded_resource = function(g, t, p) return resources[1] end,
} }
pressed = false
shroudforge = {
  log = { info = function(t) lines[#lines + 1] = t end },
  settings = { get = function(k, d) local v = SETTINGS[k] if v == nil then return d end return v end },
  input = { was_keybind_pressed = function() local p = pressed pressed = false return p end,
    keybind_label = function() return "F5" end },
  ui = { on_action = function(id, fn) ACTIONS[id] = fn end },
}
"#;

/// Members that raise (or are missing, `MODE = "missing"`).
const REFUSING: &str = r#"
lines = {}
local function boom() error("refused by the test", 0) end
local raising = setmetatable({}, { __index = function() return boom end })
if MODE == "raise" then
  runtime = { has = boom, ecs = raising, services = raising,
    world = { session_id = boom, context_kind = boom, cursor = raising, entity = raising, voxel = raising,
      building = raising } }
  game = { assets = raising }
else
  runtime, game = {}, nil
end
shroudforge = { log = { info = function(t) lines[#lines + 1] = t end },
  settings = { get = function(k, d) if k == "allowWrites" then return true end return d end },
  input = { was_keybind_pressed = function() return true end } }
"#;

/// The probe on `members` (with `setup` run first): its module table and the VM.
fn vm(members: &str, setup: &str) -> (Lua, Table) {
    let lua = Lua::new();
    lua.load(setup).exec().unwrap();
    lua.load(members).exec().unwrap();
    let probe: Table = lua.load(source()).set_name("=mod.lua").eval().unwrap();
    assert_eq!(probe.get::<u64>("update_interval_ms").unwrap(), 100);
    (lua, probe)
}
fn lines_of(lua: &Lua) -> Vec<String> {
    lua.globals().get::<Vec<String>>("lines").unwrap()
}
fn update(probe: &Table, delta: f64) {
    probe
        .get::<mlua::Function>("on_update")
        .unwrap()
        .call::<()>(delta)
        .unwrap();
}

#[test]
fn on_answering_members_the_lines_carry_what_the_guided_checks_read() {
    let (lua, probe) = vm(ANSWERING, "SETTINGS = { allowWrites = true } ACTIONS = {}");
    probe
        .get::<mlua::Function>("on_load")
        .unwrap()
        .call::<()>(())
        .unwrap();
    update(&probe, 0.1); // the first update runs every probe (a timer run)
    let l = lines_of(&lua);
    assert_eq!(
        must(&l, "loaded", 0),
        "sf-kiln-probe: loaded run=0 ok=true version=0.1.0 button=true key=F5 interval=30 writes=on"
    );
    assert!(must(&l, "run", 1).contains(" via=timer "), "{l:#?}");
    assert!(
        must(&l, "ecs.read_bytes", 1).contains(" size=56 expected=56 x=10.0000 y=2.0000 z=-3.0000"),
        "{l:#?}"
    );
    assert!(
        must(&l, "ecs.read_fields", 1)
            .contains(" paths=4 values=4 first=position.x:1,position.y:1,position.z:1,scale:1"),
        "{l:#?}"
    );
    assert_eq!(
        must(&l, "ecs.get_template", 1),
        "sf-kiln-probe: ecs.get_template run=1 ok=false entity=7 reason=loaded resources are not available yet"
    );
    assert!(
        must(&l, "world.cursor", 1)
            .contains(" sequence=42 x=3.0000 y=4.0000 z=5.0000 attachedToProp=false"),
        "{l:#?}"
    );
    // before the recipes, the query's own refusal; then the ItemInfo boxes and a 1 m box for the nearby item without one
    assert!(
        must(&l, "world.props.before", 1)
            .ends_with(" reason=native recipe-bounds prop query failed"),
        "{l:#?}"
    );
    assert!(
        must(&l, "world.props.recipes", 1)
            .contains(" ok=true has=true recipes=4 from_iteminfo=3 generic=1"),
        "{l:#?}"
    );
    // the box is ±12 m around the player's transform (10, 2, -3)
    assert!(
        must(&l, "world.props.query", 1).contains(
            " center=player box=-2.0000,-10.0000,-15.0000,22.0000,14.0000,9.0000 count=1"
        ),
        "{l:#?}"
    );
    assert!(
        must(&l, "world.props.transform", 1)
            .contains(" handle=77 entityId=900 itemId=101 x=11.5000 y=2 z=-3 inside=true"),
        "{l:#?}"
    );
    assert!(
        must(&l, "ecs.resolve", 1).ends_with(" entityId=900 handle=77 same=true"),
        "{l:#?}"
    );
    assert!(
        must(&l, "attributes.read", 1).ends_with(" entity=8 name=Health value=100.0000 tried=2"),
        "{l:#?}"
    );
    assert!(
        must(&l, "services.read_fields", 1)
            .ends_with(" key=ecs::GameSettings enableDurability=true"),
        "{l:#?}"
    );
    assert!(
        must(&l, "resources.get", 1)
            .contains(" guid=guid-1 part=0 source=loaded-runtime loaded=true"),
        "{l:#?}"
    );
    assert!(
        must(&l, "voxel.grid_spec", 1)
            .ends_with(" id=voxel origin=0,0,0 cellSize=0.5000,0.5000,0.5000 maximum=100,100,100"),
        "{l:#?}"
    );
    assert!(
        must(&l, "building.status", 1).ends_with(" request=1 status=unknown"),
        "{l:#?}"
    );
    // a timer run never writes, even with the setting on
    assert!(
        must(&l, "ecs.write_fields", 1)
            .ends_with(" ok=skip has=true reason=writes run only on the shortcut or the button"),
        "{l:#?}"
    );
    assert_eq!(lua.globals().get::<Table>("writes").unwrap().raw_len(), 0);
    for probe in EVERY_RUN {
        must(&l, probe, 1);
    }
    // between runs: the cursor once a second, its sequence growing
    update(&probe, 1.1);
    let l = lines_of(&lua);
    assert_eq!(
        l.iter()
            .filter(|x| x.starts_with("sf-kiln-probe: world.cursor run=1 "))
            .count(),
        2
    );
    assert!(l.last().unwrap().contains(" sequence=43 "), "{l:#?}");
    // the shortcut: a run that writes the float field (preferred over the flag) and writes the old value back
    lua.globals().set("pressed", true).unwrap();
    update(&probe, 0.1);
    let l = lines_of(&lua);
    assert!(must(&l, "run", 2).contains(" via=key "), "{l:#?}");
    assert!(
        line(&l, "world.props.before", 2).is_none(),
        "once per world session"
    );
    assert_eq!(
        must(&l, "ecs.write_fields", 2),
        "sf-kiln-probe: ecs.write_fields run=2 ok=true has=true component=keen::ecs::ProbeWritable entity=7 field=speed \
         kind=Float32 before=2.5000 wrote=2.5025 confirmed=2.5025"
    );
    assert_eq!(
        must(&l, "ecs.write_fields.restore", 2),
        "sf-kiln-probe: ecs.write_fields.restore run=2 ok=true has=true component=keen::ecs::ProbeWritable entity=7 \
         field=speed kind=Float32 value=2.5000 confirmed=2.5000 restored=true"
    );
    let writes: Vec<String> = lua.globals().get("writes").unwrap();
    assert_eq!(
        writes,
        [
            "keen::ecs::ProbeWritable:2.5025",
            "keen::ecs::ProbeWritable:2.5"
        ]
    );
    // the button runs the same way
    let action: mlua::Function = lua.load("ACTIONS.runProbes").eval().unwrap();
    action.call::<()>(()).unwrap();
    let l = lines_of(&lua);
    assert!(must(&l, "run", 3).contains(" via=button "), "{l:#?}");
    must(&l, "ecs.write_fields.restore", 3);
    well_formed(&l);
}

#[test]
fn with_the_setting_off_nothing_is_written_on_any_run() {
    let (lua, probe) = vm(ANSWERING, "SETTINGS = {} ACTIONS = {}");
    probe
        .get::<mlua::Function>("on_load")
        .unwrap()
        .call::<()>(())
        .unwrap();
    lua.globals().set("pressed", true).unwrap();
    update(&probe, 0.1);
    let l = lines_of(&lua);
    assert!(
        must(&l, "run", 1).contains(" via=key ") && must(&l, "run", 1).contains(" writes=off ")
    );
    assert!(
        must(&l, "ecs.write_fields", 1).ends_with(" ok=skip has=true reason=Allow writes is off"),
        "{l:#?}"
    );
    assert_eq!(lua.globals().get::<Table>("writes").unwrap().raw_len(), 0);
}

#[test]
fn members_that_raise_or_are_missing_are_caught_and_every_probe_still_logs() {
    for mode in ["raise", "missing"] {
        let (lua, probe) = vm(REFUSING, &format!("MODE = '{mode}'"));
        probe
            .get::<mlua::Function>("on_load")
            .unwrap()
            .call::<()>(())
            .unwrap();
        update(&probe, 0.1);
        let l = lines_of(&lua);
        for probe in EVERY_RUN.iter().chain(&["world.props.before"]) {
            must(&l, probe, 1);
        }
        well_formed(&l);
        let expect = |probe: &str, reason: &str| {
            let text = must(&l, probe, 1);
            assert!(
                text.contains(" ok=false ") && text.ends_with(reason),
                "{mode}: {text}"
            );
        };
        if mode == "raise" {
            expect("ecs.query", "reason=error: refused by the test");
            expect("world.cursor", "reason=error: refused by the test");
            expect("world.props.query", "reason=error: refused by the test");
            expect("resources.by_type", "reason=error: refused by the test");
            expect("building.status", "reason=error: refused by the test");
        } else {
            expect("ecs.query", "reason=runtime.ecs.query is missing");
            expect("world.cursor", "reason=runtime.world.cursor.get is missing");
            expect("services.get", "reason=runtime.services.get is missing");
            expect(
                "resources.by_type",
                "reason=game.assets.get_loaded_resources_by_type is missing",
            );
            expect(
                "voxel.grid_spec",
                "reason=runtime.world.voxel.get_grid_spec is missing",
            );
        }
        // the shortcut asked for a run with writes on, and there is nothing to write to
        assert!(must(&l, "run", 1).contains(" via=key "), "{mode}");
        assert!(
            must(&l, "ecs.write_fields", 1).contains(" ok=skip "),
            "{mode}"
        );
    }
}
