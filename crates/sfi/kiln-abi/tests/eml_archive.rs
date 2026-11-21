// SPDX-License-Identifier: MIT
use core::mem::{offset_of, size_of};
use kiln_abi::services::kiln_eml_archive::*;
use kiln_abi::*;

unsafe extern "C" fn acquire(
    _: KilnOwner,
    _: *const KilnEmlArchiveIdentityV1,
    out: *mut KilnEmlArchiveView,
) -> KilnResult {
    unsafe {
        *out = 42;
    }
    KILN_OK
}
unsafe extern "C" fn identity(
    _: KilnEmlArchiveView,
    _: *mut KilnEmlArchiveIdentityV1,
) -> KilnResult {
    KILN_OK
}
unsafe extern "C" fn release(_: KilnEmlArchiveView) -> KilnResult {
    KILN_OK
}
unsafe extern "C" fn metadata(
    _: KilnEmlArchiveView,
    _: u64,
    _: u32,
    _: *mut core::ffi::c_char,
    _: usize,
    _: *mut usize,
    _: *mut KilnArchiveErrorV1,
) -> KilnResult {
    KILN_OK
}

// Non-null placeholders used only to test structural support, never called.
macro_rules! placeholder {
    ($name:ident($($arg:ty),* $(,)?)) => {
        unsafe extern "C" fn $name($(_: $arg),*) -> KilnResult { KILN_OK }
    };
}
placeholder!(
    resource_json(
        KilnEmlArchiveView,
        KilnStr,
        *mut core::ffi::c_char,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    find_resource(
        KilnOwner,
        KilnEmlArchiveView,
        *const KilnAssetKey,
        *mut KilnArchiveResourceInfo,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    list_resources(
        KilnOwner,
        KilnEmlArchiveView,
        *const KilnArchiveQuery,
        u64,
        *mut KilnAssetKey,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    read_resource(
        KilnOwner,
        KilnEmlArchiveView,
        *const KilnAssetKey,
        *mut core::ffi::c_void,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    read_content(
        KilnOwner,
        KilnEmlArchiveView,
        *const KilnContentHash,
        u64,
        u64,
        *mut core::ffi::c_void,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    list_contents(
        KilnOwner,
        KilnEmlArchiveView,
        u64,
        *mut KilnContentHash,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    codec(
        KilnOwner,
        KilnEmlArchiveView,
        KilnStr,
        *const core::ffi::c_void,
        usize,
        *mut core::ffi::c_void,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    loaded_field(
        KilnOwner,
        KilnEmlArchiveView,
        KilnStr,
        KilnStr,
        *const core::ffi::c_void,
        usize,
        *mut KilnFieldSelection,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    attribute_catalog(
        KilnOwner,
        KilnEmlArchiveView,
        *mut core::ffi::c_char,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    attribute_resolve(
        KilnOwner,
        KilnEmlArchiveView,
        KilnStr,
        *mut core::ffi::c_char,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);
placeholder!(
    attribute_evaluate(
        KilnOwner,
        KilnEmlArchiveView,
        u32,
        *const u32,
        u32,
        *mut u32,
        *mut core::ffi::c_char,
        usize,
        *mut usize,
        *mut KilnArchiveErrorV1,
    )
);

placeholder!(copy_error(KilnOwner, KilnHandle, *mut core::ffi::c_void, usize, *mut usize));
placeholder!(close_error(KilnOwner, KilnHandle));

// Fake provider facts only: real availability also requires useful success and
// generation readiness. Each full named subset is independent of the other rows.
fn group_available(a: &KilnEmlArchiveApiV1, row: &[u8]) -> bool {
    macro_rules! has {
        ($member:ident) => {
            a.header.size as usize >= offset_of!(KilnEmlArchiveApiV1, $member) + size_of::<usize>()
                && a.header.version >= 1
                && a.$member.is_some()
        };
    }
    match row {
        KILN_EML_ARCHIVE_METADATA_CAP => {
            has!(acquire) && has!(identity) && has!(release) && has!(registry_metadata)
        }
        KILN_EML_ARCHIVE_READ_CAP => {
            has!(resource_json)
                && has!(find_resource)
                && has!(list_resources)
                && has!(read_resource)
                && has!(content_read_range)
                && has!(list_contents)
        }
        KILN_EML_ARCHIVE_CODEC_CAP => {
            has!(value_decode) && has!(value_encode) && has!(loaded_field)
        }
        KILN_EML_ARCHIVE_ERRORS_CAP => has!(error_copy) && has!(error_release),
        KILN_EML_ARCHIVE_ATTRIBUTES_CAP => {
            has!(attribute_catalog) && has!(attribute_resolve) && has!(attribute_evaluate)
        }
        _ => false,
    }
}

#[test]
fn fixed_partition_and_uuid_width() {
    assert_eq!(size_of::<KilnEmlArchiveApiV1>(), 152);
    assert_eq!(
        offset_of!(KilnEmlArchiveApiV1, release) + size_of::<usize>(),
        40
    );
    assert_eq!(offset_of!(KilnEmlArchiveApiV1, registry_metadata), 40);
    assert_eq!(offset_of!(KilnEmlArchiveApiV1, attribute_evaluate), 128);
    assert_eq!(offset_of!(KilnEmlArchiveApiV1, error_copy), 136);
    assert_eq!(offset_of!(KilnEmlArchiveApiV1, error_release), 144);
    assert_eq!(size_of::<KilnArchiveErrorV1>(), 32);
    let id: KilnEmlArchiveIdentityV1 = unsafe { core::mem::zeroed() };
    assert_eq!(id.launch_id.len(), 16);
    assert_eq!(id.plan_id.len(), 16);
}

#[test]
fn metadata_subset_does_not_require_unimplemented_resource_or_codec_slots() {
    let mut api: KilnEmlArchiveApiV1 = unsafe { core::mem::zeroed() };
    api.header = KilnServiceHeader {
        size: 48,
        version: 1,
    };
    api.acquire = Some(acquire);
    api.identity = Some(identity);
    api.release = Some(release);
    api.registry_metadata = Some(metadata);
    // In real consumers the individual field MUST only be read after its extent check.
    let supported = |a: &KilnEmlArchiveApiV1, metadata_row: bool, member_rows: bool| {
        metadata_row
            && member_rows
            && a.header.version >= 1
            && a.header.size >= 48
            && a.acquire.is_some()
            && a.identity.is_some()
            && a.release.is_some()
            && a.registry_metadata.is_some()
    };
    assert!(supported(&api, true, true));
    assert_eq!(KILN_EML_ARCHIVE_METADATA_CAP, b"eml.archive.metadata\0");
    assert!(supported(
        &api,
        group_available(&api, KILN_EML_ARCHIVE_METADATA_CAP),
        true
    ));
    assert!(!group_available(&api, KILN_EML_ARCHIVE_READ_CAP));
    assert!(!supported(&api, false, true));
    assert!(!supported(&api, true, false));
    assert!(api.resource_json.is_none() && api.value_decode.is_none());
    api.header.size = 47;
    assert!(!supported(&api, true, true));
    api.header.size = 48;
    api.registry_metadata = None;
    assert!(!supported(&api, true, true));
}

#[test]
fn capability_rows_require_every_member_of_their_own_subset() {
    let mut api: KilnEmlArchiveApiV1 = unsafe { core::mem::zeroed() };
    api.header = KilnServiceHeader {
        size: 152,
        version: 1,
    };
    api.acquire = Some(acquire);
    api.identity = Some(identity);
    api.release = Some(release);
    api.registry_metadata = Some(metadata);
    api.resource_json = Some(resource_json);
    api.find_resource = Some(find_resource);
    api.list_resources = Some(list_resources);
    api.read_resource = Some(read_resource);
    api.content_read_range = Some(read_content);
    api.list_contents = Some(list_contents);
    api.value_decode = Some(codec);
    api.value_encode = Some(codec);
    api.loaded_field = Some(loaded_field);
    api.attribute_catalog = Some(attribute_catalog);
    api.attribute_resolve = Some(attribute_resolve);
    api.attribute_evaluate = Some(attribute_evaluate);
    api.error_copy = Some(copy_error);
    api.error_release = Some(close_error);
    for row in [
        KILN_EML_ARCHIVE_METADATA_CAP,
        KILN_EML_ARCHIVE_READ_CAP,
        KILN_EML_ARCHIVE_CODEC_CAP,
        KILN_EML_ARCHIVE_ATTRIBUTES_CAP,
        KILN_EML_ARCHIVE_ERRORS_CAP,
    ] {
        assert!(group_available(&api, row));
    }
    macro_rules! incomplete {
        ($member:ident, $row:ident) => {
            let mut partial = api;
            partial.$member = None;
            assert!(!group_available(&partial, $row), stringify!($member));
        };
    }
    incomplete!(acquire, KILN_EML_ARCHIVE_METADATA_CAP);
    incomplete!(identity, KILN_EML_ARCHIVE_METADATA_CAP);
    incomplete!(release, KILN_EML_ARCHIVE_METADATA_CAP);
    incomplete!(registry_metadata, KILN_EML_ARCHIVE_METADATA_CAP);
    incomplete!(resource_json, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(find_resource, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(list_resources, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(read_resource, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(content_read_range, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(list_contents, KILN_EML_ARCHIVE_READ_CAP);
    incomplete!(value_decode, KILN_EML_ARCHIVE_CODEC_CAP);
    incomplete!(value_encode, KILN_EML_ARCHIVE_CODEC_CAP);
    incomplete!(loaded_field, KILN_EML_ARCHIVE_CODEC_CAP);
    incomplete!(attribute_catalog, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    incomplete!(attribute_resolve, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    incomplete!(attribute_evaluate, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    incomplete!(error_copy, KILN_EML_ARCHIVE_ERRORS_CAP);
    incomplete!(error_release, KILN_EML_ARCHIVE_ERRORS_CAP);
    for (extent, row) in [
        (87, KILN_EML_ARCHIVE_READ_CAP),
        (111, KILN_EML_ARCHIVE_CODEC_CAP),
        (135, KILN_EML_ARCHIVE_ATTRIBUTES_CAP),
        (151, KILN_EML_ARCHIVE_ERRORS_CAP),
    ] {
        api.header.size = extent;
        assert!(!group_available(&api, row));
    }
}

#[test]
fn generated_callback_has_c_abi() {
    let mut view = 0;
    let id: KilnEmlArchiveIdentityV1 = unsafe { core::mem::zeroed() };
    let callback: unsafe extern "C" fn(
        KilnOwner,
        *const KilnEmlArchiveIdentityV1,
        *mut KilnEmlArchiveView,
    ) -> KilnResult = acquire;
    assert_eq!(unsafe { callback(1, &id, &mut view) }, KILN_OK);
    assert_eq!(view, 42);
}
