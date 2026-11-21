// SPDX-License-Identifier: MIT
//! The one path the interpreter runtime takes to Kiln's services.
//!
//! Every engine operation is a
//! direct call of an any-thread Kiln member from the worker thread. The adapter never
//! posts, waits or retries; a refusal goes back to the family that called, which maps
//! it to ShroudForge's shape for its operation.
//!
//! What the adapter checks:
//! - **Lookup** through `KilnApi.service(name, min_version)` only ([`ServiceAdapter::bind`]).
//! - **Header**: a non-null, aligned table at least as long as its
//!   [`KilnServiceHeader`], whose `version` is at least the one asked for.
//! - **Members**: each member is read only when the table's `size` covers its whole
//!   slot, and only when it is non-null ([`service_member!`]). A missing, short or
//!   older table, or a null member, is [`Unavailable`]: ShroudForge's unavailable shape
//!   for that operation, never a read or a call past the table's size.
//! - **Copied slots, live tokens**: Kiln's service tables live for the process, so a
//!   member's function pointer is copied once, at bind time, into a [`Member`]. Live
//!   targets (`KilnObjectRef`s, handles, sessions) are never kept here: they are
//!   arguments of each call and Kiln revalidates them there.
//! - **Timed calls**: a member that reports `source_wait_ns` is bound as
//!   [`ReportsWait`] and is callable only through [`ServiceAdapter::call_reporting_wait`],
//!   which times it with [`Watchdog::native_call`] and clamps the exclusion to the
//!   measured call (§0.5 item 2).
//! - **One call, nothing else**: the adapter calls the copied function itself with the
//!   arguments given as a tuple ([`Invoke`]), so the function pointer never reaches the
//!   caller (it cannot be kept, sent to a hop or called untimed later) and a timed call
//!   measures the member call and nothing else.
//! - **The core's session record** ([`ServiceAdapter::session`]): what runs in this
//!   process now (worlds, their session counts, the session state, whether stops are seen).
//!   Any thread; a plain read, like a direct member.
//!
//! Callbacks Kiln makes on other threads lend their data for the callback only: a
//! [`CallbackSink`] copies it out ([`Lent`]) and hands an owned value to a `Send`
//! producer function, which must never enter Lua or wait for the worker.
//!
//! The adapter, its tables and members are `!Send`: they live on the worker thread, and a
//! hop's closure (which is `Send`) cannot capture a member:
//! ```compile_fail,E0277
//! fn hop_closure<T: Send + 'static>(_: T) {}
//! fn capture(member: kiln_sfi::runtime::services::Member<unsafe extern "C" fn() -> u32>) {
//!     hop_closure(move || { let _kept = member; });
//! }
//! ```
//! ```compile_fail,E0277
//! fn producer<T: Send>(_: T) {}
//! fn give(adapter: kiln_sfi::runtime::services::ServiceAdapter) { producer(adapter) }
//! ```
//! Nor can a caller get the function pointer out of a call (only argument tuples are
//! accepted, never a closure that would receive it):
//! ```compile_fail,E0308
//! use kiln_sfi::runtime::services::{Member, ServiceAdapter};
//! type F = unsafe extern "C" fn() -> u32;
//! fn leak(adapter: &ServiceAdapter, member: Member<F>) -> F {
//!     unsafe { adapter.call(member, |f: F| f) }
//! }
//! ```

use crate::lifecycle::{NativeCall, Watchdog};
use crate::worker::Host;
use kiln_abi::KilnServiceHeader;
use std::cell::Cell;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

/// Why a service or one of its members cannot be called. Every case is
/// ShroudForge's unavailable shape for the operation that needed it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unavailable {
    /// The core's `KilnApi` has no `service` member.
    NoLookup,
    /// The lookup answered NULL: no such service, or an older version only.
    Missing,
    /// The table is not aligned for its type.
    Misaligned,
    /// The table is shorter than its own header.
    Header { size: u32 },
    /// The table's version is older than the one asked for.
    Version { have: u32, need: u32 },
    /// The table's `size` does not cover this member's slot.
    Short {
        member: &'static str,
        size: u32,
        need: usize,
    },
    /// The member's slot is NULL.
    Null { member: &'static str },
}

/// [`Unavailable`] for one service, for logs and status.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ServiceError {
    pub service: &'static str,
    pub why: Unavailable,
}
impl std::fmt::Display for ServiceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let service = self.service;
        match self.why {
            Unavailable::NoLookup => write!(f, "{service}: this core has no service lookup"),
            Unavailable::Missing => write!(f, "{service}: not provided"),
            Unavailable::Misaligned => write!(f, "{service}: misaligned table"),
            Unavailable::Header { size } => {
                write!(
                    f,
                    "{service}: table of {size} bytes is shorter than its header"
                )
            }
            Unavailable::Version { have, need } => {
                write!(f, "{service}: version {have}, {need} needed")
            }
            Unavailable::Short { member, size, need } => write!(
                f,
                "{service}.{member}: the table's {size} bytes do not cover it ({need} needed)"
            ),
            Unavailable::Null { member } => write!(f, "{service}.{member}: not provided"),
        }
    }
}
impl std::error::Error for ServiceError {}

/// One service table that passed the header checks. A process-lifetime pointer;
/// its members are copied out once with [`service_member!`].
pub struct ServiceTable<A> {
    service: &'static str,
    base: *const A,
    size: u32,
    version: u32,
    _not_send: PhantomData<*mut ()>,
}
impl<A> Clone for ServiceTable<A> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<A> Copy for ServiceTable<A> {}

impl<A> ServiceTable<A> {
    pub fn service(&self) -> &'static str {
        self.service
    }
    /// The table's declared size in bytes.
    pub fn size(&self) -> u32 {
        self.size
    }
    pub fn version(&self) -> u32 {
        self.version
    }

    /// Copies the member slot at `offset` when the table's size covers it and it is
    /// non-null. Use [`service_member!`], which derives `offset` and `slot` from the
    /// same field.
    ///
    /// # Safety
    /// `offset` is the offset of the field of `A` that `slot` reads, and that field
    /// has type `Option<F>`.
    #[doc(hidden)]
    pub unsafe fn member_at<F: Copy, K: MemberKind>(
        &self,
        offset: usize,
        name: &'static str,
        slot: fn(&A) -> Option<F>,
    ) -> Result<Member<F, K>, ServiceError> {
        let _ = slot; // the field's type, never called: no reference to the table is formed
        let need = offset.saturating_add(core::mem::size_of::<Option<F>>());
        if (self.size as usize) < need {
            return Err(self.error(Unavailable::Short {
                member: name,
                size: self.size,
                need,
            }));
        }
        // SAFETY: the table is readable for `size` bytes (KilnApi.service contract),
        // `size` covers the slot, the base is aligned for `A` and `offset` is a field
        // offset of `A`, so the slot is aligned for `Option<F>` (the caller's contract).
        let value = unsafe {
            self.base
                .cast::<u8>()
                .add(offset)
                .cast::<Option<F>>()
                .read()
        };
        match value {
            Some(function) => Ok(Member {
                function,
                service: self.service,
                name,
                _kind: PhantomData,
                _not_send: PhantomData,
            }),
            None => Err(self.error(Unavailable::Null { member: name })),
        }
    }

    fn error(&self, why: Unavailable) -> ServiceError {
        ServiceError {
            service: self.service,
            why,
        }
    }
}

/// Copies one member of a bound [`ServiceTable`] into a [`Member`], after the extent
/// and non-null checks. `service_member!(table, KilnWorldApi, ecs_query)` for a
/// direct member; append `, waits` for a member whose reply carries `source_wait_ns`.
#[macro_export]
macro_rules! service_member {
    ($table:expr, $api:ty, $field:ident) => {
        $crate::service_member!(@kind $table, $api, $field, $crate::runtime::services::Direct)
    };
    ($table:expr, $api:ty, $field:ident, waits) => {
        $crate::service_member!(@kind $table, $api, $field, $crate::runtime::services::ReportsWait)
    };
    (@kind $table:expr, $api:ty, $field:ident, $kind:ty) => {{
        let table: &$crate::runtime::services::ServiceTable<$api> = &$table;
        // SAFETY: the offset and the slot reader name the same field of the same type.
        unsafe {
            table.member_at::<_, $kind>(
                ::core::mem::offset_of!($api, $field),
                stringify!($field),
                |api: &$api| api.$field,
            )
        }
    }};
}

mod sealed {
    pub trait Sealed {}
    pub trait SealedFn<Args> {}
}

/// A member function the adapter calls with its arguments as a tuple: `()`, `(a,)`,
/// `(a, b)`, ... Sealed: kiln-abi's `unsafe extern "C" fn` members, up to 12 arguments.
pub trait Invoke<Args>: Copy + sealed::SealedFn<Args> {
    type Output;
    #[doc(hidden)]
    /// # Safety
    /// `args` meet the function's C contract.
    unsafe fn invoke(self, args: Args) -> Self::Output;
}
macro_rules! invoke {
    ($($arg:ident),*) => {
        impl<R, $($arg),*> sealed::SealedFn<($($arg,)*)> for unsafe extern "C" fn($($arg),*) -> R {}
        impl<R, $($arg),*> Invoke<($($arg,)*)> for unsafe extern "C" fn($($arg),*) -> R {
            type Output = R;
            #[allow(non_snake_case, clippy::too_many_arguments)]
            unsafe fn invoke(self, ($($arg,)*): ($($arg,)*)) -> R {
                // SAFETY: the caller's contract.
                unsafe { self($($arg),*) }
            }
        }
    };
}
invoke!();
invoke!(A1);
invoke!(A1, A2);
invoke!(A1, A2, A3);
invoke!(A1, A2, A3, A4);
invoke!(A1, A2, A3, A4, A5);
invoke!(A1, A2, A3, A4, A5, A6);
invoke!(A1, A2, A3, A4, A5, A6, A7);
invoke!(A1, A2, A3, A4, A5, A6, A7, A8);
invoke!(A1, A2, A3, A4, A5, A6, A7, A8, A9);
invoke!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10);
invoke!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11);
invoke!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12);
/// Whether a member reports `source_wait_ns` (sealed: [`Direct`] or [`ReportsWait`]).
pub trait MemberKind: sealed::Sealed {}
/// A member called directly; its whole duration is Lua time.
pub enum Direct {}
/// A member whose reply carries `source_wait_ns`: callable only through
/// [`ServiceAdapter::call_reporting_wait`].
pub enum ReportsWait {}
impl sealed::Sealed for Direct {}
impl sealed::Sealed for ReportsWait {}
impl MemberKind for Direct {}
impl MemberKind for ReportsWait {}

/// A member's function pointer, copied once from a checked table. Only a
/// [`ServiceTable`] makes one:
/// ```compile_fail,E0451
/// let _ = kiln_sfi::runtime::services::Member::<fn(), kiln_sfi::runtime::services::Direct> {
///     function: || {}, service: "kiln.world", name: "x",
///     _kind: std::marker::PhantomData, _not_send: std::marker::PhantomData,
/// };
/// ```
pub struct Member<F, K = Direct> {
    function: F,
    service: &'static str,
    name: &'static str,
    _kind: PhantomData<K>,
    _not_send: PhantomData<*mut ()>,
}
impl<F: Copy, K> Clone for Member<F, K> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<F: Copy, K> Copy for Member<F, K> {}
impl<F, K> Member<F, K> {
    pub fn service(&self) -> &'static str {
        self.service
    }
    pub fn name(&self) -> &'static str {
        self.name
    }
}

/// What the adapter counted (status and tests).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AdapterCounters {
    /// Tables bound and lookups refused.
    pub bound: u64,
    pub refused: u64,
    /// Member calls, and those of them timed for `source_wait_ns`.
    pub calls: u64,
    pub timed: u64,
    /// The total wait excluded from the watchdog, in nanoseconds.
    pub excluded_ns: u64,
}

/// The runtime's service adapter: one per runtime-create, on the worker thread.
pub struct ServiceAdapter {
    host: Host,
    watchdog: Rc<Watchdog>,
    counters: Cell<AdapterCounters>,
    _not_send: PhantomData<*mut ()>,
}

impl ServiceAdapter {
    pub fn new(host: Host, watchdog: Rc<Watchdog>) -> Self {
        Self {
            host,
            watchdog,
            counters: Cell::new(AdapterCounters::default()),
            _not_send: PhantomData,
        }
    }

    /// Looks `name` up with `KilnApi.service(name, min_version)` and checks its header.
    /// `name` is a generated `*_SERVICE` constant (one trailing NUL allowed). `A` is
    /// that service's table type.
    pub fn bind<A>(
        &self,
        name: &'static [u8],
        min_version: u32,
    ) -> Result<ServiceTable<A>, ServiceError> {
        let text = name.strip_suffix(b"\0").unwrap_or(name);
        let service = std::str::from_utf8(text).unwrap_or("kiln service");
        let outcome = self.check::<A>(service, name, min_version);
        self.count(|c| match outcome {
            Ok(_) => c.bound += 1,
            Err(_) => c.refused += 1,
        });
        outcome
    }

    fn check<A>(
        &self,
        service: &'static str,
        name: &'static [u8],
        min_version: u32,
    ) -> Result<ServiceTable<A>, ServiceError> {
        let error = |why| ServiceError { service, why };
        if !self.host.has_service_lookup() {
            return Err(error(Unavailable::NoLookup));
        }
        let table = self.host.service(name, min_version).cast::<A>();
        if table.is_null() {
            return Err(error(Unavailable::Missing));
        }
        if !table.is_aligned() || !table.cast::<KilnServiceHeader>().is_aligned() {
            return Err(error(Unavailable::Misaligned));
        }
        let header = table.cast::<KilnServiceHeader>();
        // SAFETY: a non-null, aligned service table starts with its header, whose
        // `size` field (the first 4 bytes) is always readable.
        let size = unsafe { core::ptr::addr_of!((*header).size).read() };
        if (size as usize) < core::mem::size_of::<KilnServiceHeader>() {
            return Err(error(Unavailable::Header { size }));
        }
        // SAFETY: `size` covers the whole header.
        let version = unsafe { core::ptr::addr_of!((*header).version).read() };
        if version < min_version {
            return Err(error(Unavailable::Version {
                have: version,
                need: min_version,
            }));
        }
        Ok(ServiceTable {
            service,
            base: table,
            size,
            version,
            _not_send: PhantomData,
        })
    }

    /// Calls a direct member once with `args` (a tuple): no hop, no wait, no retry. Its
    /// whole duration is Lua time. The member's own result comes back untouched.
    ///
    /// # Safety
    /// `args` meet the member's C contract (its kiln_<service>.h: live pointers, `size`
    /// fields set, buffers as long as stated).
    pub unsafe fn call<F, A>(&self, member: Member<F, Direct>, args: A) -> F::Output
    where
        F: Invoke<A>,
    {
        self.count(|c| c.calls += 1);
        // SAFETY: the caller's contract; the function was copied from a checked table.
        unsafe { member.function.invoke(args) }
    }

    /// Calls a member whose reply reports `source_wait_ns` once with `args`, timed by the
    /// watchdog's [`native_call`](Watchdog::native_call) guard around the member call
    /// alone. The result is taken with [`Timed::finish`], which excludes the reported
    /// wait from the Lua-time account, clamped to the measured call. No hop, no retry.
    ///
    /// # Safety
    /// As [`Self::call`].
    pub unsafe fn call_reporting_wait<F, A>(
        &self,
        member: Member<F, ReportsWait>,
        args: A,
    ) -> Timed<'_, F::Output>
    where
        F: Invoke<A>,
    {
        let mut guard = self.watchdog.native_call();
        // SAFETY: the caller's contract; the function was copied from a checked table.
        let value = guard.call(|| unsafe { member.function.invoke(args) });
        self.count(|c| {
            c.calls += 1;
            c.timed += 1;
        });
        Timed {
            adapter: self,
            guard,
            value,
        }
    }

    /// The core's `session` record (any thread): what runs in this process now. `None` when
    /// this core has no such member or it refuses. Counted as one call, like a direct member.
    pub fn session(&self) -> Option<kiln_abi::KilnSessionInfo> {
        if !self.has_session() {
            return None;
        }
        self.count(|c| c.calls += 1);
        self.host.session_info()
    }
    /// Whether this core has the `session` member (a presence check: nothing is called).
    pub fn has_session(&self) -> bool {
        self.host.session.is_some()
    }

    pub fn counters(&self) -> AdapterCounters {
        self.counters.get()
    }
    /// One line in kiln.log under `owner` (`KilnApi.log`; quiet without one): a family's own
    /// diagnostics, such as ShroudForge's loader warnings, never a mod's log.
    pub fn log(&self, owner: kiln_abi::KilnOwner, level: i32, text: &str) {
        self.host.log(owner, level, text);
    }
    pub fn watchdog(&self) -> &Rc<Watchdog> {
        &self.watchdog
    }

    fn count(&self, update: impl FnOnce(&mut AdapterCounters)) {
        let mut counters = self.counters.get();
        update(&mut counters);
        self.counters.set(counters);
    }
}

/// The result of one timed call ([`ServiceAdapter::call_reporting_wait`]), held until the
/// reported wait is excluded.
#[must_use = "take the result with finish(source_wait_ns)"]
pub struct Timed<'a, R> {
    adapter: &'a ServiceAdapter,
    guard: NativeCall<'a>,
    value: R,
}
impl<R> Timed<'_, R> {
    /// The member's result, to read a `source_wait_ns` it carries.
    pub fn result(&self) -> &R {
        &self.value
    }
    /// The duration of the member call.
    pub fn measured(&self) -> std::time::Duration {
        self.guard.measured()
    }
    /// Excludes the call's reported `source_wait_ns` from the Lua-time account (never
    /// more than the measured call) and gives the member's result.
    pub fn finish(self, source_wait_ns: u64) -> R {
        let excluded = self.guard.exclude_ns(source_wait_ns);
        self.adapter.count(|c| {
            c.excluded_ns = c
                .excluded_ns
                .saturating_add(u64::try_from(excluded.as_nanos()).unwrap_or(u64::MAX));
        });
        self.value
    }
}

// ---- callbacks: lent data copied out, published to a producer function ----

/// An event Kiln lends to a callback: a plain C struct whose first field is its
/// `size: u32` (Kiln's appended-member rule).
///
/// # Safety
/// The type is `repr(C)`, valid for any bytes (all fields plain integers or
/// pointers that are never dereferenced) and starts with a `u32` size.
pub unsafe trait CallbackEvent: Copy + Send + 'static {}
// SAFETY: repr(C) POD starting with `size: u32` (kiln_world.h).
unsafe impl CallbackEvent for kiln_abi::services::kiln_world::KilnEcsWriteEvent {}

/// Data a callback lends for its own duration only. It cannot leave the callback:
/// ```compile_fail
/// use kiln_sfi::runtime::services::Lent;
/// fn keep(lent: Lent<'_, u32>) -> Lent<'static, u32> { lent }
/// ```
pub struct Lent<'cb, E> {
    event: *const E,
    _callback: PhantomData<&'cb E>,
}
impl<E: CallbackEvent> Lent<'_, E> {
    /// An owned copy. A shorter event (an older core) is copied up to its `size`
    /// and the rest is zero; a longer one is cut to this type.
    pub fn copy(&self) -> E {
        // SAFETY: `event` is non-null and aligned (checked when lent); its leading
        // u32 is always present.
        let size = unsafe { self.event.cast::<u32>().read() } as usize;
        let len = size.min(core::mem::size_of::<E>());
        // SAFETY: E is valid for any bytes (CallbackEvent); the source is readable
        // for `size` bytes for the callback's duration.
        unsafe {
            let mut copy = core::mem::MaybeUninit::<E>::zeroed();
            core::ptr::copy_nonoverlapping(
                self.event.cast::<u8>(),
                copy.as_mut_ptr().cast::<u8>(),
                len,
            );
            copy.assume_init()
        }
    }
}

struct SinkState<E> {
    deliver: Box<dyn Fn(E) + Send + Sync>,
    closed: AtomicBool,
    in_flight: AtomicU32,
    delivered: AtomicU64,
    dropped: AtomicU64,
}

/// A callback registration's receiving end. Kiln calls [`Self::function`] with
/// [`Self::user`] on its own thread; the event is copied out ([`Lent::copy`]) and the
/// owned copy goes to `deliver`. `deliver` runs on that thread: it must be short,
/// never block, never wait for the worker and never enter Lua (it is `Send`, so it
/// cannot hold a Lua handle):
/// ```compile_fail,E0277
/// use kiln_sfi::runtime::services::CallbackSink;
/// use kiln_abi::services::kiln_world::KilnEcsWriteEvent;
/// let lua = mlua::Lua::new();
/// let _ = CallbackSink::new(move |_: KilnEcsWriteEvent| { let _ = lua.globals(); });
/// ```
pub struct CallbackSink<E: CallbackEvent> {
    state: Arc<SinkState<E>>,
}

impl<E: CallbackEvent> CallbackSink<E> {
    pub fn new(deliver: impl Fn(E) + Send + Sync + 'static) -> Self {
        Self {
            state: Arc::new(SinkState {
                deliver: Box::new(deliver),
                closed: AtomicBool::new(false),
                in_flight: AtomicU32::new(0),
                delivered: AtomicU64::new(0),
                dropped: AtomicU64::new(0),
            }),
        }
    }
    /// The C callback to register.
    pub fn function(&self) -> unsafe extern "C" fn(*const E, *mut c_void) {
        sink_trampoline::<E>
    }
    /// The `user` pointer to register with it. Each call hands one reference to the
    /// registration, which keeps it for the process: a late callback after an
    /// unsubscribe still finds valid memory (one small leak per registration).
    pub fn user(&self) -> *mut c_void {
        Arc::into_raw(self.state.clone()).cast_mut().cast()
    }
    /// After this returns, `deliver` neither runs nor will run again, except the
    /// deliveries of this sink the calling thread is inside (a close from `deliver`
    /// itself), which finish after it returns. Producers never wait for it: a callback
    /// that arrives after the close only counts a drop. The close itself waits only for
    /// deliveries already in flight on other threads (short by contract), like Kiln's own
    /// unsubscribe, which returns at once from inside a callback.
    pub fn close(&self) {
        self.state.closed.store(true, Ordering::SeqCst);
        let own = delivering(Arc::as_ptr(&self.state).cast());
        while self.state.in_flight.load(Ordering::SeqCst) > own {
            std::thread::yield_now();
        }
    }
    /// (delivered, dropped after the close).
    pub fn counts(&self) -> (u64, u64) {
        (
            self.state.delivered.load(Ordering::SeqCst),
            self.state.dropped.load(Ordering::SeqCst),
        )
    }
}

thread_local! {
    /// The sinks (their state's address) whose `deliver` this thread is inside now.
    static DELIVERING: std::cell::RefCell<Vec<usize>> = const { std::cell::RefCell::new(Vec::new()) };
}
/// How many deliveries of the sink at `state` this thread is inside.
fn delivering(state: *const ()) -> u32 {
    DELIVERING
        .try_with(|d| d.borrow().iter().filter(|&&a| a == state as usize).count() as u32)
        .unwrap_or(0)
}
/// Marks this thread as inside a delivery of the sink at `state` until dropped.
struct Delivering(usize);
impl Delivering {
    fn enter(state: *const ()) -> Self {
        let _ = DELIVERING.try_with(|d| d.borrow_mut().push(state as usize));
        Self(state as usize)
    }
}
impl Drop for Delivering {
    fn drop(&mut self) {
        let _ = DELIVERING.try_with(|d| {
            let mut d = d.borrow_mut();
            if let Some(at) = d.iter().rposition(|&a| a == self.0) {
                d.remove(at);
            }
        });
    }
}

unsafe extern "C" fn sink_trampoline<E: CallbackEvent>(event: *const E, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    // SAFETY: `user` is a reference `CallbackSink::user` handed to the registration;
    // it is never released, so it outlives every callback.
    let state = unsafe { &*user.cast_const().cast::<SinkState<E>>() };
    state.in_flight.fetch_add(1, Ordering::SeqCst);
    if state.closed.load(Ordering::SeqCst) || event.is_null() || !event.is_aligned() {
        state.dropped.fetch_add(1, Ordering::SeqCst);
    } else {
        let lent = Lent {
            event,
            _callback: PhantomData,
        };
        let copy = lent.copy();
        let _inside = Delivering::enter(user.cast_const().cast());
        // No panic crosses the C boundary.
        if catch_unwind(AssertUnwindSafe(|| (state.deliver)(copy))).is_ok() {
            state.delivered.fetch_add(1, Ordering::SeqCst);
        }
    }
    state.in_flight.fetch_sub(1, Ordering::SeqCst);
}
