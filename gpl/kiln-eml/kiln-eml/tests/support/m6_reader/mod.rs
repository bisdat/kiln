// SPDX-License-Identifier: GPL-3.0-or-later
//! A generic protected Lua-object reader at the C ABI (`KilnEmlLuaReaderV1`, native/core/abi/services/
//! kiln_eml_values.h) over one real mlua 0.11.6 VM, and a minimal frontend whose proxies call kiln-eml's published
//! kiln.eml.values table through its function pointers, as the MIT frontend (the MIT frontend) will. Nothing here reaches the
//! graph's Rust API: every request crosses the C table, and every Lua read crosses the reader's `extern "C"` callbacks.
//!
//! - Object tokens: every reply gets one (scalars too). Temporary ones live until the frontend's outermost call ends
//!   (`Host::call`); `arg` tokens live until the test drops them; retained ones until released.
//! - Proxies: one userdata per node, interned in a weak table by node id; a new proxy ADOPTS the provisional lease and
//!   its finalizer closes it once (also when Lua collects it inside a reader callback).
//! - SOURCE_TO_STRING is mlua's `Value::to_string`; GET_FIELD_STRING is `Table::get::<String>`; LENGTH is `Table::len`
//!   (metamethods run every time); RAW_SEQUENCE_INDEX is `raw_get`.
//! - `wake` touches only [`Shared`] (any thread); everything else runs on the worker that created the arena.
#![allow(dead_code)]
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    ffi::c_void,
    panic::AssertUnwindSafe,
    rc::{Rc, Weak},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicPtr, AtomicU64, AtomicUsize, Ordering},
    },
    thread::ThreadId,
};

use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_OK, KilnResult,
    KilnStr,
    services::{
        kiln_assets::KilnAssetKey,
        kiln_eml_values::{
            KILN_LUA_ADAPTER_CONTENT, KILN_LUA_BOOLEAN, KILN_LUA_DESCRIBE, KILN_LUA_GET_FIELD_STRING,
            KILN_LUA_GET_FIELD_VALUE, KILN_LUA_INTEGER, KILN_LUA_KNOWN_USERDATA, KILN_LUA_LEASE_ADOPTED,
            KILN_LUA_LEASE_REUSED, KILN_LUA_LENGTH, KILN_LUA_MAKE_TYPED_PROXY, KILN_LUA_NIL, KILN_LUA_NUMBER,
            KILN_LUA_OTHER, KILN_LUA_OTHER_USERDATA, KILN_LUA_RAW_SEQUENCE_INDEX, KILN_LUA_SOURCE_TO_STRING,
            KILN_LUA_STRING, KILN_LUA_TABLE, KILN_LUA_TYPED_VALUE, KILN_VALUE_FIELD, KILN_VALUE_INDEX,
            KILN_VALUE_OPEN_BYTES, KILN_VALUE_OPEN_LOADED_SNAPSHOT, KILN_VALUE_OPEN_LUA_INPUT, KILN_VALUE_READ_LENGTH,
            KILN_VALUE_READ_STRINGIFY, KILN_VALUE_SEMANTIC_ERROR, KILN_VALUE_VARIANT_MAPPED, KILN_VALUE_VARIANT_OWNED,
            KilnEmlLuaAccessV1, KilnEmlLuaReaderV1, KilnEmlLuaValueV1, KilnEmlValueInfoV1, KilnEmlValueOpenV1,
            KilnEmlValueResultV1, KilnEmlValueSelectorV1, KilnEmlValuesApiV1,
        },
    },
};
use mlua::{AnyUserData, Lua, MetaMethod, UserData, UserDataMethods, Value};
use serde_json::Value as Json;

pub const HOST_OWNER: u64 = 0xa;
pub const OWNER_B: u64 = 0xb;

/// What the host observes from any thread (`wake` runs on the closing thread).
pub struct Shared {
    pub api: &'static KilnEmlValuesApiV1,
    pub wakes: AtomicUsize,
    pub wake_controls: Mutex<Vec<u64>>,
    pub wake_threads: Mutex<Vec<ThreadId>>,
    /// When set, `wake` calls back into the table (a host bug): each answer is recorded.
    pub reenter_in_wake: AtomicBool,
    /// A lease `wake` tries to close when re-entering.
    pub reenter_lease: AtomicU64,
    pub reentry_codes: Mutex<Vec<KilnResult>>,
    /// What the refused calls inside `wake` left in their outputs: a result record's v1 fields after `value_read` (each
    /// stale before), then `arena_create`'s token.
    pub reentry_outputs: Mutex<Vec<u64>>,
    /// Every callback other than `wake`, from any thread (the worker-exit test reads it after the worker is gone).
    pub callbacks: AtomicUsize,
}

impl Shared {
    fn on_wake(&self, control: u64) {
        self.wakes.fetch_add(1, Ordering::SeqCst);
        self.wake_controls.lock().unwrap().push(control);
        self.wake_threads.lock().unwrap().push(std::thread::current().id());
        if self.reenter_in_wake.load(Ordering::SeqCst) {
            let mut codes = Vec::new();
            let mut pending = 0u32;
            unsafe {
                codes.push(self.api.value_close.unwrap()(
                    OWNER_B,
                    self.reenter_lease.load(Ordering::SeqCst),
                ));
                codes.push(self.api.arena_close.unwrap()(HOST_OWNER, control, &mut pending));
                codes.push(self.api.arena_collect.unwrap()(HOST_OWNER, control, &mut pending));
                let mut info = info_record(0);
                codes.push(self.api.value_info.unwrap()(
                    OWNER_B,
                    self.reenter_lease.load(Ordering::SeqCst),
                    &mut info,
                ));
                // outputs are cleared even by a refusal from inside wake: stale tokens never survive
                let mut record = KilnEmlValueResultV1 {
                    size: std::mem::size_of::<KilnEmlValueResultV1>() as u32,
                    disposition: 0xcd,
                    flags: 0xcd,
                    arity: 0xcd,
                    item: u64::MAX,
                    payload: u64::MAX,
                    error: u64::MAX,
                    receiver_revision: u64::MAX,
                };
                codes.push(self.api.value_read.unwrap()(
                    OWNER_B,
                    self.reenter_lease.load(Ordering::SeqCst),
                    KILN_VALUE_READ_LENGTH as u32,
                    &mut record,
                ));
                let mut arena = u64::MAX;
                codes.push(self.api.arena_create.unwrap()(
                    HOST_OWNER,
                    0,
                    0,
                    std::ptr::null(),
                    &mut arena,
                ));
                self.reentry_outputs.lock().unwrap().extend([
                    u64::from(record.size),
                    u64::from(record.disposition),
                    u64::from(record.flags),
                    u64::from(record.arity),
                    record.item,
                    record.payload,
                    record.error,
                    record.receiver_revision,
                    arena,
                ]);
            }
            self.reentry_codes.lock().unwrap().extend(codes);
        }
    }
}

/// The reader's `user`: the host for worker callbacks, the shared half for `wake`.
pub struct Ctx {
    host: AtomicPtr<Host>,
    pub shared: Arc<Shared>,
}

#[derive(Default)]
pub struct Stats {
    pub accesses: Cell<usize>,
    pub by_op: RefCell<HashMap<u32, usize>>,
    pub copies: Cell<usize>,
    pub adopted: Cell<usize>,
    pub reused: Cell<usize>,
    pub retains: Cell<usize>,
    pub releases: Cell<usize>,
    pub stray_releases: Cell<usize>,
    pub releases_in_access: Cell<usize>,
    pub closes_in_proxy: Cell<usize>,
    pub releases_in_proxy_gc: Cell<usize>,
    in_access: Cell<bool>,
}

/// A Content userdata (adapter CONTENT) and a userdata the codec does not know.
pub struct ContentUd(pub [u8; 16]);
impl UserData for ContentUd {}
pub struct Foreign;
impl UserData for Foreign {}

pub struct Host {
    pub lua: Lua,
    pub api: &'static KilnEmlValuesApiV1,
    pub view: u64,
    pub control: Cell<u64>,
    pub arena_id: Cell<u64>,
    next: Cell<u64>,
    temps: RefCell<HashMap<u64, Value>>,
    args: RefCell<HashMap<u64, Value>>,
    pub retained: RefCell<HashMap<u64, (Value, usize)>>,
    depth: Cell<u32>,
    pub stats: Stats,
    pub closes: Rc<Cell<usize>>,
    pub gc_in_proxy: Cell<bool>,
    pub ctx: Box<Ctx>,
    pub reader: RefCell<KilnEmlLuaReaderV1>,
}

/// The v1 reader table over `ctx`.
pub fn reader_table(ctx: *const Ctx) -> KilnEmlLuaReaderV1 {
    KilnEmlLuaReaderV1 {
        size: std::mem::size_of::<KilnEmlLuaReaderV1>() as u32,
        version: 1,
        user: ctx as *mut c_void,
        access: Some(cb_access),
        copy: Some(cb_copy),
        retain: Some(cb_retain),
        release: Some(cb_release),
        wake: Some(cb_wake),
    }
}

pub fn result_record() -> KilnEmlValueResultV1 {
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

pub fn info_record(requested: u32) -> KilnEmlValueInfoV1 {
    let mut info: KilnEmlValueInfoV1 = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnEmlValueInfoV1>() as u32;
    info.requested = requested;
    info
}

pub fn kstr(bytes: &[u8]) -> KilnStr {
    KilnStr {
        ptr: bytes.as_ptr().cast(),
        len: bytes.len(),
    }
}

pub fn field(name: &[u8]) -> KilnEmlValueSelectorV1 {
    KilnEmlValueSelectorV1 {
        size: std::mem::size_of::<KilnEmlValueSelectorV1>() as u32,
        kind: KILN_VALUE_FIELD as u32,
        index: 0,
        field: kstr(name),
    }
}

pub fn index(index: u64) -> KilnEmlValueSelectorV1 {
    KilnEmlValueSelectorV1 {
        size: std::mem::size_of::<KilnEmlValueSelectorV1>() as u32,
        kind: KILN_VALUE_INDEX as u32,
        index,
        field: kstr(b""),
    }
}

pub fn open_record(origin: i32, qualified_type: &[u8], bytes: &[u8], lua_object: u64) -> KilnEmlValueOpenV1 {
    KilnEmlValueOpenV1 {
        size: std::mem::size_of::<KilnEmlValueOpenV1>() as u32,
        origin: origin as u32,
        qualified_type: kstr(qualified_type),
        key: KilnAssetKey {
            guid: [0; 16],
            r#type: 0,
            part: 0,
        },
        source_instance: 0,
        source_session: 0,
        bytes: bytes.as_ptr().cast(),
        length: bytes.len(),
        lua_object,
    }
}

fn host_of(user: *mut c_void) -> &'static Host {
    let ctx = unsafe { &*(user as *const Ctx) };
    unsafe { &*ctx.host.load(Ordering::SeqCst) }
}

fn guarded<R>(user: *mut c_void, fail: R, work: impl FnOnce(&Host) -> R) -> R {
    let ctx = unsafe { &*(user as *const Ctx) };
    ctx.shared.callbacks.fetch_add(1, Ordering::SeqCst);
    // a host callback never unwinds into the provider
    std::panic::catch_unwind(AssertUnwindSafe(|| work(host_of(user)))).unwrap_or(fail)
}

unsafe extern "C" fn cb_access(
    user: *mut c_void,
    request: *const KilnEmlLuaAccessV1,
    reply: *mut KilnEmlLuaValueV1,
) -> KilnResult {
    guarded(user, KILN_ERR_INTERNAL, |host| {
        let request = unsafe { *request };
        host.stats.accesses.set(host.stats.accesses.get() + 1);
        *host.stats.by_op.borrow_mut().entry(request.operation).or_default() += 1;
        let outer = host.stats.in_access.replace(true);
        let answer = host.access(&request);
        host.stats.in_access.set(outer);
        match answer {
            Ok(value) => {
                unsafe { reply.write(value) };
                KILN_OK
            }
            Err(rc) => rc,
        }
    })
}

unsafe extern "C" fn cb_copy(
    user: *mut c_void,
    object: u64,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    guarded(user, KILN_ERR_INTERNAL, |host| {
        host.stats.copies.set(host.stats.copies.get() + 1);
        let bytes = match host.copy_of(object) {
            Ok(bytes) => bytes,
            Err(rc) => return rc,
        };
        unsafe { required.write(bytes.len()) };
        if capacity < bytes.len() {
            return KILN_ERR_BUFFER_TOO_SMALL;
        }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), bytes.len()) };
        KILN_OK
    })
}

unsafe extern "C" fn cb_retain(user: *mut c_void, object: u64) -> KilnResult {
    guarded(user, KILN_ERR_INTERNAL, |host| {
        let Some(value) = host.object(object) else {
            return KILN_ERR_NOT_FOUND;
        };
        host.stats.retains.set(host.stats.retains.get() + 1);
        host.retained.borrow_mut().entry(object).or_insert((value, 0)).1 += 1;
        KILN_OK
    })
}

unsafe extern "C" fn cb_release(user: *mut c_void, object: u64) {
    guarded(user, (), |host| {
        host.stats.releases.set(host.stats.releases.get() + 1);
        if host.stats.in_access.get() {
            host.stats
                .releases_in_access
                .set(host.stats.releases_in_access.get() + 1);
        }
        let mut retained = host.retained.borrow_mut();
        match retained.get_mut(&object) {
            Some((_, count)) if *count > 1 => *count -= 1,
            Some(_) => {
                retained.remove(&object);
            }
            None => host.stats.stray_releases.set(host.stats.stray_releases.get() + 1),
        }
    })
}

unsafe extern "C" fn cb_wake(user: *mut c_void, control: u64) {
    let ctx = unsafe { &*(user as *const Ctx) };
    let _ = std::panic::catch_unwind(AssertUnwindSafe(|| ctx.shared.on_wake(control)));
}

/// The frontend userdata of a graph node: it holds its lease and closes it once when Lua collects it.
pub struct Proxy {
    pub lease: u64,
    closes: Rc<Cell<usize>>,
    api: &'static KilnEmlValuesApiV1,
}

impl Drop for Proxy {
    fn drop(&mut self) {
        self.closes.set(self.closes.get() + 1);
        // a finalizer: may run inside any reader callback; the provider defers the drain to the outer call's end
        let _ = unsafe { self.api.value_close.unwrap()(0, self.lease) };
    }
}

fn frontend(lua: &Lua) -> Rc<Host> {
    lua.app_data_ref::<Weak<Host>>().unwrap().upgrade().unwrap()
}

fn selector_of(key: &Value) -> KilnEmlValueSelectorV1 {
    match key {
        Value::Integer(i) => index(*i as u64),
        Value::String(s) => field(Box::leak(s.as_bytes().to_vec().into_boxed_slice())),
        _ => field(b"?"),
    }
}

impl UserData for Proxy {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_method(MetaMethod::Index, |lua, this, key: Value| {
            let host = frontend(lua);
            let lease = this.lease;
            host.call(|| {
                let selector = selector_of(&key);
                let reply = host.lua_op(|r| unsafe { host.api.value_child.unwrap()(OWNER_B, lease, &selector, r) })?;
                host.raise(&reply)?;
                if reply.item != 0 {
                    return Ok(host.wrap(reply.item));
                }
                host.scalar(reply.payload)
            })
        });
        methods.add_meta_method(MetaMethod::NewIndex, |lua, this, (key, value): (Value, Value)| {
            let host = frontend(lua);
            let lease = this.lease;
            host.call(|| {
                let object = host.temp(value);
                let selector = selector_of(&key);
                let reply =
                    host.lua_op(|r| unsafe { host.api.value_set.unwrap()(OWNER_B, lease, &selector, object, r) })?;
                host.raise(&reply)
            })
        });
        methods.add_meta_method(MetaMethod::Len, |lua, this, ()| {
            let host = frontend(lua);
            let lease = this.lease;
            host.call(|| {
                let reply = host.lua_op(|r| unsafe {
                    host.api.value_read.unwrap()(OWNER_B, lease, KILN_VALUE_READ_LENGTH as u32, r)
                })?;
                host.raise(&reply)?;
                host.scalar(reply.payload)
            })
        });
        methods.add_meta_method(MetaMethod::ToString, |lua, this, ()| {
            let host = frontend(lua);
            let lease = this.lease;
            let mut info = info_record(0);
            let _ = unsafe { host.api.value_info.unwrap()(OWNER_B, lease, &mut info) };
            if info.kind == KILN_VALUE_VARIANT_OWNED as u32 || info.kind == KILN_VALUE_VARIANT_MAPPED as u32 {
                return Ok(Value::String(lua.create_string(format!("Proxy: {lease:#x}"))?));
            }
            host.call(|| {
                let reply = host.lua_op(|r| unsafe {
                    host.api.value_read.unwrap()(OWNER_B, lease, KILN_VALUE_READ_STRINGIFY as u32, r)
                })?;
                host.raise(&reply)?;
                let bytes = host.bytes(reply.payload);
                host.close(reply.payload);
                Ok(Value::String(host.lua.create_string(bytes)?))
            })
        });
    }
}

impl Host {
    /// A host over `view` with a fresh arena of `phase` on this thread (its worker); panics if the arena is refused.
    pub fn new(api: &'static KilnEmlValuesApiV1, view: u64, phase: i32) -> Rc<Host> {
        let host = Self::bare(api, view);
        let rc = host.create(phase);
        assert_eq!(rc, KILN_OK, "arena_create");
        host
    }

    /// A host with no arena yet (`create` makes it).
    pub fn bare(api: &'static KilnEmlValuesApiV1, view: u64) -> Rc<Host> {
        let lua = Lua::new();
        let intern = lua.create_table().unwrap();
        let mode = lua.create_table().unwrap();
        mode.set("__mode", "v").unwrap();
        let _ = intern.set_metatable(Some(mode));
        lua.set_named_registry_value("intern", intern).unwrap();
        let shared = Arc::new(Shared {
            api,
            wakes: AtomicUsize::new(0),
            wake_controls: Mutex::new(Vec::new()),
            wake_threads: Mutex::new(Vec::new()),
            reenter_in_wake: AtomicBool::new(false),
            reenter_lease: AtomicU64::new(0),
            reentry_codes: Mutex::new(Vec::new()),
            reentry_outputs: Mutex::new(Vec::new()),
            callbacks: AtomicUsize::new(0),
        });
        let ctx = Box::new(Ctx {
            host: AtomicPtr::new(std::ptr::null_mut()),
            shared,
        });
        let reader = reader_table(&*ctx);
        let host = Rc::new(Host {
            lua,
            api,
            view,
            control: Cell::new(0),
            arena_id: Cell::new(0),
            next: Cell::new(1),
            temps: RefCell::new(HashMap::new()),
            args: RefCell::new(HashMap::new()),
            retained: RefCell::new(HashMap::new()),
            depth: Cell::new(0),
            stats: Stats::default(),
            closes: Rc::new(Cell::new(0)),
            gc_in_proxy: Cell::new(false),
            ctx,
            reader: RefCell::new(reader),
        });
        host.ctx.host.store(Rc::as_ptr(&host) as *mut Host, Ordering::SeqCst);
        host.lua.set_app_data(Rc::downgrade(&host));
        let api_ptr = api;
        let lib = host.lua.create_table().unwrap();
        // the dispatcher's typed half (sfi-core extend_table_operations, recognized userdata only)
        lib.set(
            "insert",
            host.lua
                .create_function(move |lua, args: mlua::MultiValue| {
                    let host = frontend(lua);
                    let args: Vec<Value> = args.into_iter().collect();
                    let lease = lease_of(&args[0])?;
                    host.call(|| {
                        let reply = if args.len() == 2 {
                            let object = host.temp(args[1].clone());
                            host.lua_op(|r| unsafe { api_ptr.value_array_push.unwrap()(OWNER_B, lease, object, r) })?
                        } else {
                            let position = args[1].as_integer().unwrap_or(0) as u64;
                            let object = host.temp(args[2].clone());
                            host.lua_op(|r| unsafe {
                                api_ptr.value_array_insert.unwrap()(OWNER_B, lease, position, object, r)
                            })?
                        };
                        host.raise(&reply)
                    })
                })
                .unwrap(),
        )
        .unwrap();
        lib.set(
            "remove",
            host.lua
                .create_function(move |lua, args: mlua::MultiValue| {
                    let host = frontend(lua);
                    let args: Vec<Value> = args.into_iter().collect();
                    let lease = lease_of(&args[0])?;
                    host.call(|| {
                        let reply = if args.len() == 1 {
                            host.lua_op(|r| unsafe { api_ptr.value_array_pop.unwrap()(OWNER_B, lease, r) })?
                        } else {
                            let position = args[1].as_integer().unwrap_or(0) as u64;
                            host.lua_op(|r| unsafe {
                                api_ptr.value_array_remove.unwrap()(OWNER_B, lease, position, r)
                            })?
                        };
                        host.raise(&reply)?;
                        if reply.item == 0 {
                            return Ok(Value::Nil);
                        }
                        if reply.payload != 0 {
                            host.close(reply.item);
                            return host.scalar(reply.payload);
                        }
                        Ok(host.wrap(reply.item))
                    })
                })
                .unwrap(),
        )
        .unwrap();
        lib.set(
            "clear",
            host.lua
                .create_function(move |lua, array: Value| {
                    let host = frontend(lua);
                    let lease = lease_of(&array)?;
                    host.call(|| {
                        let reply =
                            host.lua_op(|r| unsafe { api_ptr.value_array_clear.unwrap()(OWNER_B, lease, r) })?;
                        host.raise(&reply)
                    })
                })
                .unwrap(),
        )
        .unwrap();
        host.lua.globals().set("vt", lib).unwrap();
        host
    }

    /// arena_create with this host's current reader table.
    pub fn create(&self, phase: i32) -> KilnResult {
        let mut control = u64::MAX;
        let reader = *self.reader.borrow();
        let rc = unsafe { self.api.arena_create.unwrap()(HOST_OWNER, self.view, phase as u32, &reader, &mut control) };
        if rc == KILN_OK {
            self.control.set(control);
        } else {
            assert_eq!(control, 0, "a refused arena_create writes no token");
        }
        rc
    }

    pub fn shared(&self) -> &Arc<Shared> {
        &self.ctx.shared
    }

    /// One outermost frontend call: temporary objects die when it ends.
    pub fn call<R>(&self, work: impl FnOnce() -> R) -> R {
        self.depth.set(self.depth.get() + 1);
        let result = work();
        self.depth.set(self.depth.get() - 1);
        if self.depth.get() == 0 {
            self.temps.borrow_mut().clear();
        }
        result
    }

    pub fn temp(&self, value: Value) -> u64 {
        let token = self.next.get();
        self.next.set(token + 1);
        self.temps.borrow_mut().insert(token, value);
        token
    }

    /// A value the caller keeps alive across calls (a Lua stack slot): its object token.
    pub fn arg(&self, value: Value) -> u64 {
        let token = self.next.get();
        self.next.set(token + 1);
        self.args.borrow_mut().insert(token, value);
        token
    }

    pub fn drop_args(&self) {
        self.args.borrow_mut().clear();
    }

    fn object(&self, token: u64) -> Option<Value> {
        if let Some(value) = self.temps.borrow().get(&token) {
            return Some(value.clone());
        }
        if let Some(value) = self.args.borrow().get(&token) {
            return Some(value.clone());
        }
        self.retained.borrow().get(&token).map(|(value, _)| value.clone())
    }

    fn reply(&self, value: Value) -> KilnEmlLuaValueV1 {
        let object = self.temp(value.clone());
        let mut reply = KilnEmlLuaValueV1 {
            size: std::mem::size_of::<KilnEmlLuaValueV1>() as u32,
            kind: KILN_LUA_NIL as u32,
            object,
            bits: 0,
            byte_length: 0,
            typed_value: 0,
            error_object: 0,
            adapter_class: 0,
            lease_disposition: 0,
        };
        match value {
            Value::Nil => {}
            Value::Boolean(b) => {
                reply.kind = KILN_LUA_BOOLEAN as u32;
                reply.bits = b as u64;
            }
            Value::Integer(i) => {
                reply.kind = KILN_LUA_INTEGER as u32;
                reply.bits = i as u64;
            }
            Value::Number(n) => {
                reply.kind = KILN_LUA_NUMBER as u32;
                reply.bits = n.to_bits();
            }
            Value::String(s) => {
                reply.kind = KILN_LUA_STRING as u32;
                reply.byte_length = s.as_bytes().len() as u64;
            }
            Value::Table(_) => reply.kind = KILN_LUA_TABLE as u32,
            Value::UserData(ud) => {
                if let Ok(proxy) = ud.borrow::<Proxy>() {
                    reply.kind = KILN_LUA_TYPED_VALUE as u32;
                    reply.typed_value = proxy.lease;
                } else if ud.is::<ContentUd>() {
                    reply.kind = KILN_LUA_KNOWN_USERDATA as u32;
                    reply.adapter_class = KILN_LUA_ADAPTER_CONTENT as u32;
                } else {
                    reply.kind = KILN_LUA_OTHER_USERDATA as u32;
                }
            }
            _ => reply.kind = KILN_LUA_OTHER as u32,
        }
        reply
    }

    fn error(&self, error: mlua::Error) -> KilnEmlLuaValueV1 {
        let mut reply = self.reply(Value::Nil);
        reply.error_object = self.temp(Value::Error(Box::new(error)));
        reply
    }

    fn protected(&self, result: mlua::Result<Value>) -> KilnEmlLuaValueV1 {
        match result {
            Ok(value) => self.reply(value),
            Err(error) => self.error(error),
        }
    }

    pub fn count(&self, operation: i32) -> usize {
        self.stats.by_op.borrow().get(&(operation as u32)).copied().unwrap_or(0)
    }

    fn table_of(&self, object: u64) -> Result<mlua::Table, KilnResult> {
        match self.object(object) {
            Some(Value::Table(t)) => Ok(t),
            _ => Err(KILN_ERR_INVALID_ARGUMENT),
        }
    }

    fn key(request: &KilnEmlLuaAccessV1) -> Vec<u8> {
        if request.key.len == 0 {
            return Vec::new();
        }
        unsafe { std::slice::from_raw_parts(request.key.ptr.cast::<u8>(), request.key.len) }.to_vec()
    }

    fn access(&self, request: &KilnEmlLuaAccessV1) -> Result<KilnEmlLuaValueV1, KilnResult> {
        assert_eq!(request.size as usize, std::mem::size_of::<KilnEmlLuaAccessV1>());
        let operation = request.operation as i32;
        if operation != KILN_LUA_MAKE_TYPED_PROXY {
            assert_eq!(
                (request.arena, request.node_id),
                (0, 0),
                "only MAKE_TYPED_PROXY names a node"
            );
        }
        Ok(match operation {
            KILN_LUA_DESCRIBE => {
                let value = self.object(request.object).ok_or(KILN_ERR_NOT_FOUND)?;
                self.reply(value)
            }
            KILN_LUA_GET_FIELD_VALUE => {
                let table = self.table_of(request.object)?;
                let key = self.lua.create_string(Self::key(request)).unwrap();
                self.protected(table.get::<Value>(key))
            }
            KILN_LUA_GET_FIELD_STRING => {
                let table = self.table_of(request.object)?;
                let key = self.lua.create_string(Self::key(request)).unwrap();
                match table.get::<String>(key) {
                    Ok(text) => self.reply(Value::String(self.lua.create_string(text).unwrap())),
                    Err(error) => self.error(error),
                }
            }
            KILN_LUA_LENGTH => {
                let table = self.table_of(request.object)?;
                self.protected(table.len().map(Value::Integer))
            }
            KILN_LUA_RAW_SEQUENCE_INDEX => {
                let table = self.table_of(request.object)?;
                self.reply(table.raw_get::<Value>(request.index).unwrap())
            }
            KILN_LUA_SOURCE_TO_STRING => {
                let value = self.object(request.object).ok_or(KILN_ERR_NOT_FOUND)?;
                match value.to_string() {
                    Ok(text) => self.reply(Value::String(self.lua.create_string(text).unwrap())),
                    Err(error) => self.error(error),
                }
            }
            KILN_LUA_MAKE_TYPED_PROXY => self.proxy(request.object, request.arena, request.node_id),
            _ => return Err(KILN_ERR_INVALID_ARGUMENT),
        })
    }

    fn proxy(&self, provisional: u64, arena: u64, node_id: u64) -> KilnEmlLuaValueV1 {
        if self.arena_id.get() == 0 {
            self.arena_id.set(arena);
        }
        assert_eq!(
            arena,
            self.arena_id.get(),
            "MAKE_TYPED_PROXY names this arena's identity"
        );
        // identity-only value_info re-entry is allowed here
        let mut info = info_record(0);
        assert_eq!(
            unsafe { self.api.value_info.unwrap()(OWNER_B, provisional, &mut info) },
            KILN_OK
        );
        assert_eq!(info.identity.node_id, node_id);
        if self.gc_in_proxy.get() {
            let (closes, releases) = (self.closes.get(), self.stats.releases.get());
            self.lua.gc_collect().unwrap();
            self.lua.gc_collect().unwrap();
            self.stats
                .closes_in_proxy
                .set(self.stats.closes_in_proxy.get() + self.closes.get() - closes);
            self.stats
                .releases_in_proxy_gc
                .set(self.stats.releases_in_proxy_gc.get() + self.stats.releases.get() - releases);
        }
        let intern: mlua::Table = self.lua.named_registry_value("intern").unwrap();
        let existing: Value = intern.raw_get(node_id as i64).unwrap();
        if let Value::UserData(_) = existing {
            self.stats.reused.set(self.stats.reused.get() + 1);
            let mut reply = self.reply(existing);
            reply.lease_disposition = KILN_LUA_LEASE_REUSED as u32;
            return reply;
        }
        let ud = self
            .lua
            .create_userdata(Proxy {
                lease: provisional,
                closes: self.closes.clone(),
                api: self.api,
            })
            .unwrap();
        intern.raw_set(node_id as i64, ud.clone()).unwrap();
        self.stats.adopted.set(self.stats.adopted.get() + 1);
        let mut reply = self.reply(Value::UserData(ud));
        reply.lease_disposition = KILN_LUA_LEASE_ADOPTED as u32;
        reply
    }

    fn copy_of(&self, object: u64) -> Result<Vec<u8>, KilnResult> {
        match self.object(object).ok_or(KILN_ERR_NOT_FOUND)? {
            Value::String(s) => Ok(s.as_bytes().to_vec()),
            Value::Error(error) => Ok(format!("{error}").into_bytes()),
            Value::UserData(ud) => match ud.borrow::<ContentUd>() {
                Ok(content) => Ok(content.0.to_vec()),
                Err(_) => Err(KILN_ERR_INVALID_ARGUMENT),
            },
            _ => Err(KILN_ERR_INVALID_ARGUMENT),
        }
    }

    // ---- the frontend's C calls ----

    /// One member that fills a result record: the record, or the member's KilnResult.
    pub fn op(
        &self,
        call: impl FnOnce(*mut KilnEmlValueResultV1) -> KilnResult,
    ) -> Result<KilnEmlValueResultV1, KilnResult> {
        let mut record = result_record();
        match call(&mut record) {
            KILN_OK => {
                assert_eq!(record.size as usize, std::mem::size_of::<KilnEmlValueResultV1>());
                Ok(record)
            }
            rc => {
                assert_eq!(
                    (record.disposition, record.item, record.payload, record.error),
                    (0, 0, 0, 0),
                    "a refused call leaves no token in its result"
                );
                Err(rc)
            }
        }
    }

    fn lua_op(&self, call: impl FnOnce(*mut KilnEmlValueResultV1) -> KilnResult) -> mlua::Result<KilnEmlValueResultV1> {
        self.op(call)
            .map_err(|rc| mlua::Error::external(format!("refused: KilnResult {rc}")))
    }

    /// A SEMANTIC_ERROR as the Lua error the frontend raises (its root message).
    pub fn raise(&self, reply: &KilnEmlValueResultV1) -> mlua::Result<Value> {
        if reply.disposition == KILN_VALUE_SEMANTIC_ERROR as u32 {
            let tree = self.json(reply.error);
            self.close(reply.error);
            let message = unhex(tree["error"]["message"].as_str().unwrap());
            return Err(mlua::Error::external(String::from_utf8_lossy(&message).into_owned()));
        }
        Ok(Value::Nil)
    }

    pub fn info(&self, lease: u64, requested: u32) -> Result<KilnEmlValueInfoV1, KilnResult> {
        let mut info = info_record(requested);
        match unsafe { self.api.value_info.unwrap()(OWNER_B, lease, &mut info) } {
            KILN_OK => Ok(info),
            rc => Err(rc),
        }
    }

    /// An item lease as its interned userdata: a known node reuses its proxy (the item closes), a new one gets a proxy
    /// that keeps the item as its own lease.
    pub fn wrap(&self, item: u64) -> Value {
        let info = self.info(item, 0).unwrap();
        let intern: mlua::Table = self.lua.named_registry_value("intern").unwrap();
        let existing: Value = intern.raw_get(info.identity.node_id as i64).unwrap();
        if let Value::UserData(_) = existing {
            self.close(item);
            return existing;
        }
        let ud = self
            .lua
            .create_userdata(Proxy {
                lease: item,
                closes: self.closes.clone(),
                api: self.api,
            })
            .unwrap();
        intern.raw_set(info.identity.node_id as i64, ud.clone()).unwrap();
        Value::UserData(ud)
    }

    /// A VIEW (or LENGTH) payload of a scalar as its Lua value (the payload closes).
    pub fn scalar(&self, payload: u64) -> mlua::Result<Value> {
        let packet = self.json(payload);
        self.close(payload);
        let value = if packet["packet"] == "view" {
            &packet["value"]
        } else {
            &packet
        };
        let text = |prefix: &str| unhex(value["value"].as_str().unwrap().strip_prefix(prefix).unwrap());
        Ok(match (value["packet"].as_str(), value["tag"].as_str()) {
            (Some("length"), _) => Value::Integer(i64::from_be_bytes(text("i64:").try_into().unwrap())),
            (_, Some("nil")) => Value::Nil,
            (_, Some("bool")) => Value::Boolean(value["value"].as_bool().unwrap()),
            (_, Some("i64")) => Value::Integer(i64::from_be_bytes(text("i64:").try_into().unwrap())),
            (_, Some("f64")) => Value::Number(f64::from_bits(u64::from_be_bytes(text("f64:").try_into().unwrap()))),
            (_, Some("string")) => Value::String(self.lua.create_string(text("string:")).unwrap()),
            other => panic!("no scalar: {other:?} {value}"),
        })
    }

    /// A packet's bytes: a size probe (BUFFER_TOO_SMALL), then the copy.
    pub fn bytes(&self, packet: u64) -> Vec<u8> {
        let copy = self.api.value_result_copy.unwrap();
        let mut required = 0usize;
        let rc = unsafe { copy(OWNER_B, packet, std::ptr::null_mut(), 0, &mut required) };
        if required == 0 {
            assert_eq!(rc, KILN_OK);
            return Vec::new();
        }
        assert_eq!(rc, KILN_ERR_BUFFER_TOO_SMALL);
        let mut out = vec![0u8; required];
        let mut again = 0usize;
        assert_eq!(
            unsafe { copy(OWNER_B, packet, out.as_mut_ptr().cast(), out.len(), &mut again) },
            KILN_OK
        );
        assert_eq!(again, required);
        out
    }

    pub fn json(&self, packet: u64) -> Json {
        serde_json::from_slice(&self.bytes(packet)).unwrap()
    }

    pub fn close(&self, token: u64) {
        if token != 0 {
            assert_eq!(unsafe { self.api.value_close.unwrap()(OWNER_B, token) }, KILN_OK);
        }
    }

    pub fn eval(&self, chunk: &str) -> Value {
        self.lua.load(chunk).eval().unwrap()
    }

    pub fn open(&self, record: &KilnEmlValueOpenV1) -> Result<KilnEmlValueResultV1, KilnResult> {
        self.op(|r| unsafe { self.api.value_open.unwrap()(HOST_OWNER, self.control.get(), self.view, record, r) })
    }

    /// LUA_INPUT: a fresh owned root of `qualified_type` from a Lua chunk.
    pub fn open_lua(&self, qualified_type: &str, chunk: &str) -> KilnEmlValueResultV1 {
        let object = self.temp(self.eval(chunk));
        self.open(&open_record(
            KILN_VALUE_OPEN_LUA_INPUT,
            qualified_type.as_bytes(),
            &[],
            object,
        ))
        .unwrap()
    }

    /// BYTES: a fresh mapped root.
    pub fn open_bytes(&self, qualified_type: &str, bytes: &[u8]) -> KilnEmlValueResultV1 {
        self.open(&open_record(KILN_VALUE_OPEN_BYTES, qualified_type.as_bytes(), bytes, 0))
            .unwrap()
    }

    /// LOADED_SNAPSHOT: the root of one loaded ResourceInfo instance.
    pub fn open_loaded(&self, qualified_type: &str, bytes: &[u8], instance: u64) -> KilnEmlValueResultV1 {
        let mut record = open_record(KILN_VALUE_OPEN_LOADED_SNAPSHOT, qualified_type.as_bytes(), bytes, 0);
        record.source_instance = instance;
        record.source_session = 1;
        self.open(&record).unwrap()
    }

    /// A root as a Lua global (through the frontend's interning).
    pub fn global(&self, name: &str, reply: KilnEmlValueResultV1) {
        assert_eq!(reply.disposition, 1, "{}", reply.disposition);
        let value = self.wrap(reply.item);
        self.lua.globals().set(name, value).unwrap();
    }

    pub fn run(&self, chunk: &str) {
        let chunk = format!("{PERR}\n{chunk}");
        if let Err(e) = self.lua.load(&chunk).set_name("=g12").exec() {
            panic!("{e}");
        }
    }

    pub fn lease_of_global(&self, name: &str) -> u64 {
        let ud: AnyUserData = self.lua.globals().get(name).unwrap();
        ud.borrow::<Proxy>().unwrap().lease
    }

    /// arena_close + arena_collect on this worker: everything retained is released, once, outside any reader call.
    pub fn finish(&self) {
        self.args.borrow_mut().clear();
        let mut pending = u32::MAX;
        assert_eq!(
            unsafe { self.api.arena_close.unwrap()(HOST_OWNER, self.control.get(), &mut pending) },
            KILN_OK
        );
        assert_eq!(pending, 0, "teardown on the worker drains at once");
        assert_eq!(
            unsafe { self.api.arena_collect.unwrap()(HOST_OWNER, self.control.get(), &mut pending) },
            KILN_OK
        );
        assert_eq!(pending, 0);
        self.assert_balanced();
    }

    pub fn assert_balanced(&self) {
        assert!(
            self.retained.borrow().is_empty(),
            "every retained host object was released"
        );
        assert_eq!(self.stats.stray_releases.get(), 0, "no double release");
        assert_eq!(
            self.stats.releases_in_access.get(),
            0,
            "no release under an outer operation"
        );
        assert_eq!(self.stats.retains.get(), self.stats.releases.get());
    }
}

fn lease_of(value: &Value) -> mlua::Result<u64> {
    match value {
        Value::UserData(ud) => Ok(ud.borrow::<Proxy>()?.lease),
        _ => Err(mlua::Error::external("not a typed value")),
    }
}

pub const PERR: &str = r#"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

pub fn unhex(text: &str) -> Vec<u8> {
    let text = text.split_once(':').map_or(text, |(_, rest)| rest);
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap())
        .collect()
}
