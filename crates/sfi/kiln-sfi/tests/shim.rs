// SPDX-License-Identifier: MIT
use kiln_sfi::fault::{self, NativeHost, NativeResults, NativeValue};

unsafe extern "C" fn init(_: *const NativeHost) -> i32 {
    7
}
unsafe extern "C" fn native(
    context: *mut core::ffi::c_void,
    _: *const NativeValue,
    count: usize,
    _: *mut NativeResults,
) {
    unsafe {
        *context.cast::<usize>() = count;
    }
}

#[test]
fn typed_c_boundaries_forward_only_the_declared_arguments() {
    assert_eq!(unsafe { fault::call_init(init, std::ptr::null()) }, Ok(7));
    let mut count = 0usize;
    assert_eq!(
        unsafe {
            fault::call_native(
                native,
                (&mut count as *mut usize).cast(),
                std::ptr::null(),
                3,
                std::ptr::null_mut(),
            )
        },
        Ok(())
    );
    assert_eq!(count, 3);
}

#[cfg(not(windows))]
#[test]
fn portable_host_shim_explicitly_refuses_windows_dll_loading() {
    let path: Vec<u16> = "fixture.dll\0".encode_utf16().collect();
    let outcome = unsafe { fault::load_library(path.as_ptr()) };
    assert!(outcome.module.is_null());
    assert_eq!(outcome.fault, 0);
    assert_eq!(outcome.error, 50); // ERROR_NOT_SUPPORTED, not a pretend successful load.
}
