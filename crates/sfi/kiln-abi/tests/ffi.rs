// SPDX-License-Identifier: MIT
use kiln_abi::services::{kiln_caps::KilnCapsApi, kiln_creatures, kiln_rules};
use kiln_abi::*;

unsafe extern "C" fn task(user: *mut core::ffi::c_void) {
    unsafe {
        *user.cast::<u32>() += 1;
    }
}

#[test]
fn callback_alias_is_nullable_and_callable() {
    let mut value = 41u32;
    let callback: KilnTaskFn = Some(task);
    if let Some(callback) = callback {
        unsafe {
            callback((&mut value as *mut u32).cast());
        }
    }
    assert_eq!(value, 42);
    assert!(KilnTaskFn::None.is_none());
    assert_eq!(
        core::mem::size_of::<KilnTaskFn>(),
        core::mem::size_of::<usize>()
    );
}

#[test]
fn service_headers_and_root_share_one_type() {
    let header = KilnServiceHeader {
        size: core::mem::size_of::<KilnCapsApi>() as u32,
        version: 1,
    };
    let table = KilnCapsApi {
        header,
        count: None,
        at: None,
        find: None,
        report: None,
        subscribe: None,
        unsubscribe: None,
        generation: None,
    };
    assert_eq!(
        table.header.size as usize,
        core::mem::size_of::<KilnCapsApi>()
    );
}

#[test]
fn colliding_c_tags_remain_distinct_service_types() {
    assert_ne!(
        core::mem::size_of::<kiln_creatures::KilnRulesStatus>(),
        core::mem::size_of::<kiln_rules::KilnRulesStatus>()
    );
}

#[test]
fn all_generated_record_layout_assertions_compile() {
    // Every field, including inline-union members, is asserted in src/layout.rs.
    assert_eq!(core::mem::size_of::<KilnApi>(), 216);
    assert_eq!(core::mem::offset_of!(KilnApi, post_world), 192);
    assert_eq!(core::mem::size_of::<KilnPluginInfo>(), 64);
}
