// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Binding contracts in both pins: lua/bindings/extend_table_operations.rs:19-114;
// lua/bindings/buffer/mod.rs:345-364,457-480. No format/type implementation.
//! Finite worker-local attachment ports for the real table dispatcher and Buffer.
//!
//! Call `LuaVm::attach` once immediately after `LuaVm::new`, BEFORE `lua()`, package
//! registration, require/entrypoint, any Buffer type-cache initialization or instance,
//! and any Lua execution. Even obtaining trusted `lua()` access seals attachment:
//! low-level userdata constructors must never precede this setup boundary. Do not
//! re-register Buffer with mlua directly (that would split its metatable cache).
//!
//! Hosts compose their concrete owned/mapped/static representations behind one table
//! port. Recognized receiver failures are errors, never `NotHandled`. The ports have
//! no global/TLS state, active-mod gate, capability policy, FFI or archive types.
//! Keep all handles and ports on the phase's worker. A provider must not retain a
//! strong Lua/VM reference in its Rc (which would create a VM lifetime cycle), or
//! reuse Lua values/receiver handles in a different VM.
use super::MethodArgs;
use mlua::{AnyUserData, Lua, Value};
use std::rc::Rc;

/// Unsupported representation is distinct from success, including a removed nil.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Dispatch<T> {
    NotHandled,
    Handled(T),
}

/// The dispatcher performs SF argument-count/conversion checks before calling this
/// port. Positions have SF's `usize` conversion and remain one-based. Errors after
/// recognition (including borrow/static/bounds/validation failures) must propagate.
/// Mutations/cloning/dirty bytes/type validation belong to the concrete provider.
/// Use fallible receiver/state borrows; reentry must observe SF's borrow errors.
pub trait TypedTablePort {
    fn push(&self, lua: &Lua, receiver: AnyUserData, value: Value) -> mlua::Result<Dispatch<()>>;
    fn insert(
        &self,
        lua: &Lua,
        receiver: AnyUserData,
        position: usize,
        value: Value,
    ) -> mlua::Result<Dispatch<()>>;
    fn pop(&self, lua: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<Value>>;
    fn remove(
        &self,
        lua: &Lua,
        receiver: AnyUserData,
        position: usize,
    ) -> mlua::Result<Dispatch<Value>>;
    fn clear(&self, lua: &Lua, receiver: AnyUserData) -> mlua::Result<Dispatch<()>>;
}

/// Both callbacks extend the ONE `eml::buffer::Buffer` userdata, not a facade.
///
/// Borrow the actual receiver via `args.this::<&Buffer>()` / `<&mut Buffer>()`.
/// For reads call `data()` before resolving the opaque type; for writes call
/// `check_writable()` before type/value conversion. RETAIN that guard across
/// reader/codec/metatable callbacks so reentry sees the source borrow conflict.
/// Read snapshot/cursor and write encoding semantics belong to the provider.
/// Resolve types in the PHASE AppState registry, not an active package capability
/// or world-readiness gate: Buffers/values may be shared across mod environments.
/// An admitted phase requiring a missing provider must fail host setup, not install
/// dummy successful methods. With no port both Lua methods remain absent.
pub trait BufferResourcePort {
    /// Exactly one Lua result, including legitimate nil.
    fn read_resource(&self, lua: &Lua, args: MethodArgs) -> mlua::Result<Value>;
    /// Exactly zero Lua results, never a nil/reason tuple.
    fn write_resource(&self, lua: &Lua, args: MethodArgs) -> mlua::Result<()>;
}

/// One immutable hook set per phase VM. No provider is inferred from namespace
/// presence; `None` preserves standalone behavior. A partial Buffer pair is not
/// representable. Hosts must attach before obtaining any Lua access.
#[derive(Clone, Default)]
pub struct VmAttachments {
    pub typed_tables: Option<Rc<dyn TypedTablePort>>,
    pub buffer_resources: Option<Rc<dyn BufferResourcePort>>,
}
