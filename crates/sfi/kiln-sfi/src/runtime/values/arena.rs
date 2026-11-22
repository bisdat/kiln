// SPDX-License-Identifier: MIT
//! The frontend's conversation with kiln.eml.values v1: one arena per Lua VM (one per phase), the
//! members called through the [`ServiceAdapter`], the fixed reply records and packets read back, Source errors
//! raised as the Lua errors Source raises, and the arena's lifetime.
//!
//! # Lifetime
//! [`ValuesArena`] belongs to the VM's host (outside Lua) and holds the VM strongly, so the VM cannot be destroyed
//! before the arena is closed: dropping it (or [`ValuesArena::close`]) calls `arena_close` and then `arena_collect`
//! on the worker until the provider's pending count is 0, while the VM still lives, so every token the provider
//! retained is released before the VM goes and before the worker thread exits (the provider's duty list in
//! docs/kiln/services/eml-values.md). Proxies hold only [`Values`] (the arena's core, no VM reference), so no
//! reference cycle runs through the VM. The reader's `user` stays valid until pending reached 0; if it never does,
//! it is leaked rather than freed under the provider.
//!
//! # Calls
//! Every frontend call into the provider runs inside a scope: temporaries die when the outermost scope ends, and a
//! wake that arrived meanwhile is collected right after. Owners are attribution only (the provider grants nothing by
//! owner): every call names kiln-sfi's plugin owner. No call waits, retries or hops.
//!
//! # Errors
//! A transport refusal (a non-OK KilnResult) or an unreadable reply is Kiln's own error ([`ValuesError`]). A
//! SEMANTIC_ERROR's packet is read, the Lua error is built (its host error referenced first), then the error lease
//! closes; the error is raised as Source raises it (RC-11): `Error::external(text)` for Source texts,
//! `UserDataBorrowError`/`UserDataBorrowMutError` for borrows, the original `mlua::Error` for a root `lua` level.
//! The two texts the provider cannot know are rendered here: Source's `userdata \`TypeId(..)\`` label is
//! the Debug form of the found userdata's Rust TypeId in this process, and Source's `guid expected, got ..` type name
//! is `{value:#?}` of a found userdata (its `__name` and address) or mlua's `type_name()` of a function or thread.

use super::lua_reader::{self, KnownUserdata, ReaderCounters, ReaderLocal, ReaderUser, WakeSignal};
use super::packets::{self, ErrorLevel, IterValue, Packet, Scalar, View};
use super::proxies;
use crate::runtime::services::{Member, ServiceAdapter, ServiceError};
use crate::service_member;
use kiln_abi::services::kiln_assets::KilnAssetKey;
use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::{KILN_ERR_BUFFER_TOO_SMALL, KILN_OK, KilnHandle, KilnOwner, KilnResult, KilnStr};
use mlua::{Lua, Value};
use std::cell::Cell;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

type ArenaCreateFn = unsafe extern "C" fn(
    KilnOwner,
    KilnEmlArchiveView,
    u32,
    *const KilnEmlLuaReaderV1,
    *mut KilnHandle,
) -> KilnResult;
type ArenaFn = unsafe extern "C" fn(KilnOwner, KilnHandle, *mut u32) -> KilnResult;
type OpenFn = unsafe extern "C" fn(
    KilnOwner,
    KilnHandle,
    KilnEmlArchiveView,
    *const KilnEmlValueOpenV1,
    *mut KilnEmlValueResultV1,
) -> KilnResult;
type InfoFn = unsafe extern "C" fn(KilnOwner, KilnHandle, *mut KilnEmlValueInfoV1) -> KilnResult;
type ReadFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, u32, *mut KilnEmlValueResultV1) -> KilnResult;
type ChildFn = unsafe extern "C" fn(
    KilnOwner,
    KilnHandle,
    *const KilnEmlValueSelectorV1,
    *mut KilnEmlValueResultV1,
) -> KilnResult;
type SetFn = unsafe extern "C" fn(
    KilnOwner,
    KilnHandle,
    *const KilnEmlValueSelectorV1,
    u64,
    *mut KilnEmlValueResultV1,
) -> KilnResult;
type UnaryFn = unsafe extern "C" fn(KilnOwner, KilnHandle, *mut KilnEmlValueResultV1) -> KilnResult;
type ExportFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, KilnStr, *mut KilnEmlValueResultV1) -> KilnResult;
type CloseFn = unsafe extern "C" fn(KilnOwner, KilnHandle) -> KilnResult;
type CopyFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, *mut c_void, usize, *mut usize) -> KilnResult;
type WithU64Fn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, u64, *mut KilnEmlValueResultV1) -> KilnResult;
type InsertFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, u64, u64, *mut KilnEmlValueResultV1) -> KilnResult;

/// The seventeen members, copied once from the checked table.
struct Members {
    arena_create: Member<ArenaCreateFn>,
    arena_close: Member<ArenaFn>,
    arena_collect: Member<ArenaFn>,
    value_open: Member<OpenFn>,
    value_info: Member<InfoFn>,
    value_read: Member<ReadFn>,
    value_child: Member<ChildFn>,
    value_set: Member<SetFn>,
    value_clone: Member<UnaryFn>,
    value_export: Member<ExportFn>,
    value_close: Member<CloseFn>,
    value_result_copy: Member<CopyFn>,
    value_array_push: Member<WithU64Fn>,
    value_array_insert: Member<InsertFn>,
    value_array_pop: Member<UnaryFn>,
    value_array_remove: Member<WithU64Fn>,
    value_array_clear: Member<UnaryFn>,
}

impl Members {
    fn bind(services: &ServiceAdapter) -> Result<Self, ServiceError> {
        let t = services
            .bind::<KilnEmlValuesApiV1>(KILN_EML_VALUES_SERVICE, KILN_EML_VALUES_VERSION)?;
        Ok(Self {
            arena_create: service_member!(t, KilnEmlValuesApiV1, arena_create)?,
            arena_close: service_member!(t, KilnEmlValuesApiV1, arena_close)?,
            arena_collect: service_member!(t, KilnEmlValuesApiV1, arena_collect)?,
            value_open: service_member!(t, KilnEmlValuesApiV1, value_open)?,
            value_info: service_member!(t, KilnEmlValuesApiV1, value_info)?,
            value_read: service_member!(t, KilnEmlValuesApiV1, value_read)?,
            value_child: service_member!(t, KilnEmlValuesApiV1, value_child)?,
            value_set: service_member!(t, KilnEmlValuesApiV1, value_set)?,
            value_clone: service_member!(t, KilnEmlValuesApiV1, value_clone)?,
            value_export: service_member!(t, KilnEmlValuesApiV1, value_export)?,
            value_close: service_member!(t, KilnEmlValuesApiV1, value_close)?,
            value_result_copy: service_member!(t, KilnEmlValuesApiV1, value_result_copy)?,
            value_array_push: service_member!(t, KilnEmlValuesApiV1, value_array_push)?,
            value_array_insert: service_member!(t, KilnEmlValuesApiV1, value_array_insert)?,
            value_array_pop: service_member!(t, KilnEmlValuesApiV1, value_array_pop)?,
            value_array_remove: service_member!(t, KilnEmlValuesApiV1, value_array_remove)?,
            value_array_clear: service_member!(t, KilnEmlValuesApiV1, value_array_clear)?,
        })
    }
}

/// Kiln's own failure of a typed-value call: never a Source text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ValuesError {
    /// The service or one of its members is missing (the family's unavailable shape).
    Unavailable(ServiceError),
    /// The provider refused a call (transport: NOT_FOUND, REVOKED, WRONG_THREAD, INVALID_ARGUMENT, ...).
    Refused {
        member: &'static str,
        code: KilnResult,
    },
    /// The arena is closed.
    Closed,
    /// A reply or packet this frontend cannot read, or a reply that contradicts the header.
    Fault(String),
}

impl std::fmt::Display for ValuesError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unavailable(error) => write!(f, "{error}"),
            Self::Refused { member, code } => {
                write!(f, "kiln.eml.values.{member} refused (KilnResult {code})")
            }
            Self::Closed => write!(f, "kiln.eml.values: the arena is closed"),
            Self::Fault(why) => write!(f, "kiln.eml.values: {why}"),
        }
    }
}
impl std::error::Error for ValuesError {}

impl ValuesError {
    /// The Lua error a script sees for a Kiln failure (Kiln's text, never presented as ShroudForge's).
    pub fn to_lua(&self) -> mlua::Error {
        mlua::Error::runtime(format!("kiln: {self}"))
    }
}

impl From<packets::PacketError> for ValuesError {
    fn from(error: packets::PacketError) -> Self {
        Self::Fault(error.to_string())
    }
}

/// Where Kiln faults go (the host's log).
pub type FaultSink = Rc<dyn Fn(&ValuesError)>;

/// How an arena is made.
#[derive(Clone)]
pub struct ArenaOptions {
    /// `KILN_VALUE_STARTUP` or `KILN_VALUE_RUNTIME`.
    pub phase: u32,
    /// The userdata Source's codec knows besides typed values (Guid, Content, Resource), if any family has them.
    pub known: Option<Rc<dyn KnownUserdata>>,
    /// Called from `wake` on whichever thread closed something: a signal only (it must not block, wait for the worker,
    /// enter Lua or call the service). The worker then calls [`ValuesArena::collect_if_woken`].
    pub notify: Option<Arc<dyn Fn() + Send + Sync>>,
    /// Kiln faults (transport refusals, unreadable replies) for the host's log.
    pub on_fault: Option<FaultSink>,
}

impl Default for ArenaOptions {
    fn default() -> Self {
        Self {
            phase: KILN_VALUE_RUNTIME as u32,
            known: None,
            notify: None,
            on_fault: None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Open,
    /// arena_close answered; pending did not reach 0 (the reader stays allocated for good).
    Undrained,
    Drained,
}

/// The arena's core: what proxies and the table port hold (no VM reference).
pub(crate) struct ArenaCore {
    services: Rc<ServiceAdapter>,
    m: Members,
    owner: KilnOwner,
    view: KilnEmlArchiveView,
    control: KilnHandle,
    state: Cell<State>,
    signal: Arc<WakeSignal>,
    on_fault: Option<FaultSink>,
    closes: Cell<u64>,
    last: Cell<(bool, u64)>,
    /// The reader's worker half and its `user`, both from `Box::into_raw`: the provider holds raw pointers into them,
    /// so they are never moved or uniquely borrowed after the reader table exists, only shared, and are freed together
    /// in `Drop` once the provider let go of them.
    local: *mut ReaderLocal,
    user: *mut ReaderUser,
}

impl Drop for ArenaCore {
    fn drop(&mut self) {
        if self.state.get() == State::Drained {
            // SAFETY: both came from Box::into_raw in `ValuesArena::create` and are freed once, here; the provider
            // holds no reference after pending reached 0.
            unsafe {
                drop(Box::from_raw(self.user));
                drop(Box::from_raw(self.local));
            }
        }
        // otherwise the provider may still call the reader: its user context is leaked, never freed under it
    }
}

impl ArenaCore {
    fn local(&self) -> &ReaderLocal {
        // SAFETY: from Box::into_raw, freed only in Drop; only shared references to it are ever made.
        unsafe { &*self.local }
    }
}

/// A handle on the arena for the frontend's typed-value conversation (the proxies, the table port and the families
/// that open values). Cheap to clone; holds no VM reference.
#[derive(Clone)]
pub struct Values(Rc<ArenaCore>);

/// The arena of one Lua VM. Owned by the VM's host, outside Lua (never store it in the VM: it holds the VM).
pub struct ValuesArena {
    values: Values,
    lua: Lua,
}

impl std::ops::Deref for ValuesArena {
    type Target = Values;
    fn deref(&self) -> &Values {
        &self.values
    }
}

impl Drop for ValuesArena {
    fn drop(&mut self) {
        self.close();
    }
}

/// A value selector (Source's key conversion already done).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Selector<'a> {
    Field(&'a [u8]),
    Index(u64),
}

impl Selector<'_> {
    fn record(&self) -> KilnEmlValueSelectorV1 {
        let (kind, index, field) = match self {
            Selector::Field(name) => (KILN_VALUE_FIELD, 0, kstr(name)),
            Selector::Index(index) => (KILN_VALUE_INDEX, *index, kstr(b"")),
        };
        KilnEmlValueSelectorV1 {
            size: std::mem::size_of::<KilnEmlValueSelectorV1>() as u32,
            kind: kind as u32,
            index,
            field,
        }
    }
}

fn kstr(bytes: &[u8]) -> KilnStr {
    KilnStr {
        ptr: bytes.as_ptr().cast(),
        len: bytes.len(),
    }
}

fn result_record() -> KilnEmlValueResultV1 {
    KilnEmlValueResultV1 {
        size: std::mem::size_of::<KilnEmlValueResultV1>() as u32,
        disposition: 0,
        flags: 0,
        arity: 0,
        item: 0,
        payload: 0,
        error: 0,
        receiver_revision: 0,
    }
}

fn info_record() -> KilnEmlValueInfoV1 {
    // SAFETY: a plain C record; all-zero is valid.
    let mut info: KilnEmlValueInfoV1 = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnEmlValueInfoV1>() as u32;
    info
}

/// A handled reply: its item, payload and arity.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Handled {
    pub item: KilnHandle,
    pub payload: KilnHandle,
    pub arity: u32,
}

/// One frontend call: temporaries die at the end of the outermost one, then a pending wake is collected.
struct Scope<'a>(&'a Values);

impl Drop for Scope<'_> {
    fn drop(&mut self) {
        if self.0.0.local().leave() && self.0.0.signal.take() && self.0.0.state.get() == State::Open
        {
            let _ = self.0.collect();
        }
    }
}

impl ValuesArena {
    /// Binds kiln.eml.values through `services` and creates the arena of `lua` over the archive view `view`, on this
    /// thread (the VM's worker). `owner` is kiln-sfi's plugin owner (attribution; nonzero).
    pub fn create(
        services: &Rc<ServiceAdapter>,
        lua: &Lua,
        owner: KilnOwner,
        view: KilnEmlArchiveView,
        options: ArenaOptions,
    ) -> Result<Self, ValuesError> {
        let m = Members::bind(services).map_err(ValuesError::Unavailable)?;
        let signal = Arc::new(WakeSignal::new(options.notify));
        let local = Box::into_raw(Box::new(
            ReaderLocal::new(lua, options.known)
                .map_err(|e| ValuesError::Fault(format!("reader setup: {e}")))?,
        ));
        let user = Box::into_raw(Box::new(ReaderUser::new(signal.clone(), local)));
        let reader = lua_reader::table(user);
        let mut control: KilnHandle = 0;
        // SAFETY: the reader table and the control output live through the call; the reader's user stays valid while
        // the arena lives (ArenaCore keeps both boxes until pending reached 0).
        let code = unsafe {
            services.call(
                m.arena_create,
                (
                    owner,
                    view,
                    options.phase,
                    &raw const reader,
                    &raw mut control,
                ),
            )
        };
        if code != KILN_OK {
            // refused: the provider admitted no arena, so it keeps no reference to the reader
            // SAFETY: both from Box::into_raw above, freed once, here.
            unsafe {
                drop(Box::from_raw(user));
                drop(Box::from_raw(local));
            }
            return Err(ValuesError::Refused {
                member: "arena_create",
                code,
            });
        }
        if control == 0 {
            // admitted, but with no control to close and drain it: the provider may still call the reader, so its
            // user context is leaked, never freed under it
            return Err(ValuesError::Fault(
                "arena_create returned no control token".into(),
            ));
        }
        let core = Rc::new(ArenaCore {
            services: services.clone(),
            m,
            owner,
            view,
            control,
            state: Cell::new(State::Open),
            signal,
            on_fault: options.on_fault,
            closes: Cell::new(0),
            last: Cell::new((false, 0)),
            local,
            user,
        });
        *core.local().values.borrow_mut() = Some(Rc::downgrade(&core));
        Ok(Self {
            values: Values(core),
            lua: lua.clone(),
        })
    }

    /// A handle for the frontend (proxies, table port, families).
    pub fn values(&self) -> Values {
        self.values.clone()
    }

    /// Collects closes signalled by a wake (the worker loop calls this when the host's signal fired). Returns the
    /// provider's pending count, or None when nothing was signalled.
    pub fn collect_if_woken(&self) -> Option<u32> {
        if self.values.0.state.get() == State::Open && self.values.0.signal.take() {
            return self.values.collect().ok();
        }
        None
    }

    /// Closes the arena and drains it while the VM still lives: `arena_close`, then `arena_collect` until the
    /// provider's pending count is 0. Every lease is revoked; the tokens the provider retained come back through
    /// `release`. Idempotent. Returns whether the drain completed.
    pub fn close(&self) -> bool {
        let core = &self.values.0;
        if core.state.get() != State::Open {
            return core.state.get() == State::Drained;
        }
        let _keep = &self.lua;
        let mut pending = u32::MAX;
        // SAFETY: live control token and output.
        let code = unsafe {
            core.services.call(
                core.m.arena_close,
                (core.owner, core.control, &raw mut pending),
            )
        };
        if code != KILN_OK {
            core.fault(&ValuesError::Refused {
                member: "arena_close",
                code,
            });
            // a refused close: the provider still owns the arena; the reader stays allocated for good
            core.state.set(State::Undrained);
            return false;
        }
        let mut rounds = 0;
        while pending > 0 && rounds < 64 {
            rounds += 1;
            // SAFETY: as above.
            let code = unsafe {
                core.services.call(
                    core.m.arena_collect,
                    (core.owner, core.control, &raw mut pending),
                )
            };
            if code != KILN_OK {
                break;
            }
        }
        if pending == 0 {
            core.local().drop_all();
            core.state.set(State::Drained);
            true
        } else {
            core.fault(&ValuesError::Fault(format!(
                "arena still has {pending} pending after close"
            )));
            core.state.set(State::Undrained);
            false
        }
    }

    /// Whether the arena was closed and drained.
    pub fn drained(&self) -> bool {
        self.values.0.state.get() == State::Drained
    }
}

impl ArenaCore {
    fn fault(&self, error: &ValuesError) {
        if let Some(on_fault) = &self.on_fault {
            on_fault(error);
        }
    }
    fn open_check(&self) -> Result<(), ValuesError> {
        match self.state.get() {
            State::Open => Ok(()),
            _ => Err(ValuesError::Closed),
        }
    }
}

impl Values {
    pub(crate) fn from_core(core: Rc<ArenaCore>) -> Self {
        Self(core)
    }

    /// The control token (the host's; never exposed to Lua).
    pub fn control(&self) -> KilnHandle {
        self.0.control
    }
    /// The wake signal (wakes seen, the control they carried).
    pub fn signal(&self) -> &WakeSignal {
        &self.0.signal
    }
    /// What the reader counted.
    pub fn reader_counters(&self) -> ReaderCounters {
        self.0.local().counters()
    }
    /// Reader tokens alive now: (all, retained by the provider).
    pub fn live_tokens(&self) -> (usize, usize) {
        self.0.local().live_tokens()
    }
    /// Retains the provider has not released yet (summed over tokens).
    pub fn outstanding_retains(&self) -> u64 {
        self.0.local().outstanding_retains()
    }
    /// Leases this frontend closed.
    pub fn closes(&self) -> u64 {
        self.0.closes.get()
    }
    /// Whether the arena is still open.
    pub fn is_open(&self) -> bool {
        self.0.state.get() == State::Open
    }
    /// The last HANDLED reply's MUTATED flag and receiver revision (one edit, one revision).
    pub fn last_mutation(&self) -> (bool, u64) {
        self.0.last.get()
    }

    fn scope(&self) -> Scope<'_> {
        self.0.local().enter();
        Scope(self)
    }

    /// `arena_collect` on the worker: the provider's pending count.
    pub fn collect(&self) -> Result<u32, ValuesError> {
        let core = &self.0;
        core.open_check()?;
        let mut pending = 0u32;
        // SAFETY: live control token and output.
        let code = unsafe {
            core.services.call(
                core.m.arena_collect,
                (core.owner, core.control, &raw mut pending),
            )
        };
        if code != KILN_OK {
            let error = ValuesError::Refused {
                member: "arena_collect",
                code,
            };
            core.fault(&error);
            return Err(error);
        }
        Ok(pending)
    }

    /// Closes one lease (idempotent at the provider; nothing once the arena is closed).
    pub(crate) fn close_lease(&self, lease: KilnHandle) {
        let core = &self.0;
        if lease == 0 || core.state.get() != State::Open {
            return;
        }
        core.closes.set(core.closes.get() + 1);
        // SAFETY: a lease token; value_close is any-thread and non-blocking.
        let _ = unsafe { core.services.call(core.m.value_close, (core.owner, lease)) };
    }

    /// Identity-only `value_info` (requested = 0): allowed from inside a reader callback.
    pub(crate) fn identity(&self, lease: KilnHandle) -> Result<KilnEmlValueInfoV1, ValuesError> {
        self.info(lease, 0)
    }

    /// `value_info` with the requested bits.
    pub fn info(
        &self,
        lease: KilnHandle,
        requested: u32,
    ) -> Result<KilnEmlValueInfoV1, ValuesError> {
        let core = &self.0;
        core.open_check()?;
        let mut info = info_record();
        info.requested = requested;
        // SAFETY: live lease token and a sized output record.
        let code = unsafe {
            core.services
                .call(core.m.value_info, (core.owner, lease, &raw mut info))
        };
        if code != KILN_OK {
            return Err(ValuesError::Refused {
                member: "value_info",
                code,
            });
        }
        Ok(info)
    }

    /// One member that fills a result record: the record, or Kiln's error for a refusal.
    fn op(
        &self,
        member: &'static str,
        call: impl FnOnce(&ArenaCore, *mut KilnEmlValueResultV1) -> KilnResult,
    ) -> Result<KilnEmlValueResultV1, ValuesError> {
        self.0.open_check()?;
        let mut out = result_record();
        let code = call(&self.0, &raw mut out);
        if code != KILN_OK {
            let error = ValuesError::Refused { member, code };
            self.0.fault(&error);
            return Err(error);
        }
        Ok(out)
    }

    /// A reply: HANDLED as its parts, UNSUPPORTED as None, a Source error raised (its packet read, then closed).
    fn settle(&self, out: KilnEmlValueResultV1) -> mlua::Result<Option<Handled>> {
        match out.disposition as i32 {
            KILN_VALUE_HANDLED => {
                self.0.last.set((
                    out.flags & KILN_VALUE_MUTATED as u32 != 0,
                    out.receiver_revision,
                ));
                Ok(Some(Handled {
                    item: out.item,
                    payload: out.payload,
                    arity: out.arity,
                }))
            }
            KILN_VALUE_UNSUPPORTED => {
                self.close_lease(out.item);
                self.close_lease(out.payload);
                Ok(None)
            }
            KILN_VALUE_SEMANTIC_ERROR => {
                self.close_lease(out.item);
                self.close_lease(out.payload);
                Err(self.raise(out.error))
            }
            other => {
                for token in [out.item, out.payload, out.error] {
                    self.close_lease(token);
                }
                Err(self.kiln_fault(ValuesError::Fault(format!("unknown disposition {other}"))))
            }
        }
    }

    fn kiln_fault(&self, error: ValuesError) -> mlua::Error {
        self.0.fault(&error);
        error.to_lua()
    }

    /// A packet's bytes: a 256-byte probe, then once more at the exact size it asked for (never a second operation).
    pub(crate) fn packet(&self, token: KilnHandle) -> Result<Vec<u8>, ValuesError> {
        let core = &self.0;
        core.open_check()?;
        let mut probe = [0u8; 256];
        let mut required = 0usize;
        // SAFETY: a packet token, a writable buffer of its stated capacity and a separate `required`.
        let code = unsafe {
            core.services.call(
                core.m.value_result_copy,
                (
                    core.owner,
                    token,
                    probe.as_mut_ptr().cast::<c_void>(),
                    probe.len(),
                    &raw mut required,
                ),
            )
        };
        match code {
            KILN_OK if required <= probe.len() => return Ok(probe[..required].to_vec()),
            KILN_ERR_BUFFER_TOO_SMALL if required > probe.len() => {}
            KILN_OK | KILN_ERR_BUFFER_TOO_SMALL => {
                return Err(ValuesError::Fault(format!(
                    "value_result_copy answered {code} with {required} bytes"
                )));
            }
            code => {
                return Err(ValuesError::Refused {
                    member: "value_result_copy",
                    code,
                });
            }
        }
        let mut bytes = vec![0u8; required];
        let mut again = 0usize;
        // SAFETY: as above, with a buffer of exactly `required` bytes.
        let code = unsafe {
            core.services.call(
                core.m.value_result_copy,
                (
                    core.owner,
                    token,
                    bytes.as_mut_ptr().cast::<c_void>(),
                    bytes.len(),
                    &raw mut again,
                ),
            )
        };
        if code != KILN_OK || again != required {
            return Err(ValuesError::Fault(format!(
                "value_result_copy changed size ({required} then {again}, KilnResult {code})"
            )));
        }
        Ok(bytes)
    }

    /// A JSON packet, then its lease closes.
    fn json(&self, token: KilnHandle) -> Result<Packet, ValuesError> {
        let bytes = self.packet(token);
        self.close_lease(token);
        Ok(packets::parse(&bytes?)?)
    }

    /// A raw packet (STRINGIFY, SOURCE_BYTES, export), then its lease closes.
    fn raw(&self, token: KilnHandle) -> Result<Vec<u8>, ValuesError> {
        let bytes = self.packet(token);
        self.close_lease(token);
        bytes
    }

    /// A SEMANTIC_ERROR's Lua error: the packet read, the host error referenced, then the error lease closed.
    fn raise(&self, token: KilnHandle) -> mlua::Error {
        let packet = self
            .packet(token)
            .and_then(|bytes| Ok(packets::parse(&bytes)?));
        let error = match packet {
            Ok(Packet::Error(level)) => self.error_for(&level),
            Ok(_) => self.kiln_fault(ValuesError::Fault(
                "an error lease without an error packet".into(),
            )),
            Err(error) => self.kiln_fault(error),
        };
        // the frontend took its own reference to any host error above; only now does the lease (and its retain) go
        self.close_lease(token);
        error
    }

    /// The Lua error Source raises for this error tree (RC-11), with the frontend's two texts rendered.
    fn error_for(&self, level: &ErrorLevel) -> mlua::Error {
        match level.class.as_str() {
            "borrow" => match level.mode.as_deref() {
                Some("mutable") => mlua::Error::UserDataBorrowMutError,
                _ => mlua::Error::UserDataBorrowError,
            },
            "lua" => match level
                .host_error
                .and_then(|token| self.0.local().error_of(token))
            {
                Some(original) => original,
                None => mlua::Error::external(self.render(level)),
            },
            "internal" => self.kiln_fault(ValuesError::Fault(format!(
                "provider fault: {}",
                String::from_utf8_lossy(&level.message)
            ))),
            _ => mlua::Error::external(self.render(level)),
        }
    }

    /// The level's text with Source's TypeId label and `guid expected, got ..` type name filled in.
    fn render(&self, level: &ErrorLevel) -> String {
        let mut text = String::from_utf8_lossy(&level.message).into_owned();
        if text.contains(TYPE_ID_PLACEHOLDER) {
            let label = self.type_id_label(level);
            text = text.replace(TYPE_ID_PLACEHOLDER, &label);
        }
        if text.contains(TYPE_NAME_PLACEHOLDER)
            && let Some(found) = self.0.local().found()
        {
            text = text.replace(TYPE_NAME_PLACEHOLDER, &source_type_name(&found));
        }
        text
    }

    /// `userdata \`TypeId(..)\``: the found userdata's own Rust TypeId (Debug form, build-specific as in Source).
    fn type_id_label(&self, level: &ErrorLevel) -> String {
        let found = self.0.local().found().and_then(|value| match value {
            Value::UserData(userdata) => userdata.type_id(),
            _ => None,
        });
        let derived = || {
            found_userdata(level)
                .and_then(|f| f.value_kind.as_deref().and_then(proxies::type_id_of_kind))
        };
        match found.or_else(derived) {
            Some(type_id) => format!("userdata `{type_id:?}`"),
            None => TYPE_ID_PLACEHOLDER.to_owned(),
        }
    }

    // ---- the operations the proxies, the table port and the families use ----

    /// `value_child`: a typed child as its (interned) userdata, a scalar as its Lua value.
    pub fn child(
        &self,
        lua: &Lua,
        lease: KilnHandle,
        selector: Selector<'_>,
    ) -> mlua::Result<Value> {
        let _scope = self.scope();
        let record = selector.record();
        let out = self
            .op("value_child", |c, out| {
                // SAFETY: live lease, a selector whose field bytes live through the call, and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_child, (c.owner, lease, &raw const record, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        let handled = self.handled(out)?;
        self.item_or_scalar(lua, handled)
    }

    /// `value_set`: Source's assignment with the value read through the protected reader.
    pub fn set(&self, lease: KilnHandle, selector: Selector<'_>, value: Value) -> mlua::Result<()> {
        let _scope = self.scope();
        let record = selector.record();
        let object = self.0.local().argument(value);
        let out = self
            .op("value_set", |c, out| {
                // SAFETY: as in `child`; `object` is a reader token of this call.
                unsafe {
                    c.services.call(
                        c.m.value_set,
                        (c.owner, lease, &raw const record, object, out),
                    )
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out)?;
        Ok(())
    }

    fn read(&self, lease: KilnHandle, query: i32) -> mlua::Result<Handled> {
        let out = self
            .op("value_read", |c, out| {
                // SAFETY: live lease and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_read, (c.owner, lease, query as u32, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out)
    }

    /// `#`: Source's length.
    pub fn length(&self, lease: KilnHandle) -> mlua::Result<i64> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_LENGTH)?;
        match self.json(handled.payload).map_err(|e| self.kiln_fault(e))? {
            Packet::Length(length) => Ok(length),
            _ => Err(self.kiln_fault(ValuesError::Fault("LENGTH without a length packet".into()))),
        }
    }

    /// `__tostring`: Source's text (bytes as they are).
    pub fn stringify(&self, lua: &Lua, lease: KilnHandle) -> mlua::Result<mlua::String> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_STRINGIFY)?;
        let bytes = self.raw(handled.payload).map_err(|e| self.kiln_fault(e))?;
        lua.create_string(bytes)
    }

    /// Source's recursive dirtiness of a typed value.
    pub fn dirty(&self, lease: KilnHandle) -> mlua::Result<bool> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_DIRTY)?;
        match self.json(handled.payload).map_err(|e| self.kiln_fault(e))? {
            Packet::Dirty(dirty) => Ok(dirty),
            _ => Err(self.kiln_fault(ValuesError::Fault("DIRTY without a dirty packet".into()))),
        }
    }

    /// Source's `mapped_source_bytes`: None for anything but a mapped struct.
    pub fn source_bytes(&self, lease: KilnHandle) -> mlua::Result<Option<Vec<u8>>> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_SOURCE_BYTES)?;
        if handled.payload == 0 {
            return Ok(None);
        }
        self.raw(handled.payload)
            .map(Some)
            .map_err(|e| self.kiln_fault(e))
    }

    /// The VIEW packet of a value.
    pub fn view(&self, lease: KilnHandle) -> mlua::Result<View> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_VIEW)?;
        match self.json(handled.payload).map_err(|e| self.kiln_fault(e))? {
            Packet::View(view) => Ok(view),
            _ => Err(self.kiln_fault(ValuesError::Fault("VIEW without a view packet".into()))),
        }
    }

    /// `__pairs`: the iterator lease (Source's arity 3 is the caller's).
    pub(crate) fn iter_begin(&self, lease: KilnHandle) -> mlua::Result<KilnHandle> {
        let _scope = self.scope();
        let handled = self.read(lease, KILN_VALUE_READ_ITER_BEGIN)?;
        if handled.item == 0 || handled.arity != 3 {
            self.close_lease(handled.item);
            self.close_lease(handled.payload);
            return Err(
                self.kiln_fault(ValuesError::Fault("ITER_BEGIN without an iterator".into()))
            );
        }
        Ok(handled.item)
    }

    /// One iterator step: (key, value), (nil, nil) at the end.
    pub(crate) fn iter_next(
        &self,
        lua: &Lua,
        iterator: KilnHandle,
    ) -> mlua::Result<(Value, Value)> {
        let _scope = self.scope();
        let handled = self.read(iterator, KILN_VALUE_READ_ITER_NEXT)?;
        let packet = self.json(handled.payload);
        let (key, value) = match packet {
            Ok(Packet::IterNext { key, value }) => (key, value),
            Ok(_) => {
                self.close_lease(handled.item);
                return Err(self.kiln_fault(ValuesError::Fault(
                    "ITER_NEXT without an iter_next packet".into(),
                )));
            }
            Err(error) => {
                self.close_lease(handled.item);
                return Err(self.kiln_fault(error));
            }
        };
        let key = match scalar_value(lua, key) {
            Ok(key) => key,
            Err(error) => {
                // the item is this frontend's until it is wrapped
                self.close_lease(handled.item);
                return Err(error);
            }
        };
        let value = match value {
            IterValue::Scalar(scalar) => {
                self.close_lease(handled.item);
                scalar_value(lua, scalar)?
            }
            IterValue::Item if handled.item != 0 => self.wrap(lua, handled.item)?,
            IterValue::Item => {
                return Err(
                    self.kiln_fault(ValuesError::Fault("ITER_NEXT item without a lease".into()))
                );
            }
        };
        Ok((key, value))
    }

    /// `value_clone`: a detached deep copy (a typed userdata, or a scalar).
    pub fn clone_value(&self, lua: &Lua, lease: KilnHandle) -> mlua::Result<Value> {
        let _scope = self.scope();
        let out = self
            .op("value_clone", |c, out| {
                // SAFETY: live lease and a result record.
                unsafe { c.services.call(c.m.value_clone, (c.owner, lease, out)) }
            })
            .map_err(|e| e.to_lua())?;
        let handled = self.handled(out)?;
        self.item_or_scalar(lua, handled)
    }

    /// `value_export`: the value's resource bytes (`selected_type` empty: its own validated type).
    pub fn export(&self, lease: KilnHandle, selected_type: &str) -> mlua::Result<Vec<u8>> {
        let _scope = self.scope();
        let selected = kstr(selected_type.as_bytes());
        let out = self
            .op("value_export", |c, out| {
                // SAFETY: live lease, a type name that lives through the call and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_export, (c.owner, lease, selected, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        let handled = self.handled(out)?;
        self.close_lease(handled.item);
        self.raw(handled.payload).map_err(|e| self.kiln_fault(e))
    }

    /// `table.insert(array, value)`.
    pub(crate) fn push(&self, lease: KilnHandle, value: Value) -> mlua::Result<()> {
        let _scope = self.scope();
        let object = self.0.local().argument(value);
        let out = self
            .op("value_array_push", |c, out| {
                // SAFETY: live lease, a reader token of this call and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_array_push, (c.owner, lease, object, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out).map(|_| ())
    }

    /// `table.insert(array, position, value)`.
    pub(crate) fn insert(
        &self,
        lease: KilnHandle,
        position: u64,
        value: Value,
    ) -> mlua::Result<()> {
        let _scope = self.scope();
        let object = self.0.local().argument(value);
        let out = self
            .op("value_array_insert", |c, out| {
                // SAFETY: as in `push`.
                unsafe {
                    c.services.call(
                        c.m.value_array_insert,
                        (c.owner, lease, position, object, out),
                    )
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out).map(|_| ())
    }

    /// `table.remove(array)`: the removed value, nil when nothing was removed.
    pub(crate) fn pop(&self, lua: &Lua, lease: KilnHandle) -> mlua::Result<Value> {
        let _scope = self.scope();
        let out = self
            .op("value_array_pop", |c, out| {
                // SAFETY: live lease and a result record.
                unsafe { c.services.call(c.m.value_array_pop, (c.owner, lease, out)) }
            })
            .map_err(|e| e.to_lua())?;
        let handled = self.handled(out)?;
        self.removed(lua, handled)
    }

    /// `table.remove(array, position)`.
    pub(crate) fn remove(
        &self,
        lua: &Lua,
        lease: KilnHandle,
        position: u64,
    ) -> mlua::Result<Value> {
        let _scope = self.scope();
        let out = self
            .op("value_array_remove", |c, out| {
                // SAFETY: live lease and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_array_remove, (c.owner, lease, position, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        let handled = self.handled(out)?;
        self.removed(lua, handled)
    }

    /// `table.clear(array)`.
    pub(crate) fn clear(&self, lease: KilnHandle) -> mlua::Result<()> {
        let _scope = self.scope();
        let out = self
            .op("value_array_clear", |c, out| {
                // SAFETY: live lease and a result record.
                unsafe {
                    c.services
                        .call(c.m.value_array_clear, (c.owner, lease, out))
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out).map(|_| ())
    }

    /// A HANDLED reply (an UNSUPPORTED one is a Kiln fault for the frontend's own receivers).
    fn handled(&self, out: KilnEmlValueResultV1) -> mlua::Result<Handled> {
        match self.settle(out)? {
            Some(handled) => Ok(handled),
            None => Err(self.kiln_fault(ValuesError::Fault(
                "UNSUPPORTED for a typed receiver".into(),
            ))),
        }
    }

    fn removed(&self, lua: &Lua, handled: Handled) -> mlua::Result<Value> {
        if handled.item == 0 {
            self.close_lease(handled.payload);
            return Ok(Value::Nil);
        }
        self.item_or_scalar(lua, handled)
    }

    /// An item (typed: its userdata; scalar node: its VIEW payload) or a scalar payload.
    fn item_or_scalar(&self, lua: &Lua, handled: Handled) -> mlua::Result<Value> {
        if handled.payload != 0 {
            // a scalar: the VIEW is the value; a scalar node's lease is the caller's to close
            self.close_lease(handled.item);
            return match self.json(handled.payload).map_err(|e| self.kiln_fault(e))? {
                Packet::View(View::Scalar(scalar)) => scalar_value(lua, scalar),
                _ => Err(self.kiln_fault(ValuesError::Fault(
                    "a scalar reply without a scalar VIEW".into(),
                ))),
            };
        }
        if handled.item == 0 {
            return Ok(Value::Nil);
        }
        self.wrap(lua, handled.item)
    }

    /// A caller-owned item lease as its interned userdata: a live proxy of the node is reused (the item closes), else a
    /// new proxy keeps the item as its own lease.
    pub(crate) fn wrap(&self, lua: &Lua, item: KilnHandle) -> mlua::Result<Value> {
        let info = match self.identity(item) {
            Ok(info) => info,
            Err(error) => {
                self.close_lease(item);
                return Err(self.kiln_fault(error));
            }
        };
        let node = info.identity.node_id;
        if let Some(existing) = self.0.local().interned(lua, node) {
            self.close_lease(item);
            return Ok(Value::UserData(existing));
        }
        if proxies::type_of_kind(info.kind).is_none() {
            self.close_lease(item);
            return Err(self.kiln_fault(ValuesError::Fault(format!(
                "an item of kind {} is no typed value",
                info.kind
            ))));
        }
        // a userdata that could not be made never took the item: it is still the caller's (this frontend's) to close
        let userdata = match proxies::create(lua, self, info.kind, item) {
            Ok(userdata) => userdata,
            Err(error) => {
                self.close_lease(item);
                return Err(error);
            }
        };
        self.0.local().intern(lua, node, &userdata)?;
        Ok(Value::UserData(userdata))
    }

    // ---- opening roots ----

    #[allow(clippy::too_many_arguments)]
    fn open(
        &self,
        lua: &Lua,
        origin: i32,
        qualified_type: &str,
        key: KilnAssetKey,
        bytes: &[u8],
        source: (u64, u64),
        value: Option<Value>,
    ) -> mlua::Result<Value> {
        let _scope = self.scope();
        let handled = self.open_handled(origin, qualified_type, key, bytes, source, value)?;
        self.item_or_scalar(lua, handled)
    }

    /// `value_open` inside the caller's scope: the HANDLED reply, its item and payload the caller's.
    fn open_handled(
        &self,
        origin: i32,
        qualified_type: &str,
        key: KilnAssetKey,
        bytes: &[u8],
        source: (u64, u64),
        value: Option<Value>,
    ) -> mlua::Result<Handled> {
        let object = value.map_or(0, |value| self.0.local().argument(value));
        let record = KilnEmlValueOpenV1 {
            size: std::mem::size_of::<KilnEmlValueOpenV1>() as u32,
            origin: origin as u32,
            qualified_type: kstr(qualified_type.as_bytes()),
            key,
            source_instance: source.0,
            source_session: source.1,
            bytes: bytes.as_ptr().cast(),
            length: bytes.len(),
            lua_object: object,
        };
        let out = self
            .op("value_open", |c, out| {
                // SAFETY: the record and the bytes it points to live through the call (the provider copies them).
                unsafe {
                    c.services.call(
                        c.m.value_open,
                        (c.owner, c.control, c.view, &raw const record, out),
                    )
                }
            })
            .map_err(|e| e.to_lua())?;
        self.handled(out)
    }

    /// Source's `encode_owned_value` (values/encode_owned_value.rs:9-24): `value` validated and cloned as an owned root
    /// of `qualified_type` (`value_open` LUA_INPUT), then serialized as that type (`value_export`), a scalar type
    /// included; the root and its VIEW close. A Source error of either step is raised as Source raises it.
    pub fn encode(&self, qualified_type: &str, value: Value) -> mlua::Result<Vec<u8>> {
        let _scope = self.scope();
        let handled = self.open_handled(
            KILN_VALUE_OPEN_LUA_INPUT,
            qualified_type,
            no_key(),
            &[],
            (0, 0),
            Some(value),
        )?;
        self.close_lease(handled.payload);
        if handled.item == 0 {
            return Err(self.kiln_fault(ValuesError::Fault(
                "a LUA_INPUT reply without its root".into(),
            )));
        }
        let bytes = self.export(handled.item, qualified_type);
        self.close_lease(handled.item);
        bytes
    }

    /// A fresh owned root of `qualified_type` validated from a Lua value (Source's validate_and_clone).
    pub fn open_lua(&self, lua: &Lua, qualified_type: &str, value: Value) -> mlua::Result<Value> {
        self.open(
            lua,
            KILN_VALUE_OPEN_LUA_INPUT,
            qualified_type,
            no_key(),
            &[],
            (0, 0),
            Some(value),
        )
    }
    /// A fresh mapped root of resource bytes (Source's decode).
    pub fn open_bytes(&self, lua: &Lua, qualified_type: &str, bytes: &[u8]) -> mlua::Result<Value> {
        self.open(
            lua,
            KILN_VALUE_OPEN_BYTES,
            qualified_type,
            no_key(),
            bytes,
            (0, 0),
            None,
        )
    }
    /// The type's default value as a fresh root.
    pub fn open_default(&self, lua: &Lua, qualified_type: &str) -> mlua::Result<Value> {
        self.open(
            lua,
            KILN_VALUE_OPEN_DEFAULT,
            qualified_type,
            no_key(),
            &[],
            (0, 0),
            None,
        )
    }
    /// The phase's shared root of an archive resource (the arena pins it).
    pub fn open_resource(
        &self,
        lua: &Lua,
        qualified_type: &str,
        key: KilnAssetKey,
    ) -> mlua::Result<Value> {
        self.open(
            lua,
            KILN_VALUE_OPEN_RESOURCE,
            qualified_type,
            key,
            &[],
            (0, 0),
            None,
        )
    }
    /// The root of one loaded ResourceInfo instance. The provider holds it weakly: the caller keeps the returned
    /// userdata on its ResourceInfo object (Source's value cache), so the same instance answers the same root with its
    /// edits while that ResourceInfo lives, and a fresh decode after it was collected.
    pub fn open_loaded(
        &self,
        lua: &Lua,
        qualified_type: &str,
        bytes: &[u8],
        source_instance: u64,
        source_session: u64,
    ) -> mlua::Result<Value> {
        self.open(
            lua,
            KILN_VALUE_OPEN_LOADED_SNAPSHOT,
            qualified_type,
            no_key(),
            bytes,
            (source_instance, source_session),
            None,
        )
    }
}

fn no_key() -> KilnAssetKey {
    KilnAssetKey {
        guid: [0; 16],
        r#type: 0,
        part: 0,
    }
}

/// The placeholders kiln.eml.values writes where only the frontend knows Source's text.
pub(crate) const TYPE_ID_PLACEHOLDER: &str = "userdata `<frontend TypeId>`";
pub(crate) const TYPE_NAME_PLACEHOLDER: &str = "<frontend type name>";

/// The first `found_userdata` in the tree.
fn found_userdata(level: &ErrorLevel) -> Option<&packets::FoundUserdata> {
    level
        .found_userdata
        .as_ref()
        .or_else(|| level.cause.as_deref().and_then(found_userdata))
}

/// Source's `type_name` for `LuaError::type_mismatch` (lua/errors.rs): `{value:#?}` for a userdata, mlua's
/// `type_name()` otherwise.
pub(crate) fn source_type_name(value: &Value) -> String {
    match value {
        Value::UserData(_) => format!("{value:#?}"),
        _ => value.type_name().to_owned(),
    }
}

/// A packet scalar as its Lua value.
pub(crate) fn scalar_value(lua: &Lua, scalar: Scalar) -> mlua::Result<Value> {
    Ok(match scalar {
        Scalar::Nil => Value::Nil,
        Scalar::Bool(b) => Value::Boolean(b),
        Scalar::Int(i) => Value::Integer(i),
        Scalar::Num(bits) => Value::Number(f64::from_bits(bits)),
        Scalar::Str(bytes) => Value::String(lua.create_string(bytes)?),
    })
}
