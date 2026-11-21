// SPDX-License-Identifier: GPL-3.0-or-later
// kiln-eml's own test of mod-loader-base's Wine containment check (registry/fs.rs `wine_within`).
//! A folder link in a disk mod that leads out of it is refused, natively (a junction, which canonicalize resolves) and
//! under Wine (a Unix link, which every open follows and canonicalize keeps in the path), and a plain folder still reads.
//! Outside `src`, so the gate scan of mod-loader-lua's test binary (processes created through `test_gate::spawn`) does
//! not apply: this is a test binary of its own, and it makes its links with cmd, ln and rm.
#![cfg(windows)]
use mod_loader_base::FileSystem;
use std::io::{ErrorKind, Read};

/// A folder link at `link` to `target`: a Unix symbolic link under Wine (made by ln, which Wine starts but gives
/// no handle to wait on, so the link is waited for: a must-happen wait, 300 s bound), a junction natively.
/// Returns the link's Unix name under Wine (remove it with `remove_link`), `None` natively.
fn folder_link(target: &std::path::Path, link: &std::path::Path) -> Option<String> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleW(name: *const u16) -> *mut core::ffi::c_void;
        fn GetProcAddress(module: *mut core::ffi::c_void, name: *const core::ffi::c_char) -> *mut core::ffi::c_void;
        fn GetFileAttributesW(name: *const u16) -> u32;
    }
    let wide = |p: &std::path::Path| -> Vec<u16> { p.as_os_str().encode_wide().chain([0]).collect() };
    let kernel32 = wide(std::path::Path::new("kernel32.dll"));
    // SAFETY: NUL-terminated names; kernel32 is loaded in every Win32 process.
    let f = unsafe { GetProcAddress(GetModuleHandleW(kernel32.as_ptr()), c"wine_get_unix_file_name".as_ptr()) };
    if f.is_null() {
        let status = std::process::Command::new("cmd").arg("/C").arg("mklink").arg("/J").arg(link).arg(target).status().unwrap();
        assert!(status.success() && link.is_dir(), "junction {}", link.display());
        return None;
    }
    // SAFETY: Wine's export: char *CDECL wine_get_unix_file_name(LPCWSTR). The test leaks the strings.
    let f: unsafe extern "C" fn(*const u16) -> *const core::ffi::c_char = unsafe { std::mem::transmute(f) };
    let unix = |p: &std::path::Path| -> String {
        let name = wide(p);
        let r = unsafe { f(name.as_ptr()) };
        assert!(!r.is_null(), "no Unix name for {}", p.display());
        unsafe { std::ffi::CStr::from_ptr(r) }.to_str().unwrap().to_owned()
    };
    let parent = unix(link.parent().unwrap());
    let name = link.file_name().unwrap().to_str().unwrap();
    let unix_link = format!("{}/{name}", parent.trim_end_matches('/'));
    let _ = std::process::Command::new("/usr/bin/ln").arg("-s").arg(unix(target)).arg(&unix_link).spawn();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
    let link_name = wide(link);
    // SAFETY: a NUL-terminated name. Wine shows a link to a folder as a reparse point by name.
    while unsafe { GetFileAttributesW(link_name.as_ptr()) } != 0x410 {
        assert!(std::time::Instant::now() < deadline, "ln made no link");
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    Some(unix_link)
}
/// Removes a link `folder_link` made: the link itself (by its Unix name under Wine), never what it points at.
fn remove_link(link: &std::path::Path, unix_link: Option<String>) {
    match unix_link {
        Some(unix_link) => {
            let _ = std::process::Command::new("/usr/bin/rm").arg(unix_link).spawn();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
            while std::fs::symlink_metadata(link).is_ok() {
                assert!(std::time::Instant::now() < deadline, "rm left the link");
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        }
        None => std::fs::remove_dir(link).unwrap(),
    }
}

#[test]
fn a_folder_link_out_of_the_mod_is_refused() {
    let base = std::env::temp_dir().join(format!("mlb-wine-{}", std::process::id()));
    let (root, outside) = (base.join("mod"), base.join("outside"));
    std::fs::create_dir_all(root.join("inner")).unwrap();
    std::fs::create_dir_all(&outside).unwrap();
    std::fs::write(root.join("inner/g.txt"), "in").unwrap();
    std::fs::write(outside.join("f.txt"), "out").unwrap();
    let link = folder_link(&outside, &root.join("out"));
    assert_eq!(std::fs::read(root.join("out/f.txt")).unwrap(), b"out", "the link is real");
    let canonical = camino::Utf8PathBuf::from_path_buf(root.canonicalize().unwrap()).unwrap();
    let mut fs = FileSystem::new_disk(&canonical).unwrap();
    let mut text = String::new();
    fs.read_file("inner/g.txt").unwrap().read_to_string(&mut text).unwrap();
    assert_eq!(text, "in");
    let refused = fs.read_file("out/f.txt").map(|_| ()).map_err(|e| e.kind());
    assert_eq!(refused, Err(ErrorKind::PermissionDenied), "wine: {}", link.is_some());
    remove_link(&root.join("out"), link);
    assert_eq!(std::fs::read(outside.join("f.txt")).unwrap(), b"out");
    std::fs::remove_dir_all(&base).unwrap();
}
