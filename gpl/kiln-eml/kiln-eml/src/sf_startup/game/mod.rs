// SPDX-License-Identifier: GPL-3.0-or-later
//! A package's `game` table in the SF-dialect startup VM: `game.version` and `game.types`.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/bindings/game/mod.rs:15-23 and lua/bindings/register_functions.rs:5-21.
//!
//! SF builds one `game` table per package with `version`, `assets`, `types` and `guid`. This module creates that table
//! and installs the first two; the other builders add theirs to the SAME table (assets: the asset session's `install_assets`; guid
//! and the runtime alias `runtime.types == game.types`: the pass composition / the runtime registrar). Each package gets its own `types` API table (an
//! ordinary mutable Lua table, FINAL game.types), while every Type value comes from the one session cache.
pub mod assets;
pub mod casts;
pub mod content;
pub mod resolve_type;
pub mod resource;
pub mod types;

pub use types::Type;

use sfi_core::lua::FunctionArgs;

use super::TypeSession;
use assets::AssetSession;

/// A new `game` table for one package with `version` (the selected index's data version) and `types`.
pub fn create(lua: &mlua::Lua, session: &TypeSession) -> mlua::Result<mlua::Table> {
    let table = lua.create_table()?;
    install(lua, &table, session)?;
    Ok(table)
}

/// Adds `version` and `types` to an existing `game` table, keeping its identity and every other key.
pub fn install(lua: &mlua::Lua, game: &mlua::Table, session: &TypeSession) -> mlua::Result<()> {
    game.raw_set("version", session.target().game_version.as_str())?;
    game.raw_set("types", types::create(lua, session)?)?;
    Ok(())
}

/// Adds `assets` (the archive catalog and content functions over the VM's one asset session) to an existing
/// `game` table, keeping its identity and every other key. SF builds `assets` in the same table as `version` and
/// `types` (game/mod.rs:19).
pub fn install_assets(lua: &mlua::Lua, game: &mlua::Table, assets: &AssetSession) -> mlua::Result<()> {
    game.raw_set("assets", assets.create_table(lua)?)
}

/// [`install_assets`] with `loaded` adding the loaded-resource functions at ShroudForge's position in the table (the attribute catalog:
/// the pass's pregame answers, `pregame_runtime::attach_loaded_resources`).
pub fn install_assets_with(
    lua: &mlua::Lua,
    game: &mlua::Table,
    assets: &AssetSession,
    loaded: impl FnOnce(&mlua::Lua, &mlua::Table) -> mlua::Result<()>,
) -> mlua::Result<()> {
    game.raw_set("assets", assets.create_table_with(lua, loaded)?)
}

/// SF's `add_function`: a plain Lua function under `name`, set without metamethods.
pub(crate) fn add_function<F, R>(lua: &mlua::Lua, table: &mlua::Table, name: &str, func: F) -> mlua::Result<()>
where
    F: Fn(&mlua::Lua, FunctionArgs) -> mlua::Result<R> + 'static,
    R: mlua::IntoLuaMulti,
{
    let function = lua.create_function(func)?;
    table.raw_set(name, function)?;
    Ok(())
}
