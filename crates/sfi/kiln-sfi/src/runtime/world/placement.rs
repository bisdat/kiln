// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (byte-identical in both pins): modules/mod-runtime/lua/ingame/world/
// change_building_entities.rs:36-139 (place, the overloaded destroy, finish_building),
// lua/ingame/mod.rs:116-127 (lua_vec), game-runtime/bridge/windows/building/
// apply_entity_placement.rs, finish_building.rs, format_operation_error.rs:1-23,
// native/building/place_building_prop.cpp:34-38, finish_building.cpp:23-24,
// remove_building_prop.cpp:131-134, native/engine-context/track_world_context.cpp:181-207,
// native/profiles/read_game_profile.cpp:141-206 (worldOperations).
//! `runtime.world.entity.place`, `.finish_building` and the spatial form of
//! `.destroy`, refusal-only on every current profile.
//!
//! **Why refusal-only.** ShroudForge places and finishes buildings through native
//! calls its build profile's `worldOperations` must resolve. No profile of a build from
//! 25966634 on has that section, in either pin (the 6 client and 5 server profiles; only the
//! old 1076226 and 1024233 have one), so `OperationAvailable(place / finish_building)` is
//! false and the native call returns false before any dispatch with the outcome the bridge
//! preset (1): ShroudForge answers `<op> was rejected before dispatch (profile, arguments, or
//! live context unavailable)`. The spatial destroy needs `finish_building` too
//! (`remove_building_prop.cpp:131-134`), so it is refused the same way. Kiln answers that
//! text after ShroudForge's own denial, arity and argument checks, in ShroudForge's order,
//! without any Kiln member: these functions get the package's registrar host and their
//! arguments only, never the service adapter.
//!
//! **The catalog** (`operation_catalog`): `place`, `finish_building` are in ShroudForge's
//! contract on every build (`runtime-operations.json`), so their rows are
//! [`Backing::Interpreter`](super::super::operation_catalog::Backing::Interpreter) and
//! their readiness facts (`world_operation_available`, `world_entity_context_ready`, which
//! needs place and finish_building available) have no row: false, as on a current profile.
//!
//! **The overloaded destroy.** One argument is the handle form (a
//! [`HandleDestroy`] backend); any other count is the spatial form, refused here. Until a
//! handle backend is given, the handle form answers, after the denial and its argument,
//! the reason of an operation outside the contract (`destroy`'s own catalog row says the
//! same until a spawn and destroy family lands).

use crate::runtime::install::Family;
use mlua::{Lua, Table};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::{FunctionArgs, LuaError};
use std::rc::Rc;

/// The runtime family's name.
pub const FAMILY: &str = "world.placement";

/// The names it adds inside the installer's `runtime.world.entity` container.
pub const CLAIMS: [&str; 3] = [
    "runtime.world.entity.place",
    "runtime.world.entity.destroy",
    "runtime.world.entity.finish_building",
];

/// `(ok, reason)` as ShroudForge returns it: two values, `reason` nil on success.
pub type Outcome = (bool, Option<String>);

/// The handle form of `runtime.world.entity.destroy`: called with the converted
/// handle after the denial.
pub type HandleDestroy = Rc<dyn Fn(&Lua, u32) -> mlua::Result<Outcome>>;

/// ShroudForge's error for a world entity operation whose native call returned false
/// before dispatch (`format_operation_error.rs`, outcome 1, the bridge's preset).
pub fn rejected_before_dispatch(operation: Op) -> String {
    format!(
        "{} was rejected before dispatch (profile, arguments, or live context unavailable)",
        operation.as_str()
    )
}

/// `lua_vec::<N>`: exactly N array entries, each converted as an f64.
fn lua_vec<const N: usize>(table: &Table, label: &str) -> mlua::Result<[f64; N]> {
    if table.raw_len() != N {
        return Err(LuaError::generic(format!(
            "{label} must contain exactly {N} numbers"
        )));
    }
    let mut values = [0.0; N];
    for (index, value) in values.iter_mut().enumerate() {
        *value = table.raw_get(index + 1)?;
    }
    Ok(values)
}

/// `lua_world_entity_placement`: the operation's denial, then finish_building's, then the
/// arity, then each argument in order; then the native refusal.
fn placement(host: &dyn RuntimeHost, args: &FunctionArgs, destroy: bool) -> mlua::Result<Outcome> {
    let operation = if destroy {
        Op::WorldEntityDestroy
    } else {
        Op::WorldEntityPlace
    };
    if let Some(reason) = runtime_denial_reason(host, operation) {
        return Ok((false, Some(reason)));
    }
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityFinishBuilding) {
        return Ok((false, Some(reason)));
    }
    if args.len() != 5 {
        return Err(LuaError::generic(
            "runtime.world.entity.place/destroy expects position[3], rotation[4], bounds[6], trackingId, feedbackId",
        ));
    }
    lua_vec::<3>(args.get::<Table>(0)?, "position")?;
    lua_vec::<4>(args.get::<Table>(1)?, "rotation")?;
    lua_vec::<6>(args.get::<Table>(2)?, "bounds")?;
    args.get::<u32>(3)?;
    args.get::<u32>(4)?;
    Ok((false, Some(rejected_before_dispatch(operation))))
}

/// `runtime.world.entity.place(position, rotation, bounds, trackingId, feedbackId)`.
pub fn place(host: &dyn RuntimeHost, args: &FunctionArgs) -> mlua::Result<Outcome> {
    placement(host, args, false)
}

/// `runtime.world.entity.destroy`: one argument is the handle form, any other count the
/// spatial form.
pub fn destroy(
    lua: &Lua,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
    handle: Option<&HandleDestroy>,
) -> mlua::Result<Outcome> {
    if args.len() != 1 {
        return placement(host, args, true);
    }
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityDestroy) {
        return Ok((false, Some(reason)));
    }
    let id = args.get::<u32>(0)?;
    match handle {
        Some(backend) => backend(lua, id),
        None => Ok((
            false,
            Some(format!(
                "operation {} has not been verified for game build {}",
                Op::WorldEntityDestroy.as_str(),
                host.owner_context().game_build()
            )),
        )),
    }
}

/// `runtime.world.entity.finish_building(complete)`.
pub fn finish_building(host: &dyn RuntimeHost, args: &FunctionArgs) -> mlua::Result<Outcome> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityFinishBuilding) {
        return Ok((false, Some(reason)));
    }
    if args.len() != 1 {
        return Err(LuaError::generic(
            "runtime.world.entity.finish_building expects complete:boolean",
        ));
    }
    args.get::<bool>(0)?;
    Ok((
        false,
        Some(rejected_before_dispatch(Op::WorldEntityFinishBuilding)),
    ))
}

/// The family: the three functions in each package's `runtime.world.entity`, over that
/// package's own registrar host. `handle` is the handle form of destroy.
pub fn family(handle: Option<HandleDestroy>) -> Family {
    Family::new(FAMILY, &CLAIMS, move |lua, _, _, composed, cx| {
        let Some(runtime) = &composed.runtime else {
            return Ok(());
        };
        let host = cx.package_host().ok_or_else(|| {
            mlua::Error::runtime("runtime.world.entity needs the package's runtime host")
        })?;
        let entity: Table = runtime.raw_get::<Table>("world")?.raw_get("entity")?;
        let h = host.clone();
        entity.raw_set(
            "place",
            lua.create_function(move |_, args: FunctionArgs| place(h.as_ref(), &args))?,
        )?;
        let h = host.clone();
        let backend = handle.clone();
        entity.raw_set(
            "destroy",
            lua.create_function(move |lua, args: FunctionArgs| {
                destroy(lua, h.as_ref(), &args, backend.as_ref())
            })?,
        )?;
        entity.raw_set(
            "finish_building",
            lua.create_function(move |_, args: FunctionArgs| {
                finish_building(host.as_ref(), &args)
            })?,
        )
    })
}
