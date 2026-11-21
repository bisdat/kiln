// SPDX-License-Identifier: MIT
// the interpreter's live reload/unload transport, Windows tests.
//! Native Windows (and Wine): the request file written by another process while kiln-sfi reads it, and the result
//! file replaced while another process holds it open. Behaviour mirrored from ShroudForge (MIT, Copyright (c) 2026
//! bonsaibauer): modules/loader-config/write_json_atomically.rs:4-25 (the requester and the runtime both replace
//! the documents with a temp file and a rename, retrying a refused rename 5 times). The child processes are this
//! test binary again (`--ignored --exact`), told their part through the environment.
#![cfg(windows)]

use kiln_sfi::runtime::reload::{REQUEST_FILE, RESULT_FILE, read_request, write_result};
use std::os::windows::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

const ROLE: &str = "KILN_SFI_RL1_ROLE";
const DIR: &str = "KILN_SFI_RL1_DIR";
/// A bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);
const WRITES: usize = 2000;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleA(name: *const u8) -> *mut core::ffi::c_void;
    fn GetProcAddress(module: *mut core::ffi::c_void, name: *const u8) -> *mut core::ffi::c_void;
}
/// Wine (and Proton) refuse a replacing rename while any handle is open on the target, delete sharing or not;
/// native Windows allows it with delete sharing. Under Wine the requester's and the writer's retries cover it.
fn under_wine() -> bool {
    // SAFETY: plain lookups of a loaded module and an export by NUL-terminated name.
    unsafe {
        let ntdll = GetModuleHandleA(c"ntdll.dll".as_ptr().cast());
        !ntdll.is_null() && !GetProcAddress(ntdll, c"wine_get_version".as_ptr().cast()).is_null()
    }
}

const FILE_SHARE_READ: u32 = 1;
const FILE_SHARE_WRITE: u32 = 2;
const FILE_SHARE_DELETE: u32 = 4;

fn fresh(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("kiln-sfi-rl1-win-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn child(test: &str, role: &str, dir: &Path) -> Child {
    Command::new(std::env::current_exe().unwrap())
        .args([
            "--ignored",
            "--exact",
            test,
            "--nocapture",
            "--test-threads=1",
        ])
        .env(ROLE, role)
        .env(DIR, dir)
        .spawn()
        .unwrap()
}
fn wait_for(path: &Path) -> bool {
    let start = Instant::now();
    while start.elapsed() < WAIT {
        if path.exists() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    false
}
fn request(i: usize) -> Vec<u8> {
    // a padded document, so a torn read could not parse as a smaller one by chance
    serde_json::to_vec_pretty(&serde_json::json!({
        "requestId": format!("w{i}"), "operation": "reload", "modIds": ["a", "b"], "pad": "x".repeat(4096)
    }))
    .unwrap()
}

/// The requester process: replaces the request ShroudForge's way (temp file, then a rename with its retries)
/// WRITES times; every refused rename is counted in `refused`.
#[test]
#[ignore = "a child process of a_request_replaced_by_another_process_is_read_whole_or_not_at_all"]
fn child_requester() {
    if std::env::var(ROLE).as_deref() != Ok("requester") {
        return;
    }
    let dir = PathBuf::from(std::env::var_os(DIR).unwrap());
    let mut refused = 0usize;
    for i in 0..WRITES {
        let temp = dir.join(format!("request-{i}.tmp"));
        std::fs::write(&temp, request(i)).unwrap();
        let mut done = false;
        for attempt in 0..5u32 {
            match std::fs::rename(&temp, dir.join(REQUEST_FILE)) {
                Ok(()) => {
                    done = true;
                    break;
                }
                Err(_) => {
                    refused += 1;
                    std::thread::sleep(Duration::from_millis(10 << attempt));
                }
            }
        }
        assert!(done, "write {i} was never placed");
    }
    std::fs::write(dir.join("refused"), refused.to_string()).unwrap();
    std::fs::write(dir.join("done"), b"").unwrap();
}

/// kiln-sfi's IO-thread read against a requester process replacing the file as fast as it can: every read is a
/// whole document or nothing, never a torn one, and the reader never refuses the requester's replace.
#[test]
fn a_request_replaced_by_another_process_is_read_whole_or_not_at_all() {
    let dir = fresh("request");
    let path = dir.join(REQUEST_FILE);
    let mut writer = child("child_requester", "requester", &dir);
    let (mut whole, mut nothing) = (0usize, 0usize);
    let mut last = 0usize;
    while !dir.join("done").exists() {
        match read_request(&path) {
            Some((bytes, value)) => {
                let id = value["requestId"].as_str().expect("a whole document");
                let i: usize = id[1..].parse().unwrap();
                assert_eq!(bytes, request(i), "a whole document");
                assert!(i >= last, "never an older document after a newer one");
                last = i;
                whole += 1;
            }
            None => nothing += 1,
        }
    }
    assert!(writer.wait().unwrap().success());
    let refused: usize = std::fs::read_to_string(dir.join("refused"))
        .unwrap()
        .parse()
        .unwrap();
    println!(
        "renames refused and retried: {refused} (wine: {})",
        under_wine()
    );
    if !under_wine() {
        assert_eq!(
            refused, 0,
            "the reader never refused the requester's replace"
        );
    }
    let (_, value) = read_request(&path).expect("the last request");
    assert_eq!(value["requestId"], format!("w{}", WRITES - 1));
    assert!(whole > 0, "whole {whole}, nothing {nothing}");
    println!("reads: whole {whole}, nothing {nothing}");
    let _ = std::fs::remove_dir_all(&dir);
}

/// The holder process: opens the result with the sharing it is told, says so, waits for "go".
#[test]
#[ignore = "a child process of the_result_is_replaced_while_another_process_holds_it"]
fn child_holder() {
    let share = match std::env::var(ROLE).as_deref() {
        Ok("holder-delete") => FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        Ok("holder-read") => FILE_SHARE_READ,
        _ => return,
    };
    let dir = PathBuf::from(std::env::var_os(DIR).unwrap());
    let file = std::fs::OpenOptions::new()
        .read(true)
        .share_mode(share)
        .open(dir.join(RESULT_FILE))
        .unwrap();
    std::fs::write(dir.join("ready"), b"").unwrap();
    assert!(wait_for(&dir.join("go")));
    drop(file);
}

/// The result is replaced while a reader process holds the old one with delete sharing (a launcher reading it
/// with Rust's or .NET's defaults). A reader without delete sharing makes ShroudForge's write fail after its
/// retries: the old file stays whole, no temp file is left, and the next write succeeds once the reader is gone.
#[test]
fn the_result_is_replaced_while_another_process_holds_it() {
    // native: delete sharing lets the replace through; Wine refuses both (see under_wine)
    for (role, replaced) in [("holder-delete", !under_wine()), ("holder-read", false)] {
        let dir = fresh(role);
        std::fs::write(dir.join(RESULT_FILE), b"old").unwrap();
        let mut holder = child("child_holder", role, &dir);
        assert!(wait_for(&dir.join("ready")), "{role}");
        let started = Instant::now();
        let result = write_result(&dir, b"new");
        let took = started.elapsed();
        // let the holder go first, so a failed assertion below never leaves it waiting
        std::fs::write(dir.join("go"), b"").unwrap();
        assert!(holder.wait().unwrap().success(), "{role}");
        assert_eq!(result.is_ok(), replaced, "{role}: {result:?}");
        if !replaced {
            assert_eq!(
                result.unwrap_err().kind(),
                std::io::ErrorKind::PermissionDenied,
                "{role}"
            );
            assert!(
                took >= Duration::from_millis(150),
                "{role}: the retries ran ({took:?})"
            );
        }
        let expected: &[u8] = if replaced { b"new" } else { b"old" };
        assert_eq!(
            std::fs::read(dir.join(RESULT_FILE)).unwrap(),
            expected,
            "{role}"
        );
        let left: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .filter(|n| n.ends_with(".tmp"))
            .collect();
        assert!(left.is_empty(), "{role}: {left:?}");
        write_result(&dir, b"after").unwrap();
        assert_eq!(
            std::fs::read(dir.join(RESULT_FILE)).unwrap(),
            b"after",
            "{role}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
