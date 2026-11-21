// SPDX-License-Identifier: MIT
//! The interpreter worker: one LongRunning, RESIDENT kiln.tasks task under
//! kiln-sfi's own plugin owner.
//!
//! - All Lua runs in its body, and nothing else enters Lua.
//! - [`RuntimeWorkerState`] lives on the worker thread only (`!Send`);
//!   [`WorkerShared`] is the only thing other threads touch.
//! - Producers reach it through the lock-free [`Inbox`] and never wait.
//! - It turns at ShroudForge's cadence: a 16 ms wait, then a turn.
//! - Exit, drain and revoke are seen through `cancelled()` and end in a fast
//!   leave; kiln-sfi's own stop ends in an orderly one.

pub mod compose;
mod inbox;
mod shared;
mod state;
mod task;

pub use inbox::{
    Accepted, COMMAND_CLASSES, CommandClass, ConfigurationObservation, Consumer, Inbox,
    InboxConfig, InboxCounters, InputSnapshot, LifecycleEvent, NetworkAuth, Refused, ReloadRequest,
    UiAction, WORLD_KINDS, WorkerCommand, WorldLifecycle,
};
pub use shared::{
    LeaseSlot, StopReason, WorkerConfig, WorkerPhase, WorkerShared, WorkerStatus, refusal_text,
};
pub(crate) use state::InterruptProbe;
pub use state::{
    EmptyRuntime, Interrupt, RuntimeFactory, RuntimeWorkerState, StopSentinel, TURN_GAP,
    WorkerContext, WorkerRuntime, empty_runtime, stop_sentinel, stop_sentinel_in,
};
pub use task::{
    RESIDENT_SINCE, StartError, TASK_NAME, TasksTable, Worker, predates_resident, submit,
};

use kiln_abi::*;

type VersionFn = unsafe extern "C" fn() -> KilnStr;
type LogFn = unsafe extern "C" fn(KilnOwner, i32, KilnStr);
type ServiceFn = unsafe extern "C" fn(KilnStr, u32) -> *const core::ffi::c_void;
type SubscribeTickFn = unsafe extern "C" fn(
    KilnOwner,
    i32,
    KilnTickFn,
    *mut core::ffi::c_void,
    *mut KilnHandle,
) -> KilnResult;
type SubscribeWorldFn = unsafe extern "C" fn(
    KilnOwner,
    KilnWorldEventFn,
    *mut core::ffi::c_void,
    *mut KilnHandle,
) -> KilnResult;
type SessionFn = unsafe extern "C" fn(*mut KilnSessionInfo) -> KilnResult;
type PostWorldFn =
    unsafe extern "C" fn(KilnOwner, u32, KilnTaskFn, *mut core::ffi::c_void) -> KilnResult;

/// The `KilnApi` members the worker uses, copied once after a size check. Every
/// member is optional: an older core's shorter table leaves the later ones `None`.
#[derive(Clone, Copy, Default)]
pub struct Host {
    version: Option<VersionFn>,
    log: Option<LogFn>,
    service: Option<ServiceFn>,
    pub(crate) subscribe_tick: Option<SubscribeTickFn>,
    pub(crate) subscribe_world: Option<SubscribeWorldFn>,
    pub(crate) session: Option<SessionFn>,
    pub(crate) post_world: Option<PostWorldFn>,
}

impl Host {
    /// # Safety
    /// `api` is NULL or a `KilnApi` readable for its declared `size`.
    pub unsafe fn read(api: *const KilnApi) -> Self {
        if api.is_null() || !api.is_aligned() {
            return Self::default();
        }
        // SAFETY: the table starts with its size (kiln_api.h).
        let size = unsafe { api.cast::<u32>().read() } as usize;
        let has = |offset: usize| size >= offset + core::mem::size_of::<usize>();
        // SAFETY: each member is read only when `size` covers it; no reference to
        // the whole table is formed.
        macro_rules! member {
            ($name:ident) => {
                if has(core::mem::offset_of!(KilnApi, $name)) {
                    core::ptr::addr_of!((*api).$name).read()
                } else {
                    None
                }
            };
        }
        unsafe {
            Self {
                version: member!(version),
                log: member!(log),
                service: member!(service),
                subscribe_tick: member!(subscribe_tick),
                subscribe_world: member!(subscribe_world),
                session: member!(session),
                post_world: member!(post_world),
            }
        }
    }

    /// `KilnApi.version()`, copied without surrounding whitespace: `None` when the core
    /// has no such member, or its answer is empty or blank, longer than 256 bytes or not
    /// UTF-8.
    pub fn kiln_version(&self) -> Option<String> {
        // SAFETY: the core's version string lives for the process (kiln_api.h basics).
        let text = unsafe { (self.version?)() };
        if text.ptr.is_null() || text.len == 0 || text.len > 256 {
            return None;
        }
        // SAFETY: `ptr` holds `len` readable bytes (KilnStr).
        let bytes = unsafe { core::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) };
        let text = core::str::from_utf8(bytes).ok()?.trim();
        (!text.is_empty()).then(|| text.to_owned())
    }

    /// `KilnApi.log`; quiet when the core has none.
    pub fn log(&self, owner: KilnOwner, level: i32, text: &str) {
        if let Some(log) = self.log {
            // SAFETY: the core's log copies the text during the call.
            unsafe {
                log(
                    owner,
                    level,
                    KilnStr {
                        ptr: text.as_ptr().cast(),
                        len: text.len(),
                    },
                )
            };
        }
    }

    /// `KilnApi.session` (any thread): `None` when this core has no `session` member or
    /// it refused. Fields an older core does not fill stay zero (`lifecycle` among them).
    pub fn session_info(&self) -> Option<KilnSessionInfo> {
        let session = self.session?;
        // SAFETY: zeroed POD; the core fills `size` bytes of it.
        let mut info: KilnSessionInfo = unsafe { core::mem::zeroed() };
        info.size = core::mem::size_of::<KilnSessionInfo>() as u32;
        // SAFETY: valid member, live out pointer.
        if unsafe { session(&mut info) } != KILN_OK {
            return None;
        }
        Some(info)
    }

    /// The live session of world `kind` (`KilnApi.session`, any thread): `None` when
    /// that world has none, or this core cannot say (no `session` member).
    pub fn live_session(&self, kind: u32) -> Option<u32> {
        let info = self.session_info()?;
        let kind = kind as usize;
        (*info.worlds.get(kind)? != 0).then(|| info.sessions[kind])
    }

    /// Whether this core's `KilnApi` has the `service` lookup.
    pub fn has_service_lookup(&self) -> bool {
        self.service.is_some()
    }

    /// `KilnApi.service(name, min_version)`; NULL when absent. `name` may end in
    /// one NUL (the generated `*_SERVICE` constants do); it is not passed on.
    pub fn service(&self, name: &[u8], min_version: u32) -> *const core::ffi::c_void {
        let name = name.strip_suffix(b"\0").unwrap_or(name);
        match self.service {
            // SAFETY: the core's lookup reads `name` during the call only.
            Some(service) => unsafe {
                service(
                    KilnStr {
                        ptr: name.as_ptr().cast(),
                        len: name.len(),
                    },
                    min_version,
                )
            },
            None => core::ptr::null(),
        }
    }
}
