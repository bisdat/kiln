// SPDX-License-Identifier: GPL-3.0-or-later
//! The SF-dialect startup pass's Buffer resource port: `Buffer:read_resource(type)` and
//! `Buffer:write_resource(type, value)` on sfi-core's one real Buffer, through `BufferResourcePort`. The pass attaches
//! it with `LuaVm::attach` before any Lua access, so the first Buffer userdata already has both methods; without it the
//! methods are absent, never dummies.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/bindings/buffer/mod.rs:345-364 (`lua_read_resource`) and 457-480 (`lua_write_resource`),
//! identical at 1.8.0.
//!
//! - read: the receiver borrowed immutably for the whole call; closed (both open states read) before the type
//!   argument; the bytes head..tail decoded without moving head; exactly one result (the codec's decode into the value wrappers).
//! - write: the receiver borrowed MUTABLY for the whole call, so a metamethod of the input table that touches the same
//!   Buffer meets the borrow error, as in SF; closed and writable before the type argument, then the value, validated
//!   and cloned, converted and encoded (the codec's `encode`), appended at tail; no results. No PATCH gate: neither touches
//!   the archive.
//! - types resolve in the pass's registry (a qualified name or a Type of that registry; a Type of another registry is
//!   refused, see game/casts.rs `type_arg`).
//!
//! The port holds the registry and the typed-object seam only, never the VM (attachments.rs: no strong Lua in a port).
use std::rc::Rc;

use kfc::reflection::TypeRegistry;
use sfi_core::{
    eml::buffer::Buffer,
    lua::{BufferResourcePort, LuaError, LuaValue, MethodArgs},
};

use super::{
    game::casts::type_arg,
    resource_values::{self, TypeHandle, ValueObjects},
};

pub struct BufferResources {
    registry: Rc<TypeRegistry>,
    objects: ValueObjects,
}

impl BufferResources {
    /// The port over the pass's registry and its typed-object seam (`AssetSession::objects` in a patching pass, so
    /// decoded values are the same wrappers as `Resource.data`'s).
    pub fn new(registry: Rc<TypeRegistry>, objects: ValueObjects) -> Self {
        Self { registry, objects }
    }

    fn r#type(&self, args: &MethodArgs) -> mlua::Result<TypeHandle> {
        let index = type_arg(args, 0, &self.registry)?;
        Ok(TypeHandle::new(self.registry.clone(), index))
    }
}

impl BufferResourcePort for BufferResources {
    fn read_resource(&self, lua: &mlua::Lua, args: MethodArgs) -> mlua::Result<LuaValue> {
        let this = args.this::<&Buffer>()?;
        // closed first (SF check_not_closed, check_readable: both open states read)
        this.data()?;
        let r#type = self.r#type(&args)?;
        let bytes: Rc<[u8]> = Rc::from(this.data()?);
        resource_values::decode(bytes, &r#type, lua, &self.objects).map_err(LuaError::external)
    }

    fn write_resource(&self, lua: &mlua::Lua, args: MethodArgs) -> mlua::Result<()> {
        let mut this = args.this::<&mut Buffer>()?;
        this.check_writable()?;
        let r#type = self.r#type(&args)?;
        let value = args.get::<LuaValue>(1)?;
        let bytes = resource_values::encode(value, &r#type, lua, &self.objects).map_err(LuaError::external)?;
        this.write_bytes(&bytes)
    }
}
