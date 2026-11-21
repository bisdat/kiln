// SPDX-License-Identifier: MIT
//! Worker-local production Lua, shared once per phase, with owner-local environments.
//!
//! # Binding interface
//! `LuaVm::add_package` builds the base environment and owner-captured `require`, then calls
//! the supplied registrar with `(&Lua, &Table, &Package)`. Capture that package identity and
//! a clone of the host inputs when registering closures. `install_namespace` installs the
//! **same** table into `env[name]` and `env.builtin[name]`; later reassignments are independent.
//! Other API families must install real bindings here, not successful no-op placeholders.
//!
//! Use `FunctionArgs` / `MethodArgs` with `get::<T>(zero_based_index)` for SF bindings,
//! not mlua's tuple coercions. `CastLua` is extensible for family-owned userdata markers;
//! `CastLuaExt` supports the same conversion outside arguments. Missing positional values
//! are nil, while `len()` / `is_empty()` / `value()` retain presence and trailing nils.
//! `String` accepts UTF-8 strings and Rust-formatted numbers; `&[u8]` accepts only strings
//! and borrows every byte. `Option<T>` accepts nil only; `Either<A,B>` tries A first.
//! Numeric casts deliberately retain SF's signed-bit, fractional and saturation quirks.
//! Half-float conversion belongs to the later codec family (no half dependency here).
//! `LuaError` constructs SF external errors; do not flatten them into runtime strings or
//! generic nil/reason tuples. Methods can return `Value::Nil` for **one nil**, `()` for
//! **no values**, or `MultiValue` for exact multiple returns. Standard-library functions
//! remain Lua's own closures and argument-presence rules, not this custom conversion layer.
//! The environment installs SF's single table insert/remove/clear dispatcher. Attach
//! typed-container and Buffer resource ports with `LuaVm::attach` immediately after
//! construction, before `lua()` or package registration; see `attachments`. Do not
//! wrap/replace these closures: that would add an observable callback error frame.
//!
//! The registrar runs before execution. Register all packages, then call `require` or
//! `entrypoint`. The latter uses the distinct `mods:<exact-id>` cache. Configuration,
//! callback discovery, lifecycle/scheduling and watchdog policy are separate owners.
//! No Lua reference may leave its serial worker; this module enables no Send support.
//!
//! ```compile_fail
//! fn requires_send<T: Send>() {}
//! requires_send::<sfi_core::lua::LuaVm>();
//! ```

pub mod arguments;
pub mod attachments;
pub mod environment;
pub mod errors;
pub mod require;
pub mod values;
pub mod vm;

pub use arguments::{Args, FunctionArgs, MethodArgs};
pub use attachments::{BufferResourcePort, Dispatch, TypedTablePort, VmAttachments};
pub use environment::install_namespace;
pub use errors::LuaError;
pub use mlua::Value as LuaValue;
pub use values::{CastLua, CastLuaExt, Either};
pub use vm::LuaVm;
