// SPDX-License-Identifier: GPL-3.0-or-later
//! The runtime families of the SF-dialect startup VM: every `runtime.*` table ShroudForge 1.7.9 builds in
//! its pregame VM, with its pregame behaviour.
//!
//! ShroudForge registers one `runtime` table in both phases (lua/bindings/mod.rs:66-114, lua/ingame/mod.rs:45-82): its
//! pregame VM has `runtime.ecs`, `.impact`, `.services`, `.overrides`, `.world`, `.network`, `.patch`,
//! `.functions` and `.values` as well, so a package may touch them at top level. In the pregame phase:
//! - the families' operations answer ShroudForge's phase refusal ("runtime APIs are available only during the ingame
//!   phase", check_ingame_access.rs:386-414) in each function's own result shape, after the argument conversions that
//!   come before the check in its source;
//! - the members without a phase check answer what they answer without a game process: the pregame loader process
//!   never has the native provider (kfc-runtime.dll) loaded, so the provider's facts are "absent" (world session 0, not
//!   ready, no component layout) and its catalogs refuse with the bridge's texts
//!   (game-runtime/bridge/windows/{ecs/read_component_registry,functions/read_function_catalog}.rs);
//! - `runtime.values` has no phase check at all and works on the session's registry (values/register_value_api.rs);
//! - `runtime.ecs.get_catalog` lists the registry's `keen::ecs::*` types with every live fact false;
//! - `runtime.overrides` is ShroudForge's own Lua tracker factory and target factories (pure bookkeeping, works), and
//!   its four claims refuse where their source does without a provider.
//!
//! Nothing here reaches the game, a native provider or the archive beyond the session's registry and the Resource
//! userdata a package already holds. The registrar refuses any phase but `Startup`, so a denial is always present and
//! no ingame path is reachable; a function that would continue past it is a setup error, never a successful stand-in.
//!
//! `runtime.ecs.get_attributes`, `get_attribute` and `evaluate_attributes` read the archive's attribute catalog
//! (the VM's one asset session, [`crate::sf_startup::attributes`]) as ShroudForge's do before the game starts
//! ([`attributes`]), and [`attach_loaded_resources`] puts the loaded-resource functions on a package's `game.assets`
//! with their pregame answers ([`loaded_resources`]).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs), identical at the
//! fork pin 2df1de68:
//! - modules/mod-runtime/lua/ingame/mod.rs:45-82 (the family order), check_ingame_access.rs:386-414 (the denial);
//! - lua/ingame/ecs/{mod,read_component_registry,read_component_catalog,query_entities,resolve_entity,read_component,
//!   access_component_fields,write_component,access_template_component}.rs and ecs/attributes/*.rs (`ecs`);
//! - lua/ingame/impact/mod.rs, access_named_service.rs (`impact`, `services`);
//! - lua/ingame/world/*.rs, player_input/{read_building_cursor,submit_building_input}.rs (`world`);
//! - modules/transport/lua/network.rs and modules/loader-config/loader.default.json (`network`);
//! - lua/ingame/set_profile_patch_enabled.rs, functions/{mod,read_function_catalog,bind_profile_modifier}.rs (`patch`,
//!   `functions`);
//! - lua/ingame/overrides/{mod,track_runtime_overrides,claim_runtime_storage}.rs and the two Lua files
//!   track_runtime_overrides.lua and create_override_targets.lua, verbatim (`overrides`);
//! - lua/values/*.rs (`values`);
//! - lua/ingame/ecs/attributes/{read_attribute_catalog,read_attribute_descriptor,evaluate_attributes,
//!   encode_attribute_scalar,validate_attribute_bits,format_attribute_root}.rs and lua/convert_json.rs (`attributes`,
//!   the attribute catalog);
//! - lua/ingame/resources/{mod,read_loaded_resources,index_loaded_resources,write_loaded_field}.rs
//!   (`loaded_resources`, the attribute catalog).
use std::rc::Rc;

use mlua::{Lua, Table};
use sfi_core::{
    bindings::runtime::RuntimeHost,
    host::{Phase, Side},
};

use super::{TypeSession, game::assets::AssetSession, resource_values::ValueObjects};

mod access;
mod attributes;
mod ecs;
mod functions;
mod loaded_resources;
mod network;
mod overrides;
mod overrides_lua;
mod services;
mod values;
mod world;

pub use access::{FUNCTION_INVENTORY_MISSING, PHASE_REFUSAL, REGISTRY_MISSING};
pub use loaded_resources::{INGAME_ONLY, NOT_A_RUNTIME_SNAPSHOT, attach as attach_loaded_resources};
pub use values::validate_owned_type;

/// One package's inputs of the families: its runtime registrar host (the same one `compose::install` received), the VM's type
/// session, the value wrappers' production seam and the VM's one asset session (its reference archive holds the
/// attribute catalog).
#[derive(Clone)]
pub struct PregameRuntime {
    pub host: Rc<dyn RuntimeHost>,
    pub types: TypeSession,
    pub objects: ValueObjects,
    pub assets: AssetSession,
}

impl PregameRuntime {
    pub(crate) fn side(&self) -> Side {
        self.host.owner_context().side()
    }
}

/// Installs the families into the package's `runtime` table (the runtime registrar's, already holding `types`) in ShroudForge's order:
/// ecs, impact, services, overrides, world, network, patch, functions (which captures the checked wrappers installed
/// before it), then values and the overrides' claims and targets. `game` is the package's game table; its `assets`
/// must exist (the targets capture it).
pub fn install(lua: &Lua, game: &Table, runtime: &Table, context: &PregameRuntime) -> mlua::Result<()> {
    if context.host.owner_context().phase() != Phase::Startup {
        return Err(mlua::Error::runtime(
            "the pregame runtime families need a startup-phase runtime host",
        ));
    }
    for name in [
        "ecs",
        "impact",
        "services",
        "overrides",
        "world",
        "network",
        "patch",
        "functions",
        "values",
    ] {
        if !runtime.raw_get::<mlua::Value>(name)?.is_nil() {
            return Err(mlua::Error::runtime(format!("runtime.{name} is already installed")));
        }
    }
    runtime.raw_set("ecs", ecs::create(lua, context)?)?;
    runtime.raw_set("impact", services::create_impact(lua, context)?)?;
    runtime.raw_set("services", services::create_services(lua, context)?)?;
    runtime.raw_set("overrides", overrides::create(lua, context)?)?;
    runtime.raw_set("world", world::create(lua, context)?)?;
    runtime.raw_set("network", network::create(lua, context)?)?;
    runtime.raw_set("patch", functions::create_patch(lua, context)?)?;
    runtime.raw_set("functions", functions::create(lua, runtime, context)?)?;
    runtime.raw_set("values", values::create(lua, context)?)?;
    overrides::attach(lua, runtime, game, context)
}
