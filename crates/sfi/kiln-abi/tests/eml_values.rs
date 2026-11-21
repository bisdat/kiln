// SPDX-License-Identifier: MIT
// Generated-binding probes for kiln.eml.values v1. Declarations only: these
// placeholders are not the GPL provider and prove no graph or lifetime behaviour.
use core::mem::{align_of, offset_of, size_of};
use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::*;

macro_rules! placeholder {
    ($name:ident($($arg:ty),* $(,)?)) => {
        unsafe extern "C" fn $name($(_: $arg),*) -> KilnResult { KILN_OK }
    };
}
placeholder!(
    arena_create(KilnOwner, KilnEmlArchiveView, u32, *const KilnEmlLuaReaderV1, *mut KilnHandle)
);
placeholder!(arena_pending(KilnOwner, KilnHandle, *mut u32));
placeholder!(
    value_open(
        KilnOwner,
        KilnHandle,
        KilnEmlArchiveView,
        *const KilnEmlValueOpenV1,
        *mut KilnEmlValueResultV1,
    )
);
placeholder!(value_info(KilnOwner, KilnHandle, *mut KilnEmlValueInfoV1));
placeholder!(value_read(KilnOwner, KilnHandle, u32, *mut KilnEmlValueResultV1));
placeholder!(
    value_child(KilnOwner, KilnHandle, *const KilnEmlValueSelectorV1, *mut KilnEmlValueResultV1)
);
placeholder!(
    value_set(KilnOwner, KilnHandle, *const KilnEmlValueSelectorV1, u64, *mut KilnEmlValueResultV1)
);
placeholder!(value_unary(KilnOwner, KilnHandle, *mut KilnEmlValueResultV1));
placeholder!(value_export(KilnOwner, KilnHandle, KilnStr, *mut KilnEmlValueResultV1));
placeholder!(value_close(KilnOwner, KilnHandle));
placeholder!(value_result_copy(KilnOwner, KilnHandle, *mut core::ffi::c_void, usize, *mut usize));
placeholder!(value_lua(KilnOwner, KilnHandle, u64, *mut KilnEmlValueResultV1));
placeholder!(value_index_lua(KilnOwner, KilnHandle, u64, u64, *mut KilnEmlValueResultV1));
placeholder!(value_index(KilnOwner, KilnHandle, u64, *mut KilnEmlValueResultV1));
placeholder!(
    reader_access(*mut core::ffi::c_void, *const KilnEmlLuaAccessV1, *mut KilnEmlLuaValueV1)
);
placeholder!(reader_copy(*mut core::ffi::c_void, u64, *mut core::ffi::c_void, usize, *mut usize));
placeholder!(reader_retain(*mut core::ffi::c_void, u64));
unsafe extern "C" fn reader_release(_: *mut core::ffi::c_void, _: u64) {}
unsafe extern "C" fn reader_wake(_: *mut core::ffi::c_void, _: KilnHandle) {}

#[test]
fn record_sizes_reuse_the_canonical_archive_identity() {
    assert_eq!(size_of::<KilnEmlArchiveIdentityV1>(), 160);
    assert_eq!(size_of::<KilnEmlValueIdentityV1>(), 208);
    assert_eq!(offset_of!(KilnEmlValueIdentityV1, archive), 48);
    assert_eq!(size_of::<KilnEmlValueInfoV1>(), 248);
    assert_eq!(offset_of!(KilnEmlValueInfoV1, source_len), 240);
    assert_eq!(size_of::<KilnEmlValueSelectorV1>(), 32);
    assert_eq!(size_of::<KilnEmlValueResultV1>(), 48);
    assert_eq!(size_of::<KilnEmlValueOpenV1>(), 88);
    assert_eq!(size_of::<KilnEmlLuaAccessV1>(), 56);
    assert_eq!(size_of::<KilnEmlLuaValueV1>(), 56);
    assert_eq!(size_of::<KilnEmlLuaReaderV1>(), 56);
    assert_eq!(size_of::<KilnEmlValuesApiV1>(), 144);
    assert_eq!(align_of::<KilnEmlValuesApiV1>(), 8);
}

#[test]
fn seventeen_members_in_order() {
    assert_eq!(offset_of!(KilnEmlValuesApiV1, arena_create), 8);
    assert_eq!(offset_of!(KilnEmlValuesApiV1, value_open), 32);
    assert_eq!(offset_of!(KilnEmlValuesApiV1, value_close), 88);
    assert_eq!(offset_of!(KilnEmlValuesApiV1, value_result_copy), 96);
    assert_eq!(offset_of!(KilnEmlValuesApiV1, value_array_clear), 136);
    assert_eq!(offset_of!(KilnEmlLuaReaderV1, wake), 48);
}

#[test]
fn service_name_version_and_enum_values() {
    assert_eq!(KILN_EML_VALUES_SERVICE, b"kiln.eml.values\0");
    assert_eq!(KILN_EML_VALUES_VERSION, 1);
    assert_eq!(KILN_VALUE_RUNTIME as i64, 1);
    assert_eq!(KILN_VALUE_STRUCT_OWNED as i64, 16);
    assert_eq!(KILN_VALUE_ERROR as i64, 24);
    assert_eq!(KILN_VALUE_READ_ITER_NEXT as i64, 6);
    assert_eq!(KILN_VALUE_OPEN_DEFAULT as i64, 5);
    assert_eq!(KILN_VALUE_LOADED_SNAPSHOT as i64, 8);
    assert_eq!(KILN_LUA_MAKE_TYPED_PROXY as i64, 6);
    assert_eq!(KILN_LUA_OTHER as i64, 9);
    assert_eq!(KILN_LUA_ADAPTER_RESOURCE as i64, 3);
    assert_eq!(KILN_LUA_LEASE_REUSED as i64, 2);
}

#[test]
fn generated_callbacks_have_c_abi() {
    // Assigning each placeholder pins every generated member signature.
    let api = KilnEmlValuesApiV1 {
        header: KilnServiceHeader {
            size: size_of::<KilnEmlValuesApiV1>() as u32,
            version: 1,
        },
        arena_create: Some(arena_create),
        arena_close: Some(arena_pending),
        arena_collect: Some(arena_pending),
        value_open: Some(value_open),
        value_info: Some(value_info),
        value_read: Some(value_read),
        value_child: Some(value_child),
        value_set: Some(value_set),
        value_clone: Some(value_unary),
        value_export: Some(value_export),
        value_close: Some(value_close),
        value_result_copy: Some(value_result_copy),
        value_array_push: Some(value_lua),
        value_array_insert: Some(value_index_lua),
        value_array_pop: Some(value_unary),
        value_array_remove: Some(value_index),
        value_array_clear: Some(value_unary),
    };
    let reader = KilnEmlLuaReaderV1 {
        size: 56,
        version: 1,
        user: core::ptr::null_mut(),
        access: Some(reader_access),
        copy: Some(reader_copy),
        retain: Some(reader_retain),
        release: Some(reader_release),
        wake: Some(reader_wake),
    };
    let mut arena = 0;
    let create = api.arena_create.unwrap();
    assert_eq!(
        unsafe { create(1, 9, KILN_VALUE_STARTUP as u32, &reader, &mut arena) },
        KILN_OK
    );
    assert_eq!(unsafe { (api.value_close.unwrap())(1, 42) }, KILN_OK);
}
