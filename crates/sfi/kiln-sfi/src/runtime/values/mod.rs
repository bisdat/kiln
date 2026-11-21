// SPDX-License-Identifier: MIT
//! The interpreter's typed values on Kiln's provider, kiln.eml.values v1
//! (native/core/abi/services/kiln_eml_values.h, docs/kiln/services/eml-values.md).
//!
//! ShroudForge's typed-value Lua API (owned and mapped structs, arrays and variants, their iteration, typed
//! `table.insert/remove/clear`, and the values families open from resources) runs on the provider's graph, which ports
//! Source's semantics. This module is the MIT frontend of that seam:
//! - [`lua_reader`]: the protected reader the provider calls back for every read of a Lua object (tokens, protected
//!   requests, proxy interning, `wake`);
//! - [`arena`]: one arena per VM, the members called through the service adapter, replies, packets, errors and lifetime;
//! - [`packets`]: the frozen v1 payload and error packets;
//! - [`proxies`]: the userdata Lua sees, in ShroudForge's shapes;
//! - [`tables`]: the typed table port for sfi-core's dispatcher;
//! - [`host`]: one VM's arena and its archive view as the lifecycle host holds them in production.
//!
//! The VM's host creates the [`ValuesArena`] on the worker after attaching [`ValuesTables`] to the VM, keeps it outside
//! Lua, and drops (or closes) it before the VM and before the worker exits.

pub mod arena;
pub mod host;
pub mod lua_reader;
pub mod packets;
pub mod proxies;
pub mod tables;

pub use arena::{ArenaOptions, FaultSink, Selector, Values, ValuesArena, ValuesError};
pub use host::VmValues;
pub use lua_reader::{KnownUserdata, ReaderCounters, WakeSignal};
pub use proxies::type_index_of;
pub use tables::ValuesTables;
