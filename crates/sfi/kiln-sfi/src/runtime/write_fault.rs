// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins): modules/mod-runtime/session/record_runtime_state.rs:38-59 in 1.7.9, 43-64
// in 1.8.0 (the latch and its reader), lua/ingame/check_ingame_access.rs:404-408 (where the reader
// denies), lua/ingame/access_named_service.rs:231-233, lua/ingame/resources/
// write_loaded_field.rs:92-94 and lua/ingame/ecs/access_template_component.rs:455-457 (what
// latches: a write refusal whose reason contains "uncertain"), game-runtime/native/ecs/
// write_entity_components.cpp:111,154 and engine-context/lock_scene_access.cpp:171 (the
// reason texts).
//! The interpreter-local ShroudForge write-fault latch.
//!
//! ShroudForge keeps one latch for the process: the first uncertain runtime write of a world
//! session records `(session, reason)`, and from then on, while that session is still the
//! live one, `runtime.ecs.write` and `runtime.assets.write` are denied with that reason
//! (`runtime_denial_reason`, `runtime.get_operations`). A new session ends it; session 0
//! never latches and never reports. `runtime.has` / `require` / `status` never read it.
//!
//! Kiln reports outcomes in its own terms (`KILN_ECS_UNCERTAIN`, a CAS conflict after the
//! last retry, later `kiln-gate-busy`). [`REASONS`] maps each one to ShroudForge's exact
//! reason text and says whether it latches; the callers (the CAS core and the guarded-write
//! callers) report a [`WriteOutcome`] and answer with the text they get back. A change that maps a
//! new Kiln outcome adds a variant and its row, nothing else (for example `kiln-gate-busy`
//! to ShroudForge's busy reason). A row latches exactly when its text contains
//! "uncertain", ShroudForge's own rule; the tests hold every row to it.
//!
//! The latch belongs to one runtime-create (the worker thread's), shared by every package.

use std::cell::Cell;

/// A write outcome Kiln reported, in Kiln's terms.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteOutcome {
    /// `ecs_compare_exchange` answered `KILN_ECS_UNCERTAIN`: a write was attempted and could
    /// not be proven.
    EcsWriteUncertain,
    /// `ecs_compare_exchange` still answered CONFLICT on the last of its three holds
    /// (Kiln's own retry bound).
    EcsWriteConflictExhausted,
}

/// One mapping: Kiln's outcome, ShroudForge's reason text, and whether it latches.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReasonRow {
    pub outcome: WriteOutcome,
    pub reason: &'static str,
    pub latches: bool,
}

/// Every outcome's row, exactly once.
pub const REASONS: &[ReasonRow] = &[
    // write_entity_components.cpp:111,154: the session's writes stop.
    ReasonRow {
        outcome: WriteOutcome::EcsWriteUncertain,
        reason: "ecs-write-outcome-uncertain-rollback-unverified",
        latches: true,
    },
    // lock_scene_access.cpp:171: the frame changed the bytes between the holds.
    ReasonRow {
        outcome: WriteOutcome::EcsWriteConflictExhausted,
        reason: "ecs-scene-frame-busy",
        latches: false,
    },
];

/// The row of `outcome`.
pub fn reason(outcome: WriteOutcome) -> &'static ReasonRow {
    REASONS
        .iter()
        .find(|row| row.outcome == outcome)
        .expect("every write outcome has a row")
}

/// The latched fault: the world it happened in, that world's session, and the reason.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WriteFault {
    pub world_kind: u32,
    pub session: u64,
    pub reason: &'static str,
}

/// The session write latch of one runtime-create (`AppState.runtime_write_fault`).
#[derive(Debug, Default)]
pub struct WriteFaultLatch {
    fault: Cell<Option<WriteFault>>,
}

impl WriteFaultLatch {
    pub fn new() -> Self {
        Self::default()
    }

    /// A write in session `session` of world `world_kind` (`game_world_session`, the
    /// session in the write's ref and reply) ended with `outcome`: returns ShroudForge's
    /// reason text for it and latches when its row says so
    /// (`record_uncertain_runtime_write`): never for session 0, and the first latched
    /// reason of a session stays.
    pub fn report(&self, world_kind: u32, session: u64, outcome: WriteOutcome) -> &'static str {
        let row = reason(outcome);
        if row.latches && session != 0 {
            let same = self
                .fault
                .get()
                .is_some_and(|f| f.world_kind == world_kind && f.session == session);
            if !same {
                self.fault.set(Some(WriteFault {
                    world_kind,
                    session,
                    reason: row.reason,
                }));
            }
        }
        row.reason
    }

    /// The latched fault, whether or not its session is still live.
    pub fn fault(&self) -> Option<WriteFault> {
        self.fault.get()
    }

    /// ShroudForge's `runtime_write_fault()`: the reason while the latched session is the
    /// live one. `live` gives the current session of a world kind (0: none).
    pub fn active(&self, live: impl FnOnce(u32) -> u64) -> Option<String> {
        let fault = self.fault.get()?;
        let session = live(fault.world_kind);
        (session != 0 && session == fault.session).then(|| fault.reason.to_owned())
    }
}
