// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/create_sandbox.rs:10-29,137-160;
// modules/mod-runtime/lua/mod_runner.rs:40-46,99-134.
use super::{VmAttachments, environment, require::State};
use crate::host::{HostContext, LogLevel, Package};
use mlua::{Lua, LuaOptions, StdLib, Table, Value};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

/// Own one of these per execution phase, never one per package. The VM, registry and
/// all Lua handles stay on the interpreter worker. Dropping it replaces phase caches.
pub struct LuaVm {
    lua: Lua,
    state: Rc<State>,
    context: HostContext,
    attachment_open: Cell<bool>,
    attachments: RefCell<Option<VmAttachments>>,
}
impl LuaVm {
    pub fn new(context: HostContext) -> mlua::Result<Self> {
        let lua = Lua::new_with(
            StdLib::COROUTINE | StdLib::TABLE | StdLib::STRING | StdLib::UTF8 | StdLib::MATH,
            LuaOptions::default(),
        )?;
        for name in ["load", "dofile", "loadfile"] {
            lua.globals().raw_remove(name)?;
        }
        if let Some(log) = context.log.clone() {
            let output = RefCell::new(String::new());
            lua.set_warning_function(move |_, message, continues| {
                let mut output = output.borrow_mut();
                if !output.is_empty() {
                    output.push('\t');
                }
                output.push_str(message);
                if !continues {
                    if log.enabled(LogLevel::Warn) {
                        log.write("lua", LogLevel::Warn, output.as_bytes());
                    }
                    output.clear();
                }
                Ok(())
            });
        }
        Ok(Self {
            lua,
            state: Rc::default(),
            context,
            attachment_open: Cell::new(true),
            attachments: RefCell::new(None),
        })
    }
    /// Attach once, immediately after construction, before ANY Lua access or package
    /// registration. Obtaining `lua()` seals this boundary even without executing a
    /// chunk: direct Buffer constructors/type-cache initialization must follow setup.
    /// The provider set is immutable for this phase VM's entire lifetime.
    pub fn attach(&self, attachments: VmAttachments) -> mlua::Result<()> {
        if self.attachments.borrow().is_some() {
            return Err(mlua::Error::runtime("VM providers already attached"));
        }
        if !self.attachment_open.get() {
            return Err(mlua::Error::runtime(
                "VM providers require attachment during initialization, before Lua access",
            ));
        }
        if let Some(port) = &attachments.buffer_resources {
            crate::eml::buffer::register_resources(&self.lua, port.clone())?;
        }
        *self.attachments.borrow_mut() = Some(attachments);
        Ok(())
    }
    /// Host-only access for real family registrars and trusted conformance instrumentation.
    /// Seals attachment before exposing direct Lua/type-cache constructors.
    pub fn lua(&self) -> &Lua {
        self.attachment_open.set(false);
        &self.lua
    }
    pub fn context(&self) -> &HostContext {
        &self.context
    }
    /// Register without executing Lua. The host must finish setting up the selected registry
    /// before any entrypoint/import; callback/admission policy is not inferred here.
    pub fn add_package(
        &self,
        package: Package,
        register: impl FnOnce(&Lua, &Table, &Package) -> mlua::Result<()>,
    ) -> mlua::Result<Table> {
        self.attachment_open.set(false);
        if self.state.contains(package.id()) {
            return Err(mlua::Error::runtime(format!(
                "duplicate package environment: {}",
                package.id()
            )));
        }
        // Clone the worker-local Rc and end the registry borrow before any callback.
        let typed_tables = self
            .attachments
            .borrow()
            .as_ref()
            .and_then(|p| p.typed_tables.clone());
        let env = environment::create(&self.lua, typed_tables)?;
        self.state.bind_local(&self.lua, &env, package.id())?;
        register(&self.lua, &env, &package)?;
        self.state.add(package, env.clone());
        Ok(env)
    }
    pub fn environment(&self, owner: &str) -> Option<Table> {
        self.state.environment(owner)
    }
    pub fn require(&self, owner: &str, path: &str) -> mlua::Result<Value> {
        self.attachment_open.set(false);
        self.state.require(owner, path)
    }
    pub fn entrypoint(&self, exact_manifest_id: &str) -> mlua::Result<Value> {
        self.attachment_open.set(false);
        self.state.entrypoint(&self.lua, exact_manifest_id)
    }
}
