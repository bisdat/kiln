// SPDX-License-Identifier: GPL-3.0-or-later
//! The argument casts of `game.assets`: Guid, ContentHash and the `string|Type` type argument.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/bindings/game/content_id_casts.rs:11-97 (`Guid`, `ContentHash` casts);
//! - modules/mod-runtime/lua/bindings/game/resolve_type.rs:149-165 (`get_type`).
//!
//! The casts are newtypes because neither the cast trait (sfi-core) nor the identifiers (kfc) are this crate's. Same
//! accepted shapes, names (the `bad argument` texts) and order of reads. One difference, not observable through one
//! session: a Type of another registry is refused instead of being read at its index in this one (SF reads whatever
//! type has that index here, or panics past the end).
use std::borrow::Cow;

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{LookupKey, TypeIndex, TypeRegistry},
};
use mlua::ObjectLike;
use sfi_core::lua::{Args, CastLua, CastLuaExt, Either, LuaError, LuaValue};

use super::{Type, content::Content, resource::Resource};

/// SF's `Guid` cast: a GUID string, or a Content (its hash's GUID form) or Resource (its base GUID) userdata.
pub struct GuidArg;
impl CastLua<'_> for GuidArg {
    type Output = Guid;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Guid>> {
        match value {
            LuaValue::String(text) => Ok(Guid::parse(&text.to_str()?)),
            LuaValue::UserData(data) => {
                if data.is::<Content>() {
                    Ok(Some(data.borrow::<Content>()?.guid().into_guid()))
                } else if data.is::<Resource>() {
                    Ok(Some(data.borrow::<Resource>()?.info().resource_id.guid()))
                } else {
                    Ok(None)
                }
            }
            _ => Ok(None),
        }
    }
    fn name() -> Cow<'static, str> {
        "guid".into()
    }
}

/// SF's `ContentHash` cast: a userdata or table with `size`, `hash0`, `hash1` and `hash2` (all four read, in that
/// order, before any is checked; a field read error propagates), or a GUID string.
pub struct ContentHashArg;
impl CastLua<'_> for ContentHashArg {
    type Output = ContentHash;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<ContentHash>> {
        let words = |get: &dyn Fn(&str) -> mlua::Result<LuaValue>| -> mlua::Result<Option<ContentHash>> {
            let mut words = [None; 4];
            for (word, key) in words.iter_mut().zip(["size", "hash0", "hash1", "hash2"]) {
                *word = get(key)?.try_cast::<u32>()?;
            }
            Ok(match words {
                [Some(size), Some(hash0), Some(hash1), Some(hash2)] => {
                    Some(ContentHash::new(size, hash0, hash1, hash2))
                }
                _ => None,
            })
        };
        match value {
            LuaValue::UserData(data) => words(&|key| data.get::<LuaValue>(key)),
            LuaValue::Table(table) => words(&|key| table.get::<LuaValue>(key)),
            LuaValue::String(text) => Ok(ContentHash::parse(&text.to_str()?)),
            _ => Ok(None),
        }
    }
    fn name() -> Cow<'static, str> {
        "keen::ContentHash or guid".into()
    }
}

/// SF's `get_type(args, index, registry)`: a qualified name (`type '<name>' not found` when absent) or a Type.
pub fn type_arg(args: &impl Args, index: usize, registry: &TypeRegistry) -> mlua::Result<TypeIndex> {
    match args.get::<Either<String, &Type>>(index)? {
        Either::A(name) => registry
            .get_by_name(LookupKey::Qualified(&name))
            .map(|ty| ty.index)
            .ok_or_else(|| LuaError::generic(format!("type '{name}' not found"))),
        Either::B(ty) => registry
            .get(ty.index)
            .filter(|current| {
                current.qualified_hash == ty.qualified_hash && current.qualified_name == ty.qualified_name
            })
            .map(|current| current.index)
            .ok_or_else(|| LuaError::generic("type belongs to another registry")),
    }
}
