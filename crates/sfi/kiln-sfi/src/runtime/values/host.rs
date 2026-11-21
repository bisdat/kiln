// SPDX-License-Identifier: MIT
// the interpreter's typed values, production wiring.
//! One Lua VM's typed values in production, as the lifecycle host holds them
//! ([`crate::lifecycle`]: runtime-create's VM and each live reload's new VM get their own).
//!
//! - [`VmValues::create`], on the worker after the VM's providers are attached (the typed table port,
//!   [`ValuesTables`](super::ValuesTables)) and before any package is registered: a view of the archive generation
//!   granted to this start ([`GrantedView`], acquired under kiln-sfi's owner), then the VM's arena over that view
//!   (`KILN_VALUE_RUNTIME`). No userdata is known besides the typed values until the families that own Guid, Content
//!   and Resource userdata supply theirs. No `notify`: the worker's park ignores unparks, so the turn's end collects a
//!   wake ([`VmValues::collect_if_woken`]). Kiln faults go to kiln.log through `on_fault`. A failure is the VM's
//!   setup failure.
//! - [`VmValues::close`]: `arena_close`, then `arena_collect` until the provider's pending count is 0, while the VM
//!   still lives (the arena holds it). The view is released only after the arena drained (every `value_open` passes
//!   it); an arena that never drains keeps its view for good, as it keeps its reader. So does one the provider may
//!   have admitted although its create failed (OK without a control token): only a refusal releases the view.
//! - Dropping one closes it first (idempotent), then lets go of the VM, then releases the view.
//! - [`VmValues::forget`] is the fast leave: nothing is closed or released, no teardown code runs; the
//!   provider's worker-exit revocation frees the graph without calling the reader.

use super::arena::{ArenaOptions, FaultSink, Values, ValuesArena, ValuesError};
use crate::runtime::services::ServiceAdapter;
use crate::runtime::types::GrantedView;
use kiln_abi::KilnOwner;
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use kiln_abi::services::kiln_eml_values::KILN_VALUE_RUNTIME;
use mlua::Lua;
use std::rc::Rc;

/// The start of the VM setup failure when its typed values cannot be set up.
pub const SETUP_FAILED: &str = "typed values (kiln.eml.values)";

/// One VM's arena and the view it was created over (module docs).
pub struct VmValues {
    // Field order is drop order: the arena (closed and drained while the VM it holds lives), then the view.
    arena: ValuesArena,
    view: Option<GrantedView>,
}

impl VmValues {
    /// A view of the generation `identity` pins and the arena of `lua` over it, under `owner` (kiln-sfi's plugin
    /// owner), on this thread (the VM's worker). The error is the VM's setup failure text.
    pub fn create(
        services: &Rc<ServiceAdapter>,
        lua: &Lua,
        owner: KilnOwner,
        identity: &KilnEmlArchiveIdentityV1,
        on_fault: FaultSink,
    ) -> Result<Self, String> {
        let view = GrantedView::acquire(services, owner, identity)
            .map_err(|e| format!("{SETUP_FAILED}: the archive view: {e}"))?;
        let arena = match ValuesArena::create(
            services,
            lua,
            owner,
            view.view(),
            ArenaOptions {
                phase: KILN_VALUE_RUNTIME as u32,
                known: None,
                notify: None,
                on_fault: Some(on_fault),
            },
        ) {
            Ok(arena) => arena,
            Err(error) => {
                // A refused create (or a table without the service) admitted no arena: the view drops (released)
                // with the error. Any other failure may have left an arena the provider admitted and the host can
                // never close (OK without a control token): its reader is kept for good, and so is this view.
                if !matches!(
                    error,
                    ValuesError::Refused { .. } | ValuesError::Unavailable(_)
                ) {
                    view.keep();
                }
                return Err(format!("{SETUP_FAILED}: {error}"));
            }
        };
        Ok(Self {
            arena,
            view: Some(view),
        })
    }

    /// The handle the VM's families open values through.
    pub fn values(&self) -> Values {
        self.arena.values()
    }

    /// Collects what a wake signalled (the worker's turn end).
    pub fn collect_if_woken(&self) -> Option<u32> {
        self.arena.collect_if_woken()
    }

    /// Closes the arena and drains it while the VM lives. Idempotent; whether the drain completed.
    pub fn close(&self) -> bool {
        self.arena.close()
    }

    /// Whether the arena was closed and drained.
    pub fn drained(&self) -> bool {
        self.arena.drained()
    }

    /// The fast leave: neither the arena nor the view is touched again (no close, no drain, no release), and the VM
    /// reference the arena holds is never dropped.
    pub fn forget(self) {
        std::mem::forget(self);
    }
}

impl Drop for VmValues {
    fn drop(&mut self) {
        if !self.arena.close()
            && let Some(view) = self.view.take()
        {
            // the provider may still use what the arena reached through the view
            view.keep();
        }
    }
}
