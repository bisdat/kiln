// SPDX-License-Identifier: MIT
use std::{fs, path::Path};
fn check(path: &Path) {
    if path.is_dir() {
        for entry in fs::read_dir(path).unwrap() {
            check(&entry.unwrap().path());
        }
        return;
    }
    if path.extension().is_some_and(|ext| ext == "rs") {
        let text = fs::read_to_string(path).unwrap();
        for token in [
            "kiln_abi",
            "libloading",
            "extern \"C\"",
            "GetProcAddress",
            "LoadLibrary",
            "std::thread",
            "unsafe {",
            "shroudforge/config",
        ] {
            assert!(
                !text.contains(token),
                "{}: forbidden boundary token {token}",
                path.display()
            );
        }
    }
}
#[test]
fn production_lua_and_host_inputs_have_no_ffi_loader_or_worker_implementation() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    check(&root.join("src/host.rs"));
    check(&root.join("src/lua"));
}
