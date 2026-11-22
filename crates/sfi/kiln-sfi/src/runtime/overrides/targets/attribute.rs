// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/create_override_targets.lua:1-160 (attribute.lua).
//! `runtime.overrides.attribute`: ShroudForge's attribute override target, its own Lua ([`TARGET`]), over the package's
//! `runtime` members it calls when it runs (`runtime.ecs.get_attribute`, `read_attributes`, `evaluate_attributes`,
//! `update_attribute`, `runtime.world.session_id`, and `runtime.overrides.get_component_claim` on the component that
//! holds the attribute's root). Its `write` passes the snapshot's whole root as `expected_values`, so a restore never
//! lands over another writer's change; its `normalize` is the archive's recalculation of the candidate root.

use mlua::{Function, Lua, Table};

/// ShroudForge's attribute target factory.
pub const TARGET: &str = include_str!("attribute.lua");

/// Puts `attribute` into one package's `runtime.overrides` table (`runtime` is the package's runtime table).
pub fn install_into(lua: &Lua, runtime: &Table) -> mlua::Result<()> {
    let overrides: Table = runtime.raw_get("overrides")?;
    let factory: Function = lua
        .load(TARGET)
        .set_name("=runtime.overrides.attribute")
        .eval()?;
    let attribute: Function = factory.call(runtime.clone())?;
    overrides.raw_set("attribute", attribute)
}
