// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.archive v1 (native/core/abi/services/kiln_eml_archive.h), the GPL provider: the early immutable table
//! getter, the same table published as `kiln.eml.archive`, and its capability rows (the archive table's metadata, the archive table's read group).
//!
//! What this table implements now, and nothing more:
//! - the metadata subset (acquire, identity, release, registry_metadata), reported as `eml.archive.metadata` with its
//!   four member rows;
//! - the read group (resource_json, find_resource, list_resources, read_resource, content_read_range, list_contents:
//!   `read`, the archive table's read group), reported as `eml.archive.read` with its six member rows. A generation authorized without its
//!   archive (`authorize`, `authorize_with_registry`) answers UNAVAILABLE to all six;
//! - the attribute group (attribute_catalog, attribute_resolve, attribute_evaluate: ShroudForge's attribute catalog of the
//!   generation's reference archive, `attributes`), reported as `eml.archive.attributes` with its three member rows. A
//!   generation authorized without its archive answers UNAVAILABLE to all three;
//! - the per-call error channel (error_copy, error_release) its failures use, reported as `eml.archive.errors` with
//!   its two member rows. That row is the channel only: it promises no family's precise source messages.
//!
//! The codec slots (value_decode, value_encode, loaded_field) are null, and `eml.archive.codec` is reported
//! unavailable. The table's size is the header's full 152 bytes; consumers check the slots and rows they need.
//!
//! The table holds no paths or OS handles: views read the immutable registry snapshot and the archive the startup controller
//! authorized with a generation (`authorize`, `authorize_with_archive`). Getter presence grants no view. All calls may
//! come from any thread and never enter Lua. kfc types stay here: only the header's neutral records, counted bytes and
//! counted JSON cross C.
pub mod attributes;
pub mod errors;
pub mod read;
pub mod registry;
pub mod view;

use std::ffi::c_void;

use kiln_abi::{
    KILN_ERR_INTERNAL, KILN_OK, KilnOwner, KilnResult, KilnServiceHeader, KilnStr,
    services::{
        kiln_caps::{
            KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, KILN_CAP_REASON_SERVICE_OLDER, KILN_CAP_UNAVAILABLE, KilnCapsApi,
        },
        kiln_eml_archive::{KILN_EML_ARCHIVE_VERSION, KilnEmlArchiveApiV1},
    },
};

pub use read::{ArchiveBacking, NamedIndex};
pub use registry::RegistrySnapshot;
pub use view::{
    Grant, Phase, authorize, authorize_with_archive, authorize_with_registry, retire, retire_epoch, revoke_owner,
};

/// The service name the table is published under.
pub const SERVICE: &str = "kiln.eml.archive";

static TABLE: KilnEmlArchiveApiV1 = KilnEmlArchiveApiV1 {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnEmlArchiveApiV1>() as u32,
        version: KILN_EML_ARCHIVE_VERSION,
    },
    acquire: Some(view::acquire),
    identity: Some(view::identity),
    resource_json: Some(read::resource_json),
    release: Some(view::release),
    registry_metadata: Some(view::registry_metadata),
    find_resource: Some(read::find_resource),
    list_resources: Some(read::list_resources),
    read_resource: Some(read::read_resource),
    content_read_range: Some(read::content_read_range),
    list_contents: Some(read::list_contents),
    value_decode: None,
    value_encode: None,
    loaded_field: None,
    attribute_catalog: Some(attributes::attribute_catalog),
    attribute_resolve: Some(attributes::attribute_resolve),
    attribute_evaluate: Some(attributes::attribute_evaluate),
    error_copy: Some(errors::error_copy),
    error_release: Some(errors::error_release),
};

/// The process-lifetime table.
pub fn table() -> &'static KilnEmlArchiveApiV1 {
    &TABLE
}

/// The early getter (kiln_eml_archive.h): versions 0 and 1 return the table also published as `kiln.eml.archive`,
/// later versions none. It grants no view.
#[unsafe(no_mangle)]
pub extern "C" fn kiln_eml_archive_service(min_version: u32) -> *const KilnEmlArchiveApiV1 {
    if min_version <= KILN_EML_ARCHIVE_VERSION {
        &TABLE
    } else {
        std::ptr::null()
    }
}

/// A panic never crosses the C boundary: it is INTERNAL.
pub(crate) fn contained(work: impl FnOnce() -> KilnResult) -> KilnResult {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(work)).unwrap_or(KILN_ERR_INTERNAL)
}

/// The rows a group reports: (group, members, implemented).
const GROUPS: [(&str, &[&str], bool); 5] = [
    (
        "eml.archive.metadata",
        &["acquire", "identity", "release", "registry_metadata"],
        true,
    ),
    ("eml.archive.errors", &["error_copy", "error_release"], true),
    (
        "eml.archive.read",
        &[
            "resource_json",
            "find_resource",
            "list_resources",
            "read_resource",
            "content_read_range",
            "list_contents",
        ],
        true,
    ),
    (
        "eml.archive.codec",
        &["value_decode", "value_encode", "loaded_field"],
        false,
    ),
    (
        "eml.archive.attributes",
        &["attribute_catalog", "attribute_resolve", "attribute_evaluate"],
        true,
    ),
];

fn kstr(text: &str) -> KilnStr {
    KilnStr {
        ptr: text.as_ptr().cast(),
        len: text.len(),
    }
}

/// Publishes the table as `kiln.eml.archive` through `register` (KilnApi.register_service for `owner`) and, once
/// that succeeded, reports the capability rows through `caps` (kiln.caps, when present). A complete group and each
/// of its members are AVAILABLE; a group this table does not implement is UNAVAILABLE and gets no member rows.
pub fn publish(
    owner: KilnOwner,
    register: impl FnOnce(&str, *const c_void) -> KilnResult,
    caps: Option<&KilnCapsApi>,
) -> KilnResult {
    let rc = register(SERVICE, (&TABLE as *const KilnEmlArchiveApiV1).cast());
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
    for (group, members, implemented) in GROUPS {
        if implemented {
            let _ = unsafe {
                report(
                    owner,
                    kstr(group),
                    KILN_CAP_AVAILABLE as u32,
                    KILN_CAP_REASON_NONE as u32,
                    kstr(""),
                )
            };
            for member in members {
                let name = format!("{group}.{member}");
                let _ = unsafe {
                    report(
                        owner,
                        kstr(&name),
                        KILN_CAP_AVAILABLE as u32,
                        KILN_CAP_REASON_NONE as u32,
                        kstr(""),
                    )
                };
            }
        } else {
            let text = format!("kiln-eml does not implement {} yet", members.join(", "));
            let _ = unsafe {
                report(
                    owner,
                    kstr(group),
                    KILN_CAP_UNAVAILABLE as u32,
                    KILN_CAP_REASON_SERVICE_OLDER as u32,
                    kstr(&text),
                )
            };
        }
    }
    KILN_OK
}
