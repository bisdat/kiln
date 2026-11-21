// SPDX-License-Identifier: MIT
//! The interpreter runtime: the families that implement
//! ShroudForge's `runtime` API on Kiln's services.
//!
//! The seam every family uses:
//! - [`services`]: the [`ServiceAdapter`](services::ServiceAdapter), the only path to
//!   Kiln's services (checked lookup, header, member extent and non-null; timed calls
//!   for members that report `source_wait_ns`; callback data copied out);
//! - [`context`]: the [`RuntimeContext`] each family registrar receives;
//! - [`install`]: the installer order and the one-owner-per-namespace rule.
//!
//! A runtime family never looks a service up or calls a Kiln member another way.
//!
//! The registrar's production host:
//! - [`operation_catalog`]: the 31 operations and the provider facts, each with what backs
//!   it (the interpreter, a landed family's members and capability rows, or not yet);
//! - [`availability`]: [`KilnRuntimeHost`](availability::KilnRuntimeHost) and the
//!   runtime-create's [`RuntimeFacts`](availability::RuntimeFacts);
//! - [`write_fault`]: the interpreter-local ShroudForge session write latch.
//!
//! [`overrides`], the ownership store, `RuntimeWriteClaim`, the claim
//! factories and the generic override target every target-kind family builds on.
//!
//! [`world::context`], `runtime.world.session_id`, `context_active`,
//! `context_kind` and `operation_available` on Kiln's own world facts and the worker's lifecycle stream.
//!
//! The placement and patch refusals, refusal-only on current profiles (no `worldOperations`, no `runtimePatches`):
//! - [`world::placement`]: `runtime.world.entity.place`, `.finish_building`, the spatial
//!   `.destroy`;
//! - [`patch`]: `runtime.patch.available` / `.set_enabled` and `runtime.gameplay.patch`.
//!
//! [`ecs`], the ECS registry and catalog (`runtime.ecs.get_catalog`, `get_registry`,
//! `get_component`, `get_components`) on kiln.types' `ecs_registry` joined with the archive registry, and the entity
//! queries and component reads (`query`, `query_bounds`, `resolve`, `read`, `read_bytes`, `read_fields`,
//! `get_template`) on kiln.world's ECS reads, with ShroudForge's entity handles, and the plain component writes
//! (`write`, `write_fields`, `runtime.overrides.get_component_claim` and `component`) on its compare-exchange.
//!
//! [`named_services`], `runtime.services.get` and its views on kiln.world's named services (and, in
//! [`overrides::targets::service`], the service claim and override target).
//! [`assets`], `game.assets`' loaded-resource members (`get_loaded_resource`, `get_loaded_resources_by_type`,
//! `read_loaded_field`, `resolve_loaded_resource`) on kiln.assets' loaded-resource reads.
//!
//! [`network`], the `runtime.network` family core (status, send, receive, accept) on
//! kiln.net's Steam-messages lease.
//!
//! [`reload`], the host side of ShroudForge's live reload/unload request and result files (the worker side
//! is the lifecycle host's turn start).
//!
//! [`ui`], the runtime's end of the UI bridge with Kiln.Ui (action and notice files, owner epochs) and
//! ShroudForge's `shroudforge.ui` / `shroudforge.notifications` on it.
//!
//! [`input`], the keybind members of `shroudforge.input` (`is_keybind_down`, `was_keybind_pressed`,
//! `keybind_label`) on the input feed and the package's settings registry.
//!
//! [`registrar`], the production family list, ending with [`unavailable`]: ShroudForge's answer for every
//! member no merged family implements yet.
//!
//! [`values`] is the frontend of ShroudForge's typed values over kiln.eml.values (its reader, arena,
//! userdata and table port).

pub mod assets;
pub mod availability;
pub mod context;
pub mod ecs;
pub mod input;
pub mod install;
pub mod named_services;
pub mod network;
pub mod operation_catalog;
pub mod overrides;
pub mod patch;
pub mod registrar;
pub mod reload;
pub mod services;
pub mod types;
pub mod ui;
pub mod unavailable;
pub mod values;
pub mod world;
pub mod write_fault;

pub use availability::{KilnRuntimeHost, RuntimeFacts};
pub use context::RuntimeContext;
pub use install::{Families, Family};
pub use services::{ServiceAdapter, ServiceError, Unavailable};
pub use write_fault::{WriteFaultLatch, WriteOutcome};
