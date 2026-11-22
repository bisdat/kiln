// SPDX-License-Identifier: MIT
//! Native Windows: kiln-sfi's probe for Kiln.Ui's ready event (`Local\Kiln.Sfi.UiReady.<pid>`, which Kiln.Ui makes
//! once it is up and can draw): it looks whether the name exists, never waits on it, and reads a name another object
//! type holds as absent. Run under Wine and natively on kiln-win1.
#![cfg(windows)]

use kiln_sfi::notice_native::{kiln_ui_ready, ready_event_name};
use std::ffi::c_void;
use std::time::{Duration, Instant};

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateEventW(
        attributes: *const c_void,
        manual: i32,
        initial: i32,
        name: *const u16,
    ) -> *mut c_void;
    fn CreateMutexW(attributes: *const c_void, owner: i32, name: *const u16) -> *mut c_void;
    fn CloseHandle(handle: *mut c_void) -> i32;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

#[test]
fn the_ready_probe_sees_only_an_existing_event_and_never_waits() {
    let pid = std::process::id();
    assert_eq!(
        ready_event_name(pid),
        format!("Local\\Kiln.Sfi.UiReady.{pid}")
    );
    let name = wide(&ready_event_name(pid));
    let null = core::ptr::null();
    assert!(!kiln_ui_ready(pid), "no event: Kiln.Ui absent");

    // Kiln.Ui's own: manual reset, signalled, held for the process
    let event = unsafe { CreateEventW(null, 1, 1, name.as_ptr()) };
    assert!(!event.is_null());
    assert!(kiln_ui_ready(pid));
    assert!(!kiln_ui_ready(pid ^ 0x4000_0000), "another process's name");
    unsafe { CloseHandle(event) };
    assert!(!kiln_ui_ready(pid), "its holder closed it: gone");

    // a squatter's unsignalled event counts as present, and the probe never waits on it
    let squatter = unsafe { CreateEventW(null, 1, 0, name.as_ptr()) };
    assert!(!squatter.is_null());
    let start = Instant::now();
    for _ in 0..100 {
        assert!(kiln_ui_ready(pid));
    }
    assert!(
        start.elapsed() < Duration::from_secs(5),
        "a wait: {:?}",
        start.elapsed()
    );
    unsafe { CloseHandle(squatter) };

    // the name held by a mutex: Kiln.Ui's CreateEventW fails too, so the banner may draw (absent, never neither)
    let mutex = unsafe { CreateMutexW(null, 0, name.as_ptr()) };
    assert!(!mutex.is_null());
    assert!(!kiln_ui_ready(pid));
    unsafe { CloseHandle(mutex) };
    assert!(!kiln_ui_ready(pid));
}
