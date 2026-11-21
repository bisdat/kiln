// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Both pins (1.7.9 d8a394bc, 1.8.0 38ef448d):
// modules/mod-runtime/ingame/update_mod_callbacks.rs:49-64 (advance_deadline).
//! Worker-local lifecycle orchestration, not worker activation or engine admission.
//!
//! The host registers admitted packages and their binding handles before `start`, captures
//! each `OwnerState` in its runtime port, and serially calls `turn`. No lifecycle
//! bookkeeping borrow is retained across Lua/cleanup calls. The host owns watching,
//! frozen admission, engine receipts and the reviewed RuntimeWorkerState integration.
//!
//! Shutdown is explicit: call `stop(Normal)` while restoration is legal, and retain
//! the runtime while cleanup is pending. At late process exit call `stop(ProcessExit)`;
//! it makes no Lua/engine calls. Drop intentionally performs no implicit restoration:
//! Rust destruction cannot determine whether game memory is still accessible.
pub mod accounting;
pub mod configuration;
pub mod reload;
mod runtime;

pub use accounting::{CallbackAccounting, DeferredWait, WatchdogAccounting};
pub use configuration::{ConfigurationObservation, Observation};
pub use reload::ReloadProtocol;
pub use runtime::{
    CallbackPort, Cleanup, LifecycleRuntime, OwnerRegistration, OwnerState, RuntimeSetup,
    StopPhase, StopReport,
};

use crate::{bindings::loader::LoaderSnapshot, package::discover::RegistrySnapshot};
use std::time::Duration;

/// One host-owned monotonic timeline, retained across whole-runtime replacements.
/// Never pause this clock during a deferred wait: only watchdog accounting pauses.
pub trait Monotonic {
    fn now(&self) -> Duration;
}

/// Discovered membership, NOT plan/admission/activation membership. Construct
/// once per complete discovery/replacement and pass through the bindings' registrar.
/// Source (both pins): lua/eml/v1/create_loader_api.rs:7-28, relative to mod-runtime.
pub fn loader_snapshot(registry: &RegistrySnapshot, patch: bool, export: bool) -> LoaderSnapshot {
    LoaderSnapshot {
        patch,
        export,
        discovered_ids: registry.ids().map(str::to_owned).collect(),
    }
}

/// Both pins: ingame/update_mod_callbacks.rs:49-64. Advance the OLD phase,
/// skipping missed periods, with the source u32/checked-add fallback.
/// Duration's finite maximum also saturates the otherwise unrepresentable fallback.
pub fn advance_deadline(planned: Duration, interval: Duration, completed: Duration) -> Duration {
    assert!(!interval.is_zero(), "lifecycle interval must be positive");
    let periods = completed.saturating_sub(planned).as_nanos() / interval.as_nanos() + 1;
    u32::try_from(periods)
        .ok()
        .and_then(|n| planned.checked_add(interval.saturating_mul(n)))
        .filter(|next| *next > completed)
        .unwrap_or_else(|| completed.saturating_add(interval))
}
