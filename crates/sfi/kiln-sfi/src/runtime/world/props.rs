// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/world/{query_props.rs:3-99 (query_props,
// query_props_in_bounds), access_prop_transform.rs:2-70 (get_transform, set_scale), mod.rs:91-131 (prop_to_lua)};
// game-runtime/bridge/runtime_records.rs:8-17 (PropRecord), bridge/windows/props/{query_props.rs,
// query_props_in_bounds.rs, read_entity_transform.rs:12-38, read_prop_query_readiness.rs, set_entity_scale.rs} (the
// provider's texts and limits); game-runtime/native/ecs/{query_building_props.cpp:96-178,227-239 (the scan, the padding
// and recipe tests, readiness), read_building_prop.cpp:4-46 (the prop payload), :170-201 (the session read's
// outcomes)}.
//! `runtime.world.entity.query_props`, `query_props_in_bounds`, `get_transform` and `set_scale`.
//!
//! A prop is an entity of the world the shared ECS binds (a server's or a hosting client's server world, a joined
//! client's client world) with both `keen::ecs::CurrentTransform` (0x38 bytes: three Q32.32
//! positions, a quaternion, three scales) and `keen::ecs::UsedItem` (its item id, 4 bytes, not 0), each copied through
//! kiln.world:
//! - **the scan**: kiln.world's `ecs_query` on both components (complete, in ascending entity id, at most 1 << 20
//!   entities, else the query fails), then per match one `ecs_snapshot_many` of both (each validated before and after
//!   its copy). A match whose copy is refused, whose item id is 0 or whose rotation or scale is not finite is skipped,
//!   as ShroudForge's scan skips it. ShroudForge's sliced scan can answer "still scanning"; Kiln's query is complete or
//!   refused, so that answer is never given;
//! - **query_props**: a prop's position inside `[min - margin, max + margin)` on every axis, `margin = padding *
//!   max(|scale|)`. No template uuid (ShroudForge reads none here: its hex fields are zeros);
//! - **query_props_in_bounds**: the recipe catalog ([`super::recipes`]) must be ready; a prop whose item has no
//!   recipe is skipped, the recipe's box is rotated and scaled by the prop's transform and must meet the bounds, and
//!   only then is its template uuid read (`ecs_template_uuid`, a prop that names none is skipped);
//! - **handles**: every prop is given as the VM's shared entity handle (the ECS family's table, `ecs::of_vm`), so
//!   `runtime.ecs.read` and `write` take it and `get_transform` takes the handles `runtime.ecs.query` gives;
//! - **get_transform**: ShroudForge's session read: the handle's ref in the live session, its payload and template
//!   uuid, with that read's reasons (`entity-absent` for an entity that is gone, the stale and retry reasons);
//! - **set_scale**: the session write latch, then `CurrentTransform`'s write policy (kiln.world's
//!   `ecs_write_policy`: a tracked component is never writable from a task), the ownership check, and one
//!   compare-exchange of the three scale floats (the rest of the component must still be the bytes just read: a
//!   conflict is not tried again), whose every refusal is ShroudForge's "native entity scale update failed or timed
//!   out", as is a finite scale beyond a float.
//!
//! The readiness fact (`WorldEntityQueryPropsReady`, [`read_ready`]) is ECS readiness (the `Ready` row, read
//! before it); the components themselves are checked at each call, as ShroudForge's readiness does.

use super::recipes::{self, Recipes, lua_vec};
use crate::runtime::ecs::cas::Writer;
use crate::runtime::ecs::{Ecs, of_vm, probe_reads, probe_writes};
use crate::runtime::install::{Family, RuntimeHook};
use crate::runtime::overrides::{check_write, component_key};
use crate::runtime::services::{ServiceAdapter, ServiceError};
use crate::runtime::types::registry::fnv1a32;
use crate::service_member;
use kiln_abi::services::kiln_world::{
    KILN_ECS_ERR_STALE, KILN_ECS_ERR_UNREADABLE, KILN_ECS_OK, KILN_WORLD_ERR_CHANGED,
    KILN_WORLD_SERVICE, KILN_WORLD_VERSION, KilnEcsSnapshotSpec, KilnEcsWritePolicy, KilnObjectRef,
    KilnWorldApi,
};
use kiln_abi::{KILN_ERR_NOT_FOUND, KILN_OK, KILN_WORLD_SERVER, KilnResult};
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{
    RuntimeHost, RuntimeOperation as Op, RuntimeProviderFacts, runtime_denial_reason,
};
use sfi_core::lua::{FunctionArgs, LuaError};
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "world.props";

/// The names it adds inside the installer's `runtime.world.entity` container.
pub const CLAIMS: [&str; 5] = [
    "runtime.world.entity.query_props",
    "runtime.world.entity.query_props_in_bounds",
    "runtime.world.entity.register_prop_recipes",
    "runtime.world.entity.get_transform",
    "runtime.world.entity.set_scale",
];

/// A prop's transform and its layout (read_building_prop.cpp:24-27).
pub const TRANSFORM: &str = "keen::ecs::CurrentTransform";
pub const TRANSFORM_SIZE: u32 = 0x38;
/// A prop's item id.
pub const ITEM: &str = "keen::ecs::UsedItem";
pub const ITEM_SIZE: u32 = 4;
/// The most entities a scan walks (query_building_props.cpp:106).
pub const MAX_ENTITIES: usize = 1 << 20;
/// The most props one answer gives (query_props.rs:17).
pub const MAX_PROPS: usize = 1_000_000;
/// The scale floats of `CurrentTransform`.
const SCALE: std::ops::Range<usize> = 40..52;

/// ShroudForge's texts.
pub mod text {
    pub const QUERY_ARGUMENTS: &str =
        "runtime.world.entity.query_props expects bounds[6] and padding";
    pub const BOUNDS: &str = "bounds must contain six finite numbers";
    pub const PADDING: &str = "padding must be a finite non-negative number";
    pub const IN_BOUNDS_ARGUMENTS: &str =
        "runtime.world.entity.query_props_in_bounds expects bounds[6]";
    pub const MIN_MAX: &str = "bounds must contain finite minimum xyz and maximum xyz values";
    /// The bridge without its provider; the licence guard keeps the prefix out of MIT sources.
    pub const PROVIDER: &str = concat!("K", "FC Runtime provider unavailable");
    pub const QUERY_FAILED: &str = "native prop query failed";
    pub const RECIPE_QUERY_FAILED: &str = "native recipe-bounds prop query failed";
    pub const LIMIT: &str = "native prop query exceeded the 1,000,000-prop safety limit";
    pub const TRANSFORM_ARGUMENTS: &str =
        "runtime.world.entity.get_transform expects an entity handle";
    pub const ABSENT: &str = "entity-absent";
    pub const PROVIDER_READ: &str = "native-prop-provider-unavailable";
    pub const NO_SESSION: &str = "world-session-unavailable";
    pub const INVALID_HANDLE: &str = "invalid-entity-handle";
    pub const STALE: &str = "world-session-or-entity-handle-stale";
    pub const RETRY: &str = "entity-state-unavailable-retry";
    pub const UNCERTAIN: &str = "world-write-outcome-uncertain";
    pub const SCALE_ARGUMENTS: &str =
        "runtime.world.entity.set_scale expects an entity handle and scale[3]";
    pub const NO_WRITE_PHASE: &str =
        "CurrentTransform has no supported native component write phase";
    pub const SCALE_FINITE: &str = "entity scale must contain finite values";
    pub const SCALE_FAILED: &str = "native entity scale update failed or timed out";
}

/// ShroudForge's `PropRecord` (runtime_records.rs:8-17), with the template uuid as its 16 bytes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Prop {
    pub handle: u32,
    pub entity: u32,
    pub item: u32,
    pub position: [i64; 3],
    pub orientation: [f32; 4],
    pub scale: [f32; 3],
    pub template: [u8; 16],
}

/// The props' members (presence only): the ECS reads and `ecs_snapshot_many`.
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    probe_reads(services)?;
    let table = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, ecs_snapshot_many)?;
    Ok(())
}

/// `set_scale`'s members: the props' and the compare-exchange writes'.
pub fn probe_scale(services: &ServiceAdapter) -> Result<(), ServiceError> {
    probe(services)?;
    probe_writes(services)
}

/// `WorldEntityQueryPropsReady` (ShroudForge's native prop query readiness): ECS readiness, which the catalog's `Ready` row
/// (listed before this one) has just read.
pub fn read_ready(_: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.world_entity_query_props_ready = facts.ready;
}

/// `CanWriteCurrentTransform` (ShroudForge's `can_write_component("keen::ecs::CurrentTransform")`): the component
/// writes are possible (the `CanWrite` row, read before this one) and kiln.world's write policy says the server
/// world's `CurrentTransform` is writable from a task.
pub fn read_can_write_transform(services: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.can_write_current_transform = facts.can_write && transform_writable(services);
}

fn transform_writable(services: &ServiceAdapter) -> bool {
    let Ok(table) = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION) else {
        return false;
    };
    let Ok(policy) = service_member!(table, KilnWorldApi, ecs_write_policy) else {
        return false;
    };
    // SAFETY: an all-zero policy record is valid (integers and a char array).
    let mut out: KilnEcsWritePolicy = unsafe { std::mem::zeroed() };
    out.size = std::mem::size_of::<KilnEcsWritePolicy>() as u32;
    // SAFETY: plain values and a sized caller record.
    let result = unsafe {
        services.call(
            policy,
            (
                KILN_WORLD_SERVER as u32,
                fnv1a32(TRANSFORM.as_bytes()),
                &raw mut out,
            ),
        )
    };
    result == KILN_OK && out.status == KILN_ECS_OK as u32
}

/// The two components' hashes, when the registry join proves both at their layout.
fn components(ecs: &Ecs) -> Option<(u32, u32)> {
    let transform = ecs.resolve_component(TRANSFORM)?;
    let item = ecs.resolve_component(ITEM)?;
    (transform.size == TRANSFORM_SIZE && item.size == ITEM_SIZE)
        .then_some((transform.component, item.component))
}

/// Why a prop's payload could not be read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Unread {
    /// kiln.world's refusal of the copy.
    Refused(KilnResult),
    /// A copy that is not a prop (a component missing, item 0, a rotation or scale not finite).
    Invalid,
}

/// `read_prop_payload` (read_building_prop.cpp:24-46): both components in one `ecs_snapshot_many`.
fn payload(ecs: &Ecs, object: &KilnObjectRef, hashes: (u32, u32)) -> Result<Prop, Unread> {
    let many = ecs
        .world()
        .and_then(|table| service_member!(table, KilnWorldApi, ecs_snapshot_many))
        .map_err(|_| Unread::Refused(kiln_abi::KILN_ERR_UNAVAILABLE))?;
    let specs = [
        KilnEcsSnapshotSpec {
            component: hashes.0,
            flags: 0,
        },
        KilnEcsSnapshotSpec {
            component: hashes.1,
            flags: 0,
        },
    ];
    let size = (TRANSFORM_SIZE + ITEM_SIZE) as usize;
    let mut bytes = vec![0u8; size];
    let (mut required, mut presence) = (0u32, [0u32; 2]);
    // SAFETY: kiln_world.h ecs_snapshot_many: the caller's ref, two specs, a buffer of `size` bytes and two
    // presence slots.
    let result = unsafe {
        ecs.services().call(
            many,
            (
                ecs.owner(),
                std::ptr::from_ref(object),
                specs.as_ptr(),
                2u32,
                bytes.as_mut_ptr().cast::<core::ffi::c_void>(),
                size as u32,
                &raw mut required,
                presence.as_mut_ptr(),
            ),
        )
    };
    if result != KILN_OK {
        return Err(Unread::Refused(result));
    }
    if required as usize != size || presence.iter().any(|p| *p != KILN_ECS_OK as u32) {
        return Err(Unread::Invalid);
    }
    let word = |at: usize| i64::from_le_bytes(bytes[at..at + 8].try_into().unwrap());
    let float = |at: usize| f32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());
    let prop = Prop {
        handle: 0,
        entity: object.entity,
        item: u32::from_le_bytes(bytes[56..60].try_into().unwrap()),
        position: [word(0), word(8), word(16)],
        orientation: [float(24), float(28), float(32), float(36)],
        scale: [float(40), float(44), float(48)],
        template: [0; 16],
    };
    if prop.item == 0
        || !prop.orientation.iter().all(|v| v.is_finite())
        || !prop.scale.iter().all(|v| v.is_finite())
    {
        return Err(Unread::Invalid);
    }
    Ok(prop)
}

/// The template uuid of a ref's entity (`ecs_template_uuid`), when it names one.
fn template(ecs: &Ecs, object: &KilnObjectRef) -> Option<[u8; 16]> {
    let uuid = ecs
        .world()
        .and_then(|table| service_member!(table, KilnWorldApi, ecs_template_uuid))
        .ok()?;
    let mut guid = [0u8; 16];
    // SAFETY: kiln_world.h ecs_template_uuid: the caller's ref and 16 bytes.
    let result = unsafe {
        ecs.services().call(
            uuid,
            (ecs.owner(), std::ptr::from_ref(object), guid.as_mut_ptr()),
        )
    };
    (result == KILN_OK && guid != [0u8; 16]).then_some(guid)
}

/// What a scan keeps.
enum Filter<'a> {
    /// query_props: the position inside the padded bounds.
    Padded { bounds: [f64; 6], padding: f64 },
    /// query_props_in_bounds: the recipe's box meets the bounds, and the entity names a template.
    Recipes {
        bounds: [f64; 6],
        recipes: &'a std::collections::HashMap<u32, recipes::Recipe>,
    },
}

/// `query_props_native` (query_building_props.cpp:96-178): the props of the family's world the filter keeps, or the
/// provider's text (`failed` for each failure of the scan itself).
fn scan(ecs: &Ecs, filter: &Filter<'_>, failed: &'static str) -> Result<Vec<Prop>, &'static str> {
    let hashes = components(ecs).ok_or(failed)?;
    let refs = ecs.query_refs(&[hashes.0, hashes.1]).map_err(|reason| {
        if reason == crate::runtime::ecs::entities::text::PROVIDER_UNAVAILABLE {
            text::PROVIDER
        } else {
            failed
        }
    })?;
    if refs.len() > MAX_ENTITIES {
        return Err(failed);
    }
    let mut props = Vec::new();
    for object in refs {
        let Ok(mut prop) = payload(ecs, &object, hashes) else {
            continue;
        };
        match filter {
            Filter::Padded { bounds, padding } => {
                let scale = prop
                    .scale
                    .iter()
                    .map(|s| f64::from(*s).abs())
                    .fold(f64::NEG_INFINITY, f64::max);
                if !scale.is_finite() {
                    continue;
                }
                let margin = padding * scale;
                let inside = (0..3).all(|axis| {
                    let at = prop.position[axis] as f64 / 4_294_967_296.0;
                    at.is_finite() && at >= bounds[axis] - margin && at < bounds[axis + 3] + margin
                });
                if !inside {
                    continue;
                }
            }
            Filter::Recipes { bounds, recipes } => {
                let Some(recipe) = recipes.get(&prop.item) else {
                    continue;
                };
                if !recipes::intersects(
                    &prop.position,
                    &prop.orientation,
                    &prop.scale,
                    &recipe.bounds,
                    bounds,
                ) {
                    continue;
                }
                let Some(uuid) = template(ecs, &object) else {
                    continue;
                };
                prop.template = uuid;
            }
        }
        prop.handle = ecs.handle_for(&object);
        if prop.handle != 0 {
            props.push(prop);
        }
    }
    if props.len() > MAX_PROPS {
        return Err(text::LIMIT);
    }
    Ok(props)
}

/// `prop_to_lua` (world/mod.rs:91-131).
pub fn prop_to_lua(lua: &Lua, prop: &Prop) -> mlua::Result<Table> {
    let item = lua.create_table()?;
    item.raw_set("handle", prop.handle)?;
    item.raw_set("entityId", prop.entity)?;
    item.raw_set("itemId", prop.item)?;
    let high = u64::from_le_bytes(prop.template[..8].try_into().unwrap());
    let low = u64::from_le_bytes(prop.template[8..].try_into().unwrap());
    item.raw_set("templateUuidHighHex", format!("{high:016x}"))?;
    item.raw_set("templateUuidLowHex", format!("{low:016x}"))?;
    let names = ["x", "y", "z", "w"];
    let position = lua.create_table()?;
    for axis in 0..3 {
        let value = prop.position[axis] as f64 / 4_294_967_296.0;
        position.raw_set(axis + 1, value)?;
        position.raw_set(names[axis], value)?;
    }
    let orientation = lua.create_table()?;
    for axis in 0..4 {
        orientation.raw_set(axis + 1, prop.orientation[axis])?;
        orientation.raw_set(names[axis], prop.orientation[axis])?;
    }
    let scale = lua.create_table()?;
    for axis in 0..3 {
        scale.raw_set(axis + 1, prop.scale[axis])?;
        scale.raw_set(names[axis], prop.scale[axis])?;
    }
    let transform = lua.create_table()?;
    transform.raw_set("position", position)?;
    transform.raw_set("orientation", orientation)?;
    transform.raw_set("scale", scale)?;
    item.raw_set("transform", transform)?;
    Ok(item)
}

fn prop_array(lua: &Lua, props: &[Prop]) -> mlua::Result<Value> {
    let result = lua.create_table_with_capacity(props.len(), 0)?;
    for (index, prop) in props.iter().enumerate() {
        result.raw_set(index + 1, prop_to_lua(lua, prop)?)?;
    }
    Ok(Value::Table(result))
}

type Reply = (Value, Option<String>);

fn refused(reason: impl Into<String>) -> mlua::Result<Reply> {
    Ok((Value::Nil, Some(reason.into())))
}

/// `runtime.world.entity.query_props(bounds, padding)` (query_props.rs:3-44).
pub fn query_props(
    lua: &Lua,
    ecs: Option<&Ecs>,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityQueryProps) {
        return refused(reason);
    }
    if args.len() != 2 {
        return Err(LuaError::generic(text::QUERY_ARGUMENTS));
    }
    let table = args.get::<Table>(0)?;
    let mut bounds = [0.0f64; 6];
    for (index, value) in bounds.iter_mut().enumerate() {
        *value = table.raw_get::<f64>(index + 1)?;
        if !value.is_finite() {
            return refused(text::BOUNDS);
        }
    }
    let padding = args.get::<f64>(1)?;
    if !padding.is_finite() || padding < 0.0 {
        return refused(text::PADDING);
    }
    let Some(ecs) = ecs else {
        return refused(text::PROVIDER);
    };
    // the native entry point refuses empty bounds before any scan
    if (0..3).any(|axis| bounds[axis] >= bounds[axis + 3]) {
        return refused(text::QUERY_FAILED);
    }
    match scan(ecs, &Filter::Padded { bounds, padding }, text::QUERY_FAILED) {
        Ok(props) => Ok((prop_array(lua, &props)?, None)),
        Err(reason) => refused(reason),
    }
}

/// `runtime.world.entity.query_props_in_bounds(bounds)` (query_props.rs:46-99).
pub fn query_props_in_bounds(
    lua: &Lua,
    ecs: Option<&Ecs>,
    catalog: &Recipes,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityQueryPropsInBounds) {
        return refused(reason);
    }
    if args.len() != 1 {
        return Err(LuaError::generic(text::IN_BOUNDS_ARGUMENTS));
    }
    let bounds = lua_vec::<6>(args.get::<Table>(0)?, "bounds")?;
    if bounds.iter().any(|value| !value.is_finite())
        || (0..3).any(|axis| bounds[axis] >= bounds[axis + 3])
    {
        return refused(text::MIN_MAX);
    }
    let Some(ecs) = ecs else {
        return refused(text::PROVIDER);
    };
    let Some(recipes) = catalog.snapshot() else {
        return refused(text::RECIPE_QUERY_FAILED);
    };
    let filter = Filter::Recipes {
        bounds,
        recipes: &recipes,
    };
    match scan(ecs, &filter, text::RECIPE_QUERY_FAILED) {
        Ok(props) => Ok((prop_array(lua, &props)?, None)),
        Err(reason) => refused(reason),
    }
}

/// `world_entity_read_transform` (read_entity_transform.rs:12-38 with read_building_prop.cpp:170-201): the prop, None
/// for an entity that is gone, or the read's reason.
fn read_transform(
    ecs: Option<&Ecs>,
    host: &dyn RuntimeHost,
    handle: u32,
) -> Result<Option<Prop>, &'static str> {
    let ecs = ecs.ok_or(text::PROVIDER_READ)?;
    let session = ecs.world_session();
    if session == 0 {
        return Err(text::NO_SESSION);
    }
    if handle == 0 {
        return Err(text::INVALID_HANDLE);
    }
    if host.write_fault().is_some() {
        return Err(text::UNCERTAIN);
    }
    let object = ecs.object(handle).ok_or(text::STALE)?;
    if object.session != session {
        return Err(text::STALE);
    }
    let hashes = components(ecs).ok_or(text::RETRY)?;
    let read = payload(ecs, &object, hashes).and_then(|mut prop| {
        prop.template = template(ecs, &object).ok_or(Unread::Invalid)?;
        Ok(prop)
    });
    if ecs.world_session() != session {
        return Err(text::STALE);
    }
    if host.write_fault().is_some() {
        return Err(text::UNCERTAIN);
    }
    match read {
        Ok(mut prop) => {
            prop.handle = handle;
            Ok(Some(prop))
        }
        Err(Unread::Refused(KILN_ERR_NOT_FOUND)) => Ok(None),
        Err(Unread::Refused(KILN_ECS_ERR_STALE)) => Err(text::STALE),
        Err(Unread::Refused(KILN_ECS_ERR_UNREADABLE | KILN_WORLD_ERR_CHANGED))
        | Err(Unread::Invalid) => Err(text::RETRY),
        Err(Unread::Refused(_)) => Err(text::RETRY),
    }
}

/// `runtime.world.entity.get_transform(handle)` (access_prop_transform.rs:2-24).
pub fn get_transform(
    lua: &Lua,
    ecs: Option<&Ecs>,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityGetTransform) {
        return refused(reason);
    }
    if args.len() != 1 {
        return Err(LuaError::generic(text::TRANSFORM_ARGUMENTS));
    }
    let handle = args.get::<u32>(0)?;
    match read_transform(ecs, host, handle) {
        Ok(Some(prop)) => Ok((Value::Table(prop_to_lua(lua, &prop)?), None)),
        Ok(None) => refused(text::ABSENT),
        Err(reason) => refused(reason),
    }
}

/// `runtime.world.entity.set_scale(handle, scale)` (access_prop_transform.rs:26-70): `true` or `false, reason`.
pub fn set_scale(
    ecs: Option<&Ecs>,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(bool, Option<String>)> {
    let refused = |reason: String| Ok((false, Some(reason)));
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntitySetScale) {
        return refused(reason);
    }
    if args.len() != 2 {
        return Err(LuaError::generic(text::SCALE_ARGUMENTS));
    }
    if let Some(reason) = host.write_fault() {
        return refused(reason);
    }
    let transform = ecs.and_then(|ecs| {
        ecs.resolve_component(TRANSFORM)
            .filter(|t| t.size == TRANSFORM_SIZE && ecs.can_write_component(t.component))
    });
    let (Some(ecs), Some(transform)) = (ecs, transform) else {
        return refused(text::NO_WRITE_PHASE.into());
    };
    let handle = args.get::<u32>(0)?;
    let scale = lua_vec::<3>(args.get::<Table>(1)?, "scale")?;
    let writer = Writer {
        host,
        session: ecs.world_session(),
    };
    if let Some(runtime) = ecs.runtime() {
        let key = component_key(&writer, handle, TRANSFORM);
        if let Err(reason) = check_write(runtime.facts.ownership(), &writer, &key) {
            return refused(reason);
        }
    }
    // set_entity_scale.rs:9-11, then the native update's own range (set_building_prop_scale.cpp:11-15): a finite value
    // beyond a float is its refusal
    if scale.iter().any(|value| !value.is_finite()) {
        return refused(text::SCALE_FINITE.into());
    }
    if scale.iter().any(|value| value.abs() > f64::from(f32::MAX)) {
        return refused(text::SCALE_FAILED.into());
    }
    let scale = scale.map(|value| value as f32);
    let Some(before) = ecs.snapshot(handle, transform.component, TRANSFORM_SIZE) else {
        return refused(text::SCALE_FAILED.into());
    };
    let mut desired = before.clone();
    for (axis, value) in scale.iter().enumerate() {
        let at = SCALE.start + axis * 4;
        desired[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    let mut mask = vec![0u8; before.len()];
    mask[SCALE].fill(1);
    // the whole component is depended on: one compare-exchange, a conflict anywhere is not tried again
    let depends = vec![1u8; before.len()];
    match ecs.compare_exchange(
        handle,
        transform.component,
        &before,
        &mask,
        &desired,
        &depends,
    ) {
        Ok(()) => Ok((true, None)),
        // the native update answers one text for every refusal (set_entity_scale.rs:13-17); an uncertain outcome has
        // still latched the session's write fault
        Err(_) => refused(text::SCALE_FAILED.into()),
    }
}

/// Puts the five functions into one package's `runtime.world.entity` table, over the runtime-create's recipe catalog.
pub fn install_into(
    lua: &Lua,
    entity: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: super::EcsSource,
    catalog: Rc<Recipes>,
) -> mlua::Result<()> {
    let (h, e) = (host.clone(), ecs.clone());
    entity.raw_set(
        "query_props",
        lua.create_function(move |lua, args: FunctionArgs| {
            query_props(lua, e(lua).as_deref(), h.as_ref(), &args)
        })?,
    )?;
    let (h, e, c) = (host.clone(), ecs.clone(), catalog.clone());
    entity.raw_set(
        "query_props_in_bounds",
        lua.create_function(move |lua, args: FunctionArgs| {
            query_props_in_bounds(lua, e(lua).as_deref(), &c, h.as_ref(), &args)
        })?,
    )?;
    let h = host.clone();
    entity.raw_set(
        "register_prop_recipes",
        lua.create_function(move |lua, args: FunctionArgs| {
            recipes::register(lua, &catalog, h.as_ref(), &args)
        })?,
    )?;
    let (h, e) = (host.clone(), ecs.clone());
    entity.raw_set(
        "get_transform",
        lua.create_function(move |lua, args: FunctionArgs| {
            get_transform(lua, e(lua).as_deref(), h.as_ref(), &args)
        })?,
    )?;
    entity.raw_set(
        "set_scale",
        lua.create_function(move |lua, args: FunctionArgs| {
            set_scale(ecs(lua).as_deref(), host.as_ref(), &args)
        })?,
    )
}

impl RuntimeHook for Recipes {}

/// The production family: the functions on the VM's shared [`Ecs`] ([`of_vm`]) and one recipe catalog per
/// runtime-create (a live reload's VM keeps it, through the hook). A package without a `runtime` namespace gets
/// nothing.
pub fn family() -> Family {
    let catalog = Rc::new(Recipes::new());
    let hook: Rc<dyn RuntimeHook> = catalog.clone();
    Family::new(FAMILY, &CLAIMS, move |lua, _, _, composed, cx| {
        let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
            return Ok(());
        };
        let entity: Table = runtime.raw_get::<Table>("world")?.raw_get("entity")?;
        install_into(lua, &entity, host, Rc::new(of_vm), catalog.clone())
    })
    .with_hook(hook)
}
