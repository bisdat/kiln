// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/world/register_prop_recipes.rs:2-49 (the argument rules and
// texts), game-runtime/bridge/windows/props/register_prop_recipes.rs:4-15, game-runtime/native/building/
// register_prop_recipes.cpp:6-26 (the catalog: replaced whole or not at all, one recipe per item id),
// ecs/query_building_props.cpp:9-50 (recipe_bounds_intersect).
//! `runtime.world.entity.register_prop_recipes` and the recipe catalog `query_props_in_bounds` tests props against.
//!
//! ShroudForge keeps one recipe catalog in its native provider: an item id's local placement bounds (min xyz, max xyz,
//! as f32) and a feedback number. A registration replaces the whole catalog or changes nothing: a recipe whose item id
//! is 0, whose bounds are not finite (after the f32 conversion) or whose minimum exceeds its maximum is refused with
//! its index before anything is kept, and two recipes of one item id are the native refusal. Until the first
//! registration the catalog is not ready and `query_props_in_bounds` fails, as ShroudForge's does. Kiln's catalog is
//! the interpreter's own, one per runtime-create (shared by its packages and kept across a live reload), and needs
//! nothing from Kiln.

use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::{FunctionArgs, LuaError};
use std::cell::RefCell;
use std::collections::HashMap;

/// The most recipes one registration takes.
pub const MAX_RECIPES: usize = 1_000_000;

/// ShroudForge's texts.
pub mod text {
    /// register_prop_recipes.rs:16.
    pub const ARGUMENTS: &str = "runtime.world.entity.register_prop_recipes expects a recipe array";
    /// register_prop_recipes.rs:23.
    pub const LIMIT: &str = "prop recipe catalog exceeds the 1,000,000-entry safety limit";
    /// register_prop_recipes.rs:46 (the native refusal: a duplicate item id).
    pub const REJECTED: &str = "native prop recipe registration was rejected";
}

/// One recipe: an item id's local bounds (min xyz, max xyz) and its feedback number.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Recipe {
    pub bounds: [f32; 6],
    pub feedback: u32,
}

/// The catalog of one runtime-create; `None` until the first registration.
#[derive(Default)]
pub struct Recipes(RefCell<Option<HashMap<u32, Recipe>>>);

impl Recipes {
    pub fn new() -> Self {
        Self::default()
    }
    /// The catalog now (a copy), or None before the first registration.
    pub fn snapshot(&self) -> Option<HashMap<u32, Recipe>> {
        self.0.borrow().clone()
    }
    /// The native registration: the whole catalog replaced, or nothing when two recipes name one item id or a recipe
    /// breaks the rules.
    pub fn replace(&self, recipes: &[(u32, Recipe)]) -> bool {
        if recipes.len() > MAX_RECIPES {
            return false;
        }
        let mut resolved = HashMap::with_capacity(recipes.len());
        for (item, recipe) in recipes {
            if !valid(*item, &recipe.bounds) || resolved.insert(*item, *recipe).is_some() {
                return false;
            }
        }
        *self.0.borrow_mut() = Some(resolved);
        true
    }
}

fn valid(item: u32, bounds: &[f32; 6]) -> bool {
    item != 0
        && bounds.iter().all(|value| value.is_finite())
        && (0..3).all(|axis| bounds[axis] <= bounds[axis + 3])
}

/// `lua_vec::<N>` (lua/ingame/mod.rs:116-127): exactly N array entries, each converted as an f64.
pub fn lua_vec<const N: usize>(table: &Table, label: &str) -> mlua::Result<[f64; N]> {
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

/// `runtime.world.entity.register_prop_recipes(recipes)`: `true` or `nil, reason`.
pub fn register(
    _: &Lua,
    catalog: &Recipes,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(Value, Option<String>)> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldEntityRegisterPropRecipes) {
        return Ok((Value::Nil, Some(reason)));
    }
    if args.len() != 1 {
        return Err(LuaError::generic(text::ARGUMENTS));
    }
    let input = args.get::<Table>(0)?;
    let count = input.raw_len();
    if count > MAX_RECIPES {
        return Ok((Value::Nil, Some(text::LIMIT.into())));
    }
    let mut recipes = Vec::with_capacity(count);
    for index in 1..=count {
        let item = input.raw_get::<Table>(index)?;
        let item_id = item.raw_get::<u32>("itemId")?;
        let bounds =
            lua_vec::<6>(&item.raw_get::<Table>("bounds")?, "recipe bounds")?.map(|v| v as f32);
        if !valid(item_id, &bounds) {
            return Ok((
                Value::Nil,
                Some(format!("invalid placement recipe at index {index}")),
            ));
        }
        let feedback = item.raw_get::<u32>("feedback")?;
        recipes.push((item_id, Recipe { bounds, feedback }));
    }
    if catalog.replace(&recipes) {
        Ok((Value::Boolean(true), None))
    } else {
        Ok((Value::Nil, Some(text::REJECTED.into())))
    }
}

/// ShroudForge's `recipe_bounds_intersect` (query_building_props.cpp:9-50): the recipe's local box, scaled and rotated
/// by the prop's transform (positions in Q32.32 metres), as its world axis-aligned box, against the query bounds
/// (`[min, max)` on each axis). A box with (nearly) no extent counts as a quarter metre each way.
pub fn intersects(
    position: &[i64; 3],
    orientation: &[f32; 4],
    scale: &[f32; 3],
    bounds: &[f32; 6],
    query: &[f64; 6],
) -> bool {
    if (0..3).any(|axis| {
        !bounds[axis].is_finite()
            || !bounds[axis + 3].is_finite()
            || bounds[axis] > bounds[axis + 3]
    }) {
        return false;
    }
    let [qx, qy, qz, qw] = orientation.map(f64::from);
    let norm = qx * qx + qy * qy + qz * qz + qw * qw;
    let [sx, sy, sz] = scale.map(f64::from);
    if !norm.is_finite() || norm <= 1e-12 || !sx.is_finite() || !sy.is_finite() || !sz.is_finite() {
        return false;
    }
    let factor = 2.0 / norm;
    let rotation = [
        [
            1.0 - factor * (qy * qy + qz * qz),
            factor * (qx * qy - qz * qw),
            factor * (qx * qz + qy * qw),
        ],
        [
            factor * (qx * qy + qz * qw),
            1.0 - factor * (qx * qx + qz * qz),
            factor * (qy * qz - qx * qw),
        ],
        [
            factor * (qx * qz - qy * qw),
            factor * (qy * qz + qx * qw),
            1.0 - factor * (qx * qx + qy * qy),
        ],
    ];
    let scales = [sx, sy, sz];
    let mut local_center = [0.0f64; 3];
    let mut local_half = [0.0f64; 3];
    for axis in 0..3 {
        let (low, high) = (f64::from(bounds[axis]), f64::from(bounds[axis + 3]));
        local_center[axis] = (low + high) * 0.5 * scales[axis];
        local_half[axis] = (high - low).abs() * 0.5 * scales[axis].abs();
    }
    let mut world_center = [0.0f64; 3];
    let mut world_half = [0.0f64; 3];
    for row in 0..3 {
        world_center[row] = position[row] as f64 / 4_294_967_296.0;
        for column in 0..3 {
            world_center[row] += rotation[row][column] * local_center[column];
            world_half[row] += rotation[row][column].abs() * local_half[column];
        }
    }
    if world_half.iter().sum::<f64>() < 1e-6 {
        world_half = [0.25; 3];
    }
    (0..3).all(|axis| {
        world_center[axis] + world_half[axis] >= query[axis]
            && world_center[axis] - world_half[axis] < query[axis + 3]
    })
}
