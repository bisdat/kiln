// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/query_entities.rs:3-99 (query, query_bounds),
// ecs/resolve_entity.rs:3-20 (resolve); game-runtime/bridge/windows/ecs/{query_entities.rs:4-45,
// query_entities_in_bounds.rs:4-42,resolve_entity.rs:3-7} (the provider's texts and limits);
// game-runtime/native/ecs/query_entities_in_bounds.cpp:4-80 (what is inside the bounds), resolve_entity_handle.cpp:
// 196-212 (resolve: no id 0, only while ready).
//! `runtime.ecs.query`, `query_bounds` and `resolve`.
//!
//! Each follows its ShroudForge source line by line (the denial, the argument rules and errors, readiness, each
//! component resolved by the registry join), on kiln.world:
//! - **query**: kiln.world's `ecs_query` over the components' hashes in the family's world, a complete result in
//!   ascending entity id (a membership change during the walk is read again, within ShroudForge's three tries), each
//!   ref given back as its handle. ShroudForge's incremental scan can answer "still scanning"; Kiln's query is complete
//!   or refused, so that answer is never given.
//! - **query_bounds**: the same query, then the bounds filter of ShroudForge's provider on each match's
//!   `CurrentTransform` (a stable copy each): its position, in metres, must lie in `[min - margin, max + margin)` on
//!   every axis, with `margin = padding * max(|scale|)`. `CurrentTransform` must be one of the components and every
//!   minimum below its maximum; a transform that cannot be read, or whose scale is not finite, is not a match.
//! - **resolve**: kiln.world's `entity_open` of the id in the live session, as its handle; id 0, a world that is not
//!   ready and every refusal are ShroudForge's "live ECS entity unavailable: <id>".

use super::catalog::text::NOT_READY;
use super::{Ecs, runtime_type_name};
use crate::service_member;
use kiln_abi::services::kiln_world::{
    KILN_WORLD_ERR_CHANGED, KilnEcsQueryRequest, KilnObjectRef, KilnWorldApi,
};
use kiln_abi::{KILN_ERR_BUFFER_TOO_SMALL, KILN_OK, KilnVec3};
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op};
use sfi_core::lua::{FunctionArgs, LuaError};

/// ShroudForge's texts.
pub mod text {
    /// query_entities.rs:13-15.
    pub const QUERY_NEEDS_TYPE: &str = "runtime.ecs.query requires at least one keen::ecs type";
    /// query_entities.rs:54-56.
    pub const BOUNDS_ARGUMENTS: &str =
        "runtime.ecs.query_bounds requires bounds[6], padding, and at least one keen::ecs type";
    /// query_entities.rs:63.
    pub const PADDING: &str = "padding must be a finite non-negative number";
    /// query_entities.rs:72.
    pub const BOUNDS: &str = "bounds must contain six finite numbers";
    /// query_entities.rs:28 (and every read's), followed by the type's name.
    pub const COMPONENT_UNAVAILABLE: &str = "live ECS component unavailable: ";
    /// resolve_entity.rs:17, followed by the id.
    pub const ENTITY_UNAVAILABLE: &str = "live ECS entity unavailable: ";
    /// The bridge without its provider (query_entities.rs:7).
    pub const PROVIDER_UNAVAILABLE: &str = "native ECS provider unavailable";
    /// query_entities.rs:30.
    pub const QUERY_FAILED: &str = "native ECS query failed or timed out";
    /// query_entities.rs:40.
    pub const QUERY_COUNT: &str = "live ECS query returned an invalid entity count";
    /// query_entities.rs:44.
    pub const QUERY_UNSTABLE: &str = "live ECS query buffer did not stabilize";
    /// query_entities_in_bounds.rs:35.
    pub const SPATIAL_FAILED: &str = "native spatial ECS query failed or timed out";
    /// query_entities_in_bounds.rs:38.
    pub const SPATIAL_LIMIT: &str = "native spatial ECS query exceeded the entity safety limit";
}

/// The most entities a query gives (the provider's limit, query_entities.rs:39).
pub const MAX_ENTITIES: usize = 1 << 20;
/// The first buffer of a query (query_entities.rs:19: most player queries fit in one call).
const FIRST_CAPACITY: usize = 256;
/// The transform the bounds are tested on, and its layout (query_entities_in_bounds.cpp:14-15,36-37): three 32.32
/// fixed-point positions, a quaternion, three scales and padding, 0x38 bytes.
pub const TRANSFORM: &str = "keen::ecs::CurrentTransform";
pub const TRANSFORM_SIZE: u32 = 0x38;

type Reply = (Value, Option<String>);

fn refused(reason: impl Into<String>) -> mlua::Result<Reply> {
    Ok((Value::Nil, Some(reason.into())))
}

impl Ecs {
    /// kiln.world's `ecs_query` for entities of the family's world that have every component of `components`
    /// (hashes): the refs in ascending entity id, or ShroudForge's provider text.
    pub fn query_refs(&self, components: &[u32]) -> Result<Vec<KilnObjectRef>, &'static str> {
        if let Some(reason) = self.scene_error() {
            return Err(reason);
        }
        let query = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, ecs_query))
            .map_err(|_| text::PROVIDER_UNAVAILABLE)?;
        if components.is_empty() {
            return Err(text::QUERY_FAILED);
        }
        let request = KilnEcsQueryRequest {
            size: std::mem::size_of::<KilnEcsQueryRequest>() as u32,
            world_kind: self.world_kind(),
            flags: 0,
            component_count: components.len() as u32,
            components: components.as_ptr(),
            center: KilnVec3 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            radius: 0.0,
        };
        // SAFETY: an all-zero ref is valid (integers only).
        let zero: KilnObjectRef = unsafe { std::mem::zeroed() };
        let mut refs = vec![zero; FIRST_CAPACITY];
        for _ in 0..3 {
            let (mut required, mut state) = (0u32, 0u32);
            // SAFETY: kiln_world.h ecs_query: a sized request whose component list lives through the call, and a
            // buffer of `refs.len()` refs.
            let result = unsafe {
                self.services.call(
                    query,
                    (
                        self.owner(),
                        &raw const request,
                        refs.as_mut_ptr(),
                        refs.len() as u32,
                        &raw mut required,
                        &raw mut state,
                    ),
                )
            };
            match result {
                KILN_OK if (required as usize) <= refs.len() => {
                    refs.truncate(required as usize);
                    return Ok(refs);
                }
                KILN_ERR_BUFFER_TOO_SMALL => {
                    if required as usize > MAX_ENTITIES {
                        return Err(text::QUERY_COUNT);
                    }
                    refs = vec![zero; required as usize];
                }
                // the membership changed during the walk: nothing was copied, read again
                KILN_WORLD_ERR_CHANGED => {}
                _ => return Err(text::QUERY_FAILED),
            }
        }
        Err(text::QUERY_UNSTABLE)
    }
}

/// The component hashes of arguments `first..`, or the refusal of the first one the join does not resolve (with its
/// name); a selector that does not resolve, or no `keen::ecs::` type, is raised.
fn components(
    ecs: &Ecs,
    args: &FunctionArgs,
    first: usize,
) -> mlua::Result<Result<Vec<(String, super::registry::Resolved)>, String>> {
    let mut components = Vec::with_capacity(args.len().saturating_sub(first));
    for index in first..args.len() {
        let name = runtime_type_name(ecs, args, index)?;
        let Some(resolved) = ecs.resolve_component(&name) else {
            return Ok(Err(format!("{}{name}", text::COMPONENT_UNAVAILABLE)));
        };
        components.push((name, resolved));
    }
    Ok(Ok(components))
}

/// The handles of `refs` as a Lua array; `None` when the handle space is used up.
fn handle_table(lua: &Lua, ecs: &Ecs, refs: &[KilnObjectRef]) -> mlua::Result<Option<Table>> {
    let result = lua.create_table_with_capacity(refs.len(), 0)?;
    for (index, object) in refs.iter().enumerate() {
        match ecs.handle_for(object) {
            0 => return Ok(None),
            handle => result.raw_set(index + 1, handle)?,
        }
    }
    Ok(Some(result))
}

/// `runtime.ecs.query(...)` (query_entities.rs:3-42).
pub fn query(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = ecs.denial_reason(host, Op::EcsQuery) {
        return refused(reason);
    }
    if args.is_empty() {
        return Err(LuaError::generic(text::QUERY_NEEDS_TYPE));
    }
    if !host.provider_facts().ready {
        return refused(NOT_READY);
    }
    let components = match components(ecs, args, 0)? {
        Ok(components) => components,
        Err(reason) => return refused(reason),
    };
    let hashes: Vec<u32> = components.iter().map(|(_, c)| c.component).collect();
    match ecs.query_refs(&hashes) {
        Ok(refs) => match handle_table(lua, ecs, &refs)? {
            Some(table) => Ok((Value::Table(table), None)),
            None => refused(text::QUERY_FAILED),
        },
        Err(reason) => refused(reason),
    }
}

/// One match's transform, as the provider reads it: its position in metres and its largest absolute scale.
fn transform(bytes: &[u8]) -> Option<([f64; 3], f64)> {
    let word = |at: usize| -> Option<i64> {
        Some(i64::from_le_bytes(bytes.get(at..at + 8)?.try_into().ok()?))
    };
    let float = |at: usize| -> Option<f64> {
        Some(f32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?).into())
    };
    let position: [Option<i64>; 3] = [word(0), word(8), word(16)];
    let mut metres = [0.0f64; 3];
    for (axis, value) in position.into_iter().enumerate() {
        metres[axis] = value? as f64 / 4_294_967_296.0;
    }
    let scale = [float(40)?, float(44)?, float(48)?]
        .into_iter()
        .map(f64::abs)
        .fold(f64::NEG_INFINITY, f64::max);
    Some((metres, scale))
}

/// The provider's bounds query (query_entities_in_bounds.cpp:4-80) over the resolved components.
fn query_in_bounds(
    ecs: &Ecs,
    components: &[(String, super::registry::Resolved)],
    bounds: [f64; 6],
    padding: f64,
) -> Result<Vec<KilnObjectRef>, &'static str> {
    let transform_type = components
        .iter()
        .find(|(name, _)| name == TRANSFORM)
        .map(|(_, resolved)| *resolved)
        .ok_or(text::SPATIAL_FAILED)?;
    if transform_type.size != TRANSFORM_SIZE || (0..3).any(|axis| bounds[axis] >= bounds[axis + 3])
    {
        return Err(text::SPATIAL_FAILED);
    }
    let hashes: Vec<u32> = components.iter().map(|(_, c)| c.component).collect();
    let refs = ecs.query_refs(&hashes).map_err(|reason| match reason {
        text::PROVIDER_UNAVAILABLE => reason,
        _ => text::SPATIAL_FAILED,
    })?;
    let mut inside = Vec::new();
    for object in refs {
        let Some(bytes) = ecs.snapshot_ref(&object, transform_type.component, TRANSFORM_SIZE)
        else {
            continue;
        };
        let Some((position, scale)) = transform(&bytes) else {
            continue;
        };
        if !scale.is_finite() {
            continue;
        }
        let margin = padding * scale;
        let within = (0..3).all(|axis| {
            let at = position[axis];
            at.is_finite() && at >= bounds[axis] - margin && at < bounds[axis + 3] + margin
        });
        if within {
            if inside.len() >= MAX_ENTITIES {
                return Err(text::SPATIAL_LIMIT);
            }
            inside.push(object);
        }
    }
    Ok(inside)
}

/// `runtime.ecs.query_bounds(bounds, padding, ...)` (query_entities.rs:44-99).
pub fn query_bounds(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = ecs.denial_reason(host, Op::EcsQuery) {
        return refused(reason);
    }
    if args.len() < 3 {
        return Err(LuaError::generic(text::BOUNDS_ARGUMENTS));
    }
    let bounds_table = args.get::<Table>(0)?;
    let padding = args.get::<f64>(1)?;
    if !padding.is_finite() || padding < 0.0 {
        return refused(text::PADDING);
    }
    let mut bounds = [0.0f64; 6];
    for (index, value) in bounds.iter_mut().enumerate() {
        *value = bounds_table.raw_get::<f64>(index + 1)?;
        if !value.is_finite() {
            return refused(text::BOUNDS);
        }
    }
    if !host.provider_facts().ready {
        return refused(NOT_READY);
    }
    let components = match components(ecs, args, 2)? {
        Ok(components) => components,
        Err(reason) => return refused(reason),
    };
    match query_in_bounds(ecs, &components, bounds, padding) {
        Ok(refs) => match handle_table(lua, ecs, &refs)? {
            Some(table) => Ok((Value::Table(table), None)),
            None => refused(text::SPATIAL_FAILED),
        },
        Err(reason) => refused(reason),
    }
}

/// `runtime.ecs.resolve(entity_id)` (resolve_entity.rs:3-20).
pub fn resolve(
    _: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = ecs.denial_reason(host, Op::EcsResolve) {
        return refused(reason);
    }
    let entity = args.get::<u32>(0)?;
    // the provider answers no handle for id 0 or while it is not ready (resolve_entity_handle.cpp:198-201)
    if entity != 0
        && host.provider_facts().ready
        && let Ok(handle) = ecs.open(entity)
    {
        return Ok((Value::Integer(i64::from(handle)), None));
    }
    refused(format!("{}{entity}", text::ENTITY_UNAVAILABLE))
}
