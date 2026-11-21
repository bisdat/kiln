// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.values v1's C table (native/core/abi/services/kiln_eml_values.h), the GPL provider: the seventeen
//! members over the typed graph, published as `kiln.eml.values` next to `kiln.eml.archive` with its capability rows.
//! There is no early export: normal service lookup only, and the startup pass uses the graph through its Rust API.
//!
//! # Who owns what
//! - GPL (this crate): the graph, the type walk, validation, caches, conversion, packets and every token. kfc types,
//!   graph nodes and Rust errors stay on this side.
//! - Neutral (the MIT header): sized records (`KilnEmlValue*V1`, `KilnEmlLua*V1`), opaque u64 tokens, counted bytes
//!   and the frozen JSON packets of schema/sfi/eml-values/. Nothing else crosses: no Lua state, mlua value, pointer
//!   identity or panic.
//! - MIT (kiln-sfi's frontend, the MIT frontend): the Lua VM, the proxies (one userdata per node, weakly interned) and the
//!   protected reader the graph reads Lua objects through ([`super::reader`]).
//!
//! # Records
//! Every record a member reads or writes is checked first: non-null and `size` at least its v1 size (open 88, info 248,
//! selector 32, result 48); a smaller one is INVALID_ARGUMENT with no effect and no reader call. A larger one is
//! accepted: its `size` is kept and the bytes past v1 are left untouched. A valid result record is cleared (every v1
//! field 0, `size` kept) before any work, so a refused call (inside a host's `wake` too) never leaves a token in it;
//! `arena_create` writes 0 to its token output first for the same reason. A `KilnStr` or byte range with a null
//! pointer is empty only with length 0, and a count above `isize::MAX` (no object is that large) is INVALID_ARGUMENT
//! before any slice over the caller's memory exists. Owners are attribution only (any value, 0 included), except
//! `arena_create`'s host owner, which must name someone (nonzero).
//!
//! # Threads and reentry
//! - Worker only (the thread that called `arena_create`): `arena_collect`, `value_open`, `value_info`, `value_read`,
//!   `value_child`, `value_set`, `value_clone`, `value_export`, `value_result_copy` and the array members. Elsewhere
//!   WRONG_THREAD, before any reader call.
//! - Any thread, non-blocking, never Lua: `arena_close` and `value_close`. Off the worker they queue and call the
//!   reader's `wake(user, control)`.
//! - Reader callbacks run on the worker inside a member. From inside one, a host may call `value_close` (its finalizers;
//!   the close waits for the outermost call's end) and identity-only `value_info` (MAKE_TYPED_PROXY), and other
//!   members on other nodes as Source allows; a conflicting borrow of the same node is a SEMANTIC_ERROR, never a lock.
//! - `wake` runs under the arena's internal lock: every member called from inside it is refused WRONG_THREAD (its outputs
//!   cleared first).
//! - Reader callbacks return; they never unwind or jump out (a Lua error is caught by a protected call and answered as
//!   `error_object`, a C++ exception or Rust panic is caught inside the callback). An unwind or longjmp across a member's
//!   frames is undefined behaviour no provider can contain.
//!
//! # Errors
//! A KilnResult is a transport answer: NOT_FOUND (a token never issued), REVOKED (a closed lease, arena or view),
//! WRONG_THREAD, INVALID_ARGUMENT (a malformed record or a wrong-kind token), UNAVAILABLE (see `arena_create`, or an
//! origin this generation cannot serve), BUFFER_TOO_SMALL (`value_result_copy` only) and INTERNAL (a contained panic or
//! provider fault). A Source error is KILN_OK with disposition SEMANTIC_ERROR and an error lease whose packet is the
//! error tree. No panic, Lua error or C++ exception crosses: every member runs under `catch_unwind`.
//!
//! # arena_create
//! The archive view resolves to its authorized generation (unknown INVALID_ARGUMENT, closed REVOKED). The generation's
//! kfc registry (kept beside its snapshot, `authorize_with_registry`) is copied once for the arena, on the worker, and
//! the copy's registry digest must equal the view's `registry_sha256`; otherwise, or when the generation keeps no
//! registry, UNAVAILABLE. The view's `registry_id` is bound by construction, not compared: it is the id of the snapshot
//! stored in the same generation entry as that registry, so the content digest is the one independent check. No other
//! registry is ever used (not `kiln.types`, not a rebuild from the JSON snapshot). `layout_revision` is `registry_id`.
//! The reader is admitted as [`super::reader`] says. RESOURCE roots read the generation's archive (authorized with
//! `authorize_with_archive`) as find_resource/read_resource do, the transaction first, then the reference catalog; a
//! missing resource's root is nil. Over a generation authorized without its archive they answer UNAVAILABLE.
//!
//! # Lifetime duties of the host
//! - Close and drain every arena (`arena_close`, then `arena_collect` until pending is 0) before destroying the VM and
//!   before the worker exits; keep the reader's `user` valid until then. A worker that exits with an arena open
//!   revokes it without calling the host (`super::arena`, "Worker exit").
//! - Keep a LOADED_SNAPSHOT root's userdata on the frontend's ResourceInfo: the graph records that root weakly.
//! - Answer SOURCE_TO_STRING with mlua 0.11.6's `Value::to_string` of the object.
use std::{ffi::c_void, panic::AssertUnwindSafe, rc::Rc, sync::Arc, sync::atomic::AtomicU64};

use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_UNAVAILABLE,
    KILN_ERR_WRONG_THREAD, KILN_OK, KilnHandle, KilnOwner, KilnResult, KilnServiceHeader, KilnStr,
    services::{
        kiln_caps::{KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, KilnCapsApi},
        kiln_eml_archive::KilnEmlArchiveView,
        kiln_eml_values::{
            KILN_EML_VALUES_VERSION, KILN_VALUE_FIELD, KILN_VALUE_INDEX, KILN_VALUE_RUNTIME, KILN_VALUE_STARTUP,
            KilnEmlLuaReaderV1, KilnEmlValueInfoV1, KilnEmlValueOpenV1, KilnEmlValueResultV1, KilnEmlValueSelectorV1,
            KilnEmlValuesApiV1,
        },
    },
};

use super::{
    arena::{ArenaConfig, ArenaPins},
    lifetime::Refusal,
    operations::{self, OpenRequest, Reply, Selector},
    reader::{CReader, in_wake},
};
use crate::archive_service::{read::ArenaResources, registry, view};

/// The service name the table is published under.
pub const SERVICE: &str = "kiln.eml.values";

static TABLE: KilnEmlValuesApiV1 = KilnEmlValuesApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlValuesApiV1>() as u32,
        version: KILN_EML_VALUES_VERSION,
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

/// The process-lifetime table (the one `publish` registers).
pub fn table() -> &'static KilnEmlValuesApiV1 {
    &TABLE
}

/// The capability group and its members, all implemented.
const GROUP: &str = "eml.values";
const MEMBERS: [&str; 17] = [
    "arena_create",
    "arena_close",
    "arena_collect",
    "value_open",
    "value_info",
    "value_read",
    "value_child",
    "value_set",
    "value_clone",
    "value_export",
    "value_close",
    "value_result_copy",
    "value_array_push",
    "value_array_insert",
    "value_array_pop",
    "value_array_remove",
    "value_array_clear",
];

fn kstr(text: &str) -> KilnStr {
    KilnStr {
        ptr: text.as_ptr().cast(),
        len: text.len(),
    }
}

/// Publishes the table as `kiln.eml.values` through `register` (KilnApi.register_service for `owner`) and, once that
/// succeeded, reports `eml.values` and each of its seventeen members AVAILABLE through `caps` (kiln.caps, when present).
/// Static support only: an arena still needs an authorized generation that keeps its registry.
pub fn publish(
    owner: KilnOwner,
    register: impl FnOnce(&str, *const c_void) -> KilnResult,
    caps: Option<&KilnCapsApi>,
) -> KilnResult {
    let rc = register(SERVICE, (&TABLE as *const KilnEmlValuesApiV1).cast());
    if rc != KILN_OK {
        return rc;
    }
    let Some(report) = caps.and_then(|caps| {
        let reaches =
            caps.header.size as usize >= std::mem::offset_of!(KilnCapsApi, report) + std::mem::size_of::<usize>();
        if caps.header.version >= 1 && reaches {
            caps.report
        } else {
            None
        }
    }) else {
        return KILN_OK;
    };
    let available = |name: &str| unsafe {
        report(
            owner,
            kstr(name),
            KILN_CAP_AVAILABLE as u32,
            KILN_CAP_REASON_NONE as u32,
            kstr(""),
        )
    };
    let _ = available(GROUP);
    for member in MEMBERS {
        let _ = available(&format!("{GROUP}.{member}"));
    }
    KILN_OK
}

/// Every member's frame: refused inside a host's `wake` (it runs under the arena's lock), and a panic is INTERNAL. A
/// member with outputs clears them before this check (`with_out`, `arena_create`).
fn boundary(work: impl FnOnce() -> KilnResult) -> KilnResult {
    std::panic::catch_unwind(AssertUnwindSafe(|| {
        if in_wake() {
            return KILN_ERR_WRONG_THREAD;
        }
        work()
    }))
    .unwrap_or(KILN_ERR_INTERNAL)
}

/// The `size` a caller's record states, when it is non-null and at least `T`'s v1 size.
unsafe fn sized<T>(record: *const T) -> Option<u32> {
    if record.is_null() {
        return None;
    }
    let size = unsafe { record.cast::<u32>().read_unaligned() };
    (size as usize >= std::mem::size_of::<T>()).then_some(size)
}

/// A caller's counted bytes; a null pointer only with length 0, and never more than `isize::MAX` bytes (no object is
/// that large: the count is refused before a slice claims it).
unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> Result<&'a [u8], KilnResult> {
    match (ptr.is_null(), len) {
        (_, 0) => Ok(&[]),
        (_, len) if len > isize::MAX as usize => Err(KILN_ERR_INVALID_ARGUMENT),
        (true, _) => Err(KILN_ERR_INVALID_ARGUMENT),
        (false, _) => Ok(unsafe { std::slice::from_raw_parts(ptr, len) }),
    }
}

unsafe fn text<'a>(text: KilnStr) -> Result<&'a [u8], KilnResult> {
    unsafe { bytes(text.ptr.cast(), text.len) }
}

/// A checked result record, cleared before the work.
struct Out {
    record: *mut KilnEmlValueResultV1,
    size: u32,
}

impl Out {
    unsafe fn begin(record: *mut KilnEmlValueResultV1) -> Result<Self, KilnResult> {
        let size = unsafe { sized(record) }.ok_or(KILN_ERR_INVALID_ARGUMENT)?;
        let out = Self { record, size };
        out.write(&Reply::default());
        Ok(out)
    }

    fn write(&self, reply: &Reply) {
        let mut record = reply.record();
        record.size = self.size;
        unsafe { self.record.write_unaligned(record) };
    }

    fn finish(self, reply: Result<Reply, Refusal>) -> KilnResult {
        match reply {
            Ok(reply) => {
                self.write(&reply);
                KILN_OK
            }
            Err(refusal) => refusal.code(),
        }
    }
}

/// Runs a member that fills a result record: the record is checked and cleared first, also when the call is then
/// refused inside a host's `wake`.
fn with_out(
    record: *mut KilnEmlValueResultV1,
    work: impl FnOnce() -> Result<Result<Reply, Refusal>, KilnResult>,
) -> KilnResult {
    std::panic::catch_unwind(AssertUnwindSafe(|| {
        let out = match unsafe { Out::begin(record) } {
            Ok(out) => out,
            Err(rc) => return rc,
        };
        boundary(|| match work() {
            Ok(reply) => out.finish(reply),
            Err(rc) => rc,
        })
    }))
    .unwrap_or(KILN_ERR_INTERNAL)
}

unsafe fn selector<'a>(record: *const KilnEmlValueSelectorV1) -> Result<Selector<'a>, KilnResult> {
    unsafe { sized(record) }.ok_or(KILN_ERR_INVALID_ARGUMENT)?;
    let selector = unsafe { record.read_unaligned() };
    match selector.kind as i32 {
        KILN_VALUE_FIELD => Ok(Selector::Field(unsafe { text(selector.field) }?)),
        KILN_VALUE_INDEX => Ok(Selector::Index(selector.index)),
        _ => Err(KILN_ERR_INVALID_ARGUMENT),
    }
}

// ---- arenas ----

unsafe extern "C" fn arena_create(
    host_owner: KilnOwner,
    view: KilnEmlArchiveView,
    lua_phase: u32,
    reader: *const KilnEmlLuaReaderV1,
    arena: *mut KilnHandle,
) -> KilnResult {
    // the token output first: no refusal (one inside a host's `wake` included) leaves a value in it
    if !arena.is_null() {
        unsafe { arena.write_unaligned(0) };
    }
    boundary(|| {
        if arena.is_null() {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        if host_owner == 0 || (lua_phase != KILN_VALUE_STARTUP as u32 && lua_phase != KILN_VALUE_RUNTIME as u32) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let reader = match unsafe { CReader::admit(reader) } {
            Ok(reader) => reader,
            Err(rc) => return rc,
        };
        let source = match view::registry_source(view) {
            Ok(source) => source,
            Err(rc) => return rc,
        };
        let Some(shared) = source.registry else {
            return KILN_ERR_UNAVAILABLE;
        };
        // the arena's own copy, made once here on its worker; checked before any value exists. Only the content can
        // differ: pins.registry_id is the id of the snapshot kept in the same generation entry as this registry.
        let copy = Rc::new((*shared).clone());
        if registry::digest(&copy, &source.game_version) != source.pins.registry_sha256 {
            return KILN_ERR_UNAVAILABLE;
        }
        // RESOURCE roots: the generation's archive, when it was authorized with one
        let resources = source
            .archive
            .map(|archive| Rc::new(ArenaResources::new(archive, shared)) as Rc<dyn super::arena::ResourceSource>);
        let control = Arc::new(AtomicU64::new(0));
        let waker = reader.waker(control.clone());
        let created = super::arena::arena_create(
            host_owner,
            ArenaConfig {
                registry: copy,
                pins: ArenaPins {
                    archive: source.pins,
                    layout_revision: source.pins.registry_id,
                },
                phase: lua_phase,
                host: Rc::new(reader),
                waker,
                resources,
            },
        );
        match created {
            Ok(token) => {
                control.store(token, std::sync::atomic::Ordering::SeqCst);
                unsafe { arena.write_unaligned(token) };
                KILN_OK
            }
            Err(refusal) => refusal.code(),
        }
    })
}

unsafe extern "C" fn arena_close(host_owner: KilnOwner, arena: KilnHandle, pending: *mut u32) -> KilnResult {
    boundary(|| match super::arena::arena_close(host_owner, arena) {
        Ok(count) => {
            if !pending.is_null() {
                unsafe { pending.write_unaligned(count) };
            }
            KILN_OK
        }
        Err(refusal) => refusal.code(),
    })
}

unsafe extern "C" fn arena_collect(host_owner: KilnOwner, arena: KilnHandle, pending: *mut u32) -> KilnResult {
    boundary(|| match super::arena::arena_collect(host_owner, arena) {
        Ok(count) => {
            if !pending.is_null() {
                unsafe { pending.write_unaligned(count) };
            }
            KILN_OK
        }
        Err(refusal) => refusal.code(),
    })
}

// ---- values ----

unsafe extern "C" fn value_open(
    owner: KilnOwner,
    arena: KilnHandle,
    owner_view: KilnEmlArchiveView,
    open: *const KilnEmlValueOpenV1,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || {
        unsafe { sized(open) }.ok_or(KILN_ERR_INVALID_ARGUMENT)?;
        let open = unsafe { open.read_unaligned() };
        let qualified_type = unsafe { text(open.qualified_type) }?;
        let source = unsafe { bytes(open.bytes.cast(), open.length) }?;
        let pins = view::view_pins(owner_view)?;
        let request = OpenRequest {
            origin: open.origin,
            qualified_type,
            key: open.key,
            source_instance: open.source_instance,
            source_session: open.source_session,
            bytes: source,
            lua_object: open.lua_object,
        };
        Ok(operations::value_open(owner, arena, &pins, &request))
    })
}

unsafe extern "C" fn value_info(owner: KilnOwner, value: KilnHandle, info: *mut KilnEmlValueInfoV1) -> KilnResult {
    boundary(|| {
        let Some(size) = (unsafe { sized(info) }) else {
            return KILN_ERR_INVALID_ARGUMENT;
        };
        let requested = unsafe { std::ptr::addr_of!((*info).requested).read_unaligned() };
        match operations::value_info(owner, value, requested) {
            Ok(mut answer) => {
                answer.size = size;
                unsafe { info.write_unaligned(answer) };
                KILN_OK
            }
            Err(refusal) => refusal.code(),
        }
    })
}

unsafe extern "C" fn value_read(
    owner: KilnOwner,
    value: KilnHandle,
    query: u32,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || Ok(operations::value_read(owner, value, query)))
}

unsafe extern "C" fn value_child(
    owner: KilnOwner,
    value: KilnHandle,
    selector: *const KilnEmlValueSelectorV1,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || {
        let selector = unsafe { self::selector(selector) }?;
        Ok(operations::value_child(owner, value, &selector))
    })
}

unsafe extern "C" fn value_set(
    owner: KilnOwner,
    value: KilnHandle,
    selector: *const KilnEmlValueSelectorV1,
    lua_object: u64,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || {
        let selector = unsafe { self::selector(selector) }?;
        Ok(operations::value_set(owner, value, &selector, lua_object))
    })
}

unsafe extern "C" fn value_clone(owner: KilnOwner, value: KilnHandle, result: *mut KilnEmlValueResultV1) -> KilnResult {
    with_out(result, || Ok(operations::value_clone(owner, value)))
}

unsafe extern "C" fn value_export(
    owner: KilnOwner,
    value: KilnHandle,
    selected_type: KilnStr,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || {
        let selected = unsafe { text(selected_type) }?;
        Ok(operations::value_export(owner, value, selected))
    })
}

unsafe extern "C" fn value_close(owner: KilnOwner, value: KilnHandle) -> KilnResult {
    boundary(|| match super::arena::value_close(owner, value) {
        Ok(()) => KILN_OK,
        Err(refusal) => refusal.code(),
    })
}

unsafe extern "C" fn value_result_copy(
    owner: KilnOwner,
    result: KilnHandle,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    boundary(|| {
        if required.is_null() || (out.is_null() && capacity != 0) || capacity > isize::MAX as usize {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let (start, end) = (out as usize, (out as usize).saturating_add(capacity));
        let (count, count_end) = (
            required as usize,
            (required as usize).saturating_add(std::mem::size_of::<usize>()),
        );
        if capacity != 0 && start < count_end && count < end {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let buffer: &mut [u8] = if capacity == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(out.cast::<u8>(), capacity) }
        };
        match operations::value_result_copy(owner, result, buffer) {
            Ok(needed) => {
                unsafe { required.write_unaligned(needed) };
                if capacity < needed {
                    KILN_ERR_BUFFER_TOO_SMALL
                } else {
                    KILN_OK
                }
            }
            Err(refusal) => refusal.code(),
        }
    })
}

// ---- arrays ----

unsafe extern "C" fn value_array_push(
    owner: KilnOwner,
    array: KilnHandle,
    lua_object: u64,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || Ok(operations::value_array_push(owner, array, lua_object)))
}

unsafe extern "C" fn value_array_insert(
    owner: KilnOwner,
    array: KilnHandle,
    index: u64,
    lua_object: u64,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || {
        Ok(operations::value_array_insert(owner, array, index, lua_object))
    })
}

unsafe extern "C" fn value_array_pop(
    owner: KilnOwner,
    array: KilnHandle,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || Ok(operations::value_array_pop(owner, array)))
}

unsafe extern "C" fn value_array_remove(
    owner: KilnOwner,
    array: KilnHandle,
    index: u64,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || Ok(operations::value_array_remove(owner, array, index)))
}

unsafe extern "C" fn value_array_clear(
    owner: KilnOwner,
    array: KilnHandle,
    result: *mut KilnEmlValueResultV1,
) -> KilnResult {
    with_out(result, || Ok(operations::value_array_clear(owner, array)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_never_leaves_a_member() {
        assert_eq!(boundary(|| panic!("contained")), KILN_ERR_INTERNAL);
        assert_eq!(boundary(|| KILN_OK), KILN_OK);
    }

    #[test]
    fn the_table_is_the_headers() {
        let table = table();
        assert_eq!((table.header.size, table.header.version), (144, 1));
        assert_eq!(MEMBERS.len(), 17);
    }
}
