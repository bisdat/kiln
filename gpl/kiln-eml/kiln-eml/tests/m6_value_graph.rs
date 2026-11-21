// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.values v1's typed graph, driven the way the C table's C provider and the MIT frontend will drive it, through a
//! real mlua host: a protected reader over Lua objects, weakly interned frontend proxies whose finalizer closes their
//! adopted lease, and proxy metamethods that call back into the graph. Expectations are ShroudForge 1.7.9's at Kiln's
//! fork pin (resource_values/{owned_values,mapped_values,validate_resource_values,mod}.rs,
//! bindings/extend_table_operations.rs, session/asset_resources.rs:53-65 ResourceInfo's cached value), source-derived:
//! the oracle has no registry or archive to build these values from. Packets are checked against the frozen fixtures in
//! schema/sfi/eml-values/fixtures/.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_types/mod.rs"]
mod reg;
#[path = "support/m6_userdata/mod.rs"]
mod userdata;
#[path = "support/m6_values/mod.rs"]
mod values;

use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    rc::{Rc, Weak},
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};

use kfc::reflection::TypeRegistry;
use kiln_abi::{
    KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_REVOKED, KILN_ERR_WRONG_THREAD,
    services::{
        kiln_assets::KilnAssetKey,
        kiln_eml_archive::KilnEmlArchiveIdentityV1,
        kiln_eml_values::{
            KILN_VALUE_ARRAY_MAPPED, KILN_VALUE_ARRAY_OWNED, KILN_VALUE_HANDLED, KILN_VALUE_INFO_DIRTY,
            KILN_VALUE_INFO_LENGTH, KILN_VALUE_INFO_SOURCE_LENGTH, KILN_VALUE_ITERATOR, KILN_VALUE_NIL,
            KILN_VALUE_OPEN_BYTES, KILN_VALUE_OPEN_DEFAULT, KILN_VALUE_OPEN_LOADED_SNAPSHOT, KILN_VALUE_OPEN_LUA_INPUT,
            KILN_VALUE_OPEN_RESOURCE, KILN_VALUE_READ_DIRTY, KILN_VALUE_READ_ITER_BEGIN, KILN_VALUE_READ_ITER_NEXT,
            KILN_VALUE_READ_LENGTH, KILN_VALUE_READ_SOURCE_BYTES, KILN_VALUE_READ_STRINGIFY, KILN_VALUE_READ_VIEW,
            KILN_VALUE_RESOURCE_ROOT, KILN_VALUE_RUNTIME, KILN_VALUE_SEMANTIC_ERROR, KILN_VALUE_STARTUP,
            KILN_VALUE_STRUCT_MAPPED, KILN_VALUE_STRUCT_OWNED, KILN_VALUE_UNSUPPORTED, KILN_VALUE_VARIANT_OWNED,
        },
    },
};
use kiln_eml::{
    archive_values::{
        Access, Adapter, ArenaConfig, ArenaPins, HostKind, HostValue, LeaseDisposition, LuaHost, OpenRequest, Refusal,
        Reply, ResourceSource, Selector, Transport, arena_close, arena_collect, arena_create, startup_dirty_roots,
        value_array_clear, value_array_insert, value_array_pop, value_array_push, value_array_remove, value_child,
        value_clone, value_close, value_export, value_info, value_open, value_read, value_result_copy, value_set,
    },
    sf_startup::resource_values::{self as rv, ValueObjects},
};
use mlua::{AnyUserData, Lua, MetaMethod, UserData, UserDataMethods, Value};
use serde_json::{Value as Json, json};

const ROOT: usize = 8;
const CHILD: usize = 2;
const FIXED: usize = 3;
const CHILDREN: usize = 4;
const ROOT_TABLE: &str = r#"return {child = {base = 7, extra = 11}, fixed = {2, 3},
    children = {{base = 13, extra = 17}, {base = 19, extra = 23}}, maybe = {base = 29, extra = 31},
    choice = {type = "m6.Child", value = {base = 37, extra = 41}}, text = "synthetic"}"#;
const OWNER_A: u64 = 0xa;
const OWNER_B: u64 = 0xb;

const PACKETS: &str = include_str!("../../../../schema/sfi/eml-values/fixtures/packets.json");

// ---- the test host: a protected reader over one mlua VM, and a minimal frontend ------------------------------------

/// The frontend userdata of a graph node: it holds its adopted lease and closes it once when Lua collects it.
struct Proxy {
    lease: u64,
    closes: Rc<Cell<usize>>,
}

impl Drop for Proxy {
    fn drop(&mut self) {
        self.closes.set(self.closes.get() + 1);
        // a finalizer: may run under any reader callback; the graph defers the close until the outer call ends
        let _ = value_close(0, self.lease);
    }
}

/// A Content userdata (adapter CONTENT) and a userdata the codec does not know.
struct ContentUd([u8; 16]);
impl UserData for ContentUd {}
struct Foreign;
impl UserData for Foreign {}

#[derive(Default)]
struct Stats {
    accesses: Cell<usize>,
    by_op: RefCell<HashMap<&'static str, usize>>,
    adopted: Cell<usize>,
    reused: Cell<usize>,
    retains: Cell<usize>,
    releases: Cell<usize>,
    /// Releases of objects not retained (a double release).
    stray_releases: Cell<usize>,
    /// Releases while a reader callback runs (that is, under an outer graph operation): must stay 0.
    releases_in_access: Cell<usize>,
    /// Proxy finalizers (lease closes) that ran inside MAKE_TYPED_PROXY.
    closes_in_proxy: Cell<usize>,
    /// Host objects released by those finalizers' closes while MAKE_TYPED_PROXY still ran: must stay 0.
    releases_in_proxy_gc: Cell<usize>,
    in_access: Cell<bool>,
}

struct Host {
    lua: Lua,
    next: Cell<u64>,
    temps: RefCell<HashMap<u64, Value>>,
    /// Values the test (the frontend's Lua stack) holds across calls.
    args: RefCell<HashMap<u64, Value>>,
    retained: RefCell<HashMap<u64, (Value, usize)>>,
    depth: Cell<u32>,
    stats: Stats,
    closes: Rc<Cell<usize>>,
    /// Run a full collection at the start of every MAKE_TYPED_PROXY (finalizers re-enter value_close there).
    gc_in_proxy: Cell<bool>,
    arena: Cell<u64>,
    control: Cell<u64>,
    /// Close the arena from another thread inside the next GET_FIELD_VALUE (an in-flight operation's window).
    close_in_get: Cell<bool>,
    /// Raise from the RAW_SEQUENCE_INDEX of this index (a protected raw read's own error, as a memory error would be).
    raw_error_at: Cell<i64>,
    view: KilnEmlArchiveIdentityV1,
    wakes: Arc<AtomicUsize>,
}

fn pins() -> KilnEmlArchiveIdentityV1 {
    KilnEmlArchiveIdentityV1 {
        size: std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32,
        phase: 2,
        launch_id: [1; 16],
        plan_id: [2; 16],
        generation: 3,
        reload_epoch: 4,
        plan_sha256: [5; 32],
        archive_sha256: [6; 32],
        registry_id: 7,
        registry_sha256: [8; 32],
    }
}

struct Fixture {
    resources: HashMap<[u8; 16], (Vec<u8>, usize)>,
}
impl ResourceSource for Fixture {
    fn read(&self, key: &KilnAssetKey) -> Result<Option<(Vec<u8>, usize)>, String> {
        if key.guid == [0xee; 16] {
            return Err("archive read failed".into());
        }
        Ok(self.resources.get(&key.guid).cloned())
    }
}

impl Host {
    fn new(phase: i32, resources: Option<Rc<dyn ResourceSource>>) -> Rc<Host> {
        Self::with_registry(phase, resources, userdata::registry())
    }

    fn with_registry(phase: i32, resources: Option<Rc<dyn ResourceSource>>, registry: Rc<TypeRegistry>) -> Rc<Host> {
        let lua = Lua::new();
        let intern = lua.create_table().unwrap();
        let mode = lua.create_table().unwrap();
        mode.set("__mode", "v").unwrap();
        let _ = intern.set_metatable(Some(mode));
        lua.set_named_registry_value("intern", intern).unwrap();
        let wakes = Arc::new(AtomicUsize::new(0));
        let host = Rc::new(Host {
            lua,
            next: Cell::new(1),
            temps: RefCell::new(HashMap::new()),
            args: RefCell::new(HashMap::new()),
            retained: RefCell::new(HashMap::new()),
            depth: Cell::new(0),
            stats: Stats::default(),
            closes: Rc::new(Cell::new(0)),
            gc_in_proxy: Cell::new(false),
            arena: Cell::new(0),
            control: Cell::new(0),
            close_in_get: Cell::new(false),
            raw_error_at: Cell::new(0),
            view: pins(),
            wakes: wakes.clone(),
        });
        host.lua.set_app_data(Rc::downgrade(&host));
        let waker: kiln_eml::archive_values::Waker = Arc::new(move || {
            wakes.fetch_add(1, Ordering::SeqCst);
        });
        let control = arena_create(
            OWNER_A,
            ArenaConfig {
                registry,
                pins: ArenaPins {
                    archive: host.view,
                    layout_revision: 77,
                },
                phase: phase as u32,
                host: host.clone(),
                waker: Some(waker),
                resources,
            },
        )
        .unwrap();
        host.control.set(control);
        host
    }

    /// One outermost service call: temporary objects die when it ends.
    fn call<R>(&self, work: impl FnOnce() -> R) -> R {
        self.depth.set(self.depth.get() + 1);
        let result = work();
        self.depth.set(self.depth.get() - 1);
        if self.depth.get() == 0 {
            self.temps.borrow_mut().clear();
        }
        result
    }

    fn temp(&self, value: Value) -> u64 {
        let token = self.next.get();
        self.next.set(token + 1);
        self.temps.borrow_mut().insert(token, value);
        token
    }

    /// A value the caller keeps alive across calls (as a Lua stack slot would): its object token.
    fn arg(&self, value: Value) -> u64 {
        let token = self.next.get();
        self.next.set(token + 1);
        self.args.borrow_mut().insert(token, value);
        token
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

    fn reply(&self, value: Value) -> HostValue {
        let object = self.temp(value.clone());
        let mut reply = HostValue::of(HostKind::Nil, object);
        match value {
            Value::Nil => {}
            Value::Boolean(b) => {
                reply.kind = HostKind::Boolean;
                reply.bits = b as u64;
            }
            Value::Integer(i) => {
                reply.kind = HostKind::Integer;
                reply.bits = i as u64;
            }
            Value::Number(n) => {
                reply.kind = HostKind::Number;
                reply.bits = n.to_bits();
            }
            Value::String(s) => {
                reply.kind = HostKind::String;
                reply.byte_length = s.as_bytes().len() as u64;
            }
            Value::Table(_) => reply.kind = HostKind::Table,
            Value::UserData(ud) => {
                if let Ok(proxy) = ud.borrow::<Proxy>() {
                    reply.kind = HostKind::TypedValue;
                    reply.typed_value = proxy.lease;
                } else if ud.is::<ContentUd>() {
                    reply.kind = HostKind::KnownUserdata;
                    reply.adapter = Adapter::Content;
                } else {
                    reply.kind = HostKind::OtherUserdata;
                }
            }
            _ => reply.kind = HostKind::Other,
        }
        reply
    }

    fn error(&self, error: mlua::Error) -> HostValue {
        HostValue::error(self.temp(Value::Error(Box::new(error))))
    }

    fn protected(&self, result: mlua::Result<Value>) -> HostValue {
        match result {
            Ok(value) => self.reply(value),
            Err(error) => self.error(error),
        }
    }

    fn count(&self, op: &'static str) -> usize {
        self.stats.by_op.borrow().get(op).copied().unwrap_or(0)
    }

    fn proxy(&self, provisional: u64, node_id: u64) -> HostValue {
        if self.gc_in_proxy.get() {
            let (closes, releases) = (self.closes.get(), self.stats.releases.get());
            self.lua.gc_collect().unwrap();
            self.lua.gc_collect().unwrap();
            self.stats
                .closes_in_proxy
                .set(self.stats.closes_in_proxy.get() + self.closes.get() - closes);
            // a close inside the call is deferred: nothing may be released before the outer call ends
            self.stats
                .releases_in_proxy_gc
                .set(self.stats.releases_in_proxy_gc.get() + self.stats.releases.get() - releases);
        }
        let intern: mlua::Table = self.lua.named_registry_value("intern").unwrap();
        let existing: Value = intern.raw_get(node_id as i64).unwrap();
        if let Value::UserData(_) = existing {
            self.stats.reused.set(self.stats.reused.get() + 1);
            let mut reply = self.reply(existing);
            reply.lease = LeaseDisposition::Reused;
            return reply;
        }
        let ud = self
            .lua
            .create_userdata(Proxy {
                lease: provisional,
                closes: self.closes.clone(),
            })
            .unwrap();
        intern.raw_set(node_id as i64, ud.clone()).unwrap();
        self.stats.adopted.set(self.stats.adopted.get() + 1);
        let mut reply = self.reply(Value::UserData(ud));
        reply.lease = LeaseDisposition::Adopted;
        reply
    }

    fn table_of(&self, object: u64) -> Result<mlua::Table, Transport> {
        match self.object(object) {
            Some(Value::Table(t)) => Ok(t),
            _ => Err(Transport(KILN_ERR_INVALID_ARGUMENT)),
        }
    }
}

impl LuaHost for Host {
    fn access(&self, access: &Access<'_>) -> Result<HostValue, Transport> {
        self.stats.accesses.set(self.stats.accesses.get() + 1);
        *self.stats.by_op.borrow_mut().entry(access.name()).or_default() += 1;
        let outer = self.stats.in_access.replace(true);
        let reply = self.access_inner(access);
        self.stats.in_access.set(outer);
        reply
    }

    fn copy(&self, object: u64) -> Result<Vec<u8>, Transport> {
        self.copy_inner(object)
    }

    fn retain(&self, object: u64) -> Result<(), Transport> {
        let value = self.object(object).ok_or(Transport(KILN_ERR_NOT_FOUND))?;
        self.stats.retains.set(self.stats.retains.get() + 1);
        self.retained.borrow_mut().entry(object).or_insert((value, 0)).1 += 1;
        Ok(())
    }

    fn release(&self, object: u64) {
        self.stats.releases.set(self.stats.releases.get() + 1);
        if self.stats.in_access.get() {
            self.stats
                .releases_in_access
                .set(self.stats.releases_in_access.get() + 1);
        }
        let mut retained = self.retained.borrow_mut();
        match retained.get_mut(&object) {
            Some((_, count)) if *count > 1 => *count -= 1,
            Some(_) => {
                retained.remove(&object);
            }
            None => self.stats.stray_releases.set(self.stats.stray_releases.get() + 1),
        }
    }
}

impl Host {
    fn access_inner(&self, access: &Access<'_>) -> Result<HostValue, Transport> {
        Ok(match *access {
            Access::Describe { object } => {
                let value = self.object(object).ok_or(Transport(KILN_ERR_NOT_FOUND))?;
                self.reply(value)
            }
            Access::GetFieldValue { object, key } => {
                if self.close_in_get.replace(false) {
                    let control = self.control.get();
                    let pending = std::thread::spawn(move || arena_close(OWNER_A, control))
                        .join()
                        .unwrap();
                    assert!(
                        pending.unwrap() > 0,
                        "off the worker the in-flight call still holds the arena"
                    );
                }
                let table = self.table_of(object)?;
                let key = self.lua.create_string(key).unwrap();
                self.protected(table.get::<Value>(key))
            }
            Access::GetFieldString { object, key } => {
                let table = self.table_of(object)?;
                let key = self.lua.create_string(key).unwrap();
                match table.get::<String>(key) {
                    Ok(text) => self.reply(Value::String(self.lua.create_string(text).unwrap())),
                    Err(error) => self.error(error),
                }
            }
            Access::Length { object } => {
                let table = self.table_of(object)?;
                self.protected(table.len().map(Value::Integer))
            }
            Access::RawSequenceIndex { object, index } => {
                let table = self.table_of(object)?;
                if index == self.raw_error_at.get() {
                    return Ok(self.error(mlua::Error::runtime("raw read failed")));
                }
                self.reply(table.raw_get::<Value>(index).unwrap())
            }
            Access::SourceToString { object } => {
                let value = self.object(object).ok_or(Transport(KILN_ERR_NOT_FOUND))?;
                match value.to_string() {
                    Ok(text) => self.reply(Value::String(self.lua.create_string(text).unwrap())),
                    Err(error) => self.error(error),
                }
            }
            Access::MakeTypedProxy {
                provisional,
                arena,
                node_id,
            } => {
                assert_eq!(arena, self.arena_id(), "the proxy request names this arena's identity");
                self.proxy(provisional, node_id)
            }
        })
    }

    fn copy_inner(&self, object: u64) -> Result<Vec<u8>, Transport> {
        match self.object(object).ok_or(Transport(KILN_ERR_NOT_FOUND))? {
            Value::String(s) => Ok(s.as_bytes().to_vec()),
            Value::Error(error) => Ok(format!("{error}").into_bytes()),
            Value::UserData(ud) => match ud.borrow::<ContentUd>() {
                Ok(content) => Ok(content.0.to_vec()),
                Err(_) => Err(Transport(KILN_ERR_INVALID_ARGUMENT)),
            },
            _ => Err(Transport(KILN_ERR_INVALID_ARGUMENT)),
        }
    }
}

fn host_of(lua: &Lua) -> Rc<Host> {
    lua.app_data_ref::<Weak<Host>>().unwrap().upgrade().unwrap()
}

/// The frontend: what a proxy's metamethods do (the MIT frontend will do this in MIT).
impl UserData for Proxy {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_method(MetaMethod::Index, |lua, this, key: Value| {
            let host = host_of(lua);
            let lease = this.lease;
            host.call(|| host.index(lease, key))
        });
        methods.add_meta_method(MetaMethod::NewIndex, |lua, this, (key, value): (Value, Value)| {
            let host = host_of(lua);
            let lease = this.lease;
            host.call(|| {
                let object = host.temp(value);
                let reply = host.check(value_set(OWNER_B, lease, &selector(&key), object))?;
                host.raise(&reply)
            })
        });
        methods.add_meta_method(MetaMethod::Len, |lua, this, ()| {
            let host = host_of(lua);
            let lease = this.lease;
            host.call(|| {
                let reply = host.check(value_read(OWNER_B, lease, KILN_VALUE_READ_LENGTH as u32))?;
                host.raise(&reply)?;
                host.scalar(reply.payload)
            })
        });
        methods.add_meta_method(MetaMethod::ToString, |lua, this, ()| {
            let host = host_of(lua);
            let lease = this.lease;
            // Source's variants have no __tostring: mlua's default text of the userdata
            let kind = value_info(OWNER_B, lease, 0).map(|info| info.kind).unwrap_or(0);
            if kind == KILN_VALUE_VARIANT_OWNED as u32 || kind == KILN_VALUE_VARIANT_OWNED as u32 + 1 {
                return Ok(Value::String(lua.create_string(format!("Proxy: {lease:#x}"))?));
            }
            host.call(|| {
                let reply = host.check(value_read(OWNER_B, lease, KILN_VALUE_READ_STRINGIFY as u32))?;
                host.raise(&reply)?;
                let bytes = host.bytes(reply.payload);
                host.close(reply.payload);
                Ok(Value::String(host.lua.create_string(bytes)?))
            })
        });
    }
}

fn selector(key: &Value) -> Selector<'static> {
    match key {
        Value::Integer(i) => Selector::Index(*i as u64),
        Value::String(s) => Selector::Field(Box::leak(s.as_bytes().to_vec().into_boxed_slice())),
        _ => Selector::Field(b"?"),
    }
}

impl Host {
    fn arena_id(&self) -> u64 {
        if self.arena.get() == 0 {
            // identity.arena of any value of this arena
            let probe = self.call(|| self.open_lua(CHILD, "return {base = 1, extra = 2}"));
            let info = value_info(OWNER_A, probe.item, 0).unwrap();
            self.close(probe.item);
            self.arena.set(info.identity.arena);
        }
        self.arena.get()
    }

    fn check(&self, result: Result<Reply, Refusal>) -> mlua::Result<Reply> {
        result.map_err(|refusal| mlua::Error::external(format!("refused: {refusal:?}")))
    }

    /// A SEMANTIC_ERROR as the Lua error the frontend raises (its root message).
    fn raise(&self, reply: &Reply) -> mlua::Result<Value> {
        if reply.disposition == KILN_VALUE_SEMANTIC_ERROR as u32 {
            let tree = self.json(reply.error);
            self.close(reply.error);
            let message = unhex(tree["error"]["message"].as_str().unwrap());
            return Err(mlua::Error::external(String::from_utf8_lossy(&message).into_owned()));
        }
        Ok(Value::Nil)
    }

    fn index(&self, lease: u64, key: Value) -> mlua::Result<Value> {
        let reply = self.check(value_child(OWNER_B, lease, &selector(&key)))?;
        self.raise(&reply)?;
        if reply.item != 0 {
            assert_eq!(reply.payload, 0);
            return Ok(self.wrap(reply.item));
        }
        self.scalar(reply.payload)
    }

    /// An item lease as its interned userdata: a known node reuses its proxy (and the item lease closes), a new one
    /// gets a proxy that keeps the item lease as its own.
    fn wrap(&self, item: u64) -> Value {
        let info = value_info(OWNER_B, item, 0).unwrap();
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
            })
            .unwrap();
        intern.raw_set(info.identity.node_id as i64, ud.clone()).unwrap();
        Value::UserData(ud)
    }

    /// A VIEW payload of a scalar as its Lua value (the payload closes).
    fn scalar(&self, payload: u64) -> mlua::Result<Value> {
        let packet = self.json(payload);
        self.close(payload);
        let value = if packet["packet"] == "view" {
            &packet["value"]
        } else {
            &packet
        };
        Ok(self.lua_of(if packet["packet"] == "length" { &packet } else { value }))
    }

    fn lua_of(&self, value: &Json) -> Value {
        let text = |prefix: &str| unhex(value["value"].as_str().unwrap().strip_prefix(prefix).unwrap());
        match (value["packet"].as_str(), value["tag"].as_str()) {
            (Some("length"), _) => Value::Integer(i64::from_be_bytes(text("i64:").try_into().unwrap())),
            (_, Some("nil")) => Value::Nil,
            (_, Some("bool")) => Value::Boolean(value["value"].as_bool().unwrap()),
            (_, Some("i64")) => Value::Integer(i64::from_be_bytes(text("i64:").try_into().unwrap())),
            (_, Some("f64")) => Value::Number(f64::from_bits(u64::from_be_bytes(text("f64:").try_into().unwrap()))),
            (_, Some("string")) => Value::String(self.lua.create_string(text("string:")).unwrap()),
            other => panic!("no scalar: {other:?} {value}"),
        }
    }

    fn bytes(&self, packet: u64) -> Vec<u8> {
        let required = value_result_copy(OWNER_B, packet, &mut []).unwrap();
        let mut out = vec![0; required];
        assert_eq!(value_result_copy(OWNER_B, packet, &mut out).unwrap(), required);
        out
    }

    fn json(&self, packet: u64) -> Json {
        serde_json::from_slice(&self.bytes(packet)).unwrap()
    }

    fn close(&self, token: u64) {
        if token != 0 {
            value_close(OWNER_B, token).unwrap();
        }
    }

    fn eval(&self, chunk: &str) -> Value {
        self.lua.load(chunk).eval().unwrap()
    }

    fn open(&self, request: OpenRequest<'_>) -> Reply {
        value_open(OWNER_A, self.control.get(), &self.view, &request).unwrap()
    }

    fn request<'a>(origin: i32, qualified_type: &'a [u8], bytes: &'a [u8], lua_object: u64) -> OpenRequest<'a> {
        OpenRequest {
            origin: origin as u32,
            qualified_type,
            key: KilnAssetKey {
                guid: [0; 16],
                r#type: 0,
                part: 0,
            },
            source_instance: 0,
            source_session: 0,
            bytes,
            lua_object,
        }
    }

    /// LUA_INPUT: a fresh owned root of type `index` from a Lua chunk.
    fn open_lua(&self, index: usize, chunk: &str) -> Reply {
        let name = type_name(index);
        let object = self.temp(self.eval(chunk));
        self.open(Host::request(KILN_VALUE_OPEN_LUA_INPUT, name.as_bytes(), &[], object))
    }

    /// BYTES: a fresh mapped root of type `index`.
    fn open_bytes(&self, index: usize, bytes: &[u8]) -> Reply {
        let name = type_name(index);
        self.open(Host::request(KILN_VALUE_OPEN_BYTES, name.as_bytes(), bytes, 0))
    }

    /// A root as a Lua global (through the frontend's interning).
    fn global(&self, name: &str, reply: Reply) {
        assert_eq!(reply.disposition, KILN_VALUE_HANDLED as u32, "{reply:?}");
        let value = self.wrap(reply.item);
        self.lua.globals().set(name, value).unwrap();
    }

    fn run(&self, chunk: &str) {
        let chunk = format!("{PERR}\n{chunk}");
        if let Err(e) = self.lua.load(&chunk).set_name("=g11").exec() {
            panic!("{e}");
        }
    }

    /// Closes the arena and drains it on this worker: everything retained is released.
    fn finish(&self) {
        self.args.borrow_mut().clear();
        assert_eq!(
            arena_close(OWNER_A, self.control.get()).unwrap(),
            0,
            "teardown on the worker drains at once"
        );
        assert_eq!(arena_collect(OWNER_A, self.control.get()).unwrap(), 0);
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

const PERR: &str = r#"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

fn type_name(index: usize) -> String {
    userdata::registry()
        .get(kfc::reflection::TypeIndex::new(index))
        .unwrap()
        .qualified_name
        .clone()
}

fn unhex(text: &str) -> Vec<u8> {
    let text = text.split_once(':').map_or(text, |(_, rest)| rest);
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap())
        .collect()
}

/// The codec's encoder over a plain Lua table (an independent producer of resource bytes).
fn encode(chunk: &str, index: usize) -> Vec<u8> {
    let lua = Lua::new();
    let value: Value = lua.load(chunk).eval().unwrap();
    let registry = userdata::registry();
    rv::encode(&value, &userdata::handle(&registry, index), &lua, &ValueObjects::new()).unwrap()
}

fn packet_fixture(name: &str) -> Json {
    let all: Json = serde_json::from_str(PACKETS).unwrap();
    all[name].clone()
}

// ---- tests ------------------------------------------------------------------------------------------------------

// Red-first: a get hands out the same node (aliases share edits); an assignment and a clone copy.
#[test]
fn aliases_versus_clones_owned_and_mapped() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.call(|| host.global("R", host.open_lua(ROOT, ROOT_TABLE)));
    host.call(|| host.global("M", host.open_bytes(ROOT, &encode(ROOT_TABLE, ROOT))));
    host.run(
        r#"
        for _, name in ipairs({"R", "M"}) do
          local root = _G[name]
          local c = root.child
          assert(rawequal(c, root.child), name .. ": a get returns the same userdata")
          c.base = 70
          assert(root.child.base == 70, name .. ": an alias sees the edit")
          root.child = c
          assert(not rawequal(root.child, c), name .. ": an assignment clones")
          c.base = 71
          assert(root.child.base == 70, name .. ": the clone does not follow its source")
          local kids = root.children
          assert(rawequal(kids[1], kids[1]))
          assert(kids[2].extra == 23 and #kids == 2)
          local v = root.choice
          assert(v.type == "m6.Child" and rawequal(v.value, root.choice.value))
          v.value = {base = 1, extra = 2}
          assert(v.value.extra == 2)
        end
        "#,
    );
    // value_clone: a deep copy (and a mapped clone keeps its original bytes and dirtiness)
    let m: AnyUserData = host.lua.globals().get("M").unwrap();
    let lease = m.borrow::<Proxy>().unwrap().lease;
    let clone = host.call(|| value_clone(OWNER_B, lease).unwrap());
    assert_ne!(clone.item, 0);
    assert_eq!(clone.payload, 0, "a typed clone has no VIEW");
    let (a, b) = (
        value_info(OWNER_B, lease, 0).unwrap(),
        value_info(OWNER_B, clone.item, KILN_VALUE_INFO_DIRTY as u32).unwrap(),
    );
    assert_ne!(a.identity.node_id, b.identity.node_id);
    assert_eq!(b.kind, KILN_VALUE_STRUCT_MAPPED as u32);
    assert!(b.flags & 2 != 0, "the clone keeps the original's dirtiness");
    host.call(|| host.global("C", clone));
    host.run(r#"C.child.base = 5 assert(M.child.base == 70 and C.child.base == 5)"#);
    let bytes = |name: &str| {
        let ud: AnyUserData = host.lua.globals().get(name).unwrap();
        let lease = ud.borrow::<Proxy>().unwrap().lease;
        let reply = host.call(|| value_read(OWNER_B, lease, KILN_VALUE_READ_SOURCE_BYTES as u32).unwrap());
        let out = host.bytes(reply.payload);
        host.close(reply.payload);
        out
    };
    assert_eq!(bytes("C"), bytes("M"), "a mapped clone keeps the original bytes");
    host.lua.globals().raw_remove("R").unwrap();
    host.lua.globals().raw_remove("M").unwrap();
    host.lua.globals().raw_remove("C").unwrap();
    drop(m);
    host.lua.gc_collect().unwrap();
    host.finish();
}

// Red-first: a removed nil is a nonzero nil node with MUTATED; position 0, len + 1 and an empty pop remove nothing.
#[test]
fn removed_nil_versus_no_op() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    for mapped in [false, true] {
        let chunk = "return {{base = 1, extra = 2}, {base = 3, extra = 4}}";
        let reply = host.call(|| {
            if mapped {
                host.open_bytes(userdata::OPT_LIST, &encode(chunk, userdata::OPT_LIST))
            } else {
                host.open_lua(userdata::OPT_LIST, chunk)
            }
        });
        let list = reply.item;
        let nil = host.arg(Value::Nil);
        let set = host.call(|| value_set(OWNER_B, list, &Selector::Index(1), nil).unwrap());
        assert_eq!(
            (set.disposition, set.arity, set.is_mutated()),
            (KILN_VALUE_HANDLED as u32, 0, true)
        );
        for index in [0u64, 3] {
            let none = host.call(|| value_array_remove(OWNER_B, list, index).unwrap());
            assert_eq!(
                (none.disposition, none.arity, none.item, none.payload, none.is_mutated()),
                (KILN_VALUE_HANDLED as u32, 1, 0, 0, false),
                "remove({index}) removes nothing"
            );
        }
        let removed = host.call(|| value_array_remove(OWNER_B, list, 1).unwrap());
        assert!(
            removed.is_mutated() && removed.item != 0 && removed.arity == 1,
            "{removed:?}"
        );
        let info = value_info(OWNER_B, removed.item, 0).unwrap();
        assert_eq!(info.kind, KILN_VALUE_NIL as u32, "a removed nil is a nil node");
        assert_eq!(host.json(removed.payload), packet_fixture("view_nil"));
        assert_eq!(removed.receiver_revision, 2);
        host.close(removed.item);
        host.close(removed.payload);
        let pop = host.call(|| value_array_pop(OWNER_B, list).unwrap());
        assert!(pop.is_mutated() && pop.payload == 0, "a typed element has no VIEW");
        assert_eq!(
            value_info(OWNER_B, pop.item, 0).unwrap().kind,
            KILN_VALUE_STRUCT_OWNED as u32 + mapped as u32
        );
        host.close(pop.item);
        let empty = host.call(|| value_array_pop(OWNER_B, list).unwrap());
        assert_eq!(
            (empty.item, empty.arity, empty.is_mutated()),
            (0, 1, false),
            "an empty pop removes nothing"
        );
        // clear: MUTATED even when empty (a mapped array is dirty after it)
        let clear = host.call(|| value_array_clear(OWNER_B, list).unwrap());
        assert!(clear.is_mutated());
        let dirty = value_info(OWNER_B, list, KILN_VALUE_INFO_DIRTY as u32).unwrap();
        assert!(dirty.flags & 2 != 0);
        host.close(list);
    }
    host.finish();
}

// Red-first: insert/remove check the range before the static array; set checks only the range; clear checks static first.
#[test]
fn static_and_bounds_order() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    for mapped in [false, true] {
        let fixed = host.call(|| {
            if mapped {
                host.open_bytes(FIXED, &encode("return {2, 3}", FIXED))
            } else {
                host.open_lua(FIXED, "return {2, 3}")
            }
        });
        let fixed = fixed.item;
        let info = value_info(
            OWNER_B,
            fixed,
            KILN_VALUE_INFO_LENGTH as u32 | KILN_VALUE_INFO_SOURCE_LENGTH as u32,
        )
        .unwrap();
        assert!(info.flags & 1 != 0, "static");
        assert_eq!((info.logical_len, info.source_len), (2, mapped as u64 * 2));
        let error = |reply: Reply| -> Json {
            assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
            assert!(
                !reply.is_mutated() && reply.item == 0 && reply.payload == 0 && reply.arity == 0,
                "RC-1"
            );
            let tree = host.json(reply.error);
            host.close(reply.error);
            tree["error"].clone()
        };
        let five = host.arg(Value::Integer(5));
        let e = error(host.call(|| value_array_insert(OWNER_B, fixed, 4, five).unwrap()));
        assert_eq!(e["class"], "position_out_of_bounds");
        assert_eq!(
            (e["index"].as_str(), e["start"].as_str(), e["end"].as_str()),
            (
                Some("u64:0000000000000004"),
                Some("u64:0000000000000001"),
                Some("u64:0000000000000003")
            )
        );
        let e = error(host.call(|| value_array_insert(OWNER_B, fixed, 1, five).unwrap()));
        assert_eq!(e, packet_fixture("error_static_array")["error"]);
        let e = error(host.call(|| value_array_remove(OWNER_B, fixed, 9).unwrap()));
        assert_eq!(e["class"], "position_out_of_bounds", "range before static");
        // remove(0) is no removal at all, even on a static array
        let none = host.call(|| value_array_remove(OWNER_B, fixed, 0).unwrap());
        assert_eq!((none.disposition, none.item), (KILN_VALUE_HANDLED as u32, 0));
        let e = error(host.call(|| value_array_remove(OWNER_B, fixed, 1).unwrap()));
        assert_eq!(e["class"], "static_array");
        let e = error(host.call(|| value_array_clear(OWNER_B, fixed).unwrap()));
        assert_eq!(e["class"], "static_array");
        let e = error(host.call(|| value_set(OWNER_B, fixed, &Selector::Index(3), five).unwrap()));
        assert_eq!(e["class"], "position_out_of_bounds");
        // a set inside the range is allowed on a static array and validated against the element type
        let ok = host.call(|| value_set(OWNER_B, fixed, &Selector::Index(2), five).unwrap());
        assert!(ok.is_mutated());
        let text = host.arg(Value::String(host.lua.create_string("x").unwrap()));
        let e = error(host.call(|| value_set(OWNER_B, fixed, &Selector::Index(2), text).unwrap()));
        assert_eq!(e["class"], "validation");
        assert_eq!(e["kind"], "incompatible_type");
        assert_eq!(
            unhex(e["message"].as_str().unwrap()),
            b"at .: expected `unsigned integer in range [0, 4294967295]`, got `x`"
        );
        let reply = host.call(|| value_read(OWNER_B, fixed, KILN_VALUE_READ_STRINGIFY as u32).unwrap());
        assert_eq!(host.bytes(reply.payload), b"[2,5]");
        host.close(reply.payload);
        host.close(fixed);
    }
    host.finish();
}

// Red-first: identity, class and lengths; class applicability (RC-2) refuses without a reader call or an effect.
#[test]
fn class_applicability_and_reads() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_bytes(ROOT, &encode(ROOT_TABLE, ROOT))).item;
    let choice = host
        .call(|| value_child(OWNER_B, root, &Selector::Field(b"choice")).unwrap())
        .item;
    let text = host.call(|| value_child(OWNER_B, root, &Selector::Field(b"text")).unwrap());
    assert_eq!(text.item, 0, "a scalar child is no lease (RC-3)");
    assert_eq!(
        host.json(text.payload),
        json!({"schema_version": 1, "packet": "view",
        "value": {"tag": "string", "value": "string:73796e746865746963"}})
    );
    let missing = host.call(|| value_child(OWNER_B, root, &Selector::Field(b"nothing")).unwrap());
    assert_eq!(host.json(missing.payload), packet_fixture("view_nil"));
    let iter = host.call(|| value_read(OWNER_B, root, KILN_VALUE_READ_ITER_BEGIN as u32).unwrap());
    assert_eq!((iter.arity, iter.payload), (3, 0));
    let kids = host
        .call(|| value_child(OWNER_B, root, &Selector::Field(b"children")).unwrap())
        .item;
    assert_eq!(
        value_info(OWNER_B, iter.item, 0).unwrap().kind,
        KILN_VALUE_ITERATOR as u32
    );
    let before = host.stats.accesses.get();
    let refused = |token: u64, query: i32| value_read(OWNER_B, token, query as u32).unwrap_err();
    for query in [
        KILN_VALUE_READ_LENGTH,
        KILN_VALUE_READ_STRINGIFY,
        KILN_VALUE_READ_ITER_BEGIN,
        KILN_VALUE_READ_ITER_NEXT,
    ] {
        assert_eq!(
            refused(choice, query),
            Refusal::InvalidArgument,
            "variant, query {query}"
        );
        assert_eq!(
            refused(text.payload, query),
            Refusal::InvalidArgument,
            "packet, query {query}"
        );
    }
    for query in [
        KILN_VALUE_READ_LENGTH,
        KILN_VALUE_READ_STRINGIFY,
        KILN_VALUE_READ_ITER_BEGIN,
        KILN_VALUE_READ_DIRTY,
        KILN_VALUE_READ_SOURCE_BYTES,
    ] {
        assert_eq!(
            refused(iter.item, query),
            Refusal::InvalidArgument,
            "iterator, query {query}"
        );
    }
    assert_eq!(refused(root, KILN_VALUE_READ_ITER_NEXT), Refusal::InvalidArgument);
    assert_eq!(refused(root, 99), Refusal::InvalidArgument);
    assert_eq!(
        value_child(OWNER_B, root, &Selector::Index(1)).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        value_child(OWNER_B, choice, &Selector::Field(b"base")).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        value_child(OWNER_B, kids, &Selector::Field(b"1")).unwrap_err(),
        Refusal::InvalidArgument
    );
    let one = host.arg(Value::Integer(1));
    assert_eq!(
        value_set(OWNER_B, choice, &Selector::Field(b"type"), one).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        value_result_copy(OWNER_B, root, &mut []).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        value_info(OWNER_B, root, 8).map(|_| ()).unwrap_err(),
        Refusal::InvalidArgument
    );
    // an array operation on a live non-array is UNSUPPORTED, on an iterator or packet INVALID_ARGUMENT
    let unsupported = value_array_pop(OWNER_B, root).unwrap();
    assert_eq!(
        (unsupported.disposition, unsupported.arity),
        (KILN_VALUE_UNSUPPORTED as u32, 0)
    );
    assert_eq!(
        value_array_pop(OWNER_B, iter.item).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        value_array_clear(OWNER_B, text.payload).unwrap_err(),
        Refusal::InvalidArgument
    );
    assert_eq!(
        host.stats.accesses.get(),
        before,
        "no reader call for a refused request"
    );
    // DIRTY, VIEW and SOURCE_BYTES accept every value node
    let view = host.call(|| value_read(OWNER_B, choice, KILN_VALUE_READ_VIEW as u32).unwrap());
    assert_eq!(host.json(view.payload), packet_fixture("view_variant_mapped"));
    let none = host.call(|| value_read(OWNER_B, choice, KILN_VALUE_READ_SOURCE_BYTES as u32).unwrap());
    assert_eq!(
        (none.disposition, none.payload, none.arity),
        (KILN_VALUE_HANDLED as u32, 0, 1),
        "Source's None"
    );
    let dirty = host.call(|| value_read(OWNER_B, root, KILN_VALUE_READ_DIRTY as u32).unwrap());
    assert_eq!(
        host.json(dirty.payload),
        json!({"schema_version": 1, "packet": "dirty", "value": false})
    );
    let len = host.call(|| value_read(OWNER_B, kids, KILN_VALUE_READ_LENGTH as u32).unwrap());
    assert_eq!(
        host.json(len.payload),
        json!({"schema_version": 1, "packet": "length", "value": "i64:0000000000000002"})
    );
    let info = value_info(OWNER_A, root, KILN_VALUE_INFO_LENGTH as u32).unwrap();
    assert_eq!(
        (info.kind, info.type_index, info.logical_len),
        (KILN_VALUE_STRUCT_MAPPED as u32, ROOT as u32, 6)
    );
    assert_eq!(
        (
            info.identity.layout_revision,
            info.identity.lua_phase,
            info.identity.archive.registry_id
        ),
        (77, 1, 7)
    );
    assert_eq!(info.identity.creator_owner, OWNER_A);
    for token in [
        root,
        choice,
        text.payload,
        missing.payload,
        iter.item,
        kids,
        view.payload,
        dirty.payload,
        len.payload,
    ] {
        host.close(token);
    }
    host.finish();
}

// Red-first: pairs walks the type's fields / the CURRENT owned length / the ORIGINAL mapped length (k, nil) quirk.
#[test]
fn iteration_and_the_original_length_quirk() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let bytes = encode("return {{base = 1, extra = 2}, {base = 3, extra = 4}}", CHILDREN);
    let kids = host.call(|| host.open_bytes(CHILDREN, &bytes)).item;
    let view = host.call(|| value_read(OWNER_B, kids, KILN_VALUE_READ_VIEW as u32).unwrap());
    assert_eq!(host.json(view.payload), packet_fixture("view_array_mapped"));
    host.close(view.payload);
    let first = host.call(|| value_array_remove(OWNER_B, kids, 1).unwrap());
    host.close(first.item);
    let iter = host
        .call(|| value_read(OWNER_B, kids, KILN_VALUE_READ_ITER_BEGIN as u32).unwrap())
        .item;
    let mut seen = Vec::new();
    loop {
        let step = host.call(|| value_read(OWNER_B, iter, KILN_VALUE_READ_ITER_NEXT as u32).unwrap());
        assert_eq!(step.arity, 2);
        let packet = host.json(step.payload);
        host.close(step.payload);
        if packet["key"]["tag"] == "nil" {
            assert_eq!(packet, packet_fixture("iter_next_end"));
            break;
        }
        seen.push((packet.clone(), step.item != 0));
        host.close(step.item);
    }
    assert_eq!(seen.len(), 2, "the original length");
    assert_eq!(seen[0].0, packet_fixture("iter_next_item"));
    assert!(seen[0].1);
    assert_eq!(
        seen[1].0,
        packet_fixture("iter_next_key_nil"),
        "(2, nil) is not the end"
    );
    assert!(!seen[1].1);
    // an owned struct walks its type's fields, scalars inline
    let child = host.call(|| host.open_lua(CHILD, "return {base = 5, extra = 6}")).item;
    let iter2 = host
        .call(|| value_read(OWNER_B, child, KILN_VALUE_READ_ITER_BEGIN as u32).unwrap())
        .item;
    let step = host.call(|| value_read(OWNER_B, iter2, KILN_VALUE_READ_ITER_NEXT as u32).unwrap());
    assert_eq!(step.item, 0);
    let packet = host.json(step.payload);
    assert_eq!(
        packet["key"],
        json!({"tag": "string", "value": "string:62617365"}),
        "base first (inheritance order)"
    );
    assert_eq!(packet["value"], json!({"tag": "i64", "value": "i64:0000000000000005"}));
    for token in [kids, iter, child, iter2, step.payload] {
        host.close(token);
    }
    host.finish();
}

// Red-first: the error tree: validation -> lua -> the host's error object, retained and released once; a struct
// field get that raises is MissingField and is not retained.
#[test]
fn error_tree_and_host_error_lifetime() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let raising = host.arg(host.eval(r#"return setmetatable({}, {__index = function(_, k) error("no " .. k) end})"#));
    // the variant's "type" get raises: kept as the lua kind with its host error
    let reply = host.call(|| value_set(OWNER_B, root, &Selector::Field(b"choice"), raising).unwrap());
    assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
    assert!(!reply.is_mutated());
    let retained = host.stats.retains.get();
    assert_eq!(retained, 1, "the host error is retained before the call ends");
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(tree["class"], "validation");
    assert_eq!(tree["path"], "type");
    assert_eq!(tree["kind"], "lua");
    assert_eq!(tree["cause"]["class"], "lua");
    let token = tree["cause"]["host_error"].as_str().unwrap();
    assert!(token.starts_with("u64:") && token != "u64:0000000000000000");
    let message = String::from_utf8(unhex(tree["message"].as_str().unwrap())).unwrap();
    assert!(
        message.starts_with("at type: runtime error: ") && message.contains(": no type"),
        "{message}"
    );
    assert_eq!(
        unhex(tree["cause"]["message"].as_str().unwrap()),
        message.as_bytes()["at type: ".len()..].to_vec()
    );
    assert_eq!(host.retained.borrow().len(), 1);
    // copying again re-runs nothing
    let accesses = host.stats.accesses.get();
    let _ = host.json(reply.error);
    assert_eq!(host.stats.accesses.get(), accesses);
    host.close(reply.error);
    assert!(
        host.retained.borrow().is_empty(),
        "closing the error lease on the worker releases it"
    );
    assert_eq!(host.stats.releases.get(), 1);
    host.close(reply.error);
    assert_eq!(
        host.stats.releases.get(),
        1,
        "a second close is idempotent, no second release"
    );
    // a struct's field get that raises: MissingField, nothing retained
    let reply = host.call(|| value_set(OWNER_B, root, &Selector::Field(b"child"), raising).unwrap());
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(
        tree,
        json!({"class": "validation", "message": tree["message"], "path": ".", "kind": "missing_field",
        "field": "base", "type": "m6.Child"})
    );
    assert_eq!(host.stats.retains.get(), 1, "a discarded get error is not retained");
    host.close(reply.error);
    host.close(root);
    host.finish();
}

// Red-first: Source's read counts: one LENGTH for an array, three for a static array that does not fit, one
// SOURCE_TO_STRING per incompatible value.
#[test]
fn reader_calls_follow_source() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.run("calls = 0");
    let table = |chunk: &str| host.arg(host.eval(chunk));
    let counted = r#"return setmetatable({1, 2, 3}, {__len = function(t) calls = calls + 1 return rawlen(t) end})"#;
    let reply = host.call(|| {
        host.open(Host::request(
            KILN_VALUE_OPEN_LUA_INPUT,
            b"m6.Fixed",
            &[],
            table(counted),
        ))
    });
    assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
    assert_eq!(host.lua.globals().get::<i64>("calls").unwrap(), 3);
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(
        unhex(tree["message"].as_str().unwrap()),
        b"at .: expected `array of length 2`, got `array of length 3`"
    );
    host.close(reply.error);
    host.run("calls = 0");
    let reply = host.call(|| {
        host.open(Host::request(
            KILN_VALUE_OPEN_LUA_INPUT,
            b"m6.Children",
            &[],
            table(counted),
        ))
    });
    assert_eq!(
        host.lua.globals().get::<i64>("calls").unwrap(),
        1,
        "a dynamic array calls __len once"
    );
    assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32, "1 is no m6.Child");
    host.close(reply.error);
    let strings = host.count("SOURCE_TO_STRING");
    let reply = host.call(|| {
        host.open(Host::request(
            KILN_VALUE_OPEN_LUA_INPUT,
            b"m6.Child",
            &[],
            table("return {base = true, extra = 1}"),
        ))
    });
    assert_eq!(host.count("SOURCE_TO_STRING"), strings + 1);
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(tree["found"], "bytes:74727565");
    host.close(reply.error);
    // a sequence stops at the first nil (raw reads)
    let reply = host.call(|| {
        host.open(Host::request(
            KILN_VALUE_OPEN_LUA_INPUT,
            b"m6.Children",
            &[],
            table("return {{base = 1, extra = 2}, nil, {base = 3, extra = 4}}"),
        ))
    });
    let len = host.call(|| value_read(OWNER_B, reply.item, KILN_VALUE_READ_LENGTH as u32).unwrap());
    assert_eq!(host.json(len.payload)["value"], "i64:0000000000000001");
    host.close(len.payload);
    host.close(reply.item);
    host.finish();
}

// Red-first: a typed value passed back is cloned (shared borrow); the same node mutably borrowed is Source's borrow
// error; another node may be read re-entrantly; a userdata of the wrong class is found_userdata.
#[test]
fn borrows_reentry_and_found_userdata() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.call(|| host.global("R", host.open_lua(ROOT, ROOT_TABLE)));
    host.lua
        .globals()
        .set("Foreign", host.lua.create_userdata(Foreign).unwrap())
        .unwrap();
    host.run(
        r#"
        local kids = R.children
        -- re-entry into ANOTHER node during validation is allowed: the element table's metamethod reads R.child
        local seen
        R.children[1] = setmetatable({}, {__index = function(_, k) seen = R.child.base return 99 end})
        assert(seen == 7 and R.children[1].base == 99)
        -- the typed element passed back is cloned
        local c = R.child
        R.children[2] = c
        assert(not rawequal(R.children[2], c) and R.children[2].base == 7)
        -- a struct where an array is expected: found_userdata names the class
        local e = perr(function() R.children = c end)
        assert(e == "at .: expected `m6.Children`, got `userdata `<frontend TypeId>``", e)
        local e = perr(function() R.child = Foreign end)
        assert(e:find("frontend TypeId"), e)
        "#,
    );
    // the same node mutably borrowed (a push validating a table whose metamethod reads that array)
    let kids: AnyUserData = host.lua.load("return R.children").eval().unwrap();
    let lease = kids.borrow::<Proxy>().unwrap().lease;
    let input = host.arg(host.eval(r#"return setmetatable({}, {__index = function(_, k) if k == "base" then local _ = R.children[1] end return 1 end})"#));
    let reply = host.call(|| value_array_push(OWNER_B, lease, input).unwrap());
    // Source: the field get raised (the borrow error), so the struct field is MissingField
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(tree["kind"], "missing_field");
    host.close(reply.error);
    // and as a variant's value get, the borrow error is the cause the tree keeps
    let choice = host
        .call(|| value_child(OWNER_B, value_lease(&host, "R"), &Selector::Field(b"choice")).unwrap())
        .item;
    host.lua.globals().set("V", host.call(|| host.wrap(choice))).unwrap();
    let input =
        host.arg(host.eval(r#"return setmetatable({}, {__index = function(_, k) local _ = V.type return 1 end})"#));
    let reply = host.call(|| value_set(OWNER_B, value_lease(&host, "V"), &Selector::Field(b"value"), input).unwrap());
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(tree["kind"], "missing_field", "{tree}");
    host.close(reply.error);
    let typed = host.arg(host.lua.load("return R.child").eval().unwrap());
    let found = host.call(|| value_set(OWNER_B, lease, &Selector::Index(1), typed).unwrap());
    assert_eq!(
        found.disposition, KILN_VALUE_HANDLED as u32,
        "a struct of the element type is cloned in"
    );
    let wrong = host.arg(host.lua.load("return R.children").eval().unwrap());
    let reply = host.call(|| value_set(OWNER_B, value_lease(&host, "R"), &Selector::Field(b"child"), wrong).unwrap());
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(
        tree["found_userdata"],
        json!({"lua_kind": "typed_value", "adapter": "none", "value_kind": "array_owned"})
    );
    host.close(reply.error);
    drop(kids);
    host.lua.globals().raw_remove("R").unwrap();
    host.lua.globals().raw_remove("V").unwrap();
    host.lua.gc_collect().unwrap();
    host.finish();
}

fn value_lease(host: &Host, global: &str) -> u64 {
    let ud: AnyUserData = host.lua.globals().get(global).unwrap();
    ud.borrow::<Proxy>().unwrap().lease
}

// Red-first: a cached child's userdata is pinned by its parent's slot (a weak-keyed table keeps it across GC while the
// parent lives); a fresh root nobody references is collected (its adopted lease closed once) and a new open
// recreates it as a new node.
#[test]
fn pinned_versus_detached() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.call(|| host.global("R", host.open_lua(ROOT, ROOT_TABLE)));
    host.run(
        r#"
        weak = setmetatable({}, {__mode = "k"})
        weak[R.child] = true
        weak[R.children[1]] = true
        collectgarbage() collectgarbage()
        local n = 0 for _ in pairs(weak) do n = n + 1 end
        assert(n == 2, "pinned by the parent's slots: " .. n)
        assert(weak[R.child] and weak[R.children[1]])
        -- replacing the slot drops its pin: the old child's userdata may go
        local old = R.child
        R.child = {base = 1, extra = 2}
        weak2 = setmetatable({}, {__mode = "k"})
        weak2[old] = true
        old = nil
        collectgarbage() collectgarbage()
        assert(next(weak2) == nil, "a replaced child is detached")
        "#,
    );
    let closes = host.closes.get();
    let fresh = host.call(|| host.open_lua(CHILD, "return {base = 1, extra = 2}"));
    let id = value_info(OWNER_B, fresh.item, 0).unwrap().identity.node_id;
    host.call(|| host.global("F", fresh));
    host.run("F = nil collectgarbage() collectgarbage()");
    assert_eq!(
        host.closes.get(),
        closes + 1,
        "the unreferenced root's proxy closed its lease"
    );
    let again = host.call(|| host.open_lua(CHILD, "return {base = 1, extra = 2}"));
    assert_ne!(
        value_info(OWNER_B, again.item, 0).unwrap().identity.node_id,
        id,
        "a fresh root is a new node"
    );
    host.close(again.item);
    // a removed child: its slot's pin moves to the removed item and goes once the caller closed it
    let kids = host
        .call(|| value_child(OWNER_B, value_lease(&host, "R"), &Selector::Field(b"children")).unwrap())
        .item;
    let retained = host.retained.borrow().len();
    let removed = host.call(|| value_array_remove(OWNER_B, kids, 1).unwrap());
    assert_eq!(
        host.retained.borrow().len(),
        retained,
        "the pin moved with the removed value"
    );
    host.close(removed.item);
    assert_eq!(
        host.retained.borrow().len(),
        retained - 1,
        "released once the caller has it"
    );
    host.close(kids);
    host.lua.globals().raw_remove("R").unwrap();
    host.lua.gc_collect().unwrap();
    host.finish();
}

// Red-first: B's FIRST access through a reference created by A, after A's last call, succeeds: owners are attribution.
#[test]
fn creator_retires_then_another_owners_first_access() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let child = host
        .call(|| value_child(OWNER_A, root, &Selector::Field(b"child")).unwrap())
        .item;
    // A is gone: no call names it again. B reads, writes and iterates what A made.
    let base = host.call(|| value_child(OWNER_B, child, &Selector::Field(b"base")).unwrap());
    assert_eq!(
        host.json(base.payload)["value"],
        json!({"tag": "i64", "value": "i64:0000000000000007"})
    );
    let nine = host.arg(Value::Integer(9));
    assert!(
        host.call(|| value_set(OWNER_B, child, &Selector::Field(b"base"), nine).unwrap())
            .is_mutated()
    );
    let info = value_info(OWNER_B, child, 0).unwrap();
    assert_eq!(
        info.identity.creator_owner, OWNER_A,
        "the creator stays the attribution"
    );
    assert_eq!(info.identity.revision, 1);
    host.close(base.payload);
    host.close(child);
    host.close(root);
    host.finish();
}

// Red-first: arena close revokes every lease; close stays idempotent; a never-issued token is NOT_FOUND; worker-only
// calls off the worker are WRONG_THREAD; a close off the worker is queued and wakes the host.
#[test]
fn arena_close_never_issued_and_wrong_thread() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let child = host
        .call(|| value_child(OWNER_B, root, &Selector::Field(b"child")).unwrap())
        .item;
    let never = u64::MAX - 1;
    assert_eq!(value_close(OWNER_B, never).unwrap_err().code(), KILN_ERR_NOT_FOUND);
    assert_eq!(value_close(OWNER_B, 0).unwrap_err(), Refusal::NotFound);
    assert_eq!(
        value_child(OWNER_B, never, &Selector::Field(b"child")).unwrap_err(),
        Refusal::NotFound
    );
    assert_eq!(
        value_close(OWNER_B, host.control.get()).unwrap_err(),
        Refusal::InvalidArgument,
        "the control is no lease"
    );
    assert_eq!(arena_close(OWNER_A, root).unwrap_err(), Refusal::InvalidArgument);
    let control = host.control.get();
    let (wrong, closed_off_worker) = std::thread::spawn(move || {
        (
            value_child(OWNER_B, root, &Selector::Field(b"text")).unwrap_err(),
            value_close(OWNER_B, child),
        )
    })
    .join()
    .unwrap();
    assert_eq!(wrong.code(), KILN_ERR_WRONG_THREAD);
    assert_eq!(closed_off_worker, Ok(()));
    assert_eq!(
        host.wakes.load(Ordering::SeqCst),
        1,
        "an off-worker close wakes the host"
    );
    assert_eq!(
        value_info(OWNER_B, child, 0).map(|_| ()).unwrap_err().code(),
        KILN_ERR_REVOKED
    );
    assert!(
        arena_collect(OWNER_A, control).unwrap() > 0,
        "the child's pin is still retained by its slot"
    );
    let collected = std::thread::spawn(move || arena_collect(OWNER_A, control))
        .join()
        .unwrap();
    assert_eq!(collected.unwrap_err(), Refusal::WrongThread);
    // close: everything revoked at once
    assert_eq!(arena_close(OWNER_A, control).unwrap(), 0);
    assert_eq!(
        value_read(OWNER_B, root, KILN_VALUE_READ_VIEW as u32)
            .unwrap_err()
            .code(),
        KILN_ERR_REVOKED
    );
    assert_eq!(
        value_close(OWNER_B, root),
        Ok(()),
        "close of a revoked lease is idempotent"
    );
    let request = Host::request(KILN_VALUE_OPEN_BYTES, b"m6.Child", &[0; 8], 0);
    assert_eq!(
        value_open(OWNER_A, control, &host.view, &request).unwrap_err(),
        Refusal::Revoked
    );
    assert_eq!(arena_close(OWNER_A, control).unwrap(), 0, "arena_close is idempotent");
    host.finish();
}

// Red-first: GC forced inside MAKE_TYPED_PROXY runs finalizers that close other proxies' leases: those closes are
// deferred, the outer call completes, then the drain applies each exactly once; ADOPTED and REUSED stay balanced.
#[test]
fn gc_during_make_typed_proxy_and_lease_balance() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.call(|| host.global("R", host.open_lua(ROOT, ROOT_TABLE)));
    host.gc_in_proxy.set(true);
    host.run(
        r#"
        for i = 1, 20 do
          local kids = R.children                    -- pinned by R's slot
          local first = kids[1]                      -- pinned by the array's slot
          R.children = {{base = i, extra = i}, {base = i, extra = i}}  -- detaches kids, which still pins first
          kids, first = nil, nil
          local t = {}
          -- each MAKE_TYPED_PROXY runs a full GC first: the detached array's finalizer closes its lease inside the call
          for j = 1, 10 do t[j] = R.children[1] end  -- the same child: one proxy (REUSED after the first)
          local tmp = R.child
          R.child = {base = i, extra = i}            -- detaches the previous child: its proxy may be collected
          tmp = nil
          local text = tostring(R)                   -- the variant's text: MAKE_TYPED_PROXY for R.choice
          if i == 10 then local _ = R.choice end      -- pinned from now on: later texts REUSE its proxy
          assert(text:find('"choice":', 1, true))
        end
        "#,
    );
    host.gc_in_proxy.set(false);
    assert!(host.stats.adopted.get() > 0 && host.stats.reused.get() > 0);
    assert!(
        host.stats.closes_in_proxy.get() > 0,
        "finalizers closed leases inside MAKE_TYPED_PROXY"
    );
    // after the outer calls ended, nothing is queued: every deferred close was applied
    assert_eq!(
        arena_collect(OWNER_A, host.control.get()).unwrap() as usize,
        host.retained.borrow().len()
    );
    host.lua.globals().raw_remove("R").unwrap();
    host.lua.gc_collect().unwrap();
    host.lua.gc_collect().unwrap();
    assert_eq!(
        arena_collect(OWNER_A, host.control.get()).unwrap(),
        0,
        "the last proxy's close released every pin"
    );
    host.finish();
}

// Red-first: closes from many threads while the worker keeps working: never a panic, a double close or a leak.
#[test]
fn multi_thread_close_stress() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    for round in 0..100 {
        let mut leases = Vec::new();
        for _ in 0..64 {
            let reply = host.call(|| value_child(OWNER_B, root, &Selector::Field(b"children")).unwrap());
            leases.push(reply.item);
        }
        let shared = Arc::new(leases.clone());
        let threads: Vec<_> = (0..4)
            .map(|t| {
                let shared = shared.clone();
                std::thread::spawn(move || {
                    let mut ok = 0;
                    for (i, lease) in shared.iter().enumerate() {
                        // every lease by one thread, every third by all four at once (close is idempotent)
                        if i % 4 == t || i % 3 == 0 {
                            assert_eq!(value_close(OWNER_B, *lease), Ok(()));
                            ok += 1;
                        }
                        assert_eq!(
                            value_close(OWNER_B, u64::MAX - t as u64).unwrap_err(),
                            Refusal::NotFound
                        );
                    }
                    ok
                })
            })
            .collect();
        // the worker keeps reading meanwhile
        for _ in 0..32 {
            let reply = host.call(|| value_child(OWNER_B, root, &Selector::Field(b"text")).unwrap());
            host.close(reply.payload);
        }
        let closed: usize = threads.into_iter().map(|t| t.join().unwrap()).sum();
        assert!(
            closed >= 64,
            "round {round}: every lease closed at least once ({closed})"
        );
        assert_eq!(
            arena_collect(OWNER_A, host.control.get()).unwrap() as usize,
            host.retained.borrow().len()
        );
        for lease in leases {
            assert_eq!(value_close(OWNER_B, lease), Ok(()));
        }
    }
    assert!(host.wakes.load(Ordering::SeqCst) > 0);
    host.close(root);
    host.finish();
}

// Red-first: RESOURCE roots are the shared ResourceInfo value (cached, pinned, flagged); LOADED_SNAPSHOT roots are per
// ResourceInfo instance, not per GUID; DEFAULT, BYTES and LUA_INPUT make fresh roots; only edited RESOURCE roots of a
// STARTUP arena enroll for apply.
#[test]
fn open_origins_and_startup_apply() {
    let bytes = encode(ROOT_TABLE, ROOT);
    let fixture = Fixture {
        resources: HashMap::from([([1; 16], (bytes.clone(), ROOT)), ([2; 16], (bytes.clone(), ROOT))]),
    };
    let host = Host::new(KILN_VALUE_STARTUP, Some(Rc::new(fixture)));
    let resource = |guid: [u8; 16]| {
        let mut request = Host::request(KILN_VALUE_OPEN_RESOURCE, b"", &[], 0);
        request.key.guid = guid;
        host.call(|| value_open(OWNER_A, host.control.get(), &host.view, &request).unwrap())
    };
    let a = resource([1; 16]);
    let b = resource([1; 16]);
    let info = |token| value_info(OWNER_B, token, 0).unwrap();
    assert_eq!(
        info(a.item).identity.node_id,
        info(b.item).identity.node_id,
        "one shared root per resource"
    );
    assert!(info(a.item).flags & KILN_VALUE_RESOURCE_ROOT as u32 != 0);
    let missing = resource([3; 16]);
    assert_eq!(
        info(missing.item).kind,
        KILN_VALUE_NIL as u32,
        "an absent resource is a nil root"
    );
    assert_eq!(host.json(missing.payload), packet_fixture("view_nil"));
    let failing = resource([0xee; 16]);
    assert_eq!(failing.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
    host.close(failing.error);
    let loaded = |instance: u64| {
        let mut request = Host::request(KILN_VALUE_OPEN_LOADED_SNAPSHOT, b"m6.Root", &bytes, 0);
        request.key.guid = [1; 16];
        request.source_instance = instance;
        host.call(|| value_open(OWNER_A, host.control.get(), &host.view, &request).unwrap())
    };
    let (l1, l1b, l2) = (loaded(10), loaded(10), loaded(11));
    assert_eq!(info(l1.item).identity.node_id, info(l1b.item).identity.node_id);
    assert_ne!(
        info(l1.item).identity.node_id,
        info(l2.item).identity.node_id,
        "per instance, not per GUID"
    );
    assert_ne!(info(l1.item).identity.node_id, info(a.item).identity.node_id);
    let default = host.call(|| host.open(Host::request(KILN_VALUE_OPEN_DEFAULT, b"m6.Child", &[], 0)));
    let base = host.call(|| value_child(OWNER_B, default.item, &Selector::Field(b"base")).unwrap());
    assert_eq!(host.json(base.payload)["value"]["value"], "i64:0000000000ff0011");
    assert_eq!(
        host.call(|| value_open(
            OWNER_A,
            host.control.get(),
            &host.view,
            &Host::request(KILN_VALUE_OPEN_DEFAULT, b"m6.Base", &[], 0)
        ))
        .unwrap_err(),
        Refusal::Unavailable
    );
    let mut foreign_view = host.view;
    foreign_view.generation += 1;
    assert_eq!(
        value_open(
            OWNER_A,
            host.control.get(),
            &foreign_view,
            &Host::request(KILN_VALUE_OPEN_BYTES, b"m6.Child", &[0; 8], 0)
        )
        .unwrap_err(),
        Refusal::InvalidArgument
    );
    assert!(
        startup_dirty_roots(OWNER_A, host.control.get()).unwrap().is_empty(),
        "nothing edited yet"
    );
    let nine = host.arg(Value::Integer(9));
    let child = host
        .call(|| value_child(OWNER_B, a.item, &Selector::Field(b"child")).unwrap())
        .item;
    host.call(|| value_set(OWNER_B, child, &Selector::Field(b"base"), nine).unwrap());
    let loaded_child = host
        .call(|| value_child(OWNER_B, l1.item, &Selector::Field(b"child")).unwrap())
        .item;
    host.call(|| value_set(OWNER_B, loaded_child, &Selector::Field(b"base"), nine).unwrap());
    let roots = startup_dirty_roots(OWNER_A, host.control.get()).unwrap();
    assert_eq!(roots.len(), 1, "only the edited RESOURCE root enrolls");
    assert_eq!(roots[0].0.guid, [1; 16]);
    let value = roots[0].1.as_ref().unwrap();
    let registry = userdata::registry();
    let encoded = value.to_bytes(&registry, &userdata::handle(&registry, ROOT)).unwrap();
    assert_ne!(encoded, bytes);
    // export of the root: the same bytes, retained (a size probe never encodes twice)
    let export = host.call(|| value_export(OWNER_B, a.item, b"").unwrap());
    assert_eq!(host.bytes(export.payload), encoded);
    for token in [
        a.item,
        b.item,
        missing.item,
        missing.payload,
        l1.item,
        l1b.item,
        l2.item,
        default.item,
        base.payload,
        child,
        loaded_child,
        export.payload,
    ] {
        host.close(token);
    }
    host.finish();
    let runtime = Host::new(KILN_VALUE_RUNTIME, None);
    assert_eq!(
        startup_dirty_roots(OWNER_A, runtime.control.get())
            .map(|_| ())
            .unwrap_err(),
        Refusal::Unavailable
    );
    assert_eq!(
        runtime
            .call(|| value_open(
                OWNER_A,
                runtime.control.get(),
                &runtime.view,
                &Host::request(KILN_VALUE_OPEN_RESOURCE, b"", &[], 0)
            ))
            .unwrap_err(),
        Refusal::Unavailable,
        "no resource source"
    );
    runtime.finish();
}

// Red-first: export against a selected type validates and clones first (SF encode_owned_value), and the value_result_copy
// size probe followed by the copy never repeats the operation.
#[test]
fn export_selected_type_and_short_copy() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let child = host.call(|| host.open_lua(CHILD, "return {base = 1, extra = 2}")).item;
    let export = host.call(|| value_export(OWNER_B, child, b"m6.Child").unwrap());
    let mut short = [0u8; 3];
    let required = value_result_copy(OWNER_B, export.payload, &mut short).unwrap();
    assert_eq!(
        (required, short),
        (8, [0; 3]),
        "too small: nothing copied, the size reported"
    );
    assert_eq!(
        host.bytes(export.payload),
        encode("return {base = 1, extra = 2}", CHILD)
    );
    let wrong = host.call(|| value_export(OWNER_B, child, b"m6.Root").unwrap());
    let tree = host.json(wrong.error)["error"].clone();
    assert_eq!(tree["class"], "validation");
    assert_eq!(
        unhex(tree["message"].as_str().unwrap()),
        b"at .: expected `m6.Root`, got `m6.Child`"
    );
    assert_eq!(
        value_export(OWNER_B, child, b"no.Such").unwrap_err(),
        Refusal::InvalidArgument
    );
    for token in [child, export.payload, wrong.error] {
        host.close(token);
    }
    host.finish();
}

// A review in the typed graph: an unread part of a mapped value converts through
// the codec's bounded walk (resource_values::mapped_to_value), not kfc's recursive Value::from. A node whose kids array holds
// the node itself (self-referencing bytes of the recursive m6.Node) overflowed the worker's stack on export (the process
// aborted); it is now the depth error, as a conversion error tree. startup_dirty_roots converts through the same code.
#[test]
fn export_of_self_referencing_unread_bytes_is_the_depth_error_not_a_stack_overflow() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    // m6.Node {kids: m6.NodeList @0}: the kids array starts at itself (relative offset 0) and holds one node
    let bytes: Vec<u8> = [0u32, 1].iter().flat_map(|w| w.to_le_bytes()).collect();
    let node = host.call(|| host.open_bytes(userdata::NODE, &bytes));
    assert_eq!(node.disposition, KILN_VALUE_HANDLED as u32, "{node:?}");
    let export = host.call(|| value_export(OWNER_B, node.item, b"").unwrap());
    assert_eq!(export.disposition, KILN_VALUE_SEMANTIC_ERROR as u32, "{export:?}");
    let message = unhex(host.json(export.error)["error"]["message"].as_str().unwrap());
    let message = String::from_utf8_lossy(&message);
    assert!(message.contains("value nested more than 128 levels deep"), "{message}");
    for token in [node.item, node.payload, export.error] {
        host.close(token);
    }
    host.finish();
}

// eml-array-count in the typed graph: a mapped array child made one entry per stored element (the typed graph's MArray), so bytes
// claiming 2^32-1 kids asked for ~100 GiB (the process aborted; ShroudForge's wrapper does the same). kfc now refuses a
// count the bytes after the array cannot hold, and the child is a semantic error.
#[test]
fn a_mapped_array_count_beyond_its_bytes_is_a_semantic_error_not_an_allocation() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    for count in [1_000_000, u32::MAX] {
        // m6.Node {kids: m6.NodeList @0}: the kids start 1000 bytes on, past the end of the 8 bytes
        let bytes: Vec<u8> = [1000u32, count].iter().flat_map(|w| w.to_le_bytes()).collect();
        let node = host.call(|| host.open_bytes(userdata::NODE, &bytes));
        assert_eq!(node.disposition, KILN_VALUE_HANDLED as u32, "{node:?}");
        let kids = host.call(|| value_child(OWNER_B, node.item, &Selector::Field(b"kids")).unwrap());
        assert_eq!(kids.disposition, KILN_VALUE_SEMANTIC_ERROR as u32, "{kids:?}");
        let message = unhex(host.json(kids.error)["error"]["message"].as_str().unwrap());
        let message = String::from_utf8_lossy(&message);
        let want = format!("array of {count} elements (element size 8) at offset 1000 does not fit in 8 bytes of data");
        assert!(message.contains(&want), "{message}");
        for token in [node.item, node.payload, kids.error] {
            host.close(token);
        }
    }
    host.finish();
}

// Red-first: the serializer writes the frozen packets (schema/sfi/eml-values/fixtures/packets.json), negative i64 as two's
// complement, Lua strings as bytes (non-UTF-8 and NUL kept), f64 as its bits.
#[test]
fn packets_match_the_frozen_fixtures() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let scalar = |chunk: &str, index: usize| {
        let reply = host.call(|| host.open_lua(index, chunk));
        let packet = host.json(reply.payload);
        host.close(reply.item);
        host.close(reply.payload);
        packet
    };
    assert_eq!(scalar("return -1", values::S64), packet_fixture("view_i64_negative"));
    assert_eq!(scalar("return 0.5", values::F64), packet_fixture("view_f64"));
    assert_eq!(scalar("return 'a\\0b\\255'", 7), packet_fixture("view_string_bytes"));
    assert_eq!(scalar("return true", values::BOOL), packet_fixture("view_bool"));
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let view = host.call(|| value_read(OWNER_B, root, KILN_VALUE_READ_VIEW as u32).unwrap());
    assert_eq!(host.json(view.payload), packet_fixture("view_struct_owned"));
    let choice = host.call(|| value_child(OWNER_B, root, &Selector::Field(b"choice")).unwrap());
    let view2 = host.call(|| value_read(OWNER_B, choice.item, KILN_VALUE_READ_VIEW as u32).unwrap());
    assert_eq!(
        value_info(OWNER_B, choice.item, 0).unwrap().kind,
        KILN_VALUE_VARIANT_OWNED as u32
    );
    assert_eq!(
        host.json(view2.payload)["value"]["variant_type"],
        json!({"index": "u64:0000000000000002", "qualified_name": "m6.Child"})
    );
    for token in [root, view.payload, choice.item, view2.payload] {
        host.close(token);
    }
    host.finish();
}

// kfc's TypeRegistry has no interior mutability: Send + Sync, so the archive table's metadata may keep it (or the bytes it came from) for the C table.
#[test]
fn registry_is_send_and_sync() {
    fn shared<T: Send + Sync>() {}
    shared::<TypeRegistry>();
    let mapped = KILN_VALUE_ARRAY_MAPPED as u32 + KILN_VALUE_ARRAY_OWNED as u32 + KILN_VALUE_STRUCT_OWNED as u32;
    assert!(mapped > 0);
}

// Red-first: an arena closed (from another thread) while an operation is in flight: the operation is refused REVOKED
// after its reads and makes no edit; its worker tears the arena down when the call ends and releases everything.
#[test]
fn arena_closed_while_an_operation_is_in_flight() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let child = host
        .call(|| value_child(OWNER_B, root, &Selector::Field(b"child")).unwrap())
        .item;
    let input = host.arg(host.eval("return {base = 99, extra = 98}"));
    host.close_in_get.set(true);
    let refused = host.call(|| value_set(OWNER_B, root, &Selector::Field(b"child"), input));
    assert_eq!(
        refused.unwrap_err(),
        Refusal::Revoked,
        "no reply after the arena closed"
    );
    assert!(host.wakes.load(Ordering::SeqCst) >= 1);
    assert!(
        host.retained.borrow().is_empty(),
        "the call's end tore the arena down and released every pin"
    );
    assert_eq!(value_info(OWNER_B, child, 0).map(|_| ()).unwrap_err(), Refusal::Revoked);
    assert_eq!(arena_collect(OWNER_A, host.control.get()).unwrap(), 0);
    host.finish();
}

// ---- the independent review's tests ------------------------------------------------------------------------------

/// m6.Scalars with floats whose texts differ between Rust's `Display` and Lua's `%.14g` (else the codec's scalars_table).
const SCALARS_TABLE: &str = r#"return {u8 = 200, s8 = -5, u16 = 60000, s16 = -300, s32 = -70000, u32 = 4000000000,
    u64 = -2, s64 = math.mininteger, f32 = 0.1, f64 = 0.1 + 0.2, b = true, color = "Green", flags = {"B", "A"},
    guid = "01234567-89ab-cdef-0123-456789abcdef", ref = "fedcba98-7654-3210-fedc-ba9876543210", alias = 9}"#;

/// The oracle: the wrappers' mlua wrapper of the same value (the reviewed SF port; its texts are mlua's `Value::to_string`),
/// owned (validated) or mapped (encoded, then decoded), and its `tostring` or the first line of the error it raised.
fn wrapper_text(chunk: &str, index: usize, mapped: bool) -> Result<String, String> {
    let lua = Lua::new();
    let registry = userdata::registry();
    let r#type = userdata::handle(&registry, index);
    let objects = ValueObjects::new();
    let value: Value = lua.load(chunk).eval().unwrap();
    let wrapped = if mapped {
        let bytes = rv::encode(&value, &r#type, &lua, &objects).unwrap();
        rv::decode(bytes.into(), &r#type, &lua, &objects).unwrap()
    } else {
        rv::validate_and_clone(&value, &r#type, &lua, &objects).unwrap()
    };
    lua.globals().set("w", wrapped).unwrap();
    let (ok, text): (bool, String) = lua
        .load(r#"local ok, t = pcall(tostring, w) return ok, ok and t or (tostring(t):match("^[^\n]*"))"#)
        .eval()
        .unwrap();
    if ok { Ok(text) } else { Err(text) }
}

/// The top-level `"key":value` entries of an SF struct text (SF's owned field order is a hash map's, unstable).
fn entries(text: &str) -> std::collections::BTreeSet<String> {
    let inner = &text[1..text.len() - 1];
    let (mut out, mut depth, mut quoted, mut start) = (std::collections::BTreeSet::new(), 0, false, 0);
    for (i, c) in inner.char_indices() {
        match c {
            '"' => quoted = !quoted,
            '[' | '{' if !quoted => depth += 1,
            ']' | '}' if !quoted => depth -= 1,
            ',' if !quoted && depth == 0 => {
                out.insert(inner[start..i].to_owned());
                start = i + 1;
            }
            _ => {}
        }
    }
    out.insert(inner[start..].to_owned());
    out
}

// Review fix (red-first): Source's text of a value is mlua 0.11.6's `Value::to_string` (value.rs:154-187), which SF's
// `__tostring` bodies and incompatible-type texts call (owned_values.rs:146-170, 407-430; convert_resource_values.rs):
// a float is Rust's Display (`0.30000000000000004`, `1`), not Lua's `%.14g` (`0.3`, `1.0`), and a string that is not
// UTF-8 is mlua's conversion error (`to_str()?`), never raw bytes. The value wrappers of the same values are the oracle.
#[test]
fn texts_are_mluas_value_to_string() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let stringify = |lease: u64| -> Result<String, String> {
        let reply = host.call(|| value_read(OWNER_B, lease, KILN_VALUE_READ_STRINGIFY as u32).unwrap());
        if reply.disposition == KILN_VALUE_SEMANTIC_ERROR as u32 {
            let tree = host.json(reply.error);
            host.close(reply.error);
            return Err(String::from_utf8(unhex(tree["error"]["message"].as_str().unwrap())).unwrap());
        }
        let text = String::from_utf8(host.bytes(reply.payload)).unwrap();
        host.close(reply.payload);
        Ok(text)
    };
    // owned: SF's hash-map order, so the same entries
    let owned = host.call(|| host.open_lua(values::SCALARS, SCALARS_TABLE)).item;
    let text = stringify(owned).unwrap();
    assert!(
        text.contains("\"f64\":0.30000000000000004") && text.contains("\"f32\":0.1"),
        "{text}"
    );
    assert_eq!(
        entries(&text),
        entries(&wrapper_text(SCALARS_TABLE, values::SCALARS, false).unwrap())
    );
    // mapped: the type's field order, decoded floats (an f32 widened): byte-equal
    let bytes = encode(SCALARS_TABLE, values::SCALARS);
    let mapped = host.call(|| host.open_bytes(values::SCALARS, &bytes)).item;
    let text = stringify(mapped).unwrap();
    assert!(text.contains("\"f32\":0.10000000149011612"), "{text}");
    assert_eq!(Ok(text), wrapper_text(SCALARS_TABLE, values::SCALARS, true));
    // a string that is not UTF-8: SF's __tostring raises mlua's conversion error
    let bad = ROOT_TABLE.replace("\"synthetic\"", "\"\\255\"");
    let root = host.call(|| host.open_lua(ROOT, &bad)).item;
    let error = stringify(root).unwrap_err();
    assert_eq!(
        error,
        "error converting Lua string to &str (invalid utf-8 sequence of 1 bytes from index 0)"
    );
    assert_eq!(Err(error), wrapper_text(&bad, ROOT, false));
    // an incompatible value GPL holds itself (value_export against another type) prints as Source prints it
    let one = host.call(|| host.open_lua(values::F64, "return 1.0"));
    let reply = host.call(|| value_export(OWNER_B, one.item, b"m6.Child").unwrap());
    assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!(
        (&tree["kind"], &tree["found"]),
        (&json!("incompatible_type"), &json!("bytes:31")),
        "{tree}"
    );
    host.close(reply.error);
    for token in [owned, mapped, root, one.item, one.payload] {
        host.close(token);
    }
    host.finish();
}

// Review fix (red-first): a pairs step borrows nothing. SF's iterator userdata holds the value's cache, not the value's
// userdata (owned_values.rs ArrayValueIter, mapped_values.rs MappedArrayValueIter), so a step taken while
// table.insert on the same array validates its argument works; only ITER_BEGIN (`__pairs`) borrows the array.
#[test]
fn iterator_steps_borrow_nothing() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    for mapped in [false, true] {
        let root = if mapped {
            host.call(|| host.open_bytes(ROOT, &encode(ROOT_TABLE, ROOT))).item
        } else {
            host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item
        };
        let kids = host
            .call(|| value_child(OWNER_B, root, &Selector::Field(b"children")).unwrap())
            .item;
        let iter = host
            .call(|| value_read(OWNER_B, kids, KILN_VALUE_READ_ITER_BEGIN as u32).unwrap())
            .item;
        let steps: Rc<RefCell<Vec<(u32, Json)>>> = Rc::default();
        let seen = steps.clone();
        let step = host
            .lua
            .create_function(move |lua, ()| {
                let host = host_of(lua);
                host.call(|| {
                    let reply = value_read(OWNER_B, iter, KILN_VALUE_READ_ITER_NEXT as u32).unwrap();
                    let packet = host.json(if reply.payload != 0 { reply.payload } else { reply.error });
                    seen.borrow_mut().push((reply.disposition, packet));
                    for token in [reply.item, reply.payload, reply.error] {
                        host.close(token);
                    }
                });
                Ok(())
            })
            .unwrap();
        host.lua.globals().set("step", step).unwrap();
        let input = host.arg(host.eval(
            r#"return setmetatable({}, {__index = function(_, k) if k == "base" then step() end return 1 end})"#,
        ));
        let pushed = host.call(|| value_array_push(OWNER_B, kids, input).unwrap());
        let steps = steps.borrow();
        assert_eq!(steps.len(), 1);
        assert_eq!(steps[0].0, KILN_VALUE_HANDLED as u32, "mapped {mapped}: {}", steps[0].1);
        assert_eq!(steps[0].1, packet_fixture("iter_next_item"));
        assert_eq!(
            (pushed.disposition, pushed.is_mutated()),
            (KILN_VALUE_HANDLED as u32, true),
            "mapped {mapped}"
        );
        drop(steps);
        for token in [iter, kids, root] {
            host.close(token);
        }
    }
    host.lua.globals().raw_remove("step").unwrap();
    host.finish();
}

// Review fix (red-first): once a closed arena is torn down and nothing is pending the host may free its reader (its
// `user` context), so no later close from another thread may call `wake`: an idempotent arena_close, or a value_close
// of a lease the teardown revoked.
#[test]
fn no_wake_after_teardown() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    let root = host.call(|| host.open_lua(ROOT, ROOT_TABLE)).item;
    let child = host
        .call(|| value_child(OWNER_B, root, &Selector::Field(b"child")).unwrap())
        .item;
    host.finish();
    let wakes = host.wakes.load(Ordering::SeqCst);
    let control = host.control.get();
    let (closed, again) = std::thread::spawn(move || (arena_close(OWNER_A, control), value_close(OWNER_B, child)))
        .join()
        .unwrap();
    assert_eq!((closed, again), (Ok(0), Ok(())));
    assert_eq!(host.wakes.load(Ordering::SeqCst), wakes, "no wake after the teardown");
    assert_eq!(arena_collect(OWNER_A, control).unwrap(), 0);
}

// Review fix (red-first): a raw sequence read that raises names its entry's index in the path, as the codec's
// `sequence_values` loop does (the index is pushed before the entry's own error).
#[test]
fn sequence_read_errors_name_their_index() {
    let host = Host::new(KILN_VALUE_RUNTIME, None);
    host.raw_error_at.set(2);
    let reply = host.call(|| {
        host.open_lua(
            CHILDREN,
            "return {{base = 1, extra = 2}, {base = 3, extra = 4}, {base = 5, extra = 6}}",
        )
    });
    host.raw_error_at.set(0);
    assert_eq!(reply.disposition, KILN_VALUE_SEMANTIC_ERROR as u32);
    let tree = host.json(reply.error)["error"].clone();
    assert_eq!((&tree["path"], &tree["kind"]), (&json!("1"), &json!("lua")), "{tree}");
    assert_eq!(
        unhex(tree["message"].as_str().unwrap()),
        b"at 1: runtime error: raw read failed".to_vec()
    );
    host.close(reply.error);
    host.finish();
}
