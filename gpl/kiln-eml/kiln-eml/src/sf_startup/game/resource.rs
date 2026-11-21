// SPDX-License-Identifier: GPL-3.0-or-later
//! The Resource userdata and the session's per-resource record (the asset session, archive half).
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/bindings/game/assets/resource.rs:11-50 (Resource and its fields);
//! - modules/mod-runtime/session/asset_resources.rs:3-29 (`ResourceInfo`, `ResourceSource`, `source_name`);
//! - modules/mod-runtime/session/loaded_resources.rs:95-97 (`creation_session`);
//! - modules/mod-runtime/session/asset_resources.rs:3-13,31-99 (the record's value, dirty flag, `get_lua_value`,
//!   `set_lua_value` and `apply`; the asset session) and lua/bindings/game/assets/resource.rs:46-49 (`data`);
//! - the Resource half of lua/bindings/game/content_id_casts.rs:11-45 (`AssetIdentities`, the asset session).
//!
//! Identity and metadata (`guid`, `type`, `part`, `source`, `loaded`, `session_id`) and `data` are shared through one
//! `Rc<ResourceInfo>` per resource id in the session's cache, so every package and function sees the same record and
//! the same value. `data` decodes lazily into the mapped value wrappers (once; a failed decode is retried by the next read,
//! as SF's `OnceCell::get_or_try_init`), nested edits mark those wrappers dirty, and the setter validates and copies a
//! whole new value (PATCH first). `original_data` is absent, as SF registers none. Loaded-runtime records belong to the
//! runtime registrar, not to this archive session; the startup VM never makes one.
use std::{
    cell::{Cell, RefCell},
    rc::{Rc, Weak},
};

use kfc::guid::{ContentHash, Guid, ResourceId};
use mlua::{AnyUserData, UserData};
use sfi_core::lua::LuaValue;

use super::{
    super::{format_adapter, resource_values::AssetUserdata},
    assets::AssetInner,
    content,
};

/// Where a resource record came from (FINAL Resource.source).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceSource {
    /// The reference archive (`kfc-file`).
    KfcFile,
    /// Created by a package in this pass (`asset-transaction`): the asset session's create_resource.
    AssetTransaction,
}

impl ResourceSource {
    pub fn name(self) -> &'static str {
        match self {
            Self::KfcFile => "kfc-file",
            Self::AssetTransaction => "asset-transaction",
        }
    }
}

/// One resource's record in the session cache: its identity, its Lua value once read or set, and whether that value
/// was replaced (SF's `is_dirty`; nested edits are the wrappers' own dirtiness).
pub struct ResourceInfo {
    pub resource_id: ResourceId,
    pub source: ResourceSource,
    pub(crate) value: RefCell<Option<LuaValue>>,
    pub(crate) replaced: Cell<bool>,
}

impl std::fmt::Debug for ResourceInfo {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ResourceInfo")
            .field("resource_id", &self.resource_id)
            .field("source", &self.source)
            .field("read", &self.value.borrow().is_some())
            .field("replaced", &self.replaced.get())
            .finish()
    }
}

impl ResourceInfo {
    /// A reference record (`kfc-file`), its value decoded on first read.
    pub(crate) fn new(resource_id: ResourceId, source: ResourceSource) -> Self {
        Self {
            resource_id,
            source,
            value: RefCell::new(None),
            replaced: Cell::new(false),
        }
    }

    /// A record this pass created (`asset-transaction`) with its validated value, dirty from the start (SF
    /// `add_resource`).
    pub(crate) fn created(resource_id: ResourceId, value: LuaValue) -> Self {
        Self {
            resource_id,
            source: ResourceSource::AssetTransaction,
            value: RefCell::new(Some(value)),
            replaced: Cell::new(true),
        }
    }

    /// SF `creation_session`: only loaded-runtime records have one, so an archive record has none.
    pub fn creation_session(&self) -> Option<u64> {
        None
    }
}

/// A Resource: a shared record and the session it belongs to (weakly, as a Type holds its session).
pub struct Resource {
    info: Rc<ResourceInfo>,
    session: Weak<AssetInner>,
}

impl Resource {
    pub(crate) fn new(info: Rc<ResourceInfo>, session: Weak<AssetInner>) -> Self {
        Self { info, session }
    }

    pub fn info(&self) -> &Rc<ResourceInfo> {
        &self.info
    }

    fn session(&self) -> mlua::Result<Rc<AssetInner>> {
        self.session
            .upgrade()
            .ok_or_else(|| mlua::Error::runtime("the asset session of this Resource has ended"))
    }
}

impl UserData for Resource {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        // ResourceId's Display is its base GUID only; type and part are separate fields.
        fields.add_field_method_get("guid", |_, this| Ok(this.info.resource_id.to_string()));
        fields.add_field_method_get("source", |_, this| Ok(this.info.source.name()));
        // An archive record is never loaded-runtime.
        fields.add_field_method_get("loaded", |_, _| Ok(false));
        fields.add_field_method_get("type", |lua, this| {
            let session = this.session()?;
            let index = format_adapter::type_for_resource(session.types.registry(), &this.info.resource_id)
                .map(|metadata| metadata.index);
            match index {
                Some(index) => session.types.get_type(lua, index),
                None => Ok(Some(LuaValue::Nil)),
            }
        });
        fields.add_field_method_get("part", |_, this| Ok(this.info.resource_id.part_index()));
        fields.add_field_method_get("session_id", |_, this| Ok(this.info.creation_session()));
        fields.add_field_method_get("data", |lua, this| this.session()?.resource_value(&this.info, lua));
        fields.add_field_method_set("data", |lua, this, value: LuaValue| {
            this.session()?.replace_resource_value(&this.info, value, lua)
        });
    }
}

/// The wrappers' `AssetUserdata` over this session's userdata (the impl lives with the types): a Resource answers
/// with its id's GUID, a Content with its hash, anything else with neither, as SF's Guid cast reads them
/// (content_id_casts.rs:11-45). It holds nothing (no VM, session or registry reference), so the wrappers
/// that carry it cannot keep a VM alive.
#[derive(Debug, Clone, Copy, Default)]
pub struct AssetIdentities;

impl AssetUserdata for AssetIdentities {
    fn resource_guid(&self, ud: &AnyUserData) -> mlua::Result<Option<Guid>> {
        if ud.is::<Resource>() {
            Ok(Some(ud.borrow::<Resource>()?.info.resource_id.guid()))
        } else {
            Ok(None)
        }
    }

    fn content_hash(&self, ud: &AnyUserData) -> mlua::Result<Option<ContentHash>> {
        content::content_hash(ud)
    }
}
