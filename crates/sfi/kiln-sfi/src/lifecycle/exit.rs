// SPDX-License-Identifier: MIT
//! The plugin side of the lifecycle host, outside the worker: kiln-sfi's tick and
//! world-lifecycle subscriptions, made once the worker is accepted and kept for the
//! process's life (a quarantine is pool-only, so status stays visible).
//!
//! - The world subscriber turns `KilnWorldEvent`s into `WorldLifecycle` commands.
//!   It only enqueues and unparks; it never waits.
//! - The tick drains the task owner after an interpreter stop: once kiln-sfi's own stop has
//!   ended in `Stopped` (done ran), it calls `drain_owner(owner, 0)` once to seal
//!   the task owner and cancel its queued work, then polls `drain_owner(owner, 0)` on
//!   later ticks until nothing runs. Never a timed drain on the tick
//!   (kiln_tasks.h:16-19), and never while the worker still runs, which would turn
//!   the orderly leave into a fast one.
//! - Once the phase is `Faulted` (the worker's state is gone with its frames), the tick
//!   closes the runtime's Steam-messages lease, once ([`close_lease_after_fault`]); an orderly
//!   leave closes it on the worker after the Lua stop instead, and a fast leave never does.
//! - Exit is not driven from here: the pool's signal reaches the worker through
//!   `cancelled()`, and kiln-sfi installs no on_unload that waits for it.

use crate::worker::{
    LifecycleEvent, Worker, WorkerCommand, WorkerPhase, WorkerShared, WorldLifecycle,
};
use kiln_abi::*;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

/// Subscribes the tick and the world lifecycle under kiln-sfi's owner. A missing
/// member (an older core) only loses that feature; it never blocks the worker.
pub(crate) fn subscribe(worker: &'static Worker, shared: &WorkerShared) {
    let host = *shared.host();
    let user = (worker as *const Worker).cast_mut().cast::<c_void>();
    let mut handle = 0;
    if let Some(subscribe_tick) = host.subscribe_tick {
        // SAFETY: valid KilnApi member; `user` is 'static.
        let result = unsafe {
            subscribe_tick(
                shared.owner(),
                KILN_TICK_POST,
                Some(on_tick),
                user,
                &mut handle,
            )
        };
        if result != KILN_OK {
            host.log(
                shared.owner(),
                KILN_LOG_WARN,
                "kiln-sfi: tick subscription refused; an interpreter stop will not drain its owner",
            );
        }
    }
    if let Some(subscribe_world) = host.subscribe_world {
        // SAFETY: as above.
        let result = unsafe { subscribe_world(shared.owner(), Some(on_world), user, &mut handle) };
        if result != KILN_OK {
            host.log(
                shared.owner(),
                KILN_LOG_WARN,
                "kiln-sfi: world lifecycle subscription refused; mods see no world start or stop",
            );
        }
    }
}

/// What one tick did for the drain after a stop.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DrainStep {
    /// Nothing to do: no interpreter stop, or the worker has not left yet.
    Idle,
    /// Sealed or polled; `running` bodies or dones still outstanding.
    Draining { running: u32 },
    /// Quiet: every body and done of kiln-sfi's owner has returned.
    Quiescent,
}

/// The tick's drain step after a stop. Never blocks.
pub fn drive_drain(shared: &WorkerShared) -> DrainStep {
    if shared.quiescent() {
        return DrainStep::Quiescent;
    }
    if !shared.stop_requested() || shared.phase() != WorkerPhase::Stopped {
        return DrainStep::Idle;
    }
    let Some(tasks) = shared.tasks.get() else {
        return DrainStep::Idle;
    };
    let (result, running) = tasks.drain_poll(shared.owner());
    if shared.mark_drained() {
        shared.host().log(
            shared.owner(),
            KILN_LOG_INFO,
            "kiln-sfi: interpreter stopped; its owner is sealed and its queued work cancelled \
             (no restart in this process)",
        );
    }
    if result == KILN_OK && running == 0 {
        shared.mark_quiescent();
        return DrainStep::Quiescent;
    }
    DrainStep::Draining { running }
}

/// The tick's lease step: after `phase = Faulted`, `kiln.net`'s
/// `steam_close` on the lease the runtime left behind, exactly once (whoever takes it from the
/// slot closes it). Never blocks; true when it closed one.
pub fn close_lease_after_fault(shared: &WorkerShared) -> bool {
    if shared.phase() != WorkerPhase::Faulted || shared.network_lease().get() == 0 {
        return false;
    }
    let lease = shared.network_lease().take();
    if lease == 0 {
        return false;
    }
    use kiln_abi::services::kiln_net::{KILN_NET_SERVICE, KILN_NET_VERSION, KilnNetApi};
    let host = shared.host();
    let table = host
        .service(KILN_NET_SERVICE, KILN_NET_VERSION)
        .cast::<KilnNetApi>();
    let slot = core::mem::offset_of!(KilnNetApi, steam_close);
    if table.is_null() || !table.is_aligned() {
        return false;
    }
    // SAFETY: a non-null, aligned service table starts with its header.
    let size = unsafe { core::ptr::addr_of!((*table).header.size).read() } as usize;
    if size < slot + core::mem::size_of::<usize>() {
        return false;
    }
    // SAFETY: the table's size covers the member.
    let Some(close) = (unsafe { core::ptr::addr_of!((*table).steam_close).read() }) else {
        return false;
    };
    // SAFETY: kiln_net.h steam_close: any thread, idempotent, never waits; kiln-sfi's owner opened the lease.
    let result = unsafe { close(shared.owner(), lease) };
    shared.host().log(
        shared.owner(),
        if result == KILN_OK {
            KILN_LOG_INFO
        } else {
            KILN_LOG_WARN
        },
        &format!("kiln-sfi: interpreter faulted; its Steam peer-to-peer lease is closed (result {result})"),
    );
    true
}

unsafe extern "C" fn on_tick(_: *const KilnTickInfo, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: `subscribe` passes a `&'static Worker`.
        let worker = unsafe { &*user.cast_const().cast::<Worker>() };
        if let Some(shared) = worker.shared() {
            close_lease_after_fault(shared);
            drive_drain(shared);
        }
    }));
}

unsafe extern "C" fn on_world(event: *const KilnWorldEvent, user: *mut c_void) {
    if user.is_null() || event.is_null() || !event.is_aligned() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the core passes a live event for the call; read the v1 fields only.
        let event = unsafe { *event };
        if (event.size as usize) < core::mem::size_of::<KilnWorldEvent>() {
            return;
        }
        let kind = match event.event as i32 {
            KILN_WORLD_STARTED => LifecycleEvent::Started,
            KILN_WORLD_STOPPING => LifecycleEvent::Stopping,
            _ => return,
        };
        // SAFETY: as in `on_tick`.
        let worker = unsafe { &*user.cast_const().cast::<Worker>() };
        if let Some(shared) = worker.shared() {
            if kind == LifecycleEvent::Stopping {
                // Flip first and wake a hop wait; it cancels a hop of this
                // session without waiting for the inbox's next drain.
                shared.note_world_stopping(event.kind, event.session);
            }
            let _ = shared.submit(WorkerCommand::WorldLifecycle(WorldLifecycle {
                world: event.kind,
                session: event.session,
                event: kind,
            }));
        }
    }));
}
