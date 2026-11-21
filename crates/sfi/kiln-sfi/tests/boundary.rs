// SPDX-License-Identifier: MIT
use kiln_abi::services::kiln_caps::*;
use kiln_abi::*;
use kiln_sfi::{Boundary, Phase};
use std::sync::atomic::{AtomicUsize, Ordering};

static LOGS: AtomicUsize = AtomicUsize::new(0);
static REPORTS: AtomicUsize = AtomicUsize::new(0);
unsafe extern "C" fn log(_: u64, _: i32, _: KilnStr) {
    LOGS.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn report(_: u64, name: KilnStr, state: u32, reason: u32, text: KilnStr) -> i32 {
    assert_eq!(
        unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) },
        b"kiln-sfi.interpreter"
    );
    assert_eq!(state, KILN_CAP_UNAVAILABLE as u32);
    assert_eq!(reason, KILN_CAP_REASON_POLICY as u32);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) },
        b"backend negotiation not integrated"
    );
    REPORTS.fetch_add(1, Ordering::SeqCst);
    KILN_OK
}
static CAPS: KilnCapsApi = KilnCapsApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnCapsApi>() as u32,
        version: 1,
    },
    count: None,
    at: None,
    find: None,
    report: Some(report),
    subscribe: None,
    unsubscribe: None,
    generation: None,
};
unsafe extern "C" fn service(_: KilnStr, _: u32) -> *const core::ffi::c_void {
    (&CAPS as *const KilnCapsApi).cast()
}

fn api() -> Box<KilnApi> {
    let mut api: Box<KilnApi> = Box::new(unsafe { std::mem::zeroed() });
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.abi_version = 1;
    api.log = Some(log);
    api.service = Some(service);
    api
}
fn info() -> KilnPluginInfo {
    let mut info: KilnPluginInfo = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnPluginInfo>() as u32;
    info
}

#[test]
fn distinct_phase_owners_are_quiet_successful_inactive_and_idempotent() {
    LOGS.store(0, Ordering::SeqCst);
    REPORTS.store(0, Ordering::SeqCst);
    let boundary = Boundary::new();
    let api = api();
    let mut early = info();
    let mut normal = info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, 11, &mut early) },
        KILN_OK
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 12, &mut normal) },
        KILN_OK
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 12, &mut normal) },
        KILN_OK
    );
    assert!(early.on_ready.is_none() && normal.on_ready.is_none());
    assert!(early.on_unload.is_none() && normal.on_unload.is_none());
    assert_eq!(LOGS.load(Ordering::SeqCst), 1);
    assert_eq!(REPORTS.load(Ordering::SeqCst), 2);
    assert!(!boundary.worker_started());
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 13, &mut normal) },
        KILN_ERR_INVALID_ARGUMENT
    );
    let another = self::api();
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*another, 12, &mut normal) },
        KILN_ERR_INVALID_ARGUMENT
    );
}

#[test]
fn short_headers_null_slots_and_absent_tables_are_rejected_without_tail_reads() {
    let boundary = Boundary::new();
    let mut info = info();
    let short = [4u32];
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, short.as_ptr().cast(), 1, &mut info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, std::ptr::null(), 1, &mut info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    let mut table = api();
    table.log = None;
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*table, 1, &mut info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    table.log = Some(log);
    table.abi_version = 99;
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*table, 1, &mut info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    let mut valid_table = self::api();
    valid_table.log = Some(quiet_log);
    let mut small_info = self::info();
    small_info.size = 4;
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*valid_table, 1, &mut small_info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        small_info.size, 4,
        "short declared info storage must remain untouched"
    );
}

#[test]
fn short_service_table_is_not_read_as_a_full_new_table() {
    unsafe extern "C" fn short_service(_: KilnStr, _: u32) -> *const core::ffi::c_void {
        static HEADER: KilnServiceHeader = KilnServiceHeader {
            size: 8,
            version: 1,
        };
        (&HEADER as *const KilnServiceHeader).cast()
    }
    let boundary = Boundary::new();
    let mut table = api();
    table.log = Some(quiet_log);
    table.service = Some(short_service);
    let mut info = info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*table, 1, &mut info) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert!(!boundary.worker_started());
}
unsafe extern "C" fn quiet_log(_: u64, _: i32, _: KilnStr) {}

thread_local! {
    static CURRENT_CAPS: std::cell::Cell<*const core::ffi::c_void> = const { std::cell::Cell::new(std::ptr::null()) };
    static REPORT_RESULT: std::cell::Cell<i32> = const { std::cell::Cell::new(KILN_OK) };
    static REPORT_ATTEMPTS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}
unsafe extern "C" fn selected_service(_: KilnStr, _: u32) -> *const core::ffi::c_void {
    CURRENT_CAPS.get()
}
unsafe extern "C" fn selected_report(_: u64, _: KilnStr, _: u32, _: u32, _: KilnStr) -> i32 {
    REPORT_ATTEMPTS.set(REPORT_ATTEMPTS.get() + 1);
    REPORT_RESULT.get()
}

#[test]
fn unusable_caps_contracts_fail_and_same_owner_can_repair_reporting() {
    let boundary = Boundary::new();
    let mut api = api();
    api.log = Some(quiet_log);
    api.service = Some(selected_service);
    let mut info = info();
    let mut caps = CAPS;
    caps.report = Some(selected_report);
    #[repr(align(8))]
    struct AlignedBytes([u8; 64]);
    let bytes = AlignedBytes([0; 64]);
    let scenarios = [std::ptr::null(), unsafe { bytes.0.as_ptr().add(1) }.cast()];
    for table in scenarios {
        CURRENT_CAPS.set(table);
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, 7, &mut info) },
            KILN_ERR_INVALID_ARGUMENT
        );
    }
    for (size, version, populated) in [(4, 1, true), (8, 1, true), (64, 0, true), (64, 1, false)] {
        caps.header.size = size;
        caps.header.version = version;
        caps.report = if populated {
            Some(selected_report)
        } else {
            None
        };
        CURRENT_CAPS.set((&caps as *const KilnCapsApi).cast());
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, 7, &mut info) },
            KILN_ERR_INVALID_ARGUMENT
        );
    }
    caps.header.size = std::mem::size_of::<KilnCapsApi>() as u32;
    caps.header.version = KILN_CAPS_VERSION;
    caps.report = Some(selected_report);
    CURRENT_CAPS.set((&caps as *const KilnCapsApi).cast());
    REPORT_RESULT.set(KILN_OK);
    REPORT_ATTEMPTS.set(0);
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, 7, &mut info) },
        KILN_OK
    );
    assert_eq!(REPORT_ATTEMPTS.get(), 1);
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, 7, &mut info) },
        KILN_OK
    );
    assert_eq!(REPORT_ATTEMPTS.get(), 1);
    CURRENT_CAPS.set(std::ptr::null());
}

#[test]
fn failed_report_is_internal_error_and_identical_reentry_retries_it() {
    let boundary = Boundary::new();
    let mut api = api();
    api.log = Some(quiet_log);
    api.service = Some(selected_service);
    let mut caps = CAPS;
    caps.report = Some(selected_report);
    CURRENT_CAPS.set((&caps as *const KilnCapsApi).cast());
    REPORT_RESULT.set(KILN_ERR_REVOKED);
    REPORT_ATTEMPTS.set(0);
    let mut info = info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 9, &mut info) },
        KILN_ERR_INTERNAL
    );
    assert_eq!(REPORT_ATTEMPTS.get(), 1);
    REPORT_RESULT.set(KILN_OK);
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 9, &mut info) },
        KILN_OK
    );
    assert_eq!(REPORT_ATTEMPTS.get(), 2);
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 9, &mut info) },
        KILN_OK
    );
    assert_eq!(REPORT_ATTEMPTS.get(), 2);
    assert!(info.on_ready.is_none() && info.on_unload.is_none());
    assert!(!boundary.worker_started());
    CURRENT_CAPS.set(std::ptr::null());
}

#[test]
fn exported_entrypoints_reject_null_contracts() {
    assert_eq!(
        unsafe { kiln_sfi::kiln_plugin_early(std::ptr::null(), 1, std::ptr::null_mut()) },
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { kiln_sfi::kiln_plugin_main(std::ptr::null(), 1, std::ptr::null_mut()) },
        KILN_ERR_INVALID_ARGUMENT
    );
}

#[test]
fn rust_panics_are_mapped_at_exported_and_void_callback_boundaries() {
    assert_eq!(
        kiln_sfi::guard_result(|| panic!("fixture")),
        KILN_ERR_INTERNAL
    );
    assert!(!kiln_sfi::guard_callback(|| panic!("fixture")));
}
