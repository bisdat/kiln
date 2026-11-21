// SPDX-License-Identifier: MIT
//! A scripted kiln.eml.values v1 provider built from kiln-abi types, for the frontend's own tests.
//!
//! It is faithful to the C contract (the header's records and sizes, token kinds, never-reused tokens, cleared result
//! records, the reader table it admits and calls back the way kiln-eml's adapter does: size and members checked, `copy`
//! probed then repeated at the exact size), not to ShroudForge's semantics: each test scripts what the provider
//! answers. Source behaviour through the real provider is proved by the consumer library run inside kiln-eml's own
//! C-consumer test (examples/values_consumer.rs).
//!
//! State is per thread (each test thread is its worker). Included with `#[path]` by the values tests only.
#![allow(dead_code)]

use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::ServiceAdapter;
use kiln_sfi::runtime::values::{ArenaOptions, ValuesArena, ValuesError, ValuesTables};
use kiln_sfi::worker::Host;
use mlua::Table;
use sfi_core::host::{HostContext, Package, PackageSource, Phase, Side, SourceVersion};
use sfi_core::lua::{LuaVm, VmAttachments};
use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::rc::Rc;

pub const OWNER: KilnOwner = 0x5f1;
pub const VIEW: KilnEmlArchiveView = 0x77;
pub const CONTROL: KilnHandle = 0xc0_0001;
pub const ARENA_ID: KilnHandle = 0xa0_0001;

/// What the provider saw, in order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Create {
        owner: KilnOwner,
        view: KilnEmlArchiveView,
        phase: u32,
    },
    Member(&'static str),
    Close {
        lease: u64,
        in_access: bool,
    },
    ArenaClose,
    Collect {
        control: KilnHandle,
    },
    Release {
        object: u64,
    },
    Copy {
        token: u64,
        capacity: usize,
    },
    Marker(&'static str),
}

/// One member call, as a script sees it.
#[derive(Clone, Debug, Default)]
pub struct Call {
    pub member: &'static str,
    pub lease: u64,
    pub selector: Option<(u32, u64, Vec<u8>)>,
    pub object: u64,
    pub query: u32,
    pub index: u64,
    pub origin: u32,
    pub qualified_type: String,
    pub bytes: Vec<u8>,
    pub instance: u64,
}

/// A member's answer.
#[derive(Clone, Copy, Debug)]
pub enum Answer {
    Refuse(KilnResult),
    Reply {
        disposition: i32,
        flags: u32,
        arity: u32,
        item: u64,
        payload: u64,
        error: u64,
        revision: u64,
    },
}

pub fn handled(arity: u32, item: u64, payload: u64) -> Answer {
    Answer::Reply {
        disposition: KILN_VALUE_HANDLED,
        flags: 0,
        arity,
        item,
        payload,
        error: 0,
        revision: 0,
    }
}
pub fn mutated(revision: u64) -> Answer {
    Answer::Reply {
        disposition: KILN_VALUE_HANDLED,
        flags: KILN_VALUE_MUTATED as u32,
        arity: 0,
        item: 0,
        payload: 0,
        error: 0,
        revision,
    }
}
pub fn semantic(error: u64) -> Answer {
    Answer::Reply {
        disposition: KILN_VALUE_SEMANTIC_ERROR,
        flags: 0,
        arity: 0,
        item: 0,
        payload: 0,
        error,
        revision: 0,
    }
}
pub fn unsupported() -> Answer {
    Answer::Reply {
        disposition: KILN_VALUE_UNSUPPORTED,
        flags: 0,
        arity: 0,
        item: 0,
        payload: 0,
        error: 0,
        revision: 0,
    }
}

type Script = Rc<dyn Fn(&Call) -> Answer>;

struct LeaseInfo {
    kind: u32,
    node: u64,
    closed: bool,
}

#[derive(Default)]
pub struct Fake {
    reader: Option<KilnEmlLuaReaderV1>,
    next: u64,
    leases: HashMap<u64, LeaseInfo>,
    packets: HashMap<u64, Vec<u8>>,
    pub events: Vec<Event>,
    /// Reader tokens the provider retained (released at arena_close).
    pub retained: Vec<u64>,
    /// Pending counts arena_close then arena_collect answer, in order (0 when empty).
    pub pending: VecDeque<u32>,
    pub create_answer: KilnResult,
    /// arena_create admits the reader and answers OK, but returns no control token (a provider fault).
    pub create_no_control: bool,
    /// The second copy of a packet claims this many more bytes (a size that changed).
    pub copy_grow: usize,
    /// Error leases and the retained reader object each owns (released once when the lease closes).
    pub owns: HashMap<u64, u64>,
    /// The registry index `value_info` reports for a node (`type_index`; 0 when not set).
    pub type_indexes: HashMap<u64, u32>,
    in_access: u32,
    scripts: HashMap<&'static str, Script>,
}

thread_local! {
    static FAKE: RefCell<Fake> = RefCell::new(Fake { next: 0x1000, ..Fake::default() });
}

/// Runs `f` on this thread's fake (never across a reader call).
pub fn with<R>(f: impl FnOnce(&mut Fake) -> R) -> R {
    FAKE.with(|fake| f(&mut fake.borrow_mut()))
}
pub fn reset() {
    with(|f| {
        *f = Fake {
            next: 0x1000,
            ..Fake::default()
        }
    });
}
pub fn events() -> Vec<Event> {
    with(|f| f.events.clone())
}
pub fn clear_events() {
    with(|f| f.events.clear());
}
pub fn mark(name: &'static str) {
    with(|f| f.events.push(Event::Marker(name)));
}
/// The script for `member` (replacing any before).
pub fn on(member: &'static str, script: impl Fn(&Call) -> Answer + 'static) {
    with(|f| f.scripts.insert(member, Rc::new(script)));
}

/// A fresh lease of `kind` on `node`.
pub fn lease(kind: i32, node: u64) -> u64 {
    with(|f| {
        f.next += 1;
        f.leases.insert(
            f.next,
            LeaseInfo {
                kind: kind as u32,
                node,
                closed: false,
            },
        );
        f.next
    })
}
/// A fresh packet lease holding `bytes`.
pub fn packet(bytes: impl Into<Vec<u8>>) -> u64 {
    let token = lease(KILN_VALUE_RESULT, 0);
    with(|f| f.packets.insert(token, bytes.into()));
    token
}
pub fn json(value: serde_json::Value) -> u64 {
    packet(serde_json::to_vec(&value).unwrap())
}
pub fn closed(token: u64) -> bool {
    with(|f| f.leases.get(&token).is_some_and(|l| l.closed))
}
pub fn closes_of(token: u64) -> usize {
    with(|f| {
        f.events
            .iter()
            .filter(|e| matches!(e, Event::Close { lease, .. } if *lease == token))
            .count()
    })
}

// ---- the provider's side of the reader ----

/// The reader table the arena was created with.
pub fn reader() -> KilnEmlLuaReaderV1 {
    with(|f| f.reader).expect("no arena")
}

fn kstr(bytes: &[u8]) -> KilnStr {
    KilnStr {
        ptr: bytes.as_ptr().cast(),
        len: bytes.len(),
    }
}

pub fn empty_value() -> KilnEmlLuaValueV1 {
    KilnEmlLuaValueV1 {
        size: std::mem::size_of::<KilnEmlLuaValueV1>() as u32,
        kind: 0xff,
        object: 0,
        bits: 0,
        byte_length: 0,
        typed_value: 0,
        error_object: 0,
        adapter_class: 0,
        lease_disposition: 0,
    }
}

/// One reader request, as the provider makes it: (KilnResult, reply).
pub fn access_full(
    operation: i32,
    object: u64,
    key: &[u8],
    index: i64,
    arena: u64,
    node: u64,
) -> (KilnResult, KilnEmlLuaValueV1) {
    let reader = reader();
    let request = KilnEmlLuaAccessV1 {
        size: std::mem::size_of::<KilnEmlLuaAccessV1>() as u32,
        operation: operation as u32,
        object,
        index,
        key: kstr(key),
        arena,
        node_id: node,
    };
    let mut reply = empty_value();
    with(|f| f.in_access += 1);
    let code = unsafe { reader.access.unwrap()(reader.user, &request, &mut reply) };
    with(|f| f.in_access -= 1);
    (code, reply)
}
pub fn access(
    operation: i32,
    object: u64,
    key: &[u8],
    index: i64,
) -> (KilnResult, KilnEmlLuaValueV1) {
    access_full(operation, object, key, index, 0, 0)
}
/// MAKE_TYPED_PROXY for `node` with a fresh provisional lease: (provisional, KilnResult, reply).
pub fn make_proxy(kind: i32, node: u64) -> (u64, KilnResult, KilnEmlLuaValueV1) {
    let provisional = lease(kind, node);
    let (code, reply) = access_full(
        KILN_LUA_MAKE_TYPED_PROXY,
        provisional,
        b"",
        0,
        ARENA_ID,
        node,
    );
    (provisional, code, reply)
}
/// `copy` as kiln-eml calls it: a small probe, then once more at the exact size.
pub fn copy(object: u64) -> Result<Vec<u8>, KilnResult> {
    let reader = reader();
    let mut probe = [0u8; 8];
    let mut required = usize::MAX;
    let code = unsafe {
        reader.copy.unwrap()(
            reader.user,
            object,
            probe.as_mut_ptr().cast(),
            probe.len(),
            &mut required,
        )
    };
    match code {
        KILN_OK => return Ok(probe[..required].to_vec()),
        KILN_ERR_BUFFER_TOO_SMALL => {}
        code => return Err(code),
    }
    let mut out = vec![0u8; required];
    let mut again = 0;
    let code = unsafe {
        reader.copy.unwrap()(
            reader.user,
            object,
            out.as_mut_ptr().cast(),
            out.len(),
            &mut again,
        )
    };
    assert_eq!((code, again), (KILN_OK, required));
    Ok(out)
}
pub fn retain(object: u64) -> KilnResult {
    let reader = reader();
    let code = unsafe { reader.retain.unwrap()(reader.user, object) };
    if code == KILN_OK {
        with(|f| f.retained.push(object));
    }
    code
}
pub fn release(object: u64) {
    let reader = reader();
    with(|f| {
        if let Some(at) = f.retained.iter().position(|o| *o == object) {
            f.retained.remove(at);
        }
        f.events.push(Event::Release { object });
    });
    unsafe { reader.release.unwrap()(reader.user, object) };
}

// ---- the table ----

fn script(call: Call) -> KilnResult {
    // scripts may re-enter members through Lua: the fake's state is never borrowed while one runs
    let script = with(|f| {
        f.events.push(Event::Member(call.member));
        f.scripts.get(call.member).cloned()
    });
    let Some(script) = script else {
        return KILN_ERR_UNAVAILABLE;
    };
    let answer = script(&call);
    match answer {
        Answer::Refuse(code) => code,
        Answer::Reply { .. } => {
            CURRENT.with(|c| c.borrow_mut().push(answer));
            KILN_OK
        }
    }
}

thread_local! {
    static CURRENT: RefCell<Vec<Answer>> = const { RefCell::new(Vec::new()) };
}

/// Runs `call`'s script and writes its reply into `out` (cleared first, as the provider does).
unsafe fn reply(call: Call, out: *mut KilnEmlValueResultV1) -> KilnResult {
    if out.is_null()
        || unsafe { (*out).size as usize } < std::mem::size_of::<KilnEmlValueResultV1>()
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let size = unsafe { (*out).size };
    unsafe {
        out.write(KilnEmlValueResultV1 {
            size,
            disposition: 0,
            flags: 0,
            arity: 0,
            item: 0,
            payload: 0,
            error: 0,
            receiver_revision: 0,
        })
    };
    let code = script(call);
    if code != KILN_OK {
        return code;
    }
    let Answer::Reply {
        disposition,
        flags,
        arity,
        item,
        payload,
        error,
        revision,
    } = CURRENT.with(|c| c.borrow_mut().pop()).unwrap()
    else {
        unreachable!()
    };
    unsafe {
        out.write(KilnEmlValueResultV1 {
            size,
            disposition: disposition as u32,
            flags,
            arity,
            item,
            payload,
            error,
            receiver_revision: revision,
        })
    };
    KILN_OK
}

fn selector(selector: *const KilnEmlValueSelectorV1) -> Option<(u32, u64, Vec<u8>)> {
    let s = unsafe { &*selector };
    let field = if s.field.len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(s.field.ptr.cast::<u8>(), s.field.len) }.to_vec()
    };
    Some((s.kind, s.index, field))
}

unsafe extern "C" fn arena_create(
    owner: KilnOwner,
    view: KilnEmlArchiveView,
    phase: u32,
    reader: *const KilnEmlLuaReaderV1,
    out: *mut KilnHandle,
) -> KilnResult {
    unsafe { out.write(0) };
    let answer = with(|f| {
        f.events.push(Event::Create { owner, view, phase });
        f.create_answer
    });
    if answer != KILN_OK {
        return answer;
    }
    // admission as kiln-eml's adapter does it: size first, then the covered prefix
    let size = unsafe { (*reader).size } as usize;
    if size < std::mem::size_of::<KilnEmlLuaReaderV1>() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let table = unsafe { reader.read() };
    if table.version != 1
        || table.access.is_none()
        || table.copy.is_none()
        || table.retain.is_none()
        || table.release.is_none()
    {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    with(|f| f.reader = Some(table));
    if with(|f| f.create_no_control) {
        return KILN_OK;
    }
    unsafe { out.write(CONTROL) };
    KILN_OK
}

unsafe extern "C" fn arena_close(
    _owner: KilnOwner,
    control: KilnHandle,
    pending: *mut u32,
) -> KilnResult {
    if control != CONTROL {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    with(|f| f.events.push(Event::ArenaClose));
    // teardown on the worker: every token the provider retained is released, once
    for object in with(|f| f.retained.clone()) {
        release(object);
    }
    with(|f| {
        for lease in f.leases.values_mut() {
            lease.closed = true;
        }
    });
    let left = with(|f| f.pending.pop_front().unwrap_or(0));
    unsafe { pending.write(left) };
    KILN_OK
}

unsafe extern "C" fn arena_collect(
    _owner: KilnOwner,
    control: KilnHandle,
    pending: *mut u32,
) -> KilnResult {
    with(|f| f.events.push(Event::Collect { control }));
    if control != CONTROL {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let left = with(|f| f.pending.pop_front().unwrap_or(0));
    unsafe { pending.write(left) };
    KILN_OK
}

unsafe extern "C" fn value_open(
    _owner: KilnOwner,
    arena: KilnHandle,
    view: KilnEmlArchiveView,
    record: *const KilnEmlValueOpenV1,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    if arena != CONTROL || view != VIEW {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let r = unsafe { &*record };
    let qualified_type = String::from_utf8_lossy(unsafe {
        std::slice::from_raw_parts(r.qualified_type.ptr.cast::<u8>(), r.qualified_type.len)
    })
    .into_owned();
    let bytes = if r.length == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(r.bytes.cast::<u8>(), r.length) }.to_vec()
    };
    let call = Call {
        member: "value_open",
        origin: r.origin,
        qualified_type,
        bytes,
        object: r.lua_object,
        instance: r.source_instance,
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_info(
    _owner: KilnOwner,
    value: KilnHandle,
    info: *mut KilnEmlValueInfoV1,
) -> KilnResult {
    let found = with(|f| {
        f.leases.get(&value).map(|l| {
            (
                l.kind,
                l.node,
                l.closed,
                f.type_indexes.get(&l.node).copied().unwrap_or(0),
            )
        })
    });
    let Some((kind, node, closed, type_index)) = found else {
        return KILN_ERR_NOT_FOUND;
    };
    if closed {
        return KILN_ERR_REVOKED;
    }
    let size = unsafe { (*info).size };
    let requested = unsafe { (*info).requested };
    let mut record: KilnEmlValueInfoV1 = unsafe { std::mem::zeroed() };
    record.size = size;
    record.requested = requested;
    record.identity.size = std::mem::size_of::<KilnEmlValueIdentityV1>() as u32;
    record.identity.arena = ARENA_ID;
    record.identity.node_id = node;
    record.identity.creator_owner = OWNER;
    record.kind = kind;
    record.type_index = type_index;
    unsafe { info.write(record) };
    KILN_OK
}

unsafe extern "C" fn value_read(
    _owner: KilnOwner,
    value: KilnHandle,
    query: u32,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let call = Call {
        member: "value_read",
        lease: value,
        query,
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_child(
    _owner: KilnOwner,
    value: KilnHandle,
    s: *const KilnEmlValueSelectorV1,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let call = Call {
        member: "value_child",
        lease: value,
        selector: selector(s),
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_set(
    _owner: KilnOwner,
    value: KilnHandle,
    s: *const KilnEmlValueSelectorV1,
    object: u64,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let call = Call {
        member: "value_set",
        lease: value,
        selector: selector(s),
        object,
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_clone(
    _owner: KilnOwner,
    value: KilnHandle,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let call = Call {
        member: "value_clone",
        lease: value,
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_export(
    _owner: KilnOwner,
    value: KilnHandle,
    _selected: KilnStr,
    out: *mut KilnEmlValueResultV1,
) -> KilnResult {
    let call = Call {
        member: "value_export",
        lease: value,
        ..Call::default()
    };
    unsafe { reply(call, out) }
}

unsafe extern "C" fn value_close(_owner: KilnOwner, value: KilnHandle) -> KilnResult {
    // any thread; the fake's state is per thread, so a close from elsewhere only signals
    let outcome = FAKE.with(|fake| {
        let Ok(mut f) = fake.try_borrow_mut() else {
            return (KILN_ERR_INTERNAL, None);
        };
        let in_access = f.in_access > 0;
        f.events.push(Event::Close {
            lease: value,
            in_access,
        });
        match f.leases.get_mut(&value) {
            Some(lease) if lease.closed => (KILN_OK, None),
            Some(lease) => {
                lease.closed = true;
                let owned = f.owns.remove(&value);
                (KILN_OK, owned)
            }
            None => (KILN_ERR_NOT_FOUND, None),
        }
    });
    if let (KILN_OK, Some(object)) = outcome {
        release(object);
    }
    outcome.0
}

unsafe extern "C" fn value_result_copy(
    _owner: KilnOwner,
    token: KilnHandle,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    let packet = with(|f| {
        f.events.push(Event::Copy { token, capacity });
        let copies = f
            .events
            .iter()
            .filter(|e| matches!(e, Event::Copy { token: t, .. } if *t == token))
            .count();
        f.packets.get(&token).cloned().map(|mut bytes| {
            if copies > 1 && f.copy_grow > 0 {
                bytes.extend(std::iter::repeat_n(b' ', f.copy_grow));
            }
            bytes
        })
    });
    let Some(bytes) = packet else {
        return KILN_ERR_NOT_FOUND;
    };
    unsafe { required.write(bytes.len()) };
    if capacity < bytes.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    if !bytes.is_empty() {
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), bytes.len()) };
    }
    KILN_OK
}

macro_rules! array_member {
    ($name:ident, ($($arg:ident: $ty:ty),*), |$call:ident| $fill:expr) => {
        unsafe extern "C" fn $name(_owner: KilnOwner, value: KilnHandle, $($arg: $ty,)* out: *mut KilnEmlValueResultV1) -> KilnResult {
            #[allow(unused_mut)]
            let mut $call = Call { member: stringify!($name), lease: value, ..Call::default() };
            $fill;
            unsafe { reply($call, out) }
        }
    };
}
array_member!(value_array_push, (object: u64), |call| call.object = object);
array_member!(value_array_insert, (index: u64, object: u64), |call| {
    call.index = index;
    call.object = object;
});
array_member!(value_array_pop, (), |call| {
    let _ = &call;
});
array_member!(value_array_remove, (index: u64), |call| call.index = index);
array_member!(value_array_clear, (), |call| {
    let _ = &call;
});

pub static TABLE: KilnEmlValuesApiV1 = KilnEmlValuesApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlValuesApiV1>() as u32,
        version: 1,
    },
    arena_create: Some(arena_create),
    arena_close: Some(arena_close),
    arena_collect: Some(arena_collect),
    value_open: Some(value_open),
    value_info: Some(value_info),
    value_read: Some(value_read),
    value_child: Some(value_child),
    value_set: Some(value_set),
    value_clone: Some(value_clone),
    value_export: Some(value_export),
    value_close: Some(value_close),
    value_result_copy: Some(value_result_copy),
    value_array_push: Some(value_array_push),
    value_array_insert: Some(value_array_insert),
    value_array_pop: Some(value_array_pop),
    value_array_remove: Some(value_array_remove),
    value_array_clear: Some(value_array_clear),
};

/// A table whose `size` ends before `value_array_clear` (an older provider).
pub static SHORT: KilnEmlValuesApiV1 = KilnEmlValuesApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlValuesApiV1>() as u32 - 8,
        version: 1,
    },
    ..TABLE
};

thread_local! {
    static SHORT_TABLE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
/// The next lookups on this thread answer [`SHORT`].
pub fn serve_short(short: bool) {
    SHORT_TABLE.with(|s| s.set(short));
}

unsafe extern "C" fn service(name: KilnStr, _min: u32) -> *const c_void {
    let name = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
    if name == b"kiln.eml.values" {
        if SHORT_TABLE.with(|s| s.get()) {
            return (&SHORT as *const KilnEmlValuesApiV1).cast();
        }
        return (&TABLE as *const KilnEmlValuesApiV1).cast();
    }
    std::ptr::null()
}

/// A worker's VM (sfi-core's, with the frontend's table port attached) and the adapter over this fake.
pub struct TestVm {
    pub vm: LuaVm,
    pub env: Table,
    pub services: Rc<ServiceAdapter>,
    _api: Box<KilnApi>,
}

struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            path.to_owned(),
        ))
    }
}

pub fn vm() -> TestVm {
    reset();
    let mut api: Box<KilnApi> = Box::new(unsafe { std::mem::zeroed() });
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.service = Some(service);
    let host = unsafe { Host::read(&*api) };
    let services = Rc::new(ServiceAdapter::new(
        host,
        Rc::new(Watchdog::new(process_clock())),
    ));
    let vm = LuaVm::new(HostContext::new(
        Phase::Runtime,
        Side::Client,
        SourceVersion::Sf179,
    ))
    .unwrap();
    vm.attach(VmAttachments {
        typed_tables: Some(Rc::new(ValuesTables)),
        buffer_resources: None,
    })
    .unwrap();
    let env = vm
        .add_package(Package::new("m02", Rc::new(NoFiles)), |_, _, _| Ok(()))
        .unwrap();
    TestVm {
        vm,
        env,
        services,
        _api: api,
    }
}

impl TestVm {
    pub fn lua(&self) -> &mlua::Lua {
        self.vm.lua()
    }
    pub fn arena(&self, options: ArenaOptions) -> Result<ValuesArena, ValuesError> {
        ValuesArena::create(&self.services, self.lua(), OWNER, VIEW, options)
    }
    /// Runs a chunk in the package environment.
    pub fn run<R: mlua::FromLuaMulti>(&self, chunk: &str) -> mlua::Result<R> {
        self.lua()
            .load(chunk)
            .set_name("=m02")
            .set_environment(self.env.clone())
            .eval()
    }
    pub fn set(&self, name: &str, value: impl mlua::IntoLua) {
        self.env.set(name, value).unwrap();
    }
    pub fn collect(&self) {
        self.lua().gc_collect().unwrap();
        self.lua().gc_collect().unwrap();
    }
}
