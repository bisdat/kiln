// SPDX-License-Identifier: MIT
fn main() {
    println!("cargo:rerun-if-changed=shim/fault.c");
    println!("cargo:rerun-if-changed=shim/fault.h");
    println!("cargo:rerun-if-changed=shim/monitor.c");
    println!("cargo:rerun-if-changed=shim/monitor.h");
    println!("cargo:rerun-if-changed=shim/bindings.c");
    println!("cargo:rerun-if-changed=shim/notification_ring.c");
    println!("cargo:rerun-if-changed=shim/notification_ring.h");
    println!("cargo:rerun-if-changed=shim/breach_state.c");
    println!("cargo:rerun-if-changed=shim/breach_state.h");
    println!("cargo:rerun-if-changed=shim/pin_state.c");
    println!("cargo:rerun-if-changed=shim/pin_state.h");
    println!("cargo:rerun-if-changed=shim/seh_guard.c");
    println!("cargo:rerun-if-changed=shim/seh_guard.h");
    println!("cargo:rerun-if-changed=shim/api_set.c");
    println!("cargo:rerun-if-changed=shim/api_set.h");
    println!("cargo:rerun-if-changed=shim/load_window.c");
    println!("cargo:rerun-if-changed=shim/load_window.h");
    println!("cargo:rerun-if-changed=shim/owner_faults.c");
    println!("cargo:rerun-if-changed=shim/owner_faults.h");
    println!("cargo:rerun-if-changed=shim/pe_identity.c");
    println!("cargo:rerun-if-changed=shim/pe_identity.h");
    println!("cargo:rerun-if-changed=shim/copy_window.c");
    println!("cargo:rerun-if-changed=shim/copy_window.h");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        assert_eq!(
            std::env::var("CARGO_CFG_TARGET_ARCH").as_deref(),
            Ok("x86_64"),
            "kiln-sfi supports Windows x64 only"
        );
        assert_eq!(
            std::env::var("CARGO_CFG_TARGET_ENV").as_deref(),
            Ok("msvc"),
            "SEH shim needs clang-cl/MSVC"
        );
    }
    // cargo xwin configures cc's target compiler and Windows SDK. Do not force a
    // Linux clang here or use a Linux-built object in the Windows cdylib.
    let mut build = cc::Build::new();
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        // Asynchronous SEH: without /EHa clang-cl gives a C __try whose body faults only in an inline load (the PE
        // header prefilter, the loader-list walk) no handler at all, and the access violation ends the process
        // (a probe showed no .seh_handler at /O2 without /EHa, an unhandled fault on Wine and
        // Windows). MSVC's cl gives C __try asynchronous semantics anyway; /EHa keeps both compilers the same.
        build.flag("/EHa");
    }
    build
        .file("shim/fault.c")
        .file("shim/monitor.c")
        .file("shim/bindings.c")
        .file("shim/notification_ring.c")
        .file("shim/breach_state.c")
        .file("shim/pin_state.c")
        .file("shim/seh_guard.c")
        .file("shim/api_set.c")
        .file("shim/load_window.c")
        .file("shim/owner_faults.c")
        .file("shim/pe_identity.c")
        .file("shim/copy_window.c")
        .warnings_into_errors(true)
        .compile("sfi_fault");
}
