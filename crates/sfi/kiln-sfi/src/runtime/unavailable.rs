// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9, the pin the interpreter targets): modules/mod-runtime/lua/ingame/ check_ingame_access.rs:396-414
// (runtime_denial_reason), ecs/{query_entities,resolve_entity,read_component,access_component_fields,
// access_template_component,write_component}.rs and ecs/attributes/*.rs, access_named_service.rs:21-60,
// impact/mod.rs:127-145, overrides/claim_runtime_storage.rs:230-330, overrides/create_override_targets.lua,
// player_input/{submit_building_input,read_building_cursor}.rs, world/{query_props,register_prop_recipes,
// access_prop_transform,change_building_entities,read_write_voxels}.rs, resources/*.rs,
// functions/{read_function_catalog,bind_profile_modifier}.rs, values/register_value_api.rs,
// shroudforge/v1/create_mod_api.rs:117-189,333-341; game-runtime/compatibility/lib.rs:30-44 (an operation outside
// the contract); game-runtime/bridge/windows/resources/{find_resources_by_type.rs:4-9,read_resource.rs:62-68,
// check_resource_exists.rs:5-11} (a provider without the ResourceService extensions).
//! ShroudForge's answer for every member of its in-game API that no merged family implements yet.
//!
//! ShroudForge installs every member of its API in every in-game environment; a call to a member whose provider is
//! not there answers through that member's own refusal path, never `attempt to call a nil value`. Kiln's production
//! registrar ([`super::registrar`]) ends with [`family`], which fills each name of [`ROWS`] that no other family
//! of the list claims (so a family that lands later takes its names over without touching this file), and
//! [`install_function_members`], which fills the `runtime.functions` members the installer's step 5 does not.
//!
//! **What each member answers.** Its ShroudForge return shape ([`Answer`]), with the reason of its first check:
//! - a member gated by a runtime operation ([`Gate::Ops`]): `runtime_denial_reason` for that operation, in
//!   ShroudForge's order (phase, the `runtime` capability, the package's lifecycle, the write fault, then the
//!   operation's static support). The operations behind these members have no landed row in Kiln's catalog, so
//!   once the lifecycle is active the reason is ShroudForge's `operation <op> has not been verified for game build
//!   <build>`;
//! - a member ShroudForge does not gate ([`Gate::Contract`]): the same text for the member's own name, the reason
//!   ShroudForge gives for an operation outside its contract.
//!
//! Members ShroudForge never refuses keep the answer it gives when its provider has nothing: `building.status` is
//! "unknown" and `building.cancel` false (no request was ever accepted), `input.is_keybind_down` and
//! `was_keybind_pressed` false and `keybind_label` nil (no key reaches the package, as when the game window is not
//! focused), `input.is_key_down` ShroudForge's removal error. `game.assets.get_loaded_resource` and
//! `get_loaded_resources_by_type` are not "nothing loaded": without a ResourceService ShroudForge's bridge raises its
//! provider's text ([`Answer::RaiseText`]), so a package scanning loaded resources is told why nothing comes back. A member never checks its arguments: the refusal
//! comes first (a parity difference: ShroudForge converts some arguments, `building.submit`'s table among them, before it).

use super::context::RuntimeContext;
use super::install::Family;
use mlua::{Lua, MultiValue, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::LuaError;
use std::rc::Rc;

/// The runtime family's name.
pub const FAMILY: &str = "unavailable";

/// Where a member's refusal reason comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Gate {
    /// ShroudForge's denial of these operations, the first that refuses.
    Ops(&'static [Op]),
    /// No gate: the reason of an operation outside the contract, for the member's own name.
    Contract,
}

/// A member's ShroudForge return shape when it refuses (or has nothing).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Answer {
    /// `nil, reason`
    NilReason,
    /// `false, reason`
    FalseReason,
    /// `nil, reason, nil`
    NilReasonNil,
    /// `false, reason, nil`
    FalseReasonNil,
    /// raises the reason
    Raise,
    /// `nil`
    Nil,
    /// `false`
    False,
    /// an empty table
    EmptyTable,
    /// the text `unknown`
    Unknown,
    /// raises ShroudForge's removal text of `shroudforge.input.is_key_down`
    LegacyKeyDown,
    /// raises ShroudForge's text for `loader.runtime.register_dll` while its runtime DLL feature is off
    DllUnavailable,
    /// raises this text of ShroudForge's (its bridge's error without a provider)
    RaiseText(&'static str),
}

/// One member: its dotted name, its gate and its answer.
#[derive(Clone, Copy, Debug)]
pub struct Row {
    pub name: &'static str,
    pub gate: Gate,
    pub answer: Answer,
}

const fn row(name: &'static str, gate: Gate, answer: Answer) -> Row {
    Row { name, gate, answer }
}

const READ: Gate = Gate::Ops(&[Op::EcsRead]);
const WRITE: Gate = Gate::Ops(&[Op::EcsWrite]);
const C: Gate = Gate::Contract;
use Answer::*;

/// Every in-game member of ShroudForge 1.7.9's API (its frozen API inventory) a family may still be
/// missing, with ShroudForge's answer. The `runtime.functions` members are [`FUNCTION_ROWS`].
pub const ROWS: &[Row] = &[
    // runtime.ecs (the registry members are the ECS family's whenever the types are wired)
    row("runtime.ecs.get_catalog", C, NilReason),
    row("runtime.ecs.get_registry", C, NilReason),
    row("runtime.ecs.get_component", C, NilReason),
    // read_component_registry.rs:3-14: get_components refuses through runtime.ecs.query first
    row(
        "runtime.ecs.get_components",
        Gate::Ops(&[Op::EcsQuery]),
        NilReason,
    ),
    row("runtime.ecs.query", Gate::Ops(&[Op::EcsQuery]), NilReason),
    row(
        "runtime.ecs.query_bounds",
        Gate::Ops(&[Op::EcsQuery]),
        NilReason,
    ),
    row(
        "runtime.ecs.resolve",
        Gate::Ops(&[Op::EcsResolve]),
        NilReason,
    ),
    row("runtime.ecs.read", READ, NilReason),
    row("runtime.ecs.read_bytes", READ, NilReason),
    row("runtime.ecs.read_fields", READ, NilReason),
    row("runtime.ecs.get_template", READ, NilReason),
    row("runtime.ecs.get_template_component", READ, NilReason),
    row("runtime.ecs.write", WRITE, FalseReason),
    row("runtime.ecs.write_fields", WRITE, FalseReasonNil),
    row("runtime.ecs.read_attributes", READ, NilReason),
    row("runtime.ecs.read_attribute", READ, NilReason),
    row("runtime.ecs.update_attribute", WRITE, NilReason),
    row("runtime.ecs.write_attribute_storage", WRITE, NilReason),
    row("runtime.ecs.get_attributes", C, NilReason),
    row("runtime.ecs.get_attribute", C, NilReason),
    row("runtime.ecs.evaluate_attributes", C, NilReason),
    // runtime.impact, runtime.services
    row("runtime.impact.call", READ, NilReason),
    row("runtime.services.get", READ, NilReason),
    // runtime.overrides: the claims and the target constructors (`new` is the tracker family's)
    row("runtime.overrides.get_component_claim", C, NilReason),
    row("runtime.overrides.get_resource_claim", C, NilReason),
    row("runtime.overrides.get_service_claim", C, NilReason),
    row("runtime.overrides.get_template_claim", C, NilReason),
    row("runtime.overrides.attribute", C, NilReason),
    row("runtime.overrides.component", C, NilReason),
    row("runtime.overrides.resource", C, NilReason),
    row("runtime.overrides.service", C, NilReason),
    row("runtime.overrides.template", C, NilReason),
    // runtime.values
    row("runtime.values.decode", C, NilReason),
    row("runtime.values.encode", C, NilReason),
    row("runtime.values.new", C, NilReason),
    row("runtime.values.normalize_field", C, NilReason),
    // runtime.world (the context members are the world context's, place/destroy/finish_building the placement family's)
    row(
        "runtime.world.building.submit",
        Gate::Ops(&[Op::WorldBuildingInput]),
        NilReason,
    ),
    row("runtime.world.building.status", C, Unknown),
    row("runtime.world.building.cancel", C, False),
    row(
        "runtime.world.cursor.get",
        Gate::Ops(&[Op::WorldCursorGet]),
        NilReason,
    ),
    row(
        "runtime.world.entity.query_props",
        Gate::Ops(&[Op::WorldEntityQueryProps]),
        NilReason,
    ),
    row(
        "runtime.world.entity.query_props_in_bounds",
        Gate::Ops(&[Op::WorldEntityQueryPropsInBounds]),
        NilReason,
    ),
    row(
        "runtime.world.entity.register_prop_recipes",
        Gate::Ops(&[Op::WorldEntityRegisterPropRecipes]),
        NilReason,
    ),
    row(
        "runtime.world.entity.get_transform",
        Gate::Ops(&[Op::WorldEntityGetTransform]),
        NilReason,
    ),
    row(
        "runtime.world.entity.set_scale",
        Gate::Ops(&[Op::WorldEntitySetScale]),
        FalseReason,
    ),
    row(
        "runtime.world.entity.spawn",
        Gate::Ops(&[Op::WorldEntitySpawn]),
        NilReason,
    ),
    row(
        "runtime.world.voxel.get_grid_spec",
        Gate::Ops(&[Op::WorldVoxelGridSpec]),
        NilReason,
    ),
    row(
        "runtime.world.voxel.read",
        Gate::Ops(&[Op::WorldVoxelRead]),
        NilReason,
    ),
    row(
        "runtime.world.voxel.write",
        Gate::Ops(&[Op::WorldVoxelWrite]),
        FalseReason,
    ),
    // game.assets' archive members in game: ShroudForge raises when a read fails
    row("game.assets.get_resource", C, Raise),
    row("game.assets.get_resource_parts", C, Raise),
    row("game.assets.get_resources_by_type", C, Raise),
    row("game.assets.get_all_resources", C, Raise),
    row("game.assets.get_resource_types", C, Raise),
    row("game.assets.create_resource", C, Raise),
    row("game.assets.get_content", C, Raise),
    row("game.assets.get_all_contents", C, Raise),
    row("game.assets.create_content", C, Raise),
    // game.assets' loaded-resource members (the in-game ResourceService): the two lookups raise the bridge's text when
    // its provider lacks the ResourceService (read_loaded_resources.rs:79-157), never "nothing loaded"
    row(
        "game.assets.get_loaded_resource",
        C,
        RaiseText(NO_RESOURCE_READ),
    ),
    row(
        "game.assets.get_loaded_resources_by_type",
        C,
        RaiseText(NO_RESOURCE_LISTING),
    ),
    row(
        "game.assets.get_loaded_resource_by_field",
        READ,
        NilReasonNil,
    ),
    row("game.assets.prepare_loaded_index", READ, FalseReasonNil),
    row("game.assets.resolve_loaded_resource", READ, NilReason),
    row("game.assets.read_loaded_field", C, NilReasonNil),
    row(
        "game.assets.write_loaded_field",
        Gate::Ops(&[Op::AssetsWrite, Op::EcsWrite]),
        NilReason,
    ),
    // loader.runtime in game: `loader.features.runtime.dll` is false beside it, as in a ShroudForge session
    // whose native manager is off (create_mod_session.rs:76-80, create_loader_api.rs:22-44)
    row("loader.runtime.register_dll", C, DllUnavailable),
    // shroudforge.input
    row("shroudforge.input.is_key_down", C, LegacyKeyDown),
    row("shroudforge.input.is_keybind_down", C, False),
    row("shroudforge.input.was_keybind_pressed", C, False),
    row("shroudforge.input.keybind_label", C, Nil),
];

/// The `runtime.functions` members besides `get_operations` (the installer's): ShroudForge's function catalog
/// answers `nil, reason` while its catalog cannot be read (`read_function_catalog.rs:403-470`,
/// `bind_profile_modifier.rs:12-18`).
pub const FUNCTION_ROWS: &[Row] = &[
    row("runtime.functions.list", C, NilReason),
    row("runtime.functions.list_native", C, NilReason),
    row("runtime.functions.find", C, NilReason),
    row("runtime.functions.get", C, NilReason),
    row("runtime.functions.get_native", C, NilReason),
    row("runtime.functions.bind", C, NilReason),
    row("runtime.functions.bind_modifier", C, NilReason),
];

/// ShroudForge's text of the removed `shroudforge.input.is_key_down` (create_mod_api.rs:335-341).
pub const LEGACY_KEY_DOWN: &str = concat!(
    "shroudforge.input.is_key_down was removed and is unsupported in ShroudForge 1.7.6. ",
    "Declare a control: \"keybind\" setting in extended.mod.json, assign it in the ",
    "Modloader Hotkey Manager under Compatibility > Hotkeys, and use ",
    "shroudforge.input.was_keybind_pressed(settingKey) for presses or ",
    "shroudforge.input.is_keybind_down(settingKey) while held."
);

/// ShroudForge's error of `game.assets.get_loaded_resource` without a ResourceService read (read_resource.rs:66-68;
/// check_resource_exists.rs:9-11 raises the same, so the lookup raises it).
pub const NO_RESOURCE_READ: &str = "native provider lacks the ResourceService read extension";

/// ShroudForge's error of `game.assets.get_loaded_resources_by_type` without a ResourceService listing
/// (find_resources_by_type.rs:7-9).
pub const NO_RESOURCE_LISTING: &str = "native provider lacks the ResourceService listing extension";

/// ShroudForge's text of `loader.runtime.register_dll` without its runtime DLL feature (create_loader_api.rs:36-40).
pub const DLL_UNAVAILABLE: &str = "EML runtime DLL support is unavailable in this phase";

/// ShroudForge's reason for an operation outside its contract (`compatibility/lib.rs:30-41`).
pub fn outside_contract(name: &str, game_build: &str) -> String {
    format!("operation {name} has not been verified for game build {game_build}")
}

/// Whom a member refuses for: the package's registrar host, or (a package without the `runtime` capability, which has
/// no host) its id.
#[derive(Clone)]
pub enum Caller {
    Host(Rc<dyn RuntimeHost>),
    NoRuntime { id: String, game_build: String },
}

impl Caller {
    fn game_build(&self) -> String {
        match self {
            Self::Host(host) => host.owner_context().game_build().to_owned(),
            Self::NoRuntime { game_build, .. } => game_build.clone(),
        }
    }
}

/// The reason `row` answers for `caller` now.
pub fn reason(row: &Row, caller: &Caller) -> String {
    match (row.gate, caller) {
        (Gate::Ops(ops), Caller::Host(host)) => ops
            .iter()
            .find_map(|op| runtime_denial_reason(host.as_ref(), *op))
            .unwrap_or_else(|| outside_contract(row.name, &caller.game_build())),
        // check_ingame_access.rs:400-405: in game, a package without the runtime capability is refused first
        (Gate::Ops(_), Caller::NoRuntime { id, .. }) => {
            format!("mod '{id}' requires capabilities: [\"runtime\"]")
        }
        (Gate::Contract, _) => outside_contract(row.name, &caller.game_build()),
    }
}

/// What `row` returns (or raises) for `caller` now.
pub fn answer(lua: &Lua, row: &Row, caller: &Caller) -> mlua::Result<MultiValue> {
    let text = |lua: &Lua| -> mlua::Result<Value> {
        Ok(Value::String(lua.create_string(reason(row, caller))?))
    };
    let values = match row.answer {
        NilReason => vec![Value::Nil, text(lua)?],
        FalseReason => vec![Value::Boolean(false), text(lua)?],
        NilReasonNil => vec![Value::Nil, text(lua)?, Value::Nil],
        FalseReasonNil => vec![Value::Boolean(false), text(lua)?, Value::Nil],
        Raise => return Err(LuaError::generic(reason(row, caller))),
        Nil => vec![Value::Nil],
        False => vec![Value::Boolean(false)],
        EmptyTable => vec![Value::Table(lua.create_table()?)],
        Unknown => vec![Value::String(lua.create_string("unknown")?)],
        LegacyKeyDown => return Err(mlua::Error::runtime(LEGACY_KEY_DOWN)),
        DllUnavailable => return Err(mlua::Error::external(DLL_UNAVAILABLE)),
        RaiseText(text) => return Err(LuaError::generic(text)),
    };
    Ok(MultiValue::from_iter(values))
}

fn overlaps(a: &str, b: &str) -> bool {
    let nested = |outer: &str, inner: &str| {
        inner.len() > outer.len()
            && inner.starts_with(outer)
            && inner.as_bytes()[outer.len()] == b'.'
    };
    a == b || nested(a, b) || nested(b, a)
}

/// The rows no family of `others` claims (a claim of the name, of a table holding it or of a name inside it).
pub fn unclaimed(others: &[Family]) -> Vec<&'static Row> {
    ROWS.iter()
        .filter(|row| {
            !others
                .iter()
                .any(|f| f.claims().iter().any(|claim| overlaps(claim, row.name)))
        })
        .collect()
}

/// Sets `row`'s function at its dotted path below `root` (`root` is the table of the path's first part), making
/// the tables on the way; a value already there stays.
fn fill(lua: &Lua, root: &Table, row: &'static Row, caller: &Caller) -> mlua::Result<()> {
    let mut parts: Vec<&str> = row.name.split('.').skip(1).collect();
    let last = parts.pop().expect("a member below a namespace");
    let mut table = root.clone();
    for part in parts {
        table = match table.raw_get::<Value>(part)? {
            Value::Table(next) => next,
            Value::Nil => {
                let next = lua.create_table()?;
                table.raw_set(part, &next)?;
                next
            }
            // something else holds the path: leave it as it is
            _ => return Ok(()),
        };
    }
    if !table.raw_get::<Value>(last)?.is_nil() {
        return Ok(());
    }
    let caller = caller.clone();
    table.raw_set(
        last,
        lua.create_function(move |lua, _: MultiValue| answer(lua, row, &caller))?,
    )
}

fn caller(package_id: &str, cx: &RuntimeContext) -> Caller {
    match cx.package_host() {
        Some(host) => Caller::Host(host),
        None => Caller::NoRuntime {
            id: package_id.to_owned(),
            game_build: cx.facts().config().game_build.clone(),
        },
    }
}

/// The field ShroudForge sets beside `loader.runtime`, filled with `loader.runtime.register_dll`'s row.
pub const DLL_FEATURE: &str = "loader.features.runtime";

/// The family that fills `rows` (the names no other family of the list claims) in each package's environment:
/// `runtime.*` only with the registrar's `runtime` table (a package without the capability has none), `game.*` in the
/// composition's `game` table, `shroudforge.*` in the package's `shroudforge` table, `loader.*` in its `loader`
/// table (with [`DLL_FEATURE`]'s `dll = false` when `register_dll` is filled).
pub fn family(rows: Vec<&'static Row>) -> Family {
    let dll = rows.iter().any(|row| row.answer == DllUnavailable);
    let claims: Vec<&'static str> = rows
        .iter()
        .map(|row| row.name)
        .chain(dll.then_some(DLL_FEATURE))
        .collect();
    Family::new(FAMILY, &claims, move |lua, _env, package, composed, cx| {
        let caller = caller(package.id(), cx);
        for row in &rows {
            let root = match row.name.split('.').next() {
                Some("runtime") => match &composed.runtime {
                    Some(runtime) => runtime.clone(),
                    None => continue,
                },
                Some("game") => composed.game.clone(),
                Some("shroudforge") => composed.bindings.shroudforge.clone(),
                Some("loader") => composed.bindings.loader.clone(),
                _ => continue,
            };
            fill(lua, &root, row, &caller)?;
        }
        let features = &composed.bindings.loader_features;
        if dll && features.raw_get::<Value>("runtime")?.is_nil() {
            let runtime = lua.create_table()?;
            runtime.raw_set("dll", false)?;
            features.raw_set("runtime", runtime)?;
        }
        Ok(())
    })
}

/// The installer's step 5 hook: [`FUNCTION_ROWS`] into `runtime.functions` (`get_operations` is already there).
pub fn install_function_members(
    lua: &Lua,
    runtime: &Table,
    host: &Rc<dyn RuntimeHost>,
) -> mlua::Result<()> {
    let caller = Caller::Host(host.clone());
    for row in FUNCTION_ROWS {
        fill(lua, runtime, row, &caller)?;
    }
    Ok(())
}
