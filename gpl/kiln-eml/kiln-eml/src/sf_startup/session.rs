// SPDX-License-Identifier: GPL-3.0-or-later
//! The reference/type session of one SF-dialect startup VM.
//!
//! One session per VM: the reflected registry (`game.types`), the selected target index's facts (`game.version`) and
//! the Type userdata cache, so the same index always gives the same Type value. ShroudForge keeps these in its
//! `AppState` (Lua app data); here they are a session object the `game` tables' functions capture, so a test can hold
//! two registries in one VM and see a Type from the other one refused.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/session/mod.rs:45,61,118-121,177-196 (`type_registry`, `types`, `get_type`).
//!
//! Lifetime: the `game` tables' closures hold the session strongly and each Type holds it weakly, so neither the cache
//! nor a Type keeps the other alive through Rust (the cache holds Lua references to the Types). Everything here is
//! `Rc` and worker-local: the VM's serial interpreter worker is the only thread that touches it.
use std::{
    cell::RefCell,
    collections::HashMap,
    fmt,
    rc::{Rc, Weak},
};

use kfc::{
    container::KFCFile,
    reflection::{TypeHandle, TypeIndex, TypeRegistry},
};
use sfi_core::lua::LuaValue;

use super::game::types::Type;

/// The facts this session takes from the selected target's KFC index (`<stem>.kfc`), kept apart from the registry's
/// own: `game.version` is the index's data version, `game.types.version` is the registry's (FINAL game.version,
/// game.types.version).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SelectedTarget {
    /// The archive stem the pass writes (`enshrouded` or `enshrouded_server`).
    pub stem: String,
    /// `KFCFile::game_version()` of that index.
    pub game_version: String,
}

impl SelectedTarget {
    pub fn from_index(stem: impl Into<String>, index: &KFCFile) -> Self {
        Self {
            stem: stem.into(),
            game_version: index.game_version().to_owned(),
        }
    }
}

/// How later builders (typed values: the value wrappers; resources: the asset session) teach `game.types.of` their userdata. Returns the value's
/// reflected type, `None` when the value is not one of the recognizer's classes. An error propagates to the caller of
/// `of`, as SF's `type_of` borrow errors do.
pub type TypeRecognizer = Rc<dyn Fn(&LuaValue) -> mlua::Result<Option<TypeIndex>>>;

pub(crate) struct Inner {
    pub(crate) registry: Rc<TypeRegistry>,
    pub(crate) target: SelectedTarget,
    vm: mlua::WeakLua,
    types: RefCell<HashMap<TypeIndex, LuaValue>>,
    recognizers: RefCell<Vec<TypeRecognizer>>,
}

/// A handle to the session. Clones share it.
#[derive(Clone)]
pub struct TypeSession(pub(crate) Rc<Inner>);

impl fmt::Debug for TypeSession {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TypeSession")
            .field("types", &self.0.registry.len())
            .field("registry_version", &self.0.registry.version)
            .field("target", &self.0.target)
            .finish()
    }
}

impl TypeSession {
    /// A session for `lua` over `registry` and the selected target. Every Lua call into it must come from `lua`.
    pub fn new(lua: &mlua::Lua, registry: Rc<TypeRegistry>, target: SelectedTarget) -> Self {
        Self(Rc::new(Inner {
            registry,
            target,
            vm: lua.weak(),
            types: RefCell::new(HashMap::new()),
            recognizers: RefCell::new(Vec::new()),
        }))
    }

    pub fn registry(&self) -> &Rc<TypeRegistry> {
        &self.0.registry
    }

    pub fn target(&self) -> &SelectedTarget {
        &self.0.target
    }

    /// The narrow recognition seam for `game.types.of` (see `TypeRecognizer`). Recognizers are asked in the order
    /// they were added; the first that answers wins.
    pub fn add_type_recognizer(&self, recognizer: TypeRecognizer) {
        self.0.recognizers.borrow_mut().push(recognizer);
    }

    /// The cached Type userdata of `index`, created on first use; `None` for an index the registry does not have.
    pub fn get_type(&self, lua: &mlua::Lua, index: TypeIndex) -> mlua::Result<Option<LuaValue>> {
        Inner::get_type(&self.0, lua, index)
    }

    /// SF `type_of` through the registered recognizers: `None` when no recognizer knows the value.
    pub fn type_of(&self, value: &LuaValue) -> mlua::Result<Option<TypeIndex>> {
        let recognizers = self.0.recognizers.borrow().clone();
        for recognize in recognizers {
            if let Some(index) = recognize(value)? {
                return Ok(Some(index));
            }
        }
        Ok(None)
    }

    pub(crate) fn upgrade(weak: &Weak<Inner>) -> mlua::Result<Self> {
        weak.upgrade()
            .map(Self)
            .ok_or_else(|| mlua::Error::runtime("the type session of this Type has ended"))
    }
}

impl Inner {
    fn get_type(this: &Rc<Self>, lua: &mlua::Lua, index: TypeIndex) -> mlua::Result<Option<LuaValue>> {
        if lua.weak() != this.vm {
            return Err(mlua::Error::runtime("this type session belongs to another Lua VM"));
        }
        if let Some(value) = this.types.borrow().get(&index) {
            return Ok(Some(value.clone()));
        }
        if this.registry.get(index).is_none() {
            return Ok(None);
        }
        let ty = Type::new(TypeHandle::new(this.registry.clone(), index), Rc::downgrade(this));
        let value = LuaValue::UserData(lua.create_userdata(ty)?);
        // Lua can run inside create_userdata: the collector calls finalizers from the allocation, and one may ask for
        // this index and cache its Type first. Keep that one, so an index never gives two Types (SF keeps its cache
        // entry borrowed across the allocation instead, which fails such a finalizer's call).
        Ok(Some(this.types.borrow_mut().entry(index).or_insert(value).clone()))
    }
}
