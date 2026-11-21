// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.values v1's typed graph: the VM-local values behind native/core/abi/services/kiln_eml_values.h,
//! in Rust, and its C table. The startup pass uses the same graph in-process.
//!
//! - `lifetime`: the protected reader the graph calls ([`LuaHost`]), host-object pins, per-node Source borrows, Source
//!   errors ([`Fault`]) and transport refusals ([`Refusal`]).
//! - `node`: ShroudForge's six value wrappers as graph nodes (caches, laziness, aliases, clones, dirtiness, iteration,
//!   `__tostring`, conversion).
//! - `input`: the reader-driven validator (the codec's read order over host objects).
//! - `arena`: tokens, the any-thread and worker halves of an arena, scopes and the deferred drain.
//! - `operations`: open/info/read/child/set/clone/export/result_copy and the array operations, with their fixed replies.
//! - `packet`: the frozen payload and error packets (schema/sfi/eml-values/).
//! - `abi`: the C table published as `kiln.eml.values`, its records, threads, errors and the view → registry
//!   resolution of `arena_create`.
//! - `reader`: the host's `KilnEmlLuaReaderV1` admitted and adapted to [`LuaHost`].
//!
//! Lifetime is the arena's and the Lua references' (owner arguments are attribution only); Lua runs only in the host,
//! on the arena's creator worker; nothing here crosses C.
pub mod abi;
pub mod arena;
pub(crate) mod input;
pub mod lifetime;
pub mod node;
pub mod operations;
pub mod packet;
pub(crate) mod reader;

pub use arena::{ArenaConfig, ArenaPins, ResourceSource, arena_close, arena_collect, arena_create, value_close};
pub use lifetime::{
    Access, Adapter, CheckKind, Fault, Found, HostError, HostKind, HostValue, LeaseDisposition, LuaHost, Refusal,
    Transport, UserdataKind, Waker,
};
pub use operations::{
    DirtyRoot, OpenRequest, Reply, Selector, startup_dirty_roots, value_array_clear, value_array_insert,
    value_array_pop, value_array_push, value_array_remove, value_child, value_clone, value_export, value_info,
    value_open, value_read, value_result_copy, value_set,
};
