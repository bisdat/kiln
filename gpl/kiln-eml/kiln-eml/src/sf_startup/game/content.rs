// SPDX-License-Identifier: GPL-3.0-or-later
//! The Content userdata.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/bindings/game/assets/content.rs:9-55, and the Content half of
//! lua/bindings/game/content_id_casts.rs:11-45 (`content_hash`, for `resource::AssetIdentities`, the asset session).
//!
//! A Content is an immutable content-hash identity, not a cached payload. `read_data` asks the session every call
//! (the session's staged copy first, then the baseline reader) and returns a new, owned, read-only core Buffer after
//! charging the Lua GC by its size in KiB, as SF does. No asynchronous job and no 32 MiB bound: those belong to
//! `shroudforge.contents.read`, not to this method.
use std::rc::{Rc, Weak};

use kfc::guid::ContentHash;
use mlua::{AnyUserData, UserData};
use sfi_core::{
    eml::buffer::Buffer,
    lua::{LuaError, LuaValue, MethodArgs},
};

use super::assets::AssetInner;

pub struct Content {
    guid: ContentHash,
    session: Weak<AssetInner>,
}

impl Content {
    pub(crate) fn new(guid: ContentHash, session: Weak<AssetInner>) -> Self {
        Self { guid, session }
    }

    pub fn guid(&self) -> &ContentHash {
        &self.guid
    }

    fn session(&self) -> mlua::Result<Rc<AssetInner>> {
        self.session
            .upgrade()
            .ok_or_else(|| mlua::Error::runtime("the asset session of this Content has ended"))
    }

    pub fn get_data(&self, lua: &mlua::Lua) -> mlua::Result<LuaValue> {
        match self.session()?.content_bytes(&self.guid)? {
            Some(bytes) => {
                let mut buffer = Buffer::wrap(bytes);
                lua.gc_step_kbytes(buffer.capacity()?.div_ceil(1024) as i32)?;
                buffer.set_writable(false)?;
                lua.create_userdata(buffer).map(LuaValue::UserData)
            }
            None => Err(LuaError::content_not_found(self.guid.to_string())),
        }
    }
}

/// The Content half of `resource::AssetIdentities`: a Content userdata's hash, `None` for any other userdata.
pub(crate) fn content_hash(ud: &AnyUserData) -> mlua::Result<Option<ContentHash>> {
    if ud.is::<Content>() {
        Ok(Some(ud.borrow::<Content>()?.guid))
    } else {
        Ok(None)
    }
}

impl UserData for Content {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("guid", |_, this| Ok(this.guid.to_string()));
        fields.add_field_method_get("size", |_, this| Ok(this.guid.size()));
        fields.add_field_method_get("hash0", |_, this| Ok(this.guid.hash0()));
        fields.add_field_method_get("hash1", |_, this| Ok(this.guid.hash1()));
        fields.add_field_method_get("hash2", |_, this| Ok(this.guid.hash2()));
    }

    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_function("read_data", |lua, args: MethodArgs| {
            let this = args.this::<&Self>()?;
            this.get_data(lua)
        });
    }
}
