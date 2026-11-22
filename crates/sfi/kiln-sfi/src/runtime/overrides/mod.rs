// SPDX-License-Identifier: MIT
//! ShroudForge's cooperative runtime write claims, the part of
//! `runtime.overrides` every target kind shares.
//!
//! - [`store`]: the worker-local [`OwnershipStore`](store::OwnershipStore), one per
//!   runtime-create (`RuntimeFacts::ownership`), shared by every package: whole-storage
//!   claims per mod, several tokens of one mod on one storage (refcounted), session-bound
//!   keys for components and named services, session-independent keys for resources.
//! - [`claims`]: the [`RuntimeWriteClaim`](claims::RuntimeWriteClaim) token (Lua userdata
//!   with `acquire` / `release`), the [`ClaimAuthority`](claims::ClaimAuthority) it checks
//!   against (the package's runtime host and the live world session), and the claim
//!   factories each family's `get_*_claim` calls once it has resolved its storage identity.
//! - [`target`]: the [`ClaimTarget`](target::ClaimTarget) trait, the generic override
//!   target schema (`key / claim / shared / read / write / normalize`), and
//!   [`target_table`](target::target_table), which gives a target ShroudForge's Lua shape.
//!
//! - [`tracker`]: ShroudForge's override tracker, `runtime.overrides.new` (the only
//!   name installed here, inside the installer's `runtime.overrides` container), and
//!   [`OverrideTrackers`](tracker::OverrideTrackers), the lifecycle's cleanup port over it.
//!
//! The `get_*_claim` functions and the target adapters inside `runtime.overrides` come with
//! the families that resolve each target kind (component, service, resource,
//! template, attribute). No engine
//! call is made here; the target-specific acquire check (a component's layout and write
//! authority) is the family's [`AcquireCheck`](claims::AcquireCheck).

pub mod claims;
pub mod store;
pub mod target;
pub mod targets;
pub mod tracker;

pub use claims::{
    AcquireCheck, ClaimAuthority, HostAuthority, RuntimeWriteClaim, SESSION_REFUSAL, check_write,
    claim_result, component_claim, component_key, get_claim, resource_claim, service_claim,
    template_claim,
};
pub use store::{OwnershipStore, ResourceKey, StorageKey, TOKEN_EXHAUSTED};
pub use target::{ClaimTarget, Written, target_table};
pub use tracker::{OverrideTrackers, session_world_kind};
