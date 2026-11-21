// SPDX-License-Identifier: MIT
// Generated-binding probes for kiln.eml.startup v1 (published_identity appended later). Declarations only:
// these placeholders are not the GPL provider and prove no witness, outcome or identity behaviour.
use core::mem::{align_of, offset_of, size_of};
use kiln_abi::services::kiln_eml_startup::*;
use kiln_abi::*;

unsafe extern "C" fn attach(
    key: *const KilnEmlStartupKeyV1,
    _: u32,
    session: *mut u64,
) -> KilnResult {
    // SAFETY: the test passes valid pointers.
    unsafe {
        if (*key).session != 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        *session = 77;
    }
    KILN_OK
}
unsafe extern "C" fn witness_wait(
    _: *const KilnEmlStartupKeyV1,
    _: u32,
    out: *mut KilnEmlStartupWitnessV1,
    _: *mut core::ffi::c_char,
    _: usize,
    required: *mut usize,
) -> KilnResult {
    // SAFETY: the test passes valid pointers.
    unsafe {
        (*out).state = KILN_EML_WITNESS_READY as u32;
        *required = 0;
    }
    KILN_OK
}
unsafe extern "C" fn witness_recheck(
    _: *const KilnEmlStartupKeyV1,
    _: *const KilnEmlStartupWitnessV1,
    _: *mut core::ffi::c_char,
    _: usize,
    _: *mut usize,
) -> KilnResult {
    KILN_OK
}
unsafe extern "C" fn native_terminal(_: *const KilnEmlStartupKeyV1, _: KilnStr) -> KilnResult {
    KILN_OK
}
unsafe extern "C" fn outcome(
    _: *const KilnEmlStartupKeyV1,
    _: *mut KilnEmlArchiveIdentityV1,
    _: *mut core::ffi::c_char,
    _: usize,
    _: *mut usize,
) -> KilnResult {
    KILN_ERR_UNAVAILABLE
}
unsafe extern "C" fn published_identity(
    key: *const KilnEmlStartupKeyV1,
    identity: *mut KilnEmlArchiveIdentityV1,
) -> KilnResult {
    // SAFETY: the test passes valid pointers.
    unsafe {
        if (*key).session != 0 {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        (*identity).generation = 9;
    }
    KILN_OK
}
unsafe extern "C" fn service(_: u32) -> *const KilnEmlStartupApiV1 {
    core::ptr::null()
}

#[test]
fn record_sizes_and_offsets() {
    assert_eq!(size_of::<KilnEmlStartupKeyV1>(), 56);
    assert_eq!(
        size_of::<KilnEmlStartupKeyV1>(),
        KILN_EML_STARTUP_KEY_SIZE as usize
    );
    assert_eq!(offset_of!(KilnEmlStartupKeyV1, launch_id), 8);
    assert_eq!(offset_of!(KilnEmlStartupKeyV1, plan_sha256), 16);
    assert_eq!(offset_of!(KilnEmlStartupKeyV1, session), 48);
    assert_eq!(size_of::<KilnEmlStartupWitnessV1>(), 40);
    assert_eq!(
        size_of::<KilnEmlStartupWitnessV1>(),
        KILN_EML_STARTUP_WITNESS_SIZE as usize
    );
    assert_eq!(offset_of!(KilnEmlStartupWitnessV1, plan_id), 8);
    assert_eq!(offset_of!(KilnEmlStartupWitnessV1, revision), 24);
    assert_eq!(offset_of!(KilnEmlStartupWitnessV1, reserved), 36);
    assert_eq!(size_of::<KilnEmlArchiveIdentityV1>(), 160);
    for align in [
        align_of::<KilnEmlStartupKeyV1>(),
        align_of::<KilnEmlStartupWitnessV1>(),
        align_of::<KilnEmlStartupApiV1>(),
    ] {
        assert_eq!(align, 8);
    }
}

#[test]
fn six_members_in_order() {
    // published_identity is appended to the first five: 48 -> 56 bytes, still version 1
    assert_eq!(size_of::<KilnEmlStartupApiV1>(), 56);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, attach), 8);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, witness_wait), 16);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, witness_recheck), 24);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, native_terminal), 32);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, outcome), 40);
    assert_eq!(offset_of!(KilnEmlStartupApiV1, published_identity), 48);
}

#[test]
fn export_name_version_format_and_states() {
    assert_eq!(
        KILN_EML_STARTUP_SERVICE_EXPORT,
        b"kiln_eml_startup_service\0"
    );
    assert_eq!(KILN_EML_STARTUP_VERSION, 1);
    assert_eq!(KILN_EML_STARTUP_FORMAT, 1);
    assert_eq!(KILN_EML_WITNESS_PENDING, 0);
    assert_eq!(KILN_EML_WITNESS_READY, 1);
    assert_eq!(KILN_EML_WITNESS_REFUSED, 2);
}

#[test]
fn generated_callbacks_have_c_abi() {
    // Assigning each placeholder pins every generated member signature.
    let api = KilnEmlStartupApiV1 {
        header: KilnServiceHeader {
            size: size_of::<KilnEmlStartupApiV1>() as u32,
            version: KILN_EML_STARTUP_VERSION,
        },
        attach: Some(attach),
        witness_wait: Some(witness_wait),
        witness_recheck: Some(witness_recheck),
        native_terminal: Some(native_terminal),
        outcome: Some(outcome),
        published_identity: Some(published_identity),
    };
    let getter: KilnEmlStartupServiceFn = core::hint::black_box(Some(service));
    assert!(unsafe { getter.unwrap()(1) }.is_null());
    let mut key = KilnEmlStartupKeyV1 {
        size: 56,
        flags: 0,
        launch_id: 0x0123_4567_89ab_cdef,
        plan_sha256: [0xaa; 32],
        session: 0,
    };
    let mut session = 0;
    assert_eq!(
        unsafe { api.attach.unwrap()(&key, 2, &mut session) },
        KILN_OK
    );
    assert_eq!(session, 77);
    key.session = session;
    let mut witness = KilnEmlStartupWitnessV1 {
        size: 40,
        state: KILN_EML_WITNESS_PENDING as u32,
        plan_id: [0; 16],
        revision: 0,
        flags: 0,
        reserved: 0,
    };
    let mut required = 9;
    let wait = api.witness_wait.unwrap();
    let rc = unsafe {
        wait(
            &key,
            2000,
            &mut witness,
            core::ptr::null_mut(),
            0,
            &mut required,
        )
    };
    assert_eq!(rc, KILN_OK);
    assert_eq!(
        (witness.state, required),
        (KILN_EML_WITNESS_READY as u32, 0)
    );
    // published_identity is session-free: the attach's session is refused, 0 is not
    // SAFETY: integers and byte arrays.
    let mut identity: KilnEmlArchiveIdentityV1 = unsafe { core::mem::zeroed() };
    identity.size = 160;
    let published = api.published_identity.unwrap();
    assert_eq!(
        unsafe { published(&key, &mut identity) },
        KILN_ERR_INVALID_ARGUMENT
    );
    key.session = 0;
    assert_eq!(unsafe { published(&key, &mut identity) }, KILN_OK);
    assert_eq!(identity.generation, 9);
}
