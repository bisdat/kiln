// SPDX-License-Identifier: MIT
// the interpreter's runtime.world.voxel family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9, the pin the interpreter targets): modules/mod-runtime/lua/ingame/world/read_write_voxels.rs:3-40
// (lua_world_grid_get_spec), world/mod.rs:32-36 (the voxel table), lua/ingame/check_ingame_access.rs:93-98,264-265
// (grid_spec's readiness is the read operation's), game-runtime/bridge/windows/voxels/read_grid_spec.rs:6-56
// (world_grid_get_spec: the provider, the id, the session before and after, the status texts, the metadata checks),
// native/voxels/read_registered_voxel_grid.cpp:394-435 (GetSpecForSession: its statuses), :373-388 (Available).
//! `runtime.world.voxel.get_grid_spec` on kiln.build's `grid_for_session`.
//!
//! **What it answers**, in ShroudForge's order:
//! 1. the denial of `runtime.world.voxel.grid_spec` (`nil, reason`): phase, permission, lifecycle, the contract;
//! 2. any argument count but one raises ShroudForge's arity error; the id converts as a string (its argument rules);
//! 3. the provider (`nil, reason` with ShroudForge's bridge texts): kiln.build without the member is "native registered
//!    grid metadata is unavailable"; an id with a NUL, "voxel grid id contains a null byte"; no session,
//!    "world-session-changed"; then the call, the session again ("world-session-changed" when it moved), and the
//!    member's result as ShroudForge's native status: busy or unreadable is "voxel-grid-pending" (ShroudForge's Pending,
//!    which it answers whenever its snapshot does not check out), an unknown id or a bad argument "voxel grid rejected
//!    its id or buffer contract", a stale session "world-session-changed", unavailable "original registered voxel grid
//!    contract is unavailable";
//! 4. the metadata checked as ShroudForge checks it (the id NUL-terminated and the one asked for, a finite origin, cell
//!    sizes finite and positive, no zero maximum), else "native voxel grid returned invalid metadata";
//! 5. the table: `id`, `origin`, `cellSize` and `maximum`, each axis under 1..3 and x/y/z.
//!
//! **The session.** ShroudForge's world session is its live ECS world's, which must be the local authority. Kiln's
//! voxel grid is the server world's (a dedicated server, or a client while it hosts), so the call names the server
//! world's session (`kiln.world.game_world_session(KILN_WORLD_SERVER)`) while the live world context is "direct". A
//! client joined to another machine's world has no local authority: ShroudForge's session guard refuses there
//! (status 5), and so does this family, without a call; an id other than "voxel" is rejected first there (its id check
//! comes before the guard), and so here. Without a live context the session is 0.
//!
//! **The caller.** The member is called with kiln-sfi's own plugin owner, the ShroudForge-dialect owner the core
//! designates: the engine gate never latches it after a busy refusal, so a mod that asks again in the same step is
//! not refused for another caller's (or its own earlier) busy answer, as under ShroudForge.
//!
//! **The catalog** (`operation_catalog`): grid_spec is backed by the member and the kiln.caps row
//! `build.voxel_grid_tasks`, and so is `world_operation_available("runtime.world.voxel.read")`, the fact ShroudForge's
//! grid_spec readiness reads (ShroudForge's own value is a live check of the same grid; Kiln's is the row, and a call
//! while the grid is not ready answers "voxel-grid-pending"). The read operation itself stays outside the contract
//! until its member lands, so `runtime.world.voxel.read` and `.write` keep ShroudForge's refusals.

use crate::runtime::availability::RuntimeFacts;
use crate::runtime::install::Family;
use crate::runtime::services::{ServiceAdapter, ServiceError};
use crate::runtime::world::context::ContextKind;
use crate::service_member;
use kiln_abi::services::kiln_build::{
    KILN_BUILD_SERVICE, KILN_BUILD_VERSION, KilnBuildApi, KilnBuildGridSpec,
};
use kiln_abi::services::kiln_world::{
    KILN_ECS_ERR_BUSY, KILN_ECS_ERR_STALE, KILN_ECS_ERR_UNREADABLE,
};
use kiln_abi::{
    KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_UNAVAILABLE, KILN_OK,
    KILN_WORLD_SERVER, KilnOwner, KilnStr,
};
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::{FunctionArgs, LuaError};
use std::rc::Rc;

/// The runtime family's name.
pub const FAMILY: &str = "world.voxels";

/// The names it adds inside the installer's `runtime.world.voxel` container.
pub const CLAIMS: [&str; 1] = ["runtime.world.voxel.get_grid_spec"];

/// The kiln.caps row of kiln.build's task-thread grid member.
pub const GRID_CAPABILITY: &str = "build.voxel_grid_tasks";

/// ShroudForge's arity error (read_write_voxels.rs:13-16).
pub const GRID_ARITY: &str =
    "runtime.world.voxel.get_grid_spec expects a grid id, currently 'voxel'";
/// ShroudForge's bridge texts (read_grid_spec.rs:7-52).
pub const NO_GRID_METADATA: &str = "native registered grid metadata is unavailable";
pub const NUL_ID: &str = "voxel grid id contains a null byte";
pub const SESSION_CHANGED: &str = "world-session-changed";
pub const GRID_PENDING: &str = "voxel-grid-pending";
pub const GRID_REJECTED: &str = "voxel grid rejected its id or buffer contract";
pub const GRID_UNAVAILABLE: &str = "original registered voxel grid contract is unavailable";
pub const GRID_UNKNOWN: &str = "native voxel grid returned an unknown status";
pub const GRID_INVALID: &str = "native voxel grid returned invalid metadata";

/// The grid member: kiln.build's `grid_for_session` (presence only).
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnBuildApi>(KILN_BUILD_SERVICE, KILN_BUILD_VERSION)?;
    service_member!(table, KilnBuildApi, grid_for_session).map(drop)
}

/// A grid's metadata as ShroudForge's bridge returns it (`GridSpecResult`).
#[derive(Clone, Debug, PartialEq)]
pub struct GridSpec {
    pub id: String,
    pub origin: [f64; 3],
    pub cell_size: [f64; 3],
    pub maximum: [u64; 3],
}

/// The session the grid member is asked for: the server world's while the live context is direct (see the module docs).
fn server_session(facts: &RuntimeFacts) -> Result<u64, &'static str> {
    match facts.world_snapshot().kind {
        ContextKind::Unknown => Err(SESSION_CHANGED),
        ContextKind::ClientReadOnly => Err(GRID_UNAVAILABLE),
        ContextKind::Direct => match facts.world_session(KILN_WORLD_SERVER as u32) {
            0 => Err(SESSION_CHANGED),
            session => Ok(session),
        },
    }
}

/// ShroudForge's `world_grid_get_spec(id)` (read_grid_spec.rs:6-56) on kiln.build's `grid_for_session`.
pub fn grid_spec(
    services: &ServiceAdapter,
    facts: &RuntimeFacts,
    owner: KilnOwner,
    id: &str,
) -> Result<GridSpec, String> {
    let grid = services
        .bind::<KilnBuildApi>(KILN_BUILD_SERVICE, KILN_BUILD_VERSION)
        .and_then(|table| service_member!(table, KilnBuildApi, grid_for_session))
        .map_err(|_| NO_GRID_METADATA.to_owned())?;
    if id.as_bytes().contains(&0) {
        return Err(NUL_ID.into());
    }
    let session = server_session(facts).map_err(|reason| {
        // ShroudForge's native grid checks the id before its session guard (read_registered_voxel_grid.cpp:395).
        if reason == GRID_UNAVAILABLE && id != "voxel" {
            GRID_REJECTED
        } else {
            reason
        }
    })?;
    // SAFETY: plain C record; every field valid when zeroed.
    let mut spec: KilnBuildGridSpec = unsafe { std::mem::zeroed() };
    spec.size = std::mem::size_of::<KilnBuildGridSpec>() as u32;
    let text = KilnStr {
        ptr: id.as_ptr().cast(),
        len: id.len(),
    };
    // SAFETY: kiln_build.h grid_for_session: any thread but a world's own (the worker is none); the owner is ours, the
    // id is readable for len bytes and the record is ours, its size set.
    let status = unsafe { services.call(grid, (owner, session, text, &raw mut spec)) };
    if server_session(facts) != Ok(session) {
        return Err(SESSION_CHANGED.into());
    }
    match status {
        KILN_OK => {}
        KILN_ECS_ERR_BUSY | KILN_ECS_ERR_UNREADABLE => return Err(GRID_PENDING.into()),
        KILN_ERR_INVALID_ARGUMENT | KILN_ERR_NOT_FOUND => return Err(GRID_REJECTED.into()),
        KILN_ECS_ERR_STALE => return Err(SESSION_CHANGED.into()),
        KILN_ERR_UNAVAILABLE => return Err(GRID_UNAVAILABLE.into()),
        _ => return Err(GRID_UNKNOWN.into()),
    }
    let bytes: Vec<u8> = spec.id.iter().map(|c| *c as u8).collect();
    let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    let returned = String::from_utf8_lossy(&bytes[..end]).into_owned();
    if end == bytes.len()
        || returned != id
        || spec.origin.iter().any(|v| !v.is_finite())
        || spec.cell_size.iter().any(|v| !v.is_finite() || *v <= 0.)
        || spec.maximum.contains(&0)
    {
        return Err(GRID_INVALID.into());
    }
    Ok(GridSpec {
        id: returned,
        origin: spec.origin,
        cell_size: spec.cell_size,
        maximum: spec.maximum,
    })
}

/// The grid table (read_write_voxels.rs:20-35).
fn spec_table(lua: &Lua, spec: GridSpec) -> mlua::Result<Table> {
    let result = lua.create_table()?;
    result.raw_set("id", spec.id)?;
    for (field, values) in [("origin", spec.origin), ("cellSize", spec.cell_size)] {
        let value = lua.create_table()?;
        for (axis, (name, v)) in ["x", "y", "z"].into_iter().zip(values).enumerate() {
            value.raw_set(axis + 1, v)?;
            value.raw_set(name, v)?;
        }
        result.raw_set(field, value)?;
    }
    let maximum = lua.create_table()?;
    for (axis, (name, v)) in ["x", "y", "z"].into_iter().zip(spec.maximum).enumerate() {
        maximum.raw_set(axis + 1, v)?;
        maximum.raw_set(name, v)?;
    }
    result.raw_set("maximum", maximum)?;
    Ok(result)
}

/// `runtime.world.voxel.get_grid_spec(id)` (read_write_voxels.rs:3-40).
pub fn get_grid_spec(
    lua: &Lua,
    host: &dyn RuntimeHost,
    facts: &RuntimeFacts,
    services: &ServiceAdapter,
    owner: KilnOwner,
    args: &FunctionArgs,
) -> mlua::Result<(Value, Option<String>)> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldVoxelGridSpec) {
        return Ok((Value::Nil, Some(reason)));
    }
    if args.len() != 1 {
        return Err(LuaError::generic(GRID_ARITY));
    }
    let id = args.get::<String>(0)?;
    match grid_spec(services, facts, owner, &id) {
        Ok(spec) => Ok((Value::Table(spec_table(lua, spec)?), None)),
        Err(reason) => Ok((Value::Nil, Some(reason))),
    }
}

/// Puts the family's functions into one package's `runtime.world.voxel` table; `owner` is kiln-sfi's plugin owner
/// (`RuntimeContext::owner`), the member's caller.
pub fn install_into(
    lua: &Lua,
    voxel: &Table,
    host: Rc<dyn RuntimeHost>,
    facts: Rc<RuntimeFacts>,
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
) -> mlua::Result<()> {
    voxel.raw_set(
        "get_grid_spec",
        lua.create_function(move |lua, args: FunctionArgs| {
            get_grid_spec(lua, host.as_ref(), &facts, &services, owner, &args)
        })?,
    )
}

/// The production family. A package without a `runtime` namespace gets nothing.
pub fn family() -> Family {
    Family::new(FAMILY, &CLAIMS, |lua, _, _, composed, cx| {
        let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
            return Ok(());
        };
        let world: Table = runtime.raw_get("world")?;
        let voxel: Table = world.raw_get("voxel")?;
        install_into(
            lua,
            &voxel,
            host,
            cx.facts().clone(),
            cx.services().clone(),
            cx.owner(),
        )
    })
}
