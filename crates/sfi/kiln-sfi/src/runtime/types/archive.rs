// SPDX-License-Identifier: MIT
//! [`TypeRegistryPort`] on `kiln.eml.archive` v1
//! (`native/core/abi/services/kiln_eml_archive.h`), through the runtime's service adapter.
//!
//! [`ArchiveRegistryPort`] opens its view at the first page, on the worker thread:
//! 1. the table, bound as `KILN_EML_ARCHIVE_SERVICE` v1, and its `acquire`, `identity`,
//!    `release` and `registry_metadata` slots (extent and non-null, [`service_member!`]);
//! 2. the kiln.caps rows `eml.archive.metadata` and its four member rows, each AVAILABLE:
//!    a held, off or absent row refuses use ([`PortError::Withheld`]);
//! 3. `acquire` with the launch identity this start was granted (every pin, no wildcard);
//! 4. `identity` of the granted view, which must equal that identity in full: the view is
//!    released and the read refused otherwise ([`PortError::IdentityMismatch`]). The same
//!    identity is [`TypeRegistryPort::expected_pin`], so every page's pin is checked too.
//!
//! Each page is two calls of `registry_metadata`: a sizing call with no buffer, whose
//! `BUFFER_TOO_SMALL` gives the complete length in `required`, then the read into exactly
//! that many bytes. A page that does not fit then, or a non-OK result, is
//! [`PortError::Failed`] with the member and its result. Its message is the provider's own
//! text: when `eml.archive.errors` and its two member rows are available and both slots
//! are there, each call passes a `KilnArchiveErrorV1` record and a failure's token is
//! copied with `error_copy` (sized the same way) and released with `error_release`
//! exactly once, whether the copy worked or not. Without that channel the record is NULL
//! (the header's opt-out) and no token is ever issued.
//!
//! The view is released when the port is dropped. Direct calls only: no hop, no
//! wait, no retry; a failed open is remembered and answered again, never tried twice.
//!
//! [`GrantedView`] is steps 2 to 4 on their own (the table's `acquire`, `identity` and `release`
//! slots, the same kiln.caps rows, then the checked view): a view of the granted generation,
//! released once when it drops. The typed values hold one per Lua VM for that VM's typed-value arena
//! (kiln.eml.values), which every `value_open` passes the view to.

use super::port::{PortError, RegistryPin, TypeRegistryPort};
use crate::runtime::services::{Member, ServiceAdapter, ServiceTable};
use crate::service_member;
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_eml_archive::*;
use kiln_abi::*;
use std::cell::RefCell;
use std::ffi::{c_char, c_void};
use std::rc::Rc;

type AcquireFn = unsafe extern "C" fn(
    KilnOwner,
    *const KilnEmlArchiveIdentityV1,
    *mut KilnEmlArchiveView,
) -> KilnResult;
type IdentityFn =
    unsafe extern "C" fn(KilnEmlArchiveView, *mut KilnEmlArchiveIdentityV1) -> KilnResult;
type ReleaseFn = unsafe extern "C" fn(KilnEmlArchiveView) -> KilnResult;
type MetadataFn = unsafe extern "C" fn(
    KilnEmlArchiveView,
    u64,
    u32,
    *mut c_char,
    usize,
    *mut usize,
    *mut KilnArchiveErrorV1,
) -> KilnResult;
type ErrorCopyFn =
    unsafe extern "C" fn(KilnOwner, KilnHandle, *mut c_void, usize, *mut usize) -> KilnResult;
type ErrorReleaseFn = unsafe extern "C" fn(KilnOwner, KilnHandle) -> KilnResult;

/// The capability group the registry read needs, and its member rows.
pub const METADATA_ROWS: [&str; 5] = [
    "eml.archive.metadata",
    "eml.archive.metadata.acquire",
    "eml.archive.metadata.identity",
    "eml.archive.metadata.release",
    "eml.archive.metadata.registry_metadata",
];
/// The precise-error channel's group and member rows.
pub const ERRORS_ROWS: [&str; 3] = [
    "eml.archive.errors",
    "eml.archive.errors.error_copy",
    "eml.archive.errors.error_release",
];
/// The largest page this reader allocates (a page holds at most 256 types).
pub const PAGE_MAX: usize = 256 << 20;
/// The largest error text copied.
pub const MESSAGE_MAX: usize = 1 << 20;

struct ErrorChannel {
    copy: Member<ErrorCopyFn>,
    release: Member<ErrorReleaseFn>,
}

struct Open {
    view: GrantedView,
    registry_metadata: Member<MetadataFn>,
    errors: Option<ErrorChannel>,
}

/// The three members a granted view needs.
#[derive(Clone, Copy)]
struct ViewMembers {
    acquire: Member<AcquireFn>,
    identity: Member<IdentityFn>,
    release: Member<ReleaseFn>,
}

impl ViewMembers {
    fn bind(table: &ServiceTable<KilnEmlArchiveApiV1>) -> Result<Self, PortError> {
        Ok(Self {
            acquire: service_member!(*table, KilnEmlArchiveApiV1, acquire)
                .map_err(PortError::Unavailable)?,
            identity: service_member!(*table, KilnEmlArchiveApiV1, identity)
                .map_err(PortError::Unavailable)?,
            release: service_member!(*table, KilnEmlArchiveApiV1, release)
                .map_err(PortError::Unavailable)?,
        })
    }
}

/// A view of the archive generation granted to this start: `acquire` with the granted identity
/// under an owner, then `identity` of the view, which must equal it in every pin (the record
/// sizes aside). Released exactly once, when it drops, unless it is [`kept`](Self::keep).
pub struct GrantedView {
    services: Rc<ServiceAdapter>,
    view: KilnEmlArchiveView,
    release: Member<ReleaseFn>,
}

impl GrantedView {
    /// Binds kiln.eml.archive v1 through `services`, checks the kiln.caps rows
    /// `eml.archive.metadata` and its member rows (each AVAILABLE, as the header asks before each
    /// use: a held, off or absent row refuses with [`PortError::Withheld`] and nothing is
    /// acquired), and acquires a view of the generation `expected` pins, under `owner`
    /// (kiln-sfi's plugin owner). A view whose identity differs is released at once and refused
    /// ([`PortError::IdentityMismatch`]).
    pub fn acquire(
        services: &Rc<ServiceAdapter>,
        owner: KilnOwner,
        expected: &KilnEmlArchiveIdentityV1,
    ) -> Result<Self, PortError> {
        let table = services
            .bind::<KilnEmlArchiveApiV1>(KILN_EML_ARCHIVE_SERVICE, KILN_EML_ARCHIVE_VERSION)
            .map_err(PortError::Unavailable)?;
        let members = ViewMembers::bind(&table)?;
        let caps = Caps::bind(services)?;
        for row in METADATA_ROWS {
            caps.require(row)?;
        }
        Self::acquire_with(services, members, owner, expected)
    }

    fn acquire_with(
        services: &Rc<ServiceAdapter>,
        members: ViewMembers,
        owner: KilnOwner,
        expected: &KilnEmlArchiveIdentityV1,
    ) -> Result<Self, PortError> {
        let mut expected = *expected;
        expected.size = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
        let mut view: KilnEmlArchiveView = 0;
        // SAFETY: a sized identity record and a live out-pointer (kiln_eml_archive.h).
        let result =
            unsafe { services.call(members.acquire, (owner, &raw const expected, &raw mut view)) };
        if result != KILN_OK {
            return Err(PortError::Failed {
                member: "acquire",
                result,
                message: None,
            });
        }
        if view == 0 {
            return Err(PortError::Failed {
                member: "acquire",
                result: KILN_ERR_INTERNAL,
                message: Some("the archive granted view 0".into()),
            });
        }
        // From here the view is released when this drops (also on the refusals below).
        let granted_view = Self {
            services: services.clone(),
            view,
            release: members.release,
        };
        // SAFETY: an all-zero identity record is valid (integers and byte arrays).
        let mut granted: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
        granted.size = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
        // SAFETY: a view acquire issued and a sized caller record.
        let result = unsafe { services.call(members.identity, (view, &raw mut granted)) };
        if result != KILN_OK {
            return Err(PortError::Failed {
                member: "identity",
                result,
                message: None,
            });
        }
        if !same_identity(&granted, &expected) {
            return Err(PortError::IdentityMismatch);
        }
        Ok(granted_view)
    }

    /// The view token.
    pub fn view(&self) -> KilnEmlArchiveView {
        self.view
    }

    /// Never releases the view (a provider that may still use it: an arena that did not drain, or a
    /// fast leave, which runs no teardown).
    pub fn keep(self) {
        std::mem::forget(self);
    }
}

impl Drop for GrantedView {
    fn drop(&mut self) {
        // SAFETY: the view acquire issued, released once.
        let _ = unsafe { self.services.call(self.release, (self.view,)) };
    }
}

/// The archive registry of the generation this start was granted, read through the
/// service adapter (module docs).
pub struct ArchiveRegistryPort {
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
    expected: KilnEmlArchiveIdentityV1,
    state: RefCell<Option<Result<Open, PortError>>>,
}

impl ArchiveRegistryPort {
    /// A port that acquires its view under `owner` (kiln-sfi's plugin owner, the view's
    /// transport-root owner) with `expected`, at the first page.
    pub fn new(
        services: Rc<ServiceAdapter>,
        owner: KilnOwner,
        expected: KilnEmlArchiveIdentityV1,
    ) -> Self {
        let mut expected = expected;
        expected.size = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
        Self {
            services,
            owner,
            expected,
            state: RefCell::new(None),
        }
    }

    /// The view this port holds, once acquired and checked.
    pub fn view(&self) -> Option<KilnEmlArchiveView> {
        match self.state.borrow().as_ref() {
            Some(Ok(open)) => Some(open.view.view()),
            _ => None,
        }
    }

    fn open(&self) -> Result<Open, PortError> {
        let services = &self.services;
        let table = services
            .bind::<KilnEmlArchiveApiV1>(KILN_EML_ARCHIVE_SERVICE, KILN_EML_ARCHIVE_VERSION)
            .map_err(PortError::Unavailable)?;
        let members = ViewMembers::bind(&table)?;
        let registry_metadata = service_member!(table, KilnEmlArchiveApiV1, registry_metadata)
            .map_err(PortError::Unavailable)?;
        let caps = Caps::bind(services)?;
        for row in METADATA_ROWS {
            caps.require(row)?;
        }
        let errors = self.error_channel(&table, &caps);
        let view = GrantedView::acquire_with(services, members, self.owner, &self.expected)?;
        Ok(Open {
            view,
            registry_metadata,
            errors,
        })
    }

    /// The precise-error channel when its rows and both slots are there; otherwise none
    /// (the calls then pass a NULL record and no token is issued).
    fn error_channel(
        &self,
        table: &ServiceTable<KilnEmlArchiveApiV1>,
        caps: &Caps<'_>,
    ) -> Option<ErrorChannel> {
        let copy = service_member!(*table, KilnEmlArchiveApiV1, error_copy).ok()?;
        let release = service_member!(*table, KilnEmlArchiveApiV1, error_release).ok()?;
        if ERRORS_ROWS.iter().any(|row| caps.require(row).is_err()) {
            return None;
        }
        Some(ErrorChannel { copy, release })
    }

    /// One call of registry_metadata into `buffer`: (result, required, error record).
    fn metadata(
        &self,
        open: &Open,
        first: u64,
        limit: u32,
        buffer: &mut [u8],
    ) -> (KilnResult, usize, KilnArchiveErrorV1) {
        let mut record = error_record();
        let record_ptr: *mut KilnArchiveErrorV1 = if open.errors.is_some() {
            &raw mut record
        } else {
            std::ptr::null_mut()
        };
        let data: *mut c_char = if buffer.is_empty() {
            std::ptr::null_mut()
        } else {
            buffer.as_mut_ptr().cast()
        };
        let mut required = 0usize;
        // SAFETY: an issued view; `data` is NULL with capacity 0 or writable for
        // `buffer.len()` bytes; required and the record are live and do not overlap it.
        let result = unsafe {
            self.services.call(
                open.registry_metadata,
                (
                    open.view.view(),
                    first,
                    limit,
                    data,
                    buffer.len(),
                    &raw mut required,
                    record_ptr,
                ),
            )
        };
        (result, required, record)
    }

    /// A non-OK result of `member`, with the provider's text when its record carried a
    /// token (copied, then released once).
    fn failed(
        &self,
        open: &Open,
        member: &'static str,
        result: KilnResult,
        record: &KilnArchiveErrorV1,
    ) -> PortError {
        let message = match &open.errors {
            Some(channel) if record.token != 0 => {
                let text = self.copy_message(channel, record.token);
                // SAFETY: a token this owner's call was issued; released once, here.
                let _ = unsafe {
                    self.services
                        .call(channel.release, (self.owner, record.token))
                };
                text
            }
            _ => None,
        };
        PortError::Failed {
            member,
            result,
            message,
        }
    }

    fn copy_message(&self, channel: &ErrorChannel, token: KilnHandle) -> Option<String> {
        let mut required = 0usize;
        // SAFETY: a probe: NULL data with capacity 0 and a live required.
        let result = unsafe {
            self.services.call(
                channel.copy,
                (
                    self.owner,
                    token,
                    std::ptr::null_mut(),
                    0,
                    &raw mut required,
                ),
            )
        };
        if result == KILN_OK {
            return (required == 0).then(String::new);
        }
        if result != KILN_ERR_BUFFER_TOO_SMALL || required == 0 || required > MESSAGE_MAX {
            return None;
        }
        let mut text = vec![0u8; required];
        let mut written = 0usize;
        // SAFETY: `text` is writable for `required` bytes.
        let result = unsafe {
            self.services.call(
                channel.copy,
                (
                    self.owner,
                    token,
                    text.as_mut_ptr().cast::<c_void>(),
                    required,
                    &raw mut written,
                ),
            )
        };
        if result != KILN_OK || written > required {
            return None;
        }
        text.truncate(written);
        Some(String::from_utf8_lossy(&text).into_owned())
    }
}

impl TypeRegistryPort for ArchiveRegistryPort {
    fn page(&self, first: u64, limit: u32) -> Result<Vec<u8>, PortError> {
        let mut state = self.state.borrow_mut();
        let open = match state.get_or_insert_with(|| self.open()) {
            Ok(open) => &*open,
            Err(error) => return Err(error.clone()),
        };
        let member = "registry_metadata";
        let (result, required, record) = self.metadata(open, first, limit, &mut []);
        if result == KILN_OK {
            // A page is never empty JSON; the loader refuses what this returns.
            return Ok(Vec::new());
        }
        if result != KILN_ERR_BUFFER_TOO_SMALL {
            return Err(self.failed(open, member, result, &record));
        }
        if required == 0 || required > PAGE_MAX {
            return Err(PortError::Failed {
                member,
                result,
                message: Some(format!(
                    "a page of {required} bytes is outside what this reader takes (1 to {PAGE_MAX})"
                )),
            });
        }
        let mut page = vec![0u8; required];
        let (result, written, record) = self.metadata(open, first, limit, &mut page);
        if result == KILN_ERR_BUFFER_TOO_SMALL {
            return Err(PortError::Failed {
                member,
                result,
                message: Some(format!(
                    "the page needs {written} bytes after the sizing call said {required}"
                )),
            });
        }
        if result != KILN_OK {
            return Err(self.failed(open, member, result, &record));
        }
        if written != required {
            return Err(PortError::Failed {
                member,
                result: KILN_ERR_INTERNAL,
                message: Some(format!(
                    "the page is {written} bytes after the sizing call said {required}"
                )),
            });
        }
        Ok(page)
    }

    fn expected_pin(&self) -> Option<RegistryPin> {
        Some(pin_of(&self.expected))
    }
}

fn error_record() -> KilnArchiveErrorV1 {
    KilnArchiveErrorV1 {
        size: std::mem::size_of::<KilnArchiveErrorV1>() as u32,
        result: KILN_OK,
        token: 0,
        message_bytes: 0,
        flags: 0,
        reserved: 0,
    }
}

/// Every pin of the two records (the record sizes aside).
fn same_identity(a: &KilnEmlArchiveIdentityV1, b: &KilnEmlArchiveIdentityV1) -> bool {
    a.phase == b.phase
        && a.launch_id == b.launch_id
        && a.plan_id == b.plan_id
        && a.generation == b.generation
        && a.reload_epoch == b.reload_epoch
        && a.plan_sha256 == b.plan_sha256
        && a.archive_sha256 == b.archive_sha256
        && a.registry_id == b.registry_id
        && a.registry_sha256 == b.registry_sha256
}

/// The identity as the pages' `pin` writes it (`common.schema.json#/$defs/pin`).
pub fn pin_of(identity: &KilnEmlArchiveIdentityV1) -> RegistryPin {
    RegistryPin {
        launch_id: uuid(&identity.launch_id),
        plan_id: uuid(&identity.plan_id),
        generation: identity.generation,
        reload_epoch: identity.reload_epoch,
        plan_sha256: hex(&identity.plan_sha256),
        archive_sha256: hex(&identity.archive_sha256),
        registry_id: identity.registry_id,
        registry_sha256: hex(&identity.registry_sha256),
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// RFC 4122 bytes (network order) as lowercase 8-4-4-4-12 text.
fn uuid(bytes: &[u8; 16]) -> String {
    let h = hex(bytes);
    format!(
        "{}-{}-{}-{}-{}",
        &h[0..8],
        &h[8..12],
        &h[12..16],
        &h[16..20],
        &h[20..32]
    )
}

/// kiln.caps' `find`, through the adapter.
struct Caps<'a> {
    services: &'a ServiceAdapter,
    find: Member<unsafe extern "C" fn(KilnStr, *mut KilnCapRow) -> KilnResult>,
}

impl<'a> Caps<'a> {
    fn bind(services: &'a ServiceAdapter) -> Result<Self, PortError> {
        let table = services
            .bind::<KilnCapsApi>(KILN_CAPS_SERVICE, KILN_CAPS_VERSION)
            .map_err(PortError::Unavailable)?;
        let find = service_member!(table, KilnCapsApi, find).map_err(PortError::Unavailable)?;
        Ok(Self { services, find })
    }

    /// `row` must be reported AVAILABLE.
    fn require(&self, row: &'static str) -> Result<(), PortError> {
        // SAFETY: an all-zero row is valid (integers and empty strings).
        let mut found: KilnCapRow = unsafe { std::mem::zeroed() };
        found.size = std::mem::size_of::<KilnCapRow>() as u32;
        let name = KilnStr {
            ptr: row.as_ptr().cast(),
            len: row.len(),
        };
        // SAFETY: a counted name and a sized caller row (kiln_caps.h).
        let result = unsafe { self.services.call(self.find, (name, &raw mut found)) };
        if result != KILN_OK {
            return Err(PortError::Withheld {
                row,
                text: format!("nobody reported it ({result})"),
            });
        }
        if found.state == KILN_CAP_AVAILABLE as u32 {
            return Ok(());
        }
        let state = match found.state as i32 {
            KILN_CAP_PENDING => "pending",
            KILN_CAP_UNAVAILABLE => "unavailable",
            _ => "in an unknown state",
        };
        let text = if found.text.ptr.is_null() || found.text.len == 0 {
            String::new()
        } else {
            // SAFETY: find's strings stay valid until this thread's next find (kiln_caps.h);
            // copied at once.
            let bytes =
                unsafe { std::slice::from_raw_parts(found.text.ptr.cast::<u8>(), found.text.len) };
            format!(": {}", String::from_utf8_lossy(bytes))
        };
        Err(PortError::Withheld {
            row,
            text: format!("it is {state}{text}"),
        })
    }
}
