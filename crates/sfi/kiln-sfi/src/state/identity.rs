// SPDX-License-Identifier: MIT
//! Who this start is, for sfi.json's `launch`: the core's frozen kiln.launch record (never kiln/state/launch.json),
//! this process's pid and start time, its side, the booted profile, and where the state folder is. Every
//! `KilnApi` member and service table is checked against its declared size before use; a missing one leaves its field
//! at the schema's "unknown" value.

use super::reducer::{Backend, Identity, Side};
use crate::LaunchRecord;
use kiln_abi::services::kiln_launch::*;
use kiln_abi::services::kiln_profile::*;
use kiln_abi::*;
use std::path::PathBuf;

/// kiln.launch's reason code as sfi.json's `reasonCode` word.
pub fn reason_word(code: u32) -> String {
    match code as i32 {
        KILN_LAUNCH_REASON_HOSTED_DEFAULT => "hosted-default".into(),
        KILN_LAUNCH_REASON_SETTING => "setting".into(),
        KILN_LAUNCH_REASON_USE_INSTALLED => "use-installed".into(),
        KILN_LAUNCH_REASON_ENV_RUNTIME => "env-runtime".into(),
        KILN_LAUNCH_REASON_PLAN => "plan".into(),
        KILN_LAUNCH_REASON_SETTING_INVALID => "setting-invalid".into(),
        KILN_LAUNCH_REASON_INTERPRETER_HELD => "interpreter-held".into(),
        KILN_LAUNCH_REASON_PLAN_MISSING => "plan-missing".into(),
        KILN_LAUNCH_REASON_PLAN_INVALID => "plan-invalid".into(),
        KILN_LAUNCH_REASON_PLAN_STAMPED => "plan-stamped".into(),
        KILN_LAUNCH_REASON_PLAN_OTHER_GAME => "plan-other-game".into(),
        KILN_LAUNCH_REASON_SF18_VETO => "sf18-veto".into(),
        KILN_LAUNCH_REASON_SAFE_IDLE => "safe-idle".into(),
        KILN_LAUNCH_REASON_NO_MODS => "no-mods".into(),
        KILN_LAUNCH_REASON_PLUGINS_OFF => "plugins-off".into(),
        KILN_LAUNCH_REASON_PROFILE_INVALID => "profile-invalid".into(),
        KILN_LAUNCH_REASON_UNDECIDED => "undecided".into(),
        _ => format!("reason-{code:x}"),
    }
}

/// The `launch` identity of this start from the frozen record and the host.
///
/// # Safety
/// `api` passed `Boundary::enter`'s prefix checks (readable for its declared size).
pub unsafe fn read(api: *const KilnApi, record: &LaunchRecord) -> Identity {
    let service = unsafe { member_service(api) };
    let backend = match record.backend {
        Some(b) if b == KILN_LAUNCH_BACKEND_HOSTED as u32 => Backend::Hosted,
        Some(b) if b == KILN_LAUNCH_BACKEND_INSTALLED as u32 => Backend::Installed,
        Some(b) if b == KILN_LAUNCH_BACKEND_KILN as u32 => Backend::Kiln,
        _ => Backend::None,
    };
    let (reason_code, reason_text) = match record.launch_id {
        Some(_) => (
            reason_word(record.reason_code),
            Some(record.reason.clone()).filter(|r| !r.is_empty()),
        ),
        None => (
            "no-kiln-launch".to_owned(),
            Some("this core has no kiln.launch record kiln-sfi can read".to_owned()),
        ),
    };
    Identity {
        launch_id: record.launch_id,
        plan_hash: record.launch_id.map(|_| record.plan_hash),
        pid: std::process::id(),
        process_start_ms: process_start_ms(),
        side: unsafe { side(api) },
        profile: service.and_then(|s| unsafe { profile(s) }),
        backend,
        reason_code,
        reason_text,
        frozen_at_ms: record.launch_id.map(|_| record.freeze_stamp),
    }
}

type ServiceFn = unsafe extern "C" fn(KilnStr, u32) -> *const core::ffi::c_void;
type PathFn = unsafe extern "C" fn(i32, *mut core::ffi::c_char, usize, *mut usize) -> KilnResult;

/// Whether `api`'s declared size covers the member at `offset`.
unsafe fn covers(api: *const KilnApi, offset: usize) -> bool {
    !api.is_null()
        && api.is_aligned()
        && unsafe { api.cast::<u32>().read() } as usize >= offset + size_of::<usize>()
}
unsafe fn member_service(api: *const KilnApi) -> Option<ServiceFn> {
    if !unsafe { covers(api, core::mem::offset_of!(KilnApi, service)) } {
        return None;
    }
    unsafe { core::ptr::addr_of!((*api).service).read() }
}

/// KILN_SIDE_* from `KilnApi.side`, else from the executable's name (`enshrouded_server.exe` is the server).
unsafe fn side(api: *const KilnApi) -> Side {
    if unsafe { covers(api, core::mem::offset_of!(KilnApi, side)) }
        && let Some(side) = unsafe { core::ptr::addr_of!((*api).side).read() }
    {
        // SAFETY: a core member, any thread.
        return if unsafe { side() } == KILN_SIDE_SERVER as u32 {
            Side::Server
        } else {
            Side::Client
        };
    }
    let server = std::env::current_exe()
        .ok()
        .and_then(|p| {
            p.file_stem()
                .map(|s| s.to_string_lossy().to_ascii_lowercase())
        })
        .is_some_and(|stem| stem == "enshrouded_server");
    if server { Side::Server } else { Side::Client }
}

/// The booted profile's id and generation (kiln.profile v1, modes active and once); `None` otherwise.
unsafe fn profile(service: ServiceFn) -> Option<(String, u64)> {
    let name = KILN_PROFILE_SERVICE
        .strip_suffix(b"\0")
        .unwrap_or(KILN_PROFILE_SERVICE);
    let table = unsafe {
        service(
            KilnStr {
                ptr: name.as_ptr().cast(),
                len: name.len(),
            },
            KILN_PROFILE_VERSION,
        )
    }
    .cast::<KilnProfileApi>();
    if table.is_null() || !table.is_aligned() {
        return None;
    }
    // SAFETY: every table starts with its header; members are read only when the declared size covers them.
    let header = unsafe { table.cast::<KilnServiceHeader>().read() };
    if header.version < KILN_PROFILE_VERSION
        || (header.size as usize)
            < core::mem::offset_of!(KilnProfileApi, generation) + size_of::<usize>()
    {
        return None;
    }
    let (mode, id, generation) = unsafe {
        (
            core::ptr::addr_of!((*table).mode).read()?,
            core::ptr::addr_of!((*table).id).read()?,
            core::ptr::addr_of!((*table).generation).read()?,
        )
    };
    // SAFETY: core members; the strings stay valid for the life of the process (kiln_profile.h).
    let mode = unsafe { mode() } as i32;
    if mode != KILN_PROFILE_ACTIVE && mode != KILN_PROFILE_ONCE {
        return None;
    }
    let id = unsafe { id() };
    if id.ptr.is_null() || id.len == 0 || id.len > 256 {
        return None;
    }
    let bytes = unsafe { std::slice::from_raw_parts(id.ptr.cast::<u8>(), id.len) };
    Some((std::str::from_utf8(bytes).ok()?.to_owned(), unsafe {
        generation()
    }))
}

/// `<game>/kiln/state`: `KilnApi.path(KILN_PATH_STATE)`, else (an older core) `path(KILN_PATH_KILN)` + `state`.
///
/// # Safety
/// As [`read`].
pub unsafe fn state_dir(api: *const KilnApi) -> Option<PathBuf> {
    unsafe { api_path(api, KILN_PATH_STATE) }
        .or_else(|| unsafe { api_path(api, KILN_PATH_KILN) }.map(|kiln| kiln.join("state")))
}

/// `KilnApi.path(kind)` (KILN_PATH_*) as a path; `None` when the core has no such member or
/// cannot answer.
///
/// # Safety
/// As [`read`].
pub unsafe fn api_path(api: *const KilnApi, kind: i32) -> Option<PathBuf> {
    if !unsafe { covers(api, core::mem::offset_of!(KilnApi, path)) } {
        return None;
    }
    let path: PathFn = unsafe { core::ptr::addr_of!((*api).path).read() }?;
    let mut buffer = vec![0u8; 1024];
    let mut len = 0usize;
    // SAFETY: a core member; the buffer is live for `capacity` bytes.
    let mut result = unsafe { path(kind, buffer.as_mut_ptr().cast(), buffer.len(), &mut len) };
    if result == KILN_ERR_BUFFER_TOO_SMALL && len < 1 << 16 {
        buffer.resize(len + 1, 0);
        result = unsafe { path(kind, buffer.as_mut_ptr().cast(), buffer.len(), &mut len) };
    }
    if result != KILN_OK || len == 0 || len > buffer.len() {
        return None;
    }
    buffer.truncate(len);
    String::from_utf8(buffer).ok().map(PathBuf::from)
}

/// This process's start, milliseconds since 1970: the identity kiln-manage's StartIdentity pins with the pid.
#[cfg(windows)]
pub fn process_start_ms() -> u64 {
    #[repr(C)]
    #[derive(Default)]
    struct FileTime {
        low: u32,
        high: u32,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> *mut core::ffi::c_void;
        fn GetProcessTimes(
            process: *mut core::ffi::c_void,
            creation: *mut FileTime,
            exit: *mut FileTime,
            kernel: *mut FileTime,
            user: *mut FileTime,
        ) -> i32;
    }
    let (mut creation, mut exit, mut kernel, mut user) = (
        FileTime::default(),
        FileTime::default(),
        FileTime::default(),
        FileTime::default(),
    );
    // SAFETY: the pseudo handle needs no closing; the out pointers are live.
    if unsafe {
        GetProcessTimes(
            GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        )
    } == 0
    {
        return fallback_start_ms();
    }
    // 100 ns ticks since 1601-01-01 to ms since 1970-01-01
    let ticks = ((creation.high as u64) << 32) | creation.low as u64;
    (ticks / 10_000).saturating_sub(11_644_473_600_000)
}
/// Test hosts: when kiln-sfi first asked (no ShroudForge runs outside Windows).
#[cfg(not(windows))]
pub fn process_start_ms() -> u64 {
    fallback_start_ms()
}
fn fallback_start_ms() -> u64 {
    static FIRST: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    *FIRST.get_or_init(super::now_ms)
}
