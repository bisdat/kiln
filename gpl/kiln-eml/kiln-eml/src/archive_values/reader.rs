// SPDX-License-Identifier: GPL-3.0-or-later
//! The protected reader adapter: a host's `KilnEmlLuaReaderV1` (native/core/abi/services/kiln_eml_values.h) as
//! the graph's [`LuaHost`]. Only the header's neutral records and opaque object tokens cross here: no Lua state, mlua
//! value, kfc type or Rust error.
//!
//! # Admission
//! `arena_create` admits a reader once and keeps a snapshot of it. It reads `size` first and nothing past it; the
//! snapshot holds only the covered prefix (`min(size, 56)` bytes), so a truncated table never lends a pointer it does
//! not contain, not even for cleanup. Admitted: version 1, `user` covered, and `access`, `copy`, `retain` and `release`
//! covered and non-null (the graph cannot keep Source's semantics without any of them; refused INVALID_ARGUMENT).
//! `wake` is optional: without it no close ever signals the host, which then calls `arena_collect` itself.
//!
//! # Calls
//! Before every callback the snapshot is checked again (extent, version, non-null), so no path calls a member the
//! admitted table does not hold. Every call runs on the arena's creator worker except `wake`. A non-OK KilnResult is a
//! transport failure (the graph's root `internal` fault, never a Source error); a reply with a nonzero `error_object`
//! is the Lua error the request raised. A reply whose kind, adapter or lease disposition is not a header value is a
//! transport failure too (INTERNAL). `copy` is probed with a small buffer and repeated once at the exact size it asks
//! for; an object whose size changes between the two calls, or that does not fit the reply buffer it named, is refused.
//!
//! Every callback returns to its caller. It never unwinds or jumps out: a Lua error is caught by a protected call
//! inside the callback and answered as `error_object`, and a C++ exception or Rust panic is caught there too (the
//! function pointers are plain `extern "C"`). A longjmp or unwind across the provider's frames skips their cleanup
//! (borrow flags, scopes, the deferred drain) and is undefined behaviour the provider cannot contain. Pointers in a
//! request (`key`) and the request and reply records are valid only during the call.
//!
//! # wake
//! `wake(user, control)` gets the arena's control token (the one `arena_close`/`arena_collect` take). It runs on whichever
//! thread closed something off the worker, under the arena's internal lock, so it must only signal: it runs no Lua,
//! allocates nothing it must free, calls no kiln.eml.values member, and never waits for the worker (nor for a lock the
//! worker may hold while it calls a member, which would deadlock against `arena_collect`). A member called from inside
//! it is refused WRONG_THREAD (`in_wake`) instead of deadlocking on that lock.
use std::{
    cell::Cell,
    ffi::c_void,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_UNAVAILABLE, KILN_OK, KilnResult,
    KilnStr,
    services::kiln_eml_values::{
        KILN_LUA_DESCRIBE, KILN_LUA_GET_FIELD_STRING, KILN_LUA_GET_FIELD_VALUE, KILN_LUA_LENGTH,
        KILN_LUA_MAKE_TYPED_PROXY, KILN_LUA_RAW_SEQUENCE_INDEX, KILN_LUA_SOURCE_TO_STRING, KilnEmlLuaAccessV1,
        KilnEmlLuaReaderV1, KilnEmlLuaValueV1,
    },
};

use super::lifetime::{Access, Adapter, HostKind, HostValue, LeaseDisposition, LuaHost, Transport, Waker};

/// The reader table version this provider speaks.
const READER_VERSION: u32 = 1;
/// The v1 table's size (56 bytes on x64).
const READER_SIZE: usize = std::mem::size_of::<KilnEmlLuaReaderV1>();
/// `copy`'s first probe: most objects (field names, short strings, error texts) fit without a second call.
const PROBE: usize = 256;

thread_local! {
    /// Set while this thread runs a host's `wake`.
    static IN_WAKE: Cell<bool> = const { Cell::new(false) };
}

/// Whether this thread is inside a host's `wake` (every C member refuses then).
pub(crate) fn in_wake() -> bool {
    IN_WAKE.try_with(Cell::get).unwrap_or(false)
}

/// Whether `covered` bytes hold the pointer-sized member at `offset`.
fn covers(covered: usize, offset: usize) -> bool {
    covered >= offset + std::mem::size_of::<usize>()
}

/// An admitted reader: the covered prefix of the host's table, zero past it.
pub(crate) struct CReader {
    table: KilnEmlLuaReaderV1,
    covered: usize,
}

macro_rules! member {
    ($self:expr, $name:ident) => {{
        let offset = std::mem::offset_of!(KilnEmlLuaReaderV1, $name);
        if $self.table.version == READER_VERSION && covers($self.covered, offset) {
            $self.table.$name
        } else {
            None
        }
    }};
}

impl CReader {
    /// Admits `reader` (see the module docs); INVALID_ARGUMENT otherwise. Reads `size` first and never past it.
    ///
    /// # Safety
    /// `reader` must be null or point to at least the `size` bytes its first field states.
    pub(crate) unsafe fn admit(reader: *const KilnEmlLuaReaderV1) -> Result<Self, KilnResult> {
        if reader.is_null() {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        let size = unsafe { std::ptr::addr_of!((*reader).size).read_unaligned() } as usize;
        let user_end = std::mem::offset_of!(KilnEmlLuaReaderV1, user) + std::mem::size_of::<usize>();
        if size < user_end {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        let covered = size.min(READER_SIZE);
        let mut table = KilnEmlLuaReaderV1 {
            size: 0,
            version: 0,
            user: std::ptr::null_mut(),
            access: None,
            copy: None,
            retain: None,
            release: None,
            wake: None,
        };
        // the covered prefix only, byte for byte: a member past `size` stays None
        unsafe {
            std::ptr::copy_nonoverlapping(reader.cast::<u8>(), std::ptr::addr_of_mut!(table).cast::<u8>(), covered)
        };
        let admitted = Self { table, covered };
        if admitted.table.version != READER_VERSION
            || member!(admitted, access).is_none()
            || member!(admitted, copy).is_none()
            || member!(admitted, retain).is_none()
            || member!(admitted, release).is_none()
        {
            return Err(KILN_ERR_INVALID_ARGUMENT);
        }
        Ok(admitted)
    }

    /// The host's `wake` as the arena's waker, signalling `control` (set once `arena_create` has the token); None when
    /// the admitted table has no `wake`.
    pub(crate) fn waker(&self, control: Arc<AtomicU64>) -> Option<Waker> {
        let wake = member!(self, wake)?;
        let user = self.table.user as usize;
        Some(Arc::new(move || {
            let outer = IN_WAKE.try_with(|flag| flag.replace(true)).unwrap_or(false);
            unsafe { wake(user as *mut c_void, control.load(Ordering::SeqCst)) };
            let _ = IN_WAKE.try_with(|flag| flag.set(outer));
        }))
    }

    fn reply(value: &KilnEmlLuaValueV1) -> Result<HostValue, Transport> {
        if value.error_object != 0 {
            return Ok(HostValue::error(value.error_object));
        }
        let kind = match value.kind {
            0 => HostKind::Nil,
            1 => HostKind::Boolean,
            2 => HostKind::Integer,
            3 => HostKind::Number,
            4 => HostKind::String,
            5 => HostKind::Table,
            6 => HostKind::TypedValue,
            7 => HostKind::KnownUserdata,
            8 => HostKind::OtherUserdata,
            9 => HostKind::Other,
            _ => return Err(Transport(KILN_ERR_INTERNAL)),
        };
        let adapter = match value.adapter_class {
            0 => Adapter::None,
            1 => Adapter::Guid,
            2 => Adapter::Content,
            3 => Adapter::Resource,
            _ => return Err(Transport(KILN_ERR_INTERNAL)),
        };
        let lease = match value.lease_disposition {
            0 => LeaseDisposition::None,
            1 => LeaseDisposition::Adopted,
            2 => LeaseDisposition::Reused,
            _ => return Err(Transport(KILN_ERR_INTERNAL)),
        };
        Ok(HostValue {
            kind,
            object: value.object,
            bits: value.bits,
            byte_length: value.byte_length,
            typed_value: value.typed_value,
            error_object: 0,
            adapter,
            lease,
        })
    }

    /// One `copy` call into `out`: OK with the size written, BUFFER_TOO_SMALL with the size needed, or the host's
    /// refusal.
    fn copy_into(&self, object: u64, out: &mut [u8]) -> Result<Result<usize, usize>, Transport> {
        let copy = member!(self, copy).ok_or(Transport(KILN_ERR_UNAVAILABLE))?;
        let mut required = usize::MAX;
        let rc = unsafe {
            copy(
                self.table.user,
                object,
                out.as_mut_ptr().cast(),
                out.len(),
                &mut required,
            )
        };
        match rc {
            KILN_OK if required <= out.len() => Ok(Ok(required)),
            KILN_ERR_BUFFER_TOO_SMALL if required > out.len() && required != usize::MAX => Ok(Err(required)),
            KILN_OK | KILN_ERR_BUFFER_TOO_SMALL => Err(Transport(KILN_ERR_INTERNAL)),
            refused => Err(Transport(refused)),
        }
    }
}

impl LuaHost for CReader {
    fn access(&self, access: &Access<'_>) -> Result<HostValue, Transport> {
        let call = member!(self, access).ok_or(Transport(KILN_ERR_UNAVAILABLE))?;
        let empty = KilnStr {
            ptr: std::ptr::null(),
            len: 0,
        };
        let key = |key: &[u8]| KilnStr {
            ptr: key.as_ptr().cast(),
            len: key.len(),
        };
        let (operation, object, index, key, arena, node_id) = match *access {
            Access::Describe { object } => (KILN_LUA_DESCRIBE, object, 0, empty, 0, 0),
            Access::GetFieldValue { object, key: k } => (KILN_LUA_GET_FIELD_VALUE, object, 0, key(k), 0, 0),
            Access::GetFieldString { object, key: k } => (KILN_LUA_GET_FIELD_STRING, object, 0, key(k), 0, 0),
            Access::Length { object } => (KILN_LUA_LENGTH, object, 0, empty, 0, 0),
            Access::RawSequenceIndex { object, index } => (KILN_LUA_RAW_SEQUENCE_INDEX, object, index, empty, 0, 0),
            Access::SourceToString { object } => (KILN_LUA_SOURCE_TO_STRING, object, 0, empty, 0, 0),
            Access::MakeTypedProxy {
                provisional,
                arena,
                node_id,
            } => (KILN_LUA_MAKE_TYPED_PROXY, provisional, 0, empty, arena, node_id),
        };
        let request = KilnEmlLuaAccessV1 {
            size: std::mem::size_of::<KilnEmlLuaAccessV1>() as u32,
            operation: operation as u32,
            object,
            index,
            key,
            arena,
            node_id,
        };
        let mut value = KilnEmlLuaValueV1 {
            size: std::mem::size_of::<KilnEmlLuaValueV1>() as u32,
            kind: 0,
            object: 0,
            bits: 0,
            byte_length: 0,
            typed_value: 0,
            error_object: 0,
            adapter_class: 0,
            lease_disposition: 0,
        };
        let rc = unsafe { call(self.table.user, &request, &mut value) };
        if rc != KILN_OK {
            return Err(Transport(rc));
        }
        Self::reply(&value)
    }

    fn copy(&self, object: u64) -> Result<Vec<u8>, Transport> {
        let mut probe = [0u8; PROBE];
        let required = match self.copy_into(object, &mut probe)? {
            Ok(written) => return Ok(probe[..written].to_vec()),
            Err(required) => required,
        };
        let mut out = Vec::new();
        out.try_reserve_exact(required)
            .map_err(|_| Transport(KILN_ERR_INTERNAL))?;
        out.resize(required, 0);
        match self.copy_into(object, &mut out)? {
            Ok(written) if written == required => Ok(out),
            _ => Err(Transport(KILN_ERR_INTERNAL)),
        }
    }

    fn retain(&self, object: u64) -> Result<(), Transport> {
        let retain = member!(self, retain).ok_or(Transport(KILN_ERR_UNAVAILABLE))?;
        match unsafe { retain(self.table.user, object) } {
            KILN_OK => Ok(()),
            refused => Err(Transport(refused)),
        }
    }

    fn release(&self, object: u64) {
        if let Some(release) = member!(self, release) {
            unsafe { release(self.table.user, object) };
        }
    }
}
