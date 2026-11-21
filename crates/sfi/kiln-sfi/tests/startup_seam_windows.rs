// SPDX-License-Identifier: MIT
//! The startup seam on Windows: kiln-sfi finds kiln-eml's early export `kiln_eml_startup_service` in the kiln-eml.dll module this
//! process already loaded (`GetModuleHandleW`, `GetProcAddress`), never by loading one, and never in a module of
//! another name. Synthetic stand-ins only (tests/wine/startup_standin.c, built as kiln-eml.dll and other-name.dll by
//! the runner into the folder `SFI_STARTUP_STANDIN` names); run under Wine and natively with `--ignored`.
#![cfg(windows)]
use kiln_abi::services::kiln_eml_startup::*;
use kiln_sfi::startup_seam::{self, Seam};
use std::ffi::c_void;
use std::path::PathBuf;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryW(path: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
}

fn load(path: &std::path::Path) -> *mut c_void {
    use std::os::windows::ffi::OsStrExt;
    let wide: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
    let module = unsafe { LoadLibraryW(wide.as_ptr()) };
    assert!(!module.is_null(), "{} did not load", path.display());
    module
}

#[test]
#[ignore = "needs the stand-in DLLs: SFI_STARTUP_STANDIN names their folder"]
fn kiln_emls_export_is_found_in_the_loaded_kiln_eml_module_only() {
    let dir = PathBuf::from(std::env::var_os("SFI_STARTUP_STANDIN").expect("SFI_STARTUP_STANDIN"));
    // nothing named kiln-eml.dll in this process yet
    assert!(startup_seam::kiln_eml_export().is_none());
    // the same export in a module of another name is not kiln-eml's
    let other = load(&dir.join("other-name.dll"));
    assert!(
        !unsafe { GetProcAddress(other, c"kiln_eml_startup_service".as_ptr().cast()) }.is_null()
    );
    assert!(startup_seam::kiln_eml_export().is_none());
    // kiln-eml.dll, once loaded, is found by its module name
    let module = load(&dir.join(startup_seam::KILN_EML_MODULE));
    let getter = startup_seam::kiln_eml_export().expect("kiln-eml's export");
    assert!(unsafe { getter(2) }.is_null(), "version 2 is not served");
    let table = unsafe { getter(1) };
    assert!(!table.is_null());
    let header = unsafe { (*table).header };
    assert_eq!(
        (header.size, header.version),
        (48, KILN_EML_STARTUP_VERSION)
    );
    // it binds, and attach reaches that module with kiln.launch's key
    let seam = unsafe { Seam::bind(getter, 0x0123_4567_89ab_cdef, [9; 32], [5; 16]) }.unwrap();
    assert_eq!(seam.attach(3), Ok(0x51));
    let launch_id: unsafe extern "C" fn() -> u64 = unsafe {
        std::mem::transmute(GetProcAddress(module, c"standin_launch_id".as_ptr().cast()))
    };
    let planned: unsafe extern "C" fn() -> u32 =
        unsafe { std::mem::transmute(GetProcAddress(module, c"standin_planned".as_ptr().cast())) };
    assert_eq!(
        unsafe { (launch_id(), planned()) },
        (0x0123_4567_89ab_cdef, 3)
    );
    // the other module's attach never ran
    let other_planned: unsafe extern "C" fn() -> u32 =
        unsafe { std::mem::transmute(GetProcAddress(other, c"standin_planned".as_ptr().cast())) };
    assert_eq!(unsafe { other_planned() }, 0xffff_ffff);
}
