// SPDX-License-Identifier: MIT
//! The protected Lua-object reader kiln-sfi gives the typed-value provider: kiln.eml.values v1's
//! `KilnEmlLuaReaderV1` (native/core/abi/services/kiln_eml_values.h) over the interpreter's own Lua VM.
//!
//! # The boundary (both licences)
//! This file is MIT and is the MIT half of the seam: the provider of kiln.eml.values is GPL kiln-eml (its typed graph
//! and its reader adapter, documented on its side in `gpl/kiln-eml/kiln-eml/src/archive_values/reader.rs`), and the
//! two halves meet only at the C header. Nothing else crosses: neutral sized records and opaque 64-bit tokens, never a
//! `lua_State`, an mlua value, a Rust error or object, or a pointer into the VM. kiln-sfi never links GPL code; it
//! reaches the table only through [`ServiceAdapter`](crate::runtime::ServiceAdapter).
//!
//! # Tokens
//! Every reply carries an object token, scalars included. A token names a Lua value (or an error the request raised)
//! in this reader's store. Tokens are never reused. A token made during a frontend call (a temporary) lives until the
//! frontend's *outermost* call into the provider returns, nested calls included; the provider keeps one longer only
//! with `retain`, and each `release` drops one retain (on the worker; a release with no retain left is counted and
//! ignored, never a double free).
//!
//! # Requests (each runs once per request, on the arena's worker, inside a protected call)
//! - DESCRIBE: the value's kind; a typed value of this VM (one of the frontend's proxies) carries its lease, and a
//!   userdata a [`KnownUserdata`] adapter recognises carries its adapter class.
//! - GET_FIELD_VALUE: `Table::get` (`__index` runs); GET_FIELD_STRING: `Table::get::<String>` (mlua's own string
//!   conversion, as Source's `table.get::<String>`); LENGTH: `Table::len` (`__len` runs every time);
//!   RAW_SEQUENCE_INDEX: `raw_get` (Source's `sequence_values` reads raw).
//! - SOURCE_TO_STRING: mlua 0.11.6's `Value::to_string` (numbers in Rust's Display, `__tostring` results only when
//!   they are UTF-8); a failure is an error object.
//! - MAKE_TYPED_PROXY: the frontend's userdata for the node, interned in a weak table by node id. A new proxy ADOPTS
//!   the provisional lease (its finalizer closes it once); a live one answers REUSED (the provider closes the
//!   provisional). Only this request re-enters the provider, and only with an identity-only `value_info`.
//!
//! A Lua error raised by a request (a metamethod, a conversion) becomes an error object in the reply: Lua errors,
//! longjmps and Rust panics never cross the C boundary. Every callback is `extern "C"` and runs its work under
//! `catch_unwind`; a panic answers INTERNAL (a transport failure for the provider). A callback on a thread other than
//! the arena's worker answers WRONG_THREAD and touches nothing.
//!
//! # wake
//! `wake(user, control)` may run on any thread, under the provider's lock: it only sets a flag and calls the host's
//! signal function (which must not block, wait for the worker or call the service). The worker collects with that
//! control token at the end of its next outermost call, or when its loop calls
//! [`collect_if_woken`](super::ValuesArena::collect_if_woken).

use super::arena::Values;
use super::proxies;
use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND,
    KILN_ERR_UNAVAILABLE, KILN_ERR_WRONG_THREAD, KILN_OK, KilnHandle, KilnResult, KilnStr,
};
use mlua::{AnyUserData, Lua, RegistryKey, Table, Value, WeakLua};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::ThreadId;

/// A userdata Source's codec knows besides the typed values (a GUID, Content or Resource object): its adapter class
/// (`KILN_LUA_ADAPTER_*`) and its neutral record (GUID: 16 game-order bytes; CONTENT: a 16-byte `KilnContentHash`;
/// RESOURCE: a 24-byte `KilnAssetKey`). The families that own those userdata supply it; none is known by default.
pub trait KnownUserdata {
    fn classify(&self, userdata: &AnyUserData) -> Option<(u32, Vec<u8>)>;
}

/// The any-thread half of the reader: what `wake` touches.
pub struct WakeSignal {
    flag: AtomicBool,
    wakes: AtomicU64,
    control: AtomicU64,
    notify: Option<Arc<dyn Fn() + Send + Sync>>,
}

impl WakeSignal {
    pub(crate) fn new(notify: Option<Arc<dyn Fn() + Send + Sync>>) -> Self {
        Self {
            flag: AtomicBool::new(false),
            wakes: AtomicU64::new(0),
            control: AtomicU64::new(0),
            notify,
        }
    }
    /// How many times the provider woke this arena.
    pub fn wakes(&self) -> u64 {
        self.wakes.load(Ordering::SeqCst)
    }
    /// The control token the last wake carried.
    pub fn last_control(&self) -> KilnHandle {
        self.control.load(Ordering::SeqCst)
    }
    /// Whether a wake arrived since the last call (and clears it).
    pub(crate) fn take(&self) -> bool {
        self.flag.swap(false, Ordering::SeqCst)
    }
    fn wake(&self, control: KilnHandle) {
        self.control.store(control, Ordering::SeqCst);
        self.wakes.fetch_add(1, Ordering::SeqCst);
        self.flag.store(true, Ordering::SeqCst);
        if let Some(notify) = &self.notify {
            notify();
        }
    }
}

/// What the reader counted (status and tests).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReaderCounters {
    /// `access` calls, and per request (`KILN_LUA_*` 0..=6).
    pub accesses: u64,
    pub by_request: [u64; 7],
    /// Requests that answered an error object (a Lua error the request raised).
    pub errors: u64,
    pub copies: u64,
    pub retains: u64,
    pub releases: u64,
    /// Releases of a token with no retain left (ignored).
    pub stray_releases: u64,
    /// MAKE_TYPED_PROXY answers.
    pub adopted: u64,
    pub reused: u64,
    /// Callbacks refused (a bad record, an unknown token) and panics contained. A callback off the worker is refused
    /// without touching this (worker-only) state.
    pub refused: u64,
    pub panics: u64,
}

enum Obj {
    Value(Value),
    Error(mlua::Error),
}

struct Entry {
    obj: Obj,
    retains: u32,
    temporary: bool,
}

#[derive(Default)]
struct Store {
    next: u64,
    entries: HashMap<u64, Entry>,
}

impl Store {
    fn temp(&mut self, obj: Obj) -> u64 {
        self.next += 1;
        let token = self.next;
        self.entries.insert(
            token,
            Entry {
                obj,
                retains: 0,
                temporary: true,
            },
        );
        token
    }
    /// The outermost call ended: temporaries nobody retained die, retained ones live on until released.
    fn end_scope(&mut self) {
        self.entries.retain(|_, entry| {
            if entry.temporary {
                entry.temporary = false;
                entry.retains > 0
            } else {
                true
            }
        });
    }
}

/// The reader's `user`: the any-thread signal, and the worker half behind a thread check.
pub(crate) struct ReaderUser {
    signal: Arc<WakeSignal>,
    local: *const ReaderLocal,
    worker: ThreadId,
}

impl ReaderUser {
    /// `local` comes from `Box::into_raw` and outlives this user (the arena frees both together, or neither).
    pub(crate) fn new(signal: Arc<WakeSignal>, local: *const ReaderLocal) -> Self {
        Self {
            signal,
            local,
            worker: std::thread::current().id(),
        }
    }
}

/// The v1 reader table over `user` (56 bytes, every member set); `user` comes from `Box::into_raw`.
pub(crate) fn table(user: *const ReaderUser) -> KilnEmlLuaReaderV1 {
    KilnEmlLuaReaderV1 {
        size: std::mem::size_of::<KilnEmlLuaReaderV1>() as u32,
        version: 1,
        user: user.cast_mut().cast::<c_void>(),
        access: Some(reader_access),
        copy: Some(reader_copy),
        retain: Some(reader_retain),
        release: Some(reader_release),
        wake: Some(reader_wake),
    }
}

/// The worker half: the VM (weakly: proxies in the VM hold the arena, which holds this), the token store, the proxy
/// interning and the frontend's call depth.
pub(crate) struct ReaderLocal {
    lua: WeakLua,
    pub(crate) values: RefCell<Option<std::rc::Weak<super::arena::ArenaCore>>>,
    store: RefCell<Store>,
    depth: Cell<u32>,
    /// The last userdata/function-like value a reply described in this outermost call (Source's error texts name it).
    found: RefCell<Option<Value>>,
    intern: RegistryKey,
    known: Option<Rc<dyn KnownUserdata>>,
    counters: Cell<ReaderCounters>,
}

impl ReaderLocal {
    pub(crate) fn new(lua: &Lua, known: Option<Rc<dyn KnownUserdata>>) -> mlua::Result<Self> {
        let intern = lua.create_table()?;
        let mode = lua.create_table()?;
        mode.raw_set("__mode", "v")?;
        intern.set_metatable(Some(mode))?;
        Ok(Self {
            lua: lua.weak(),
            values: RefCell::new(None),
            store: RefCell::new(Store::default()),
            depth: Cell::new(0),
            found: RefCell::new(None),
            intern: lua.create_registry_value(intern)?,
            known,
            counters: Cell::new(ReaderCounters::default()),
        })
    }

    pub(crate) fn counters(&self) -> ReaderCounters {
        self.counters.get()
    }
    fn count(&self, update: impl FnOnce(&mut ReaderCounters)) {
        let mut counters = self.counters.get();
        update(&mut counters);
        self.counters.set(counters);
    }

    /// Tokens alive now: (all, retained).
    pub(crate) fn live_tokens(&self) -> (usize, usize) {
        let store = self.store.borrow();
        let retained = store.entries.values().filter(|e| e.retains > 0).count();
        (store.entries.len(), retained)
    }
    /// Retains not yet released, summed over tokens (what the provider counts as pending host objects).
    pub(crate) fn outstanding_retains(&self) -> u64 {
        self.store
            .borrow()
            .entries
            .values()
            .map(|e| u64::from(e.retains))
            .sum()
    }

    /// Enters a frontend call; true for the outermost one.
    pub(crate) fn enter(&self) -> bool {
        let depth = self.depth.get();
        self.depth.set(depth + 1);
        if depth == 0 {
            self.found.borrow_mut().take();
        }
        depth == 0
    }
    /// Leaves a frontend call; true when that was the outermost one (its temporaries are gone now).
    pub(crate) fn leave(&self) -> bool {
        let depth = self.depth.get().saturating_sub(1);
        self.depth.set(depth);
        if depth == 0 {
            self.store.borrow_mut().end_scope();
            // the found value served this call's error texts only; it never outlives the call
            self.found.borrow_mut().take();
            return true;
        }
        false
    }

    /// A temporary token for a value the frontend passes (a `lua_object` argument).
    pub(crate) fn argument(&self, value: Value) -> u64 {
        self.store.borrow_mut().temp(Obj::Value(value))
    }

    /// The error a retained host-error token names (the frontend's own reference, taken before the error lease
    /// closes).
    pub(crate) fn error_of(&self, token: u64) -> Option<mlua::Error> {
        match &self.store.borrow().entries.get(&token)?.obj {
            Obj::Error(error) => Some(error.clone()),
            Obj::Value(_) => None,
        }
    }

    /// The value Source's text would name for a userdata or function-like value found where another was expected.
    pub(crate) fn found(&self) -> Option<Value> {
        self.found.borrow().clone()
    }

    /// Every token goes (teardown, after the provider released what it retained).
    pub(crate) fn drop_all(&self) {
        let entries = std::mem::take(&mut self.store.borrow_mut().entries);
        drop(entries);
        self.found.borrow_mut().take();
    }

    fn intern_table(&self, lua: &Lua) -> mlua::Result<Table> {
        lua.registry_value(&self.intern)
    }
    /// The live proxy of `node_id`, if Lua still has it.
    pub(crate) fn interned(&self, lua: &Lua, node_id: u64) -> Option<AnyUserData> {
        match self
            .intern_table(lua)
            .ok()?
            .raw_get::<Value>(node_id as i64)
            .ok()?
        {
            Value::UserData(userdata) => Some(userdata),
            _ => None,
        }
    }
    pub(crate) fn intern(
        &self,
        lua: &Lua,
        node_id: u64,
        userdata: &AnyUserData,
    ) -> mlua::Result<()> {
        self.intern_table(lua)?
            .raw_set(node_id as i64, userdata.clone())
    }

    fn value(&self, token: u64) -> Option<Value> {
        match &self.store.borrow().entries.get(&token)?.obj {
            Obj::Value(value) => Some(value.clone()),
            Obj::Error(_) => None,
        }
    }

    fn table_of(&self, token: u64) -> Result<Table, KilnResult> {
        match self.value(token) {
            Some(Value::Table(table)) => Ok(table),
            Some(_) => Err(KILN_ERR_INVALID_ARGUMENT),
            None => Err(KILN_ERR_NOT_FOUND),
        }
    }

    fn values(&self) -> Option<Values> {
        self.values
            .borrow()
            .as_ref()?
            .upgrade()
            .map(Values::from_core)
    }

    /// The reply for `value` (a fresh temporary token). `found`: whether Source's texts may name it.
    fn reply(&self, value: Value, found: bool) -> KilnEmlLuaValueV1 {
        let mut reply = empty_reply();
        let kind = match &value {
            Value::Nil => KILN_LUA_NIL,
            Value::Boolean(b) => {
                reply.bits = u64::from(*b);
                KILN_LUA_BOOLEAN
            }
            Value::Integer(i) => {
                reply.bits = *i as u64;
                KILN_LUA_INTEGER
            }
            Value::Number(n) => {
                reply.bits = n.to_bits();
                KILN_LUA_NUMBER
            }
            Value::String(s) => {
                reply.byte_length = s.as_bytes().len() as u64;
                KILN_LUA_STRING
            }
            Value::Table(_) => KILN_LUA_TABLE,
            Value::UserData(userdata) => {
                if let Some(lease) = proxies::lease_of(userdata) {
                    reply.typed_value = lease;
                    KILN_LUA_TYPED_VALUE
                } else if let Some((adapter, record)) =
                    self.known.as_ref().and_then(|k| k.classify(userdata))
                {
                    reply.adapter_class = adapter;
                    reply.byte_length = record.len() as u64;
                    KILN_LUA_KNOWN_USERDATA
                } else {
                    KILN_LUA_OTHER_USERDATA
                }
            }
            _ => KILN_LUA_OTHER,
        };
        reply.kind = kind as u32;
        if found && kind >= KILN_LUA_TYPED_VALUE {
            *self.found.borrow_mut() = Some(value.clone());
        }
        reply.object = self.store.borrow_mut().temp(Obj::Value(value));
        reply
    }

    fn error_reply(&self, error: mlua::Error) -> KilnEmlLuaValueV1 {
        self.count(|c| c.errors += 1);
        let mut reply = self.reply(Value::Nil, false);
        reply.error_object = self.store.borrow_mut().temp(Obj::Error(error));
        reply
    }

    fn protected(&self, result: mlua::Result<Value>) -> KilnEmlLuaValueV1 {
        match result {
            Ok(value) => self.reply(value, true),
            Err(error) => self.error_reply(error),
        }
    }

    fn access(&self, request: &KilnEmlLuaAccessV1) -> Result<KilnEmlLuaValueV1, KilnResult> {
        let lua = self.lua.try_upgrade().ok_or(KILN_ERR_UNAVAILABLE)?;
        let operation = request.operation as i32;
        self.count(|c| {
            c.accesses += 1;
            if let Some(slot) = c.by_request.get_mut(request.operation as usize) {
                *slot += 1;
            }
        });
        if operation != KILN_LUA_MAKE_TYPED_PROXY && (request.arena != 0 || request.node_id != 0) {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        let key = || -> Result<mlua::String, KilnResult> {
            let bytes = str_bytes(&request.key).ok_or(KILN_ERR_INVALID_ARGUMENT)?;
            lua.create_string(bytes).map_err(|_| KILN_ERR_INTERNAL)
        };
        Ok(match operation {
            KILN_LUA_DESCRIBE => {
                let value = self.value(request.object).ok_or(KILN_ERR_NOT_FOUND)?;
                self.reply(value, true)
            }
            KILN_LUA_GET_FIELD_VALUE => {
                let table = self.table_of(request.object)?;
                self.protected(table.get::<Value>(key()?))
            }
            KILN_LUA_GET_FIELD_STRING => {
                let table = self.table_of(request.object)?;
                match table.get::<String>(key()?) {
                    Ok(text) => self.reply(
                        Value::String(lua.create_string(text).map_err(|_| KILN_ERR_INTERNAL)?),
                        true,
                    ),
                    Err(error) => self.error_reply(error),
                }
            }
            KILN_LUA_LENGTH => {
                let table = self.table_of(request.object)?;
                self.protected(table.len().map(Value::Integer))
            }
            KILN_LUA_RAW_SEQUENCE_INDEX => {
                let table = self.table_of(request.object)?;
                self.protected(table.raw_get::<Value>(request.index))
            }
            KILN_LUA_SOURCE_TO_STRING => {
                let value = self.value(request.object).ok_or(KILN_ERR_NOT_FOUND)?;
                match value.to_string() {
                    Ok(text) => self.reply(
                        Value::String(lua.create_string(text).map_err(|_| KILN_ERR_INTERNAL)?),
                        false,
                    ),
                    Err(error) => self.error_reply(error),
                }
            }
            KILN_LUA_MAKE_TYPED_PROXY => self.make_proxy(&lua, request)?,
            _ => return Err(KILN_ERR_INVALID_ARGUMENT),
        })
    }

    fn make_proxy(
        &self,
        lua: &Lua,
        request: &KilnEmlLuaAccessV1,
    ) -> Result<KilnEmlLuaValueV1, KilnResult> {
        if request.index != 0 || request.key.len != 0 {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        let values = self.values().ok_or(KILN_ERR_UNAVAILABLE)?;
        let provisional = request.object;
        // identity-only value_info: the one re-entry the header allows here
        let info = values
            .identity(provisional)
            .map_err(|_| KILN_ERR_INVALID_ARGUMENT)?;
        if info.identity.node_id != request.node_id || info.identity.arena != request.arena {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        if let Some(existing) = self.interned(lua, request.node_id) {
            self.count(|c| c.reused += 1);
            let mut reply = self.reply(Value::UserData(existing), false);
            reply.lease_disposition = KILN_LUA_LEASE_REUSED as u32;
            return Ok(reply);
        }
        let userdata =
            proxies::create(lua, &values, info.kind, provisional).map_err(|_| KILN_ERR_INTERNAL)?;
        if self.intern(lua, request.node_id, &userdata).is_err() {
            // not interned: it gives the lease back (the provider closes the provisional it withdraws)
            proxies::disown(&userdata);
            return Err(KILN_ERR_INTERNAL);
        }
        self.count(|c| c.adopted += 1);
        let mut reply = self.reply(Value::UserData(userdata), false);
        reply.lease_disposition = KILN_LUA_LEASE_ADOPTED as u32;
        Ok(reply)
    }

    fn copy(&self, token: u64) -> Result<Vec<u8>, KilnResult> {
        self.count(|c| c.copies += 1);
        // the store is not borrowed while a family's classifier runs (it may reach Lua, and a finalizer the store)
        let userdata = {
            let store = self.store.borrow();
            match &store.entries.get(&token).ok_or(KILN_ERR_NOT_FOUND)?.obj {
                Obj::Value(Value::String(s)) => return Ok(s.as_bytes().to_vec()),
                Obj::Error(error) => return Ok(format!("{error}").into_bytes()),
                Obj::Value(Value::UserData(userdata)) => userdata.clone(),
                Obj::Value(_) => return Err(KILN_ERR_INVALID_ARGUMENT),
            }
        };
        self.known
            .as_ref()
            .and_then(|k| k.classify(&userdata))
            .map(|(_, record)| record)
            .ok_or(KILN_ERR_INVALID_ARGUMENT)
    }

    fn retain(&self, token: u64) -> KilnResult {
        let mut store = self.store.borrow_mut();
        match store.entries.get_mut(&token) {
            Some(entry) => {
                entry.retains = entry.retains.saturating_add(1);
                drop(store);
                self.count(|c| c.retains += 1);
                KILN_OK
            }
            None => KILN_ERR_NOT_FOUND,
        }
    }

    fn release(&self, token: u64) {
        let mut store = self.store.borrow_mut();
        let stray = match store.entries.get_mut(&token) {
            Some(entry) if entry.retains > 0 => {
                entry.retains -= 1;
                if entry.retains == 0 && !entry.temporary {
                    // the value drops after the borrow ends (its __gc runs at the next collection, not here)
                    let gone = store.entries.remove(&token);
                    drop(store);
                    drop(gone);
                }
                false
            }
            _ => true,
        };
        self.count(|c| {
            c.releases += 1;
            if stray {
                c.stray_releases += 1;
            }
        });
    }
}

fn empty_reply() -> KilnEmlLuaValueV1 {
    KilnEmlLuaValueV1 {
        size: std::mem::size_of::<KilnEmlLuaValueV1>() as u32,
        kind: KILN_LUA_NIL as u32,
        object: 0,
        bits: 0,
        byte_length: 0,
        typed_value: 0,
        error_object: 0,
        adapter_class: KILN_LUA_ADAPTER_NONE as u32,
        lease_disposition: KILN_LUA_LEASE_NONE as u32,
    }
}

/// A request's key bytes (valid during the call only); None for a null pointer with a nonzero length.
fn str_bytes(key: &KilnStr) -> Option<&[u8]> {
    if key.len == 0 {
        return Some(&[]);
    }
    if key.ptr.is_null() || key.len > isize::MAX as usize {
        return None;
    }
    // SAFETY: the provider lends `len` readable bytes at `ptr` for this call (header contract).
    Some(unsafe { std::slice::from_raw_parts(key.ptr.cast::<u8>(), key.len) })
}

/// Runs `work` for a worker callback: `wrong` off the worker, `fail` on a panic, never an unwind out.
fn on_worker<R>(user: *mut c_void, wrong: R, fail: R, work: impl FnOnce(&ReaderLocal) -> R) -> R {
    if user.is_null() {
        return fail;
    }
    // SAFETY: `user` is the ReaderUser this reader table was built over; the arena keeps it (and the ReaderLocal it
    // points to) alive until the provider's pending count reached 0 after arena_close (never freed otherwise).
    let user = unsafe { &*user.cast_const().cast::<ReaderUser>() };
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        if std::thread::current().id() != user.worker {
            return None;
        }
        // SAFETY: as above; only the worker touches the worker half.
        let local = unsafe { &*user.local };
        Some(catch_unwind(AssertUnwindSafe(|| work(local))).map_err(|_| local))
    }));
    match outcome {
        Ok(Some(Ok(result))) => result,
        Ok(Some(Err(local))) => {
            local.count(|c| c.panics += 1);
            fail
        }
        Ok(None) => wrong,
        Err(_) => fail,
    }
}

unsafe extern "C" fn reader_access(
    user: *mut c_void,
    request: *const KilnEmlLuaAccessV1,
    reply: *mut KilnEmlLuaValueV1,
) -> KilnResult {
    on_worker(user, KILN_ERR_WRONG_THREAD, KILN_ERR_INTERNAL, |local| {
        if request.is_null() || reply.is_null() || !request.is_aligned() || !reply.is_aligned() {
            local.count(|c| c.refused += 1);
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: non-null, aligned records the provider lends for this call; sizes checked before the rest is read.
        let (request_size, reply_size) =
            unsafe { ((*request).size as usize, (*reply).size as usize) };
        if request_size < std::mem::size_of::<KilnEmlLuaAccessV1>()
            || reply_size < std::mem::size_of::<KilnEmlLuaValueV1>()
        {
            local.count(|c| c.refused += 1);
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: as above, the whole v1 request is readable.
        let request = unsafe { request.read() };
        match local.access(&request) {
            Ok(mut value) => {
                // a larger reply record keeps its size and its tail
                value.size = reply_size as u32;
                // SAFETY: the reply is writable for at least the v1 record.
                unsafe { reply.write(value) };
                KILN_OK
            }
            Err(code) => {
                local.count(|c| c.refused += 1);
                code
            }
        }
    })
}

unsafe extern "C" fn reader_copy(
    user: *mut c_void,
    object: u64,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    on_worker(user, KILN_ERR_WRONG_THREAD, KILN_ERR_INTERNAL, |local| {
        if required.is_null() || !required.is_aligned() {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let bytes = match local.copy(object) {
            Ok(bytes) => bytes,
            Err(code) => {
                local.count(|c| c.refused += 1);
                return code;
            }
        };
        // SAFETY: `required` is non-null, aligned and writable for this call.
        unsafe { required.write(bytes.len()) };
        if capacity < bytes.len() {
            return KILN_ERR_BUFFER_TOO_SMALL;
        }
        if bytes.is_empty() {
            return KILN_OK;
        }
        if out.is_null() {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        // SAFETY: `out` is writable for `capacity` >= len bytes for this call.
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), bytes.len()) };
        KILN_OK
    })
}

unsafe extern "C" fn reader_retain(user: *mut c_void, object: u64) -> KilnResult {
    on_worker(user, KILN_ERR_WRONG_THREAD, KILN_ERR_INTERNAL, |local| {
        local.retain(object)
    })
}

unsafe extern "C" fn reader_release(user: *mut c_void, object: u64) {
    on_worker(user, (), (), |local| local.release(object))
}

unsafe extern "C" fn reader_wake(user: *mut c_void, control: KilnHandle) {
    if user.is_null() {
        return;
    }
    // SAFETY: as in `on_worker`; `wake` touches only the any-thread signal (atomics and the host's Send + Sync
    // signal function), never the worker half, Lua or the service.
    let user = unsafe { &*user.cast_const().cast::<ReaderUser>() };
    let _ = catch_unwind(AssertUnwindSafe(|| user.signal.wake(control)));
}
