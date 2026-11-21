// SPDX-License-Identifier: MIT
//! Direct C SEH wrappers, never a Rust trampoline around a foreign call.
//! Signature declarations adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
//! modules/mod-runtime/native_bridge/shroudforge_mod_native.h:79-81,133-136 (1.8.0).
//! These opaque types do not implement the native bridge or its layouts.

use core::ffi::c_void;
#[repr(C)]
pub struct NativeHost {
    _private: [u8; 0],
}
#[repr(C)]
pub struct NativeValue {
    _private: [u8; 0],
}
#[repr(C)]
pub struct NativeResults {
    _private: [u8; 0],
}
pub type Init = unsafe extern "C" fn(*const NativeHost) -> i32;
pub type NativeCall =
    unsafe extern "C" fn(*mut c_void, *const NativeValue, usize, *mut NativeResults);

unsafe extern "C" {
    fn sfi_seh_init(function: Init, host: *const NativeHost, out: *mut i32) -> u32;
    fn sfi_seh_native(
        function: NativeCall,
        context: *mut c_void,
        args: *const NativeValue,
        count: usize,
        results: *mut NativeResults,
    ) -> u32;
    fn sfi_seh_load(path: *const u16, module: *mut *mut c_void, error: *mut u32) -> u32;
}

/// # Safety
/// The function and host follow the actual mod-init ABI; host storage/callbacks
/// stay live until process exit. Any Rust callback must catch its own panics.
/// On fault quarantine the package, retain modules, and discard registrations.
pub unsafe fn call_init(function: Init, host: *const NativeHost) -> Result<i32, u32> {
    let mut output = 0;
    match unsafe { sfi_seh_init(function, host, &mut output) } {
        0 => Ok(output),
        code => Err(code),
    }
}
/// # Safety
/// All pointer/count/context/result lifetimes satisfy the native-mod contract.
/// Lua errors and Rust panics must already be contained by host callbacks. No
/// foreign code may longjmp or unwind across Rust frames. Quarantine on fault.
pub unsafe fn call_native(
    function: NativeCall,
    context: *mut c_void,
    args: *const NativeValue,
    count: usize,
    results: *mut NativeResults,
) -> Result<(), u32> {
    match unsafe { sfi_seh_native(function, context, args, count, results) } {
        0 => Ok(()),
        code => Err(code),
    }
}

#[derive(Debug)]
pub struct LoadOutcome {
    pub module: *mut c_void,
    pub error: u32,
    pub fault: u32,
}
/// # Safety
/// `path` is a NUL-terminated UTF-16 canonical absolute path. Admission must have
/// inspected ordinary/delay imports and approved the frozen package BEFORE this
/// call; this low-level shim does not implement that policy or run by default.
/// The caller is outside DllMain. DLL search uses SF's DLL_LOAD_DIR|DEFAULT_DIRS.
/// A nonzero fault quarantines the package; never FreeLibrary the module, even
/// after a partial load, failed init, or missing bridge. Calls can still hang:
/// SEH catches supported faults, not hangs or arbitrary native memory corruption.
pub unsafe fn load_library(path: *const u16) -> LoadOutcome {
    let mut module = core::ptr::null_mut();
    let mut error = 0;
    let fault = unsafe { sfi_seh_load(path, &mut module, &mut error) };
    LoadOutcome {
        module,
        error,
        fault,
    }
}
