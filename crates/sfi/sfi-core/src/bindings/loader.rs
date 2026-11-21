// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/eml/v1/create_loader_api.rs:7-28;
// modules/mod-runtime/lua/ingame/check_ingame_access.rs:12-19 (both pins).
use crate::{host::Side, package::manifest::Capability};
use mlua::{Lua, Table};
use std::collections::BTreeSet;

/// Host construction snapshot, not package activation. Discovery must supply ALL discovered
/// IDs (including disabled and other-target packages), never LuaVm's active subset.
/// Flags are global phase feature facts; manifest permissions are applied separately.
#[derive(Clone, Debug)]
pub struct LoaderSnapshot {
    pub patch: bool,
    pub export: bool,
    pub discovered_ids: BTreeSet<String>,
}

pub(super) fn install(
    lua: &Lua,
    table: &Table,
    side: Side,
    capabilities: &[Capability],
    snapshot: LoaderSnapshot,
) -> mlua::Result<Table> {
    table.raw_set("is_client", side == Side::Client)?;
    table.raw_set("is_server", side == Side::Server)?;
    table.raw_set(
        "has_mod",
        lua.create_function(move |_, id: String| Ok(snapshot.discovered_ids.contains(&id)))?,
    )?;
    let features = super::common::child(lua, table, "features")?;
    features.raw_set(
        "patch",
        snapshot.patch && capabilities.contains(&Capability::Patch),
    )?;
    features.raw_set(
        "export",
        snapshot.export && capabilities.contains(&Capability::Export),
    )?;
    // The native manager owns loader.runtime AND features.runtime.dll. It extends these returned
    // tables using real native admission facts; pure scaffolding invents neither.
    Ok(features)
}
