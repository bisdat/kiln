// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.world` in the pregame VM (lua/ingame/world/mod.rs:14-97): every operation refuses with its own denial in
//! its own shape; `context_kind` answers "unknown" and `session_id` 0; `building` has no request, so `status` is
//! "unknown" and `cancel` false. Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs): world/{read_world_context,read_write_voxels,query_props,register_prop_recipes,
//! access_prop_transform,change_building_entities}.rs, player_input/{read_building_cursor,submit_building_input}.rs.
use mlua::{Lua, Table};
use sfi_core::lua::{FunctionArgs, LuaValue};

use super::{
    PregameRuntime,
    access::{add, denial, past_denial},
};

#[derive(Clone, Copy)]
enum Shape {
    /// `(nil, reason)`
    Nil,
    /// `(false, reason)`
    False,
}

fn refusing(
    lua: &Lua,
    table: &Table,
    context: &PregameRuntime,
    name: &str,
    operation: &'static str,
    shape: Shape,
) -> mlua::Result<()> {
    let host = context.host.clone();
    add(
        lua,
        table,
        name,
        move |_, _: FunctionArgs| -> mlua::Result<(LuaValue, Option<String>)> {
            let reason = denial(host.as_ref(), operation).ok_or_else(|| past_denial(operation))?;
            Ok((
                match shape {
                    Shape::Nil => LuaValue::Nil,
                    Shape::False => LuaValue::Boolean(false),
                },
                Some(reason),
            ))
        },
    )
}

pub(super) fn create(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let world = lua.create_table()?;
    // read_world_context.rs:3-60
    let host = context.host.clone();
    add(lua, &world, "operation_available", move |_, args| {
        let name = args.get::<String>(0)?;
        match denial(host.as_ref(), &name) {
            Some(_) => Ok(false),
            None => Err(past_denial("runtime.world.operation_available")),
        }
    })?;
    refusing(
        lua,
        &world,
        context,
        "context_active",
        "runtime.world.context.active",
        Shape::False,
    )?;
    let host = context.host.clone();
    add(lua, &world, "context_kind", move |_, _| {
        match denial(host.as_ref(), "runtime.world.context.active") {
            Some(_) => Ok("unknown"),
            None => Err(past_denial("runtime.world.context_kind")),
        }
    })?;
    let host = context.host.clone();
    add(lua, &world, "session_id", move |_, _| {
        match denial(host.as_ref(), "runtime.ecs.query") {
            Some(_) => Ok(0u64),
            None => Err(past_denial("runtime.world.session_id")),
        }
    })?;
    // read_write_voxels.rs
    let voxel = lua.create_table()?;
    refusing(lua, &voxel, context, "read", "runtime.world.voxel.read", Shape::Nil)?;
    refusing(lua, &voxel, context, "write", "runtime.world.voxel.write", Shape::False)?;
    refusing(
        lua,
        &voxel,
        context,
        "get_grid_spec",
        "runtime.world.voxel.grid_spec",
        Shape::Nil,
    )?;
    world.raw_set("voxel", voxel)?;
    // player_input/read_building_cursor.rs:3-14
    let cursor = lua.create_table()?;
    refusing(lua, &cursor, context, "get", "runtime.world.cursor.get", Shape::Nil)?;
    world.raw_set("cursor", cursor)?;
    // query_props.rs, register_prop_recipes.rs, access_prop_transform.rs, change_building_entities.rs
    let entity = lua.create_table()?;
    for (name, operation, shape) in [
        ("query_props", "runtime.world.entity.query_props", Shape::Nil),
        (
            "query_props_in_bounds",
            "runtime.world.entity.query_props_in_bounds",
            Shape::Nil,
        ),
        (
            "register_prop_recipes",
            "runtime.world.entity.register_prop_recipes",
            Shape::Nil,
        ),
        ("get_transform", "runtime.world.entity.get_transform", Shape::Nil),
        ("set_scale", "runtime.world.entity.set_scale", Shape::False),
        ("spawn", "runtime.world.entity.spawn", Shape::Nil),
        ("place", "runtime.world.entity.place", Shape::False),
        // one handle: the destroy denial; otherwise the placement path, whose first check is the destroy denial
        ("destroy", "runtime.world.entity.destroy", Shape::False),
        ("finish_building", "runtime.world.entity.finish_building", Shape::False),
    ] {
        refusing(lua, &entity, context, name, operation, shape)?;
    }
    world.raw_set("entity", entity)?;
    building(lua, &world, context)?;
    Ok(world)
}

/// SF `submit_building_input::attach` (submit_building_input.rs:61-161) without a request: the input table converts,
/// then the denial; `status` and `cancel` know no request id (its owner's current request is 0).
fn building(lua: &Lua, world: &Table, context: &PregameRuntime) -> mlua::Result<()> {
    let api = lua.create_table()?;
    let host = context.host.clone();
    api.set(
        "submit",
        lua.create_function(move |_, _input: Table| -> mlua::Result<(Option<u32>, Option<String>)> {
            match denial(host.as_ref(), "runtime.world.building.input") {
                Some(reason) => Ok((None, Some(reason))),
                None => Err(past_denial("runtime.world.building.submit")),
            }
        })?,
    )?;
    api.set("status", lua.create_function(|_, _id: u32| Ok("unknown"))?)?;
    api.set("cancel", lua.create_function(|_, _id: u32| Ok(false))?)?;
    world.set("building", api)
}
