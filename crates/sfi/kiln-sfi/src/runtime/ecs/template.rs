// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/resolve_entity.rs:22-70 (get_template),
// game-runtime/bridge/windows/ecs/read_entity_template_uuid.rs:3-7, game-runtime/bridge/windows/resources/
// check_resource_exists.rs:5-35 (the loaded ResourceService lookup, and its text without the provider's extension).
//! `runtime.ecs.get_template(entity)`: the loaded `TemplateResource` an entity was spawned from.
//!
//! ShroudForge's checks in its order: the read denial, the read's availability (readiness), one argument, a live world
//! session, the entity's template uuid (kiln.world's `ecs_template_uuid`, validated before and after the read), the
//! `keen::ecs::TemplateResource` type in the archive registry, then the loaded ResourceService's lookup of that
//! resource (kiln.assets' `loaded_snapshot`, a size query, through the VM's loaded-resource provider:
//! `crate::runtime::assets`), the entity's template uuid again and the session again. The answer is a loaded
//! `Resource` bound to that session, which reads its snapshot on first use. A VM without the loaded-resource family
//! answers ShroudForge's text for a provider without the ResourceService read extension, as `game.assets` does.

use super::{Ecs, available};
use crate::runtime::assets::resource::LoadedResource;
use crate::runtime::unavailable::NO_RESOURCE_READ;
use kiln_abi::services::kiln_assets::KilnAssetKey;
use mlua::{Lua, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op};
use sfi_core::lua::FunctionArgs;

/// The reflected type of an entity's template.
pub const TEMPLATE_TYPE: &str = "keen::ecs::TemplateResource";

/// ShroudForge's texts (resolve_entity.rs:33-60).
pub mod text {
    pub const READS_UNAVAILABLE: &str = "live ECS reads are unavailable";
    pub const ONE_HANDLE: &str = "get_template expects one current entity handle";
    pub const NO_SESSION: &str = "get_template requires an active world session";
    pub const NO_UUID: &str = "entity template UUID is unavailable or unstable";
    pub const NO_TYPE: &str = "TemplateResource is absent from the active reflection";
    pub const NOT_LOADED: &str = "entity template is not loaded in the active ResourceService";
    pub const CHANGED: &str = "entity template changed during resource resolution";
    pub const SESSION: &str = "world session changed during entity template resolution";
}

/// `runtime.ecs.get_template(entity)`.
pub fn get_template(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(Value, Option<String>)> {
    let resolve = || -> Result<Value, String> {
        if let Some(reason) = ecs.denial_reason(host, Op::EcsRead) {
            return Err(reason);
        }
        if !available(host, Op::EcsRead) {
            return Err(text::READS_UNAVAILABLE.into());
        }
        if args.len() != 1 {
            return Err(text::ONE_HANDLE.into());
        }
        let session = ecs.world_session();
        if session == 0 {
            return Err(text::NO_SESSION.into());
        }
        let entity = args.get::<u32>(0).map_err(|error| error.to_string())?;
        let uuid = ecs.template_uuid(entity).ok_or(text::NO_UUID)?;
        let template = ecs
            .types()
            .by_qualified_name(TEMPLATE_TYPE)
            .ok_or(text::NO_TYPE)?;
        let key = KilnAssetKey {
            guid: uuid,
            r#type: template.qualified_hash,
            part: 0,
        };
        // `runtime_provider::resource_exists(&id)?`
        let assets = crate::runtime::assets::of_vm(lua).ok_or(NO_RESOURCE_READ)?;
        if !assets.exists(&key)? {
            return Err(text::NOT_LOADED.into());
        }
        // the entity-to-template association again after the lookup
        if ecs.template_uuid(entity) != Some(uuid) {
            return Err(text::CHANGED.into());
        }
        if ecs.world_session() != session {
            return Err(text::SESSION.into());
        }
        let resource = LoadedResource::for_session(assets, key, template.index, session);
        Ok(Value::UserData(
            lua.create_userdata(resource)
                .map_err(|error| error.to_string())?,
        ))
    };
    Ok(match resolve() {
        Ok(resource) => (resource, None),
        Err(reason) => (Value::Nil, Some(reason)),
    })
}
