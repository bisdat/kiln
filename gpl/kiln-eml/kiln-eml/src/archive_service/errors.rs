// SPDX-License-Identifier: GPL-3.0-or-later
//! The per-call detailed-error channel of kiln.eml.archive v1: `KilnArchiveErrorV1` records, retained diagnostics
//! behind fresh tokens, `error_copy` and `error_release` (the archive table's metadata, for the metadata subset).
//!
//! A diagnostic is captured once, when the provider fails, and kept as owned bytes (`Arc`): a copy never repeats the
//! failing work, and a copy already in flight keeps its bytes while a release or the view's retirement removes the
//! entry. Tokens are process-unique, nonzero and never reused; a closed entry stays known (a tombstone) so release
//! stays idempotent and a closed token reads as REVOKED, not as unknown.
use std::{
    collections::BTreeMap,
    ffi::c_void,
    sync::{Arc, Mutex, MutexGuard},
};

use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_REVOKED, KILN_OK, KilnHandle, KilnOwner, KilnResult,
    services::kiln_eml_archive::KilnArchiveErrorV1,
};

const V1_SIZE: u32 = std::mem::size_of::<KilnArchiveErrorV1>() as u32;

struct Entry {
    owner: KilnOwner,
    view: u64,
    message: Option<Arc<[u8]>>,
}

struct Store {
    next: u64,
    entries: BTreeMap<u64, Entry>,
}

static STORE: Mutex<Store> = Mutex::new(Store {
    next: 1,
    entries: BTreeMap::new(),
});

fn store() -> MutexGuard<'static, Store> {
    STORE.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// A caller's error record, checked before any lookup or IO: NULL opts out; otherwise size at least v1 and zero
/// flags/reserved. `None` with `ok == false` means the record itself is invalid (nothing may be written to it).
pub(super) struct ErrorOut {
    record: *mut KilnArchiveErrorV1,
    size: u32,
}

impl ErrorOut {
    /// # Safety
    /// `record` is null or points to at least its own `size` bytes (the header's caller contract).
    pub(super) unsafe fn check(record: *mut KilnArchiveErrorV1) -> Result<Self, ()> {
        if record.is_null() {
            return Ok(Self { record, size: 0 });
        }
        let size = unsafe { std::ptr::addr_of!((*record).size).read_unaligned() };
        if size < V1_SIZE {
            return Err(());
        }
        let flags = unsafe { std::ptr::addr_of!((*record).flags).read_unaligned() };
        let reserved = unsafe { std::ptr::addr_of!((*record).reserved).read_unaligned() };
        if flags != 0 || reserved != 0 {
            return Err(());
        }
        Ok(Self { record, size })
    }

    /// The record's byte extent (empty for NULL), for the overlap check.
    pub(super) fn span(&self) -> (usize, usize) {
        (self.record as usize, self.size as usize)
    }

    /// Initializes the whole v1 record (keeping the caller's size and anything past v1) and returns `result`.
    pub(super) fn finish(&self, result: KilnResult, token: u64, message_bytes: u64) -> KilnResult {
        if !self.record.is_null() {
            let record = KilnArchiveErrorV1 {
                size: self.size,
                result,
                token,
                message_bytes,
                flags: 0,
                reserved: 0,
            };
            unsafe { self.record.write_unaligned(record) };
        }
        result
    }

    /// A genuine provider failure: with a record, `cause` is retained once behind a fresh token of `owner`'s.
    pub(super) fn fail(&self, result: KilnResult, owner: KilnOwner, view: u64, cause: Arc<[u8]>) -> KilnResult {
        if self.record.is_null() {
            return result;
        }
        let bytes = cause.len() as u64;
        let token = issue(owner, view, cause);
        self.finish(result, token, bytes)
    }
}

fn issue(owner: KilnOwner, view: u64, message: Arc<[u8]>) -> u64 {
    let mut store = store();
    let token = store.next;
    store.next += 1;
    store.entries.insert(
        token,
        Entry {
            owner,
            view,
            message: Some(message),
        },
    );
    token
}

/// The view's retirement (release, its owner's revocation, its generation's retirement) reclaims its diagnostics.
pub(super) fn reclaim_views(views: &[u64]) {
    if views.is_empty() {
        return;
    }
    let mut store = store();
    for entry in store.entries.values_mut() {
        if views.contains(&entry.view) {
            entry.message = None;
        }
    }
}

/// Whether the byte ranges overlap (empty ranges never do).
pub(super) fn overlap(spans: &[(usize, usize)]) -> bool {
    for (i, &(a, alen)) in spans.iter().enumerate() {
        for &(b, blen) in &spans[i + 1..] {
            if alen != 0 && blen != 0 && a < b.saturating_add(blen) && b < a.saturating_add(alen) {
                return true;
            }
        }
    }
    false
}

pub(super) unsafe extern "C" fn error_copy(
    owner: KilnOwner,
    token: KilnHandle,
    out: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> KilnResult {
    super::contained(|| {
        if required.is_null() || (out.is_null() && capacity != 0) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        if overlap(&[
            (out as usize, capacity),
            (required as usize, std::mem::size_of::<usize>()),
        ]) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        let message = {
            let store = store();
            let Some(entry) = store.entries.get(&token) else {
                return KILN_ERR_INVALID_ARGUMENT;
            };
            if entry.owner != owner {
                return KILN_ERR_REVOKED;
            }
            let Some(message) = entry.message.clone() else {
                return KILN_ERR_REVOKED;
            };
            message
        };
        // The bytes are pinned by this call's own reference; a release meanwhile does not take them away.
        unsafe { required.write_unaligned(message.len()) };
        if capacity < message.len() {
            return KILN_ERR_BUFFER_TOO_SMALL;
        }
        if !message.is_empty() {
            unsafe { std::ptr::copy_nonoverlapping(message.as_ptr(), out.cast::<u8>(), message.len()) };
        }
        KILN_OK
    })
}

pub(super) unsafe extern "C" fn error_release(owner: KilnOwner, token: KilnHandle) -> KilnResult {
    super::contained(|| {
        let mut store = store();
        let Some(entry) = store.entries.get_mut(&token) else {
            return KILN_ERR_INVALID_ARGUMENT;
        };
        if entry.owner != owner {
            return KILN_ERR_REVOKED;
        }
        entry.message = None;
        KILN_OK
    })
}
