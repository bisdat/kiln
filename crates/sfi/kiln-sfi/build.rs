// SPDX-License-Identifier: MIT
fn main() {
    build_stamp();
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

/// kiln_build_stamp() (src/lib.rs): "<VERSION><KILN_BUILD>", byte for byte the stamp native/cmake/kiln_build_stamp.cpp
/// gives the native DLLs of the same build: the repository's VERSION (its first line, as native/CMakeLists.txt reads it)
/// and KILN_BUILD, the build metadata that build's native/cmake/kiln_build_stamp.sh generated ("+<yyyymmdd>.<git>" or
/// "+nogit.<nonce>"; build.sh and tools/package/stage.sh pass it). Without KILN_BUILD the DLL exports the version only,
/// like a native DLL built without build metadata: no core trusts it.
fn build_stamp() {
    let manifest = std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let version_file = std::path::Path::new(&manifest).join("../../../VERSION");
    println!("cargo:rerun-if-changed={}", version_file.display());
    println!("cargo:rerun-if-env-changed=KILN_BUILD");
    let build = std::env::var("KILN_BUILD").unwrap_or_default();
    let stamp_char = |c: char| c.is_ascii_alphanumeric() || c == '.' || c == '-';
    assert!(
        build.is_empty() || (build.len() > 1 && build.starts_with('+') && build[1..].chars().all(stamp_char)),
        "KILN_BUILD must be empty or +[0-9A-Za-z.-]+ (native/cmake/kiln_build_stamp.sh), found {build:?}"
    );
    // A checkout without VERSION (a scratch copy of the crate) builds an unstamped DLL; a stamped build needs it.
    let version = match std::fs::read_to_string(&version_file) {
        Ok(text) => text.lines().next().unwrap_or("").trim().to_string(),
        Err(e) if build.is_empty() => {
            println!("cargo:warning=kiln_build_stamp: no {} ({e}): the DLL exports no version", version_file.display());
            String::new()
        }
        Err(e) => panic!("KILN_BUILD is set but {} cannot be read: {e}", version_file.display()),
    };
    assert!(
        version.chars().all(|c| stamp_char(c) || c == '+'),
        "VERSION must hold a semantic version, found {version:?}"
    );
    println!("cargo:rustc-env=KILN_SFI_BUILD_STAMP={version}{build}");
}
