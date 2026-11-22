// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/access_named_service.rs:1-273 (RuntimeService, get,
// snapshot, read_fields, write_field, the userdata's fields and methods); game-runtime/bridge/windows/named_services/
// {read_service.rs:5-35,update_service.rs:5-45} (the provider's read and compare-exchange, their checks and texts).
//! `runtime.services.get` and its service views, on kiln.world's named services.
//!
//! - **get(name)**: ShroudForge's checks in its order (the denial and the availability of `runtime.ecs.read`, one
//!   argument, the exact original DI name, its reflected `keen::` type in the archive registry, a bounded plain-data
//!   struct whose every field is an inline field, a live session), then one read through the provider, then the session
//!   again. The view keeps the key, the type, its hash and size and ShroudForge's world session marker; it holds no
//!   pointer and no Kiln state but the VM's shared views below.
//! - **view:read_fields(paths)**, **view:write_field(path, value, expected[, expected_fields])**: ShroudForge's
//!   reflected inline fields (the ECS family's [`crate::runtime::ecs::fields`]) over a fresh snapshot each time; the
//!   write compares the whole service with that snapshot and changes only the field's leaves under its mask, after the
//!   ownership check (another mod's claim on the service refuses). An uncertain outcome latches the session's writes
//!   (the write latch) and this view's.
//! - **The provider** ([`Views`]): ShroudForge's `service_read` and `service_compare_exchange` on kiln.world's
//!   `named_service_open`, `named_service_snapshot` and `named_service_compare_exchange`. One kiln.world view per
//!   service key per VM, opened on first use and opened again when kiln.world says it is stale or gone (a new server
//!   session); the views close with the VM. kiln.world finds the service again under the engine's gate for every
//!   snapshot and compare-exchange.
//! - [`crate::runtime::overrides::targets::service`]: `runtime.overrides.get_service_claim` and `service`.
//!
//! The family ([`family`]) shares the VM's ECS ([`crate::runtime::ecs::of_vm`]): its type session, inline field
//! plans, owner, world session marker and runtime facts. The registrar adds it right after the ECS family.

use crate::runtime::context::RuntimeContext;
use crate::runtime::ecs::{self, Ecs};
use crate::runtime::install::Family;
use crate::runtime::overrides::{StorageKey, check_write};
use crate::runtime::services::{ServiceAdapter, ServiceError, ServiceTable};
use crate::runtime::types::selectors;
use crate::runtime::unavailable;
use crate::runtime::write_fault::WriteOutcome;
use crate::service_member;
use kiln_abi::services::kiln_world::{
    KILN_ECS_CONFLICT, KILN_ECS_ERR_STALE, KILN_ECS_OK, KILN_ECS_UNCERTAIN, KILN_WORLD_SERVICE,
    KILN_WORLD_VERSION, KilnMutationReply, KilnWorldApi,
};
use kiln_abi::{
    KILN_ERR_NOT_FOUND, KILN_OK, KILN_WORLD_SERVER, KilnHandle, KilnOwner, KilnResult, KilnStr,
};
use mlua::{AnyUserData, Lua, MultiValue, Table, UserData, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::FunctionArgs;
use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap};
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "named-services";

/// The names the family installs: `runtime.services.get` and the service claim and target in the installer's
/// `runtime.overrides` container.
pub const CLAIMS: [&str; 3] = [
    "runtime.services.get",
    "runtime.overrides.get_service_claim",
    "runtime.overrides.service",
];

/// The capability rows of kiln.world's named services (kiln.caps; docs/kiln/services/caps.md).
pub const READ_CAPABILITY: &str = "world.named_read";
pub const WRITE_CAPABILITY: &str = "world.named_write";

/// What ShroudForge calls a service's scope.
pub const SCOPE: &str = "host-world-shared";

/// ShroudForge's texts.
pub mod text {
    // access_named_service.rs
    pub const GET_ARGUMENTS: &str = "runtime.services.get expects one original service name";
    pub const EXACT_NAME: &str = "use the exact original DI name, for example ecs::GameSettings";
    pub const NO_TYPE: &str = "original service has no active reflected type ";
    pub const NOT_PLAIN: &str = "named service requires a bounded reflected plain-data struct";
    pub const NO_FIELDS: &str = "named service has no reflected inline fields";
    pub const VIEW_SESSION: &str = "service view belongs to another or an inactive world session";
    pub const NEEDS_CONTRACT: &str =
        " needs an admitted original plain-data contract and an available authoritative Scene: ";
    pub const OTHER_MOD: &str = "service view belongs to a different runtime mod";
    pub const NO_REFLECTION: &str = "service reflection is unavailable";
    pub const REFLECTION_CHANGED: &str = "service reflection changed since view creation";
    pub const READ_ARGUMENTS: &str = "read_fields expects one dense array of reflected field paths";
    pub const READ_TABLE: &str = "read_fields expects a dense array of field paths";
    pub const DENSE: &str = "field paths must be a dense array";
    pub const AT_MOST: &str = "select a dense array of at most 1024 field paths";
    pub const STRINGS: &str = "field paths must be strings";
    pub const DUPLICATE: &str = "duplicate field path";
    pub const NONEMPTY: &str = "select a nonempty dense array of field paths";
    pub const WRITE_ARGUMENTS: &str =
        "write_field expects path, value, expected current value and optional expected_fields";
    pub const WRITE_PATH: &str = "write_field expects a reflected field path";
    pub const FIELD_CHANGED: &str =
        "service field changed before update. Read current fields and retry";
    pub const EXPECTED_FIELDS: &str = "expected_fields must be a nonempty field-path/value map";
    // read_service.rs
    pub const SNAPSHOT_SIZE: &str = "invalid reflected service snapshot size";
    pub const SNAPSHOT_SESSION: &str =
        "service snapshot belongs to another or an inactive world session";
    pub const INVALID_KEY: &str = "invalid service key";
    pub const NO_SNAPSHOTS: &str = "native provider lacks typed service snapshots";
    pub const GUARDED_READ: &str =
        "guarded service snapshot is unavailable or changed during retrieval";
    pub const SESSION_CHANGED: &str = "world session changed during service snapshot retrieval";
    // update_service.rs
    pub const SIZES: &str = "service snapshot, mask and reflected layout sizes disagree";
    pub const UPDATE_SESSION: &str =
        "service update belongs to another or an inactive world session";
    pub const NO_WRITES: &str = "native provider lacks typed service writes";
    pub const CONFLICT: &str =
        "Service snapshot changed before update. Read current values and retry";
    pub const UNCERTAIN: &str =
        "Service write outcome is uncertain. Stop writes and do not retry blindly";
    pub const FAILED: &str = "guarded service update failed";
}

/// The largest snapshot the provider takes (read_service.rs:6).
const MAX_SNAPSHOT: u32 = 256 * 1024 * 1024;

fn fnv1a(text: &str) -> u32 {
    text.bytes().fold(0x811c_9dc5_u32, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

/// The provider: one VM's kiln.world views of its named services, by key (module docs). Closing is the views'
/// `Drop`: the VM's end closes every view it opened.
pub struct Views {
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    open: RefCell<HashMap<String, KilnHandle>>,
}

impl Views {
    pub fn new(services: Rc<ServiceAdapter>, owner: KilnOwner) -> Self {
        Self {
            services,
            owner,
            open: RefCell::new(HashMap::new()),
        }
    }

    fn world(&self) -> Result<ServiceTable<KilnWorldApi>, ServiceError> {
        self.services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
    }

    /// How many kiln.world views this VM holds now.
    pub fn open_views(&self) -> usize {
        self.open.borrow().len()
    }

    /// The kiln.world view of `key`, opened when there is none: its handle, or kiln.world's refusal.
    fn view(&self, key: &str) -> Result<KilnHandle, KilnResult> {
        if let Some(handle) = self.open.borrow().get(key) {
            return Ok(*handle);
        }
        let open = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, named_service_open))
            .map_err(|_| kiln_abi::KILN_ERR_UNAVAILABLE)?;
        let mut handle: KilnHandle = 0;
        let name = KilnStr {
            ptr: key.as_ptr().cast(),
            len: key.len(),
        };
        // SAFETY: kiln_world.h named_service_open: the caller's owner, a counted name, the live session, an out handle,
        // no type wanted.
        let result = unsafe {
            self.services.call(
                open,
                (
                    self.owner,
                    name,
                    0u64,
                    &raw mut handle,
                    std::ptr::null_mut(),
                ),
            )
        };
        if result != KILN_OK || handle == 0 {
            return Err(result);
        }
        self.open.borrow_mut().insert(key.to_owned(), handle);
        Ok(handle)
    }

    /// Closes and forgets the view of `key` (kiln.world said it is stale or gone).
    fn forget(&self, key: &str) {
        let handle = self.open.borrow_mut().remove(key);
        if let Some(handle) = handle {
            self.close(handle);
        }
    }

    fn close(&self, handle: KilnHandle) {
        if let Ok(close) = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, named_service_close))
        {
            // SAFETY: kiln_world.h named_service_close: the caller's owner and a handle it opened.
            unsafe { self.services.call(close, (self.owner, handle)) };
        }
    }

    /// ShroudForge's `service_read(key, type_hash, size, session)` (read_service.rs:5-35): the service's whole bytes
    /// through kiln.world, in ShroudForge's session marker `session` (`live` reads it now).
    pub fn read(
        &self,
        key: &str,
        type_hash: u32,
        size: u32,
        session: u64,
        live: &dyn Fn() -> u64,
    ) -> Result<Vec<u8>, String> {
        if size == 0 || size > MAX_SNAPSHOT {
            return Err(text::SNAPSHOT_SIZE.into());
        }
        if session == 0 || live() != session {
            return Err(text::SNAPSHOT_SESSION.into());
        }
        if key.contains('\0') {
            return Err(text::INVALID_KEY.into());
        }
        let snapshot = self
            .world()
            .and_then(|table| {
                service_member!(table, KilnWorldApi, named_service_open)?;
                service_member!(table, KilnWorldApi, named_service_close)?;
                service_member!(table, KilnWorldApi, named_service_snapshot)
            })
            .map_err(|_| text::NO_SNAPSHOTS)?;
        // the contract's type: kiln.world's services are the profile's, whose hash is the qualified name's
        if type_hash != fnv1a(&format!("keen::{key}")) {
            return Err(text::GUARDED_READ.into());
        }
        let mut bytes = vec![0u8; size as usize];
        let mut done = false;
        for attempt in 0..2 {
            let handle = self.view(key).map_err(|_| text::GUARDED_READ)?;
            let mut required = 0u32;
            // SAFETY: kiln_world.h named_service_snapshot: the caller's owner and view, a buffer of `size` bytes and an
            // out count.
            let result = unsafe {
                self.services.call(
                    snapshot,
                    (
                        self.owner,
                        handle,
                        bytes.as_mut_ptr().cast::<core::ffi::c_void>(),
                        size,
                        &raw mut required,
                    ),
                )
            };
            match result {
                KILN_OK if required == size => {
                    done = true;
                    break;
                }
                // a view of an ended session, or one kiln.world no longer has: open it again, once
                KILN_ECS_ERR_STALE | KILN_ERR_NOT_FOUND if attempt == 0 => self.forget(key),
                _ => break,
            }
        }
        if !done {
            return Err(text::GUARDED_READ.into());
        }
        if live() != session {
            return Err(text::SESSION_CHANGED.into());
        }
        Ok(bytes)
    }

    /// ShroudForge's `service_compare_exchange(key, type_hash, size, session, expected, mask, desired)`
    /// (update_service.rs:5-45) through kiln.world. An uncertain outcome is reported to `report` with kiln.world's
    /// server session of the attempt.
    #[allow(clippy::too_many_arguments)]
    pub fn compare_exchange(
        &self,
        key: &str,
        type_hash: u32,
        size: u32,
        session: u64,
        live: &dyn Fn() -> u64,
        expected: &[u8],
        mask: &[u8],
        desired: &[u8],
        report: &dyn Fn(u64) -> String,
    ) -> Result<(), String> {
        let n = size as usize;
        if size == 0
            || size > MAX_SNAPSHOT
            || expected.len() != n
            || mask.len() != n
            || desired.len() != n
            || mask.iter().any(|byte| *byte > 1)
        {
            return Err(text::SIZES.into());
        }
        if session == 0 || live() != session {
            return Err(text::UPDATE_SESSION.into());
        }
        if key.contains('\0') {
            return Err(text::INVALID_KEY.into());
        }
        let exchange = self
            .world()
            .and_then(|table| {
                service_member!(table, KilnWorldApi, named_service_open)?;
                service_member!(table, KilnWorldApi, named_service_close)?;
                service_member!(table, KilnWorldApi, named_service_compare_exchange)
            })
            .map_err(|_| text::NO_WRITES)?;
        // ShroudForge's native refuses a contract of another type and a mask that selects nothing
        if type_hash != fnv1a(&format!("keen::{key}")) || mask.iter().all(|byte| *byte == 0) {
            return Err(text::FAILED.into());
        }
        let handle = self.view(key).map_err(|_| text::FAILED)?;
        // SAFETY: an all-zero reply is valid (integers only).
        let mut reply: KilnMutationReply = unsafe { std::mem::zeroed() };
        reply.size = std::mem::size_of::<KilnMutationReply>() as u32;
        // SAFETY: kiln_world.h named_service_compare_exchange: the caller's owner and view, three buffers of `size`
        // bytes, no confirmed copy and a sized reply record.
        let result = unsafe {
            self.services.call(
                exchange,
                (
                    self.owner,
                    handle,
                    expected.as_ptr().cast::<core::ffi::c_void>(),
                    mask.as_ptr().cast::<core::ffi::c_void>(),
                    desired.as_ptr().cast::<core::ffi::c_void>(),
                    size,
                    std::ptr::null_mut::<core::ffi::c_void>(),
                    &raw mut reply,
                ),
            )
        };
        if result == KILN_ECS_ERR_STALE || result == KILN_ERR_NOT_FOUND {
            // the next read opens a view of the live session
            self.forget(key);
        }
        match reply.status as i32 {
            KILN_ECS_OK if result == KILN_OK => Ok(()),
            KILN_ECS_CONFLICT => Err(text::CONFLICT.into()),
            KILN_ECS_UNCERTAIN => Err(report(reply.session)),
            _ => Err(text::FAILED.into()),
        }
    }
}

impl Drop for Views {
    fn drop(&mut self) {
        let handles: Vec<KilnHandle> = self.open.borrow_mut().drain().map(|(_, h)| h).collect();
        for handle in handles {
            self.close(handle);
        }
    }
}

/// kiln.world's named-service members (presence only): open, snapshot, close and compare-exchange.
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)?;
    service_member!(table, KilnWorldApi, named_service_open)?;
    service_member!(table, KilnWorldApi, named_service_snapshot)?;
    service_member!(table, KilnWorldApi, named_service_close)?;
    service_member!(table, KilnWorldApi, named_service_compare_exchange)?;
    Ok(())
}

/// ShroudForge's `require_access`: the denial of `operation`, then its availability.
fn require_access(host: &dyn RuntimeHost, operation: Op) -> Result<(), String> {
    if let Some(reason) = runtime_denial_reason(host, operation) {
        return Err(reason);
    }
    if !ecs::available(host, operation) {
        return Err(format!("{} is currently unavailable", operation.as_str()));
    }
    Ok(())
}

/// A view of one original named service for one package (`RuntimeService`): its identity and session only.
pub struct RuntimeService {
    owner: String,
    key: String,
    type_index: usize,
    type_hash: u32,
    size: u32,
    session: u64,
    write_fault: RefCell<Option<String>>,
    ecs: Rc<Ecs>,
    views: Rc<Views>,
    host: Rc<dyn RuntimeHost>,
}

impl RuntimeService {
    pub fn key(&self) -> &str {
        &self.key
    }
    pub fn session(&self) -> u64 {
        self.session
    }

    /// ShroudForge's `ownership_identity`: (key, session) for the view's own package only.
    pub fn ownership_identity(&self, owner: &str) -> Result<(&str, u64), String> {
        if self.owner != owner {
            return Err(text::OTHER_MOD.into());
        }
        Ok((&self.key, self.session))
    }

    fn require_session(&self) -> Result<(), String> {
        require_session(&self.ecs, self.session)
    }

    fn live(&self) -> impl Fn() -> u64 + '_ {
        || self.ecs.world_session()
    }

    fn snapshot(&self) -> Result<Vec<u8>, String> {
        require_access(self.host.as_ref(), Op::EcsRead)?;
        self.require_session()?;
        let types = self.ecs.types();
        let ty = types.get(self.type_index).ok_or(text::NO_REFLECTION)?;
        if ty.qualified_name != format!("keen::{}", self.key)
            || ty.qualified_hash != self.type_hash
            || ty.size != self.size
        {
            return Err(text::REFLECTION_CHANGED.into());
        }
        let bytes = self.views.read(
            &self.key,
            self.type_hash,
            self.size,
            self.session,
            &self.live(),
        )?;
        self.require_session()?;
        Ok(bytes)
    }

    fn read_fields(&self, lua: &Lua, args: &MultiValue) -> Result<Table, String> {
        if args.len() != 1 {
            return Err(text::READ_ARGUMENTS.into());
        }
        let Some(Value::Table(paths)) = args.front() else {
            return Err(text::READ_TABLE.into());
        };
        let mut selected = BTreeSet::new();
        for pair in paths.clone().pairs::<Value, Value>() {
            let (key, path) = pair.map_err(|e| e.to_string())?;
            let Value::Integer(index) = key else {
                return Err(text::DENSE.into());
            };
            if index <= 0 || index as usize > paths.raw_len() || selected.len() >= 1024 {
                return Err(text::AT_MOST.into());
            }
            let Value::String(path) = path else {
                return Err(text::STRINGS.into());
            };
            if !selected.insert(path.to_str().map_err(|e| e.to_string())?.to_owned()) {
                return Err(text::DUPLICATE.into());
            }
        }
        if selected.is_empty() || selected.len() != paths.raw_len() {
            return Err(text::NONEMPTY.into());
        }
        let bytes = self.snapshot()?;
        let values = self.ecs.values().map_err(|e| e.to_string())?;
        let result = lua
            .create_table_with_capacity(0, selected.len())
            .map_err(|e| e.to_string())?;
        for path in selected {
            let field = self.ecs.inline_field(self.type_index, &path)?;
            result
                .raw_set(path.as_str(), field.read(lua, &values, &bytes)?)
                .map_err(|e| e.to_string())?;
        }
        self.require_session()?;
        Ok(result)
    }

    fn write_field(&self, args: &MultiValue) -> Result<(), String> {
        if let Some(reason) = self.write_fault.borrow().as_ref() {
            return Err(reason.clone());
        }
        if args.len() != 3 && args.len() != 4 {
            return Err(text::WRITE_ARGUMENTS.into());
        }
        let Some(Value::String(path)) = args.front() else {
            return Err(text::WRITE_PATH.into());
        };
        let path = path.to_str().map_err(|e| e.to_string())?.to_owned();
        require_access(self.host.as_ref(), Op::EcsWrite)?;
        let before = self.snapshot()?;
        let field = self.ecs.inline_field(self.type_index, &path)?;
        let values = self.ecs.values().map_err(|e| e.to_string())?;
        let mut expected = before.clone();
        let mut expected_mask = vec![0u8; before.len()];
        field.write_into(&values, &args[2], &mut expected, &mut expected_mask)?;
        if expected != before {
            return Err(text::FIELD_CHANGED.into());
        }
        if args.len() == 4 {
            let Value::Table(expected_fields) = &args[3] else {
                return Err(text::EXPECTED_FIELDS.into());
            };
            let mut depends = vec![0u8; before.len()];
            ecs::fields::expect_inline_fields(
                &self.ecs,
                &values,
                self.type_index,
                &before,
                expected_fields,
                &mut depends,
            )?;
        }
        let mut after = before.clone();
        let mut mask = vec![0u8; before.len()];
        field.write_into(&values, &args[1], &mut after, &mut mask)?;
        self.require_session()?;
        if let Some(runtime) = self.ecs.runtime() {
            let writer = ecs::cas::Writer {
                host: self.host.as_ref(),
                session: self.ecs.world_session(),
            };
            let key = StorageKey::Service {
                session: self.session,
                key: self.key.clone(),
            };
            check_write(runtime.facts.ownership(), &writer, &key)?;
        }
        let facts = self.ecs.runtime().map(|r| r.facts);
        let report = |session: u64| -> String {
            match &facts {
                Some(facts) => facts
                    .write_faults()
                    .report(
                        KILN_WORLD_SERVER as u32,
                        session,
                        WriteOutcome::ServiceWriteUncertain,
                    )
                    .to_owned(),
                None => text::UNCERTAIN.to_owned(),
            }
        };
        if let Err(reason) = self.views.compare_exchange(
            &self.key,
            self.type_hash,
            self.size,
            self.session,
            &self.live(),
            &before,
            &mask,
            &after,
            &report,
        ) {
            if reason.to_lowercase().contains("uncertain") {
                *self.write_fault.borrow_mut() = Some(reason.clone());
            }
            return Err(reason);
        }
        // kiln.world read the whole service back under the gate and found it again before the release: no second,
        // unlocked observation is taken for the outcome.
        Ok(())
    }
}

impl UserData for RuntimeService {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("key", |_, this| Ok(this.key.clone()));
        fields.add_field_method_get("scope", |_, _| Ok(SCOPE));
        fields.add_field_method_get("session_id", |_, this| Ok(this.session));
        fields.add_field_method_get("write_supported", |_, _| Ok(true));
        fields.add_field_method_get("type", |lua, this| {
            this.ecs.type_value(lua, this.type_index)
        });
    }
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("read_fields", |lua, this, args: MultiValue| {
            Ok(match this.read_fields(lua, &args) {
                Ok(values) => (Some(values), None),
                Err(reason) => (None, Some(reason)),
            })
        });
        methods.add_method("write_field", |_, this, args: MultiValue| {
            Ok(match this.write_field(&args) {
                Ok(()) => (true, None),
                Err(reason) => (false, Some(reason)),
            })
        });
    }
}

fn require_session(ecs: &Ecs, session: u64) -> Result<(), String> {
    if session == 0 || ecs.world_session() != session {
        return Err(text::VIEW_SESSION.into());
    }
    Ok(())
}

/// `runtime.services.get(name)` (access_named_service.rs:46-104): `view` or `nil, reason`.
pub fn get(
    lua: &Lua,
    ecs: &Rc<Ecs>,
    views: &Rc<Views>,
    host: Rc<dyn RuntimeHost>,
    args: &FunctionArgs,
) -> mlua::Result<(Option<AnyUserData>, Option<String>)> {
    let resolve = || -> Result<RuntimeService, String> {
        require_access(host.as_ref(), Op::EcsRead)?;
        if args.len() != 1 {
            return Err(text::GET_ARGUMENTS.into());
        }
        let key = args.get::<String>(0).map_err(|e| e.to_string())?;
        if key.is_empty() || key.len() > 127 || key.contains('\0') || key.starts_with("keen::") {
            return Err(text::EXACT_NAME.into());
        }
        let qualified = format!("keen::{key}");
        let types = ecs.types();
        let metadata = types
            .by_qualified_name(&qualified)
            .ok_or_else(|| format!("{}{qualified}", text::NO_TYPE))?;
        if metadata.primitive_type != "Struct" || metadata.size == 0 || metadata.size > 65536 {
            return Err(text::NOT_PLAIN.into());
        }
        // A service contract covers its whole object: any pointer, dynamic storage, reference or overlapping
        // descriptor is refused at creation.
        let fields = selectors::fields(types, metadata.index)?;
        if fields.is_empty() {
            return Err(text::NO_FIELDS.into());
        }
        for (_, field) in &fields {
            ecs.inline_field(metadata.index, &field.name)?;
        }
        let session = ecs.world_session();
        require_session(ecs, session)?;
        let live = || ecs.world_session();
        views
            .read(&key, metadata.qualified_hash, metadata.size, session, &live)
            .map_err(|reason| format!("{key}{}{reason}", text::NEEDS_CONTRACT))?;
        require_session(ecs, session)?;
        Ok(RuntimeService {
            owner: host.owner_context().id().to_owned(),
            key,
            type_index: metadata.index,
            type_hash: metadata.qualified_hash,
            size: metadata.size,
            session,
            write_fault: RefCell::new(None),
            ecs: ecs.clone(),
            views: views.clone(),
            host: host.clone(),
        })
    };
    Ok(match resolve() {
        Ok(service) => (Some(lua.create_userdata(service)?), None),
        Err(reason) => (None, Some(reason)),
    })
}

/// The VM's shared views, as Lua app data (closed with the VM).
struct VmViews(Rc<Views>);

/// Puts `runtime.services.get` and the service claim and target into one package's `runtime` table.
pub fn install_into(
    lua: &Lua,
    runtime: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: Rc<Ecs>,
) -> mlua::Result<()> {
    let views = match lua.app_data_ref::<VmViews>() {
        Some(views) => views.0.clone(),
        None => {
            let views = Rc::new(Views::new(ecs.services().clone(), ecs.owner()));
            lua.set_app_data(VmViews(views.clone()));
            views
        }
    };
    let services = match runtime.raw_get::<Value>("services")? {
        Value::Table(table) => table,
        _ => {
            let table = lua.create_table()?;
            runtime.raw_set("services", &table)?;
            table
        }
    };
    let (e, v, h) = (ecs.clone(), views.clone(), host.clone());
    services.raw_set(
        "get",
        lua.create_function(move |lua, args: FunctionArgs| get(lua, &e, &v, h.clone(), &args))?,
    )?;
    crate::runtime::overrides::targets::service::install_into(lua, runtime, host, ecs)
}

/// The production family. It needs the VM's shared ECS (the ECS family installs it first in the registrar's list);
/// a package without one (no `runtime` namespace or no host) gets nothing, and should a VM have no ECS, the names
/// answer ShroudForge's refusals (the unavailable family's rows), never `nil`.
pub fn family() -> Family {
    Family::new(
        FAMILY,
        &CLAIMS,
        move |lua, _, package, composed, cx: &RuntimeContext| {
            let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
                return Ok(());
            };
            match ecs::of_vm(lua) {
                Some(ecs) => install_into(lua, runtime, host, ecs),
                None => {
                    let _ = package;
                    let caller = unavailable::Caller::Host(host);
                    for name in CLAIMS {
                        let row = unavailable::ROWS
                            .iter()
                            .find(|row| row.name == name)
                            .expect("every claim has an unavailable row");
                        let (table, last) = match name.strip_prefix("runtime.services.") {
                            Some(last) => {
                                let table = match runtime.raw_get::<Value>("services")? {
                                    Value::Table(table) => table,
                                    _ => {
                                        let table = lua.create_table()?;
                                        runtime.raw_set("services", &table)?;
                                        table
                                    }
                                };
                                (table, last)
                            }
                            None => (
                                runtime.raw_get::<Table>("overrides")?,
                                name.strip_prefix("runtime.overrides.")
                                    .expect("an override member"),
                            ),
                        };
                        let caller = caller.clone();
                        table.raw_set(
                            last,
                            lua.create_function(move |lua, _: MultiValue| {
                                unavailable::answer(lua, row, &caller)
                            })?,
                        )?;
                    }
                    Ok(())
                }
            }
        },
    )
}
