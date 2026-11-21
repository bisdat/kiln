// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): native/steam_network.cpp:33-68
// (the mapping and its publisher), :450-482 (the reader).
//! same-PC discovery (row 45): the dedicated server publishes its Steam id in the named mapping
//! `Local\ShroudForge.Network.ServerIdentity.v1` (32 bytes, [`LocalServerIdentity`]); a client on the same PC reads
//! it as `status().local_dedicated_server_steam_id` while it is at most 10 s old.
//!
//! - Publish ([`publish_into`]): the magic is cleared first, each field written with an interlocked exchange, the
//!   magic restored last. The mapping handle is made once and never closed (ShroudForge's too), so the name lives as
//!   long as the server process.
//! - Read ([`read_from`]): a read-only view; the magic read twice around the fields (with barriers), rejected when the
//!   two differ, the magic or the size is wrong, the id or the pid is 0, or the time is in the future or more than
//!   10,000 ms old (GetTickCount64, which every process on the PC shares). No read-modify-write: the view is read-only.
//! - A server never reads it and a client never publishes it.
//!
//! The same mapping name and layout as hosted ShroudForge, so a Kiln client finds a hosted-ShroudForge server on the
//! same PC and the other way round.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering, fence};

/// The mapping's name (a session-local kernel object).
pub const MAPPING_NAME: &str = "Local\\ShroudForge.Network.ServerIdentity.v1";
pub const MAGIC: u64 = 0x5346_5745_4449_4431;
pub const MAX_AGE_MS: u64 = 10_000;

/// The mapping's 32 bytes.
#[repr(C)]
#[derive(Debug, Default)]
pub struct LocalServerIdentity {
    pub magic: AtomicU64,
    pub steam_id: AtomicU64,
    pub updated_at_ms: AtomicU64,
    pub process_id: AtomicU32,
    pub size: AtomicU32,
}
const _: () = {
    assert!(size_of::<LocalServerIdentity>() == 32);
    assert!(core::mem::offset_of!(LocalServerIdentity, steam_id) == 8);
    assert!(core::mem::offset_of!(LocalServerIdentity, updated_at_ms) == 16);
    assert!(core::mem::offset_of!(LocalServerIdentity, process_id) == 24);
    assert!(core::mem::offset_of!(LocalServerIdentity, size) == 28);
};

/// The publisher's writes into a writable view, in ShroudForge's order (`InterlockedExchange*`: full barriers).
pub fn publish_into(view: &LocalServerIdentity, steam_id: u64, now_ms: u64, process_id: u32) {
    view.magic.swap(0, Ordering::SeqCst);
    view.steam_id.swap(steam_id, Ordering::SeqCst);
    view.updated_at_ms.swap(now_ms, Ordering::SeqCst);
    view.process_id.swap(process_id, Ordering::SeqCst);
    view.size
        .swap(size_of::<LocalServerIdentity>() as u32, Ordering::SeqCst);
    view.magic.swap(MAGIC, Ordering::SeqCst);
}

/// The reader's checks over a view it may only read (steam_network.cpp:450-482).
///
/// # Safety
/// `view` points to 32 readable bytes for the call (another process may write them meanwhile).
pub unsafe fn read_from(view: *const LocalServerIdentity, now_ms: u64) -> Option<u64> {
    // Plain volatile loads, as ShroudForge's reader: aligned 8-byte loads are atomic on x64, and nothing here may write.
    let field = |offset: usize| unsafe { view.cast::<u8>().add(offset) };
    let read64 = |offset: usize| unsafe { field(offset).cast::<u64>().read_volatile() };
    let read32 = |offset: usize| unsafe { field(offset).cast::<u32>().read_volatile() };
    let magic_before = read64(0);
    fence(Ordering::SeqCst);
    let steam_id = read64(8);
    let updated_at = read64(16);
    let process_id = read32(24);
    let size = read32(28);
    fence(Ordering::SeqCst);
    let magic_after = read64(0);
    accept(
        magic_before,
        magic_after,
        steam_id,
        updated_at,
        process_id,
        size,
        now_ms,
    )
}

/// Whether one read is a fresh, whole record; its Steam id if so.
pub fn accept(
    magic_before: u64,
    magic_after: u64,
    steam_id: u64,
    updated_at_ms: u64,
    process_id: u32,
    size: u32,
    now_ms: u64,
) -> Option<u64> {
    let whole = magic_before == magic_after
        && magic_after == MAGIC
        && size as usize == size_of::<LocalServerIdentity>();
    let fresh = now_ms >= updated_at_ms && now_ms - updated_at_ms <= MAX_AGE_MS;
    (whole && steam_id != 0 && process_id != 0 && fresh).then_some(steam_id)
}

/// Where the mapping lives: the named Windows mapping in production, memory in tests.
pub trait Discovery {
    /// The server's publish (a nonzero id only).
    fn publish(&self, steam_id: u64, now_ms: u64);
    /// The client's read.
    fn read(&self, now_ms: u64) -> Option<u64>;
}

/// No same-PC discovery (a platform without named mappings: never published, never found).
pub struct NoDiscovery;
impl Discovery for NoDiscovery {
    fn publish(&self, _: u64, _: u64) {}
    fn read(&self, _: u64) -> Option<u64> {
        None
    }
}

/// The milliseconds clock the network family runs on: GetTickCount64 on Windows (shared by every process, which the
/// mapping's freshness needs); elsewhere this process's monotonic milliseconds from 1.
pub fn tick_ms() -> u64 {
    #[cfg(windows)]
    {
        windows::tick_ms()
    }
    #[cfg(not(windows))]
    {
        static ORIGIN: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
        ORIGIN
            .get_or_init(std::time::Instant::now)
            .elapsed()
            .as_millis() as u64
            + 1
    }
}

/// The named mapping (Windows): made once at the first publish and never closed.
#[cfg(windows)]
pub use windows::NamedMapping;

/// The production discovery of this platform.
pub fn production() -> std::rc::Rc<dyn Discovery> {
    #[cfg(windows)]
    {
        std::rc::Rc::new(NamedMapping::default())
    }
    #[cfg(not(windows))]
    {
        std::rc::Rc::new(NoDiscovery)
    }
}

#[cfg(windows)]
mod windows {
    use super::{Discovery, LocalServerIdentity, MAPPING_NAME, publish_into, read_from};
    use core::ffi::c_void;
    use std::sync::atomic::{AtomicUsize, Ordering};

    const PAGE_READWRITE: u32 = 0x04;
    const FILE_MAP_WRITE: u32 = 0x0002;
    const FILE_MAP_READ: u32 = 0x0004;
    const INVALID_HANDLE_VALUE: isize = -1;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn CreateFileMappingW(
            file: isize,
            attributes: *const c_void,
            protect: u32,
            size_high: u32,
            size_low: u32,
            name: *const u16,
        ) -> *mut c_void;
        fn OpenFileMappingW(access: u32, inherit: i32, name: *const u16) -> *mut c_void;
        fn MapViewOfFile(
            mapping: *mut c_void,
            access: u32,
            offset_high: u32,
            offset_low: u32,
            bytes: usize,
        ) -> *mut c_void;
        fn UnmapViewOfFile(view: *const c_void) -> i32;
        fn FlushViewOfFile(view: *const c_void, bytes: usize) -> i32;
        fn CloseHandle(handle: *mut c_void) -> i32;
        fn GetTickCount64() -> u64;
    }

    pub fn tick_ms() -> u64 {
        // SAFETY: no arguments.
        unsafe { GetTickCount64() }
    }

    fn name() -> Vec<u16> {
        MAPPING_NAME.encode_utf16().chain([0]).collect()
    }

    /// The publisher's mapping handle (0 until made; never closed).
    #[derive(Debug, Default)]
    pub struct NamedMapping(AtomicUsize);

    impl Discovery for NamedMapping {
        fn publish(&self, steam_id: u64, now_ms: u64) {
            if steam_id == 0 {
                return;
            }
            let mut handle = self.0.load(Ordering::Acquire) as *mut c_void;
            if handle.is_null() {
                let name = name();
                // SAFETY: a pagefile-backed mapping of 32 bytes under a NUL-terminated name.
                handle = unsafe {
                    CreateFileMappingW(
                        INVALID_HANDLE_VALUE,
                        core::ptr::null(),
                        PAGE_READWRITE,
                        0,
                        size_of::<LocalServerIdentity>() as u32,
                        name.as_ptr(),
                    )
                };
                if handle.is_null() {
                    return;
                }
                self.0.store(handle as usize, Ordering::Release);
            }
            // SAFETY: a live mapping handle; the view is 32 bytes.
            let view = unsafe {
                MapViewOfFile(
                    handle,
                    FILE_MAP_WRITE,
                    0,
                    0,
                    size_of::<LocalServerIdentity>(),
                )
            };
            if view.is_null() {
                return;
            }
            // SAFETY: a writable, page-aligned 32-byte view, valid until it is unmapped below.
            publish_into(
                unsafe { &*view.cast::<LocalServerIdentity>() },
                steam_id,
                now_ms,
                std::process::id(),
            );
            // SAFETY: the view mapped above.
            unsafe {
                FlushViewOfFile(view, size_of::<LocalServerIdentity>());
                UnmapViewOfFile(view);
            }
        }
        fn read(&self, now_ms: u64) -> Option<u64> {
            let name = name();
            // SAFETY: a NUL-terminated name.
            let mapping = unsafe { OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr()) };
            if mapping.is_null() {
                return None;
            }
            // SAFETY: a live mapping handle; read access only.
            let view = unsafe {
                MapViewOfFile(
                    mapping,
                    FILE_MAP_READ,
                    0,
                    0,
                    size_of::<LocalServerIdentity>(),
                )
            };
            if view.is_null() {
                // SAFETY: the handle opened above.
                unsafe { CloseHandle(mapping) };
                return None;
            }
            // SAFETY: a readable 32-byte view, unmapped right after.
            let found = unsafe { read_from(view.cast::<LocalServerIdentity>(), now_ms) };
            // SAFETY: the view and handle opened above. ShroudForge takes the time after closing; the reader here
            // takes it before mapping (its caller's `now`), which is at most this call's own duration earlier.
            unsafe {
                UnmapViewOfFile(view);
                CloseHandle(mapping);
            }
            found
        }
    }
}
