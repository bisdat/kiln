// SPDX-License-Identifier: MIT
//! Host fakes for the interpreter worker's tests: a `kiln.tasks` pool that runs
//! each accepted body on a real thread, with the cancel, drain and exit-park rules
//! of `native/core/abi/services/kiln_tasks.h` and `docs/kiln/services/tasks.md`,
//! and a `KilnApi` whose log lines are captured per owner.
//!
//! The C entry points carry no context pointer, so every fake pool registers
//! under its own owner id; tests take fresh ids from [`next_owner`] and run in
//! parallel.
#![allow(dead_code)]

use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_launch::*;
use kiln_abi::services::kiln_profile::*;
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, LazyLock, Mutex};
use std::thread::ThreadId;
use std::time::{Duration, Instant};

static OWNERS: AtomicU64 = AtomicU64::new(1000);
pub fn next_owner() -> KilnOwner {
    OWNERS.fetch_add(1, Ordering::Relaxed)
}

// ---- log capture ----

type LogLines = HashMap<KilnOwner, Vec<(i32, String)>>;
static LOGS: LazyLock<Mutex<LogLines>> = LazyLock::new(Default::default);
unsafe extern "C" fn log(owner: KilnOwner, level: i32, text: KilnStr) {
    let bytes = unsafe { std::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) };
    LOGS.lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push((level, String::from_utf8_lossy(bytes).into_owned()));
}
pub fn logs(owner: KilnOwner) -> Vec<(i32, String)> {
    LOGS.lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

// ---- KilnApi ----

// Capability reports, captured per owner as (name, text).
type Reports = HashMap<KilnOwner, Vec<(String, String)>>;
static REPORTS: LazyLock<Mutex<Reports>> = LazyLock::new(Default::default);
unsafe extern "C" fn report(
    owner: KilnOwner,
    capability: KilnStr,
    _: u32,
    _: u32,
    text: KilnStr,
) -> KilnResult {
    REPORTS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push((name(capability), name(text)));
    KILN_OK
}
pub fn reports(owner: KilnOwner) -> Vec<(String, String)> {
    REPORTS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}
static CAPS: KilnCapsApi = KilnCapsApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnCapsApi>() as u32,
        version: 1,
    },
    count: None,
    at: None,
    find: None,
    report: Some(report),
    subscribe: None,
    unsubscribe: None,
    generation: None,
};
fn name(text: KilnStr) -> String {
    let bytes = unsafe { std::slice::from_raw_parts(text.ptr.cast::<u8>(), text.len) };
    String::from_utf8_lossy(bytes).into_owned()
}
unsafe extern "C" fn service_full(text: KilnStr, _: u32) -> *const c_void {
    match name(text).as_str() {
        "kiln.tasks" => (&TASKS as *const KilnTasksApi).cast(),
        "kiln.caps" => (&CAPS as *const KilnCapsApi).cast(),
        "kiln.launch" => launch_table(),
        "kiln.profile" => profile_table(),
        _ => std::ptr::null(),
    }
}
unsafe extern "C" fn service_no_tasks(text: KilnStr, _: u32) -> *const c_void {
    match name(text).as_str() {
        "kiln.caps" => (&CAPS as *const KilnCapsApi).cast(),
        "kiln.launch" => launch_table(),
        "kiln.profile" => profile_table(),
        _ => std::ptr::null(),
    }
}

// ---- kiln.launch: the frozen record the calling thread set ----

thread_local! {
    // (backend, reason code); None: an older core without the table
    static LAUNCH: Cell<Option<(i32, i32)>> = const { Cell::new(None) };
}
pub const LAUNCH_REASON: &str = "the reason the core froze";
/// What kiln.launch serves to lookups from this thread (the boundary reads it on
/// the thread that enters): a frozen v1 record, or no table at all.
pub fn set_launch(record: Option<(i32, i32)>) {
    LAUNCH.set(record);
}
thread_local! {
    // The record's native_policy and build_verified, and build_info's verified
    static NATIVE: Cell<(u32, u32, bool)> = const { Cell::new((0, 0, false)) };
}
/// Kiln's native.policy as this thread's fake core serves it: the record's `native_policy`
/// (KILN_LAUNCH_NATIVE_POLICY_*) and `build_verified`, and `build_info().verified`.
pub fn set_native(policy: u32, build_verified: u32, info_verified: bool) {
    NATIVE.set((policy, build_verified, info_verified));
}
unsafe extern "C" fn launch_status(out: *mut KilnLaunchStatus) -> KilnResult {
    let Some((backend, code)) = LAUNCH.get() else {
        return KILN_ERR_UNAVAILABLE;
    };
    let out = unsafe { &mut *out };
    if out.size < std::mem::size_of::<KilnLaunchStatus>() as u32 {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    out.backend = backend as u32;
    out.frozen = 1;
    out.reason_code = code as u32;
    out.launch_id = 0x0123_4567_89ab_cdef;
    out.plan_hash = frozen_hash();
    (out.native_policy, out.build_verified, _) = NATIVE.get();
    out.reason = [0; 128];
    for (i, b) in LAUNCH_REASON.bytes().enumerate() {
        out.reason[i] = b as core::ffi::c_char;
    }
    KILN_OK
}
static LAUNCH_API: KilnLaunchApi = KilnLaunchApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnLaunchApi>() as u32,
        version: 1,
    },
    status: Some(launch_status),
    notice_copy: None,
};
fn launch_table() -> *const c_void {
    match LAUNCH.get() {
        Some(_) => (&LAUNCH_API as *const KilnLaunchApi).cast(),
        None => std::ptr::null(),
    }
}

// ---- the launch plan the record froze, the profile that names it, build_info ----

/// The executable hash the fake build_info serves (a plan must carry it).
pub const FAKE_EXE: &str = "ab4c0ffee0ab4c0ffee0ab4c0ffee0ab4c0ffee0ab4c0ffee0ab4c0ffee0ab4c";
/// The plan id of the default plan.
pub const DEFAULT_PLAN_ID: &str = "0123abcd-0000-4000-8000-000000000001";

thread_local! {
    /// The game folder the fake core names on this thread (made on first use).
    static GAME_DIR: RefCell<Option<PathBuf>> = const { RefCell::new(None) };
    /// The plan bytes the fake profile names on this thread; unset: an empty plan for the client.
    static PLAN: RefCell<Option<Vec<u8>>> = const { RefCell::new(None) };
    /// A frozen plan hash other than the plan's own (a plan that changed after the freeze).
    static FROZEN: Cell<Option<[u8; 32]>> = const { Cell::new(None) };
    /// The profile text kiln.profile serves; unset: one naming the plan.
    static PROFILE: RefCell<Option<String>> = const { RefCell::new(None) };
    static PROFILE_TEXT: RefCell<String> = const { RefCell::new(String::new()) };
    /// KILN_SIDE_* of this process.
    static SIDE: Cell<u32> = const { Cell::new(KILN_SIDE_CLIENT as u32) };
}

/// A launch plan the strict reader accepts: no packages, this fake's executable, `side`.
pub fn plan_document(side: &str) -> serde_json::Value {
    let h = "a".repeat(64);
    serde_json::json!({"format":1,"minReader":1,"plan_id":DEFAULT_PLAN_ID,"launch_id":null,
        "startupDialect":"none","writer":{"component":"kiln-manage","version":"0.1.0"},
        "profile":{"id":"test","generation":1,"mode":"active","semantic_sha256":h},
        "game":{"side":side,"exe_sha256":FAKE_EXE,"archiveBase_sha256":h,"archiveInput_sha256":h},
        "backend":{"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml","selectionReason":"test"},
        "contract":{"apiVersion":"1.7.9","sourceCommit":"b".repeat(40),"upstream180Final":false,"inventory_sha256":h,"parity_sha256":h,"serviceContracts_sha256":h},
        "fingerprints":{"packages_sha256":h,"settings_sha256":h,"chain_sha256":h,"loaderInventory_sha256":h,"compat_sha256":h,"releasePolicy_sha256":h,"placementReceipts_sha256":h},
        "packages":[],"loadOrder":[],"warnings":[]})
}
/// The game folder the fake core names on this thread from now on (`<game>/kiln/launch-plans` is made).
pub fn set_game_dir(game: PathBuf) {
    std::fs::create_dir_all(game.join("kiln").join("launch-plans")).unwrap();
    GAME_DIR.set(Some(game));
}
/// The game folder the fake core names on this thread (`<game>/kiln/launch-plans` holds the plan).
pub fn game_dir() -> PathBuf {
    GAME_DIR.with_borrow_mut(|dir| {
        dir.get_or_insert_with(|| {
            let game = fresh_dir("game");
            std::fs::create_dir_all(game.join("kiln").join("launch-plans")).unwrap();
            game
        })
        .clone()
    })
}
fn plan_bytes() -> Vec<u8> {
    PLAN.with_borrow(Clone::clone).unwrap_or_else(|| {
        let side = if SIDE.get() == KILN_SIDE_SERVER as u32 {
            "server"
        } else {
            "client"
        };
        serde_json::to_vec_pretty(&plan_document(side)).unwrap()
    })
}
/// The plan id the plan bytes carry (the default one when they carry none).
fn plan_id(bytes: &[u8]) -> String {
    serde_json::from_slice::<serde_json::Value>(bytes)
        .ok()
        .and_then(|v| v["plan_id"].as_str().map(str::to_owned))
        .unwrap_or_else(|| DEFAULT_PLAN_ID.to_owned())
}
/// `<game>/kiln/launch-plans/<plan id>.json` on this thread.
pub fn plan_path() -> PathBuf {
    game_dir()
        .join("kiln")
        .join("launch-plans")
        .join(format!("{}.json", plan_id(&plan_bytes())))
}
fn write_plan() {
    std::fs::write(plan_path(), plan_bytes()).unwrap();
}
/// The plan the profile names on this thread, written to its file now (`None`: the default one).
/// The record freezes its hash.
pub fn set_plan(bytes: Option<Vec<u8>>) {
    PLAN.set(bytes);
    write_plan();
}
/// The record freezes this hash instead of the plan's (`None`: the plan's).
pub fn set_frozen_hash(hash: Option<[u8; 32]>) {
    FROZEN.set(hash);
}
/// kiln.profile's text on this thread (`None`: one naming the plan).
pub fn set_profile(text: Option<String>) {
    PROFILE.set(text);
}
/// This process's side on this thread (KILN_SIDE_*): build_info and `KilnApi.side` say it.
pub fn set_side(server: bool) {
    SIDE.set(if server {
        KILN_SIDE_SERVER
    } else {
        KILN_SIDE_CLIENT
    } as u32);
}
fn frozen_hash() -> [u8; 32] {
    FROZEN.get().unwrap_or_else(|| {
        if !plan_path().exists() {
            write_plan();
        }
        sfi_policy::digest::sha256(&plan_bytes())
    })
}
fn profile_text() -> String {
    PROFILE.with_borrow(Clone::clone).unwrap_or_else(|| {
        let bytes = plan_bytes();
        let hash: String = sfi_policy::digest::sha256(&bytes)
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        format!(
            r#"{{"id":"test","generation":1,"sfLaunchPlan":{{"path":"launch-plans/{}.json","sha256":"{hash}"}}}}"#,
            plan_id(&bytes)
        )
    })
}
unsafe extern "C" fn profile_mode() -> u32 {
    KILN_PROFILE_ACTIVE as u32
}
unsafe extern "C" fn profile_id() -> KilnStr {
    KilnStr {
        ptr: "test".as_ptr().cast(),
        len: 4,
    }
}
unsafe extern "C" fn profile_generation() -> u64 {
    1
}
unsafe extern "C" fn profile_json() -> KilnStr {
    // kiln.profile's strings live for the process; this thread's copy lives until the next call
    PROFILE_TEXT.with_borrow_mut(|text| {
        *text = profile_text();
        KilnStr {
            ptr: text.as_ptr().cast(),
            len: text.len(),
        }
    })
}
static PROFILE_API: KilnProfileApi = KilnProfileApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnProfileApi>() as u32,
        version: 1,
    },
    mode: Some(profile_mode),
    id: Some(profile_id),
    name: None,
    generation: Some(profile_generation),
    count: None,
    get: None,
    dropins: None,
    config_dir: None,
    data_dir: None,
    json: Some(profile_json),
};
fn profile_table() -> *const c_void {
    (&PROFILE_API as *const KilnProfileApi).cast()
}
unsafe extern "C" fn build_info(out: *mut KilnBuildInfo) -> KilnResult {
    let out = unsafe { &mut *out };
    if (out.size as usize) < std::mem::size_of::<KilnBuildInfo>() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    out.side = SIDE.get();
    out.verified = u8::from(NATIVE.get().2);
    out.exe_sha256 = [0; 65];
    for (i, b) in FAKE_EXE.bytes().enumerate() {
        out.exe_sha256[i] = b as core::ffi::c_char;
    }
    KILN_OK
}
unsafe extern "C" fn side() -> u32 {
    SIDE.get()
}

// ---- tick and world subscriptions ----

#[derive(Clone, Copy)]
enum Sub {
    Tick(
        unsafe extern "C" fn(*const KilnTickInfo, *mut c_void),
        usize,
    ),
    World(
        unsafe extern "C" fn(*const KilnWorldEvent, *mut c_void),
        usize,
    ),
}
static SUBS: LazyLock<Mutex<HashMap<KilnOwner, Vec<Sub>>>> = LazyLock::new(Default::default);
static UNSUBSCRIBES: AtomicU64 = AtomicU64::new(0);
static HANDLES: AtomicU64 = AtomicU64::new(1);
unsafe extern "C" fn subscribe_tick(
    owner: KilnOwner,
    _: i32,
    function: KilnTickFn,
    user: *mut c_void,
    out: *mut KilnHandle,
) -> KilnResult {
    let Some(function) = function else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    SUBS.lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push(Sub::Tick(function, user as usize));
    if !out.is_null() {
        unsafe { *out = HANDLES.fetch_add(1, Ordering::SeqCst) };
    }
    KILN_OK
}
unsafe extern "C" fn subscribe_world(
    owner: KilnOwner,
    function: KilnWorldEventFn,
    user: *mut c_void,
    out: *mut KilnHandle,
) -> KilnResult {
    let Some(function) = function else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    SUBS.lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push(Sub::World(function, user as usize));
    if !out.is_null() {
        unsafe { *out = HANDLES.fetch_add(1, Ordering::SeqCst) };
    }
    KILN_OK
}
unsafe extern "C" fn unsubscribe(_: KilnHandle) -> KilnResult {
    UNSUBSCRIBES.fetch_add(1, Ordering::SeqCst);
    KILN_OK
}
/// (tick subscriptions, world subscriptions) of `owner`.
pub fn subscriptions(owner: KilnOwner) -> (usize, usize) {
    let subs = SUBS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default();
    let ticks = subs.iter().filter(|s| matches!(s, Sub::Tick(..))).count();
    (ticks, subs.len() - ticks)
}
/// Unsubscribe calls by anyone (kiln-sfi never makes one).
pub fn unsubscribes() -> u64 {
    UNSUBSCRIBES.load(Ordering::SeqCst)
}
/// Runs `owner`'s tick callbacks once, as the tick thread would.
pub fn fire_tick(owner: KilnOwner) {
    let subs = SUBS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default();
    let info = KilnTickInfo {
        size: std::mem::size_of::<KilnTickInfo>() as u32,
        world_kind: KILN_WORLD_SERVER as u32,
        step: 0,
        clock: 0,
        clock_delta: 0,
        clock_period: 0,
        wall_delta_ms: 16.0,
    };
    for sub in subs {
        if let Sub::Tick(function, user) = sub {
            unsafe { function(&info, user as *mut c_void) };
        }
    }
}
/// Delivers one world event to `owner`'s world subscribers.
pub fn fire_world(owner: KilnOwner, event: i32, kind: i32, session: u32) {
    let subs = SUBS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default();
    let record = KilnWorldEvent {
        size: std::mem::size_of::<KilnWorldEvent>() as u32,
        event: event as u32,
        kind: kind as u32,
        session,
        world: 0x1000,
    };
    for sub in subs {
        if let Sub::World(function, user) = sub {
            unsafe { function(&record, user as *mut c_void) };
        }
    }
}

// ---- sessions and post_world (hops) ----

/// `KilnApi.session` has no owner argument, so the fake session table is global:
/// tests that change it hold [`session_lock`] for their whole run.
static SESSION: Mutex<[(usize, u32); 3]> = Mutex::new([(0, 0); 3]);
static SESSION_LOCK: Mutex<()> = Mutex::new(());
pub fn session_lock() -> std::sync::MutexGuard<'static, ()> {
    SESSION_LOCK.lock().unwrap_or_else(|p| p.into_inner())
}
/// World `kind` runs session `session` (0 = no live world).
pub fn set_session(kind: i32, session: u32) {
    SESSION.lock().unwrap()[kind as usize] = (if session == 0 { 0 } else { 0x1000 }, session);
}
/// The side the fake `KilnApi.session` reports (process-wide like its table): the server by default.
static SESSION_SIDE: AtomicU32 = AtomicU32::new(KILN_SIDE_SERVER as u32);
/// The process side `KilnApi.session` reports, and from which, with the live worlds, it derives
/// `state` as the core does (tick.cpp session()). Hold [`session_lock`] while it is not the server.
pub fn set_session_side(server: bool) {
    SESSION_SIDE.store(
        if server {
            KILN_SIDE_SERVER
        } else {
            KILN_SIDE_CLIENT
        } as u32,
        Ordering::SeqCst,
    );
}
static LIFECYCLE: AtomicU32 = AtomicU32::new(1);
/// Whether the fake core has `world.lifecycle` (`KilnSessionInfo.lifecycle`); default
/// yes. Hold [`session_lock`] while it is off and turn it back on before releasing.
pub fn set_lifecycle(on: bool) {
    LIFECYCLE.store(on as u32, Ordering::SeqCst);
}
unsafe extern "C" fn session(out: *mut KilnSessionInfo) -> KilnResult {
    if out.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let table = *SESSION.lock().unwrap();
    let info = unsafe { &mut *out };
    for (kind, (world, session)) in table.iter().enumerate() {
        info.worlds[kind] = *world;
        // sessions[] counts worlds since boot; the live one is the newest.
        info.sessions[kind] = *session;
    }
    info.lifecycle = LIFECYCLE.load(Ordering::SeqCst);
    // The core's classification: a dedicated server runs its server world; a client hosts while
    // a server world runs here, or has joined another machine while only its client world runs.
    let live = |kind: i32| table[kind as usize].0 != 0;
    info.size = std::mem::size_of::<KilnSessionInfo>() as u32;
    info.side = SESSION_SIDE.load(Ordering::SeqCst);
    let state = if info.side == KILN_SIDE_SERVER as u32 {
        if live(KILN_WORLD_SERVER) {
            KILN_SESSION_DEDICATED
        } else {
            KILN_SESSION_NONE
        }
    } else if live(KILN_WORLD_SERVER) {
        KILN_SESSION_HOST
    } else if live(KILN_WORLD_CLIENT) {
        KILN_SESSION_REMOTE
    } else {
        KILN_SESSION_NONE
    };
    info.state = state as u32;
    KILN_OK
}

type Posted = (unsafe extern "C" fn(*mut c_void), usize);
type PostQueues = HashMap<(KilnOwner, u32), Vec<Posted>>;
static POSTED: LazyLock<Mutex<PostQueues>> = LazyLock::new(Default::default);
static POST_REFUSED: LazyLock<Mutex<HashSet<KilnOwner>>> = LazyLock::new(Default::default);
static POSTS_TOTAL: LazyLock<Mutex<HashMap<KilnOwner, u64>>> = LazyLock::new(Default::default);
/// Every `post_world` call `owner` made, refused ones included (the hop counter).
pub fn posts_total(owner: KilnOwner) -> u64 {
    POSTS_TOTAL
        .lock()
        .unwrap()
        .get(&owner)
        .copied()
        .unwrap_or(0)
}
unsafe extern "C" fn post_world(
    owner: KilnOwner,
    kind: u32,
    function: KilnTaskFn,
    user: *mut c_void,
) -> KilnResult {
    let Some(function) = function else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    *POSTS_TOTAL.lock().unwrap().entry(owner).or_default() += 1;
    if POST_REFUSED.lock().unwrap().contains(&owner) {
        return KILN_ERR_UNAVAILABLE;
    }
    POSTED
        .lock()
        .unwrap()
        .entry((owner, kind))
        .or_default()
        .push((function, user as usize));
    KILN_OK
}
/// Makes `post_world` refuse for `owner` (or accept again).
pub fn refuse_posts(owner: KilnOwner, refuse: bool) {
    let mut refused = POST_REFUSED.lock().unwrap();
    if refuse {
        refused.insert(owner);
    } else {
        refused.remove(&owner);
    }
}
pub fn posted(owner: KilnOwner, kind: i32) -> usize {
    POSTED
        .lock()
        .unwrap()
        .get(&(owner, kind as u32))
        .map_or(0, Vec::len)
}
/// Runs `owner`'s queued functions of world `kind` on this thread, as that world's
/// thread does before its next Pre, and returns how long each took.
pub fn run_posted(owner: KilnOwner, kind: i32) -> Vec<Duration> {
    let queued = POSTED
        .lock()
        .unwrap()
        .remove(&(owner, kind as u32))
        .unwrap_or_default();
    queued
        .into_iter()
        .map(|(function, user)| {
            let start = Instant::now();
            unsafe { function(user as *mut c_void) };
            start.elapsed()
        })
        .collect()
}
/// Drops `owner`'s queued functions without running them (a world that never ticks).
pub fn forget_posted(owner: KilnOwner) -> usize {
    let mut posted = POSTED.lock().unwrap();
    let keys: Vec<_> = posted
        .keys()
        .filter(|(o, _)| *o == owner)
        .copied()
        .collect();
    keys.into_iter()
        .map(|key| posted.remove(&key).map_or(0, |v| v.len()))
        .sum()
}

// ---- KilnApi.path: the state folder kiln-sfi writes sfi.json into ----

thread_local! {
    /// What `path(KILN_PATH_STATE)` answers on this thread; unset: a fresh folder of its own on first use.
    static STATE_DIR: RefCell<Option<PathBuf>> = const { RefCell::new(None) };
}
static STATE_DIRS: AtomicU64 = AtomicU64::new(0);
/// A new empty folder under the test target's tmp dir.
pub fn fresh_dir(name: &str) -> PathBuf {
    let n = STATE_DIRS.fetch_add(1, Ordering::Relaxed);
    // CARGO_TARGET_TMPDIR is set for integration tests only; an example of another test package
    // can compile this file too.
    let base = option_env!("CARGO_TARGET_TMPDIR")
        .map(Path::new)
        .filter(|base| base.is_dir())
        .map_or_else(std::env::temp_dir, Path::to_path_buf);
    let dir = base
        .join("kiln-sfi-state")
        .join(format!("{name}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
/// The state folder the fake core names on this thread from now on.
pub fn set_state_dir(dir: PathBuf) {
    STATE_DIR.set(Some(dir));
}
/// The state folder the fake core names on this thread (made on first use).
pub fn state_dir() -> PathBuf {
    STATE_DIR.with_borrow_mut(|dir| dir.get_or_insert_with(|| fresh_dir("default")).clone())
}
unsafe extern "C" fn path(
    kind: i32,
    buffer: *mut core::ffi::c_char,
    capacity: usize,
    len: *mut usize,
) -> KilnResult {
    let dir = match kind {
        KILN_PATH_STATE => state_dir(),
        KILN_PATH_GAME => game_dir(),
        KILN_PATH_KILN => game_dir().join("kiln"),
        _ => return KILN_ERR_INVALID_ARGUMENT,
    };
    let text = dir.to_str().unwrap().as_bytes();
    if !len.is_null() {
        unsafe { *len = text.len() };
    }
    if buffer.is_null() || capacity < text.len() {
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), text.len()) };
    KILN_OK
}

// ---- KilnApi.version ----

/// The release that added `KILN_TASK_RESIDENT` (kiln_tasks.h: "since 0.2.0-dev.29.19").
pub const KILN_WITH_RESIDENT: &str = "0.2.0-dev.29.19";
/// The release before it.
pub const KILN_BEFORE_RESIDENT: &str = "0.2.0-dev.29.18";
fn kstr(text: &'static str) -> KilnStr {
    KilnStr {
        ptr: text.as_ptr().cast(),
        len: text.len(),
    }
}
/// `KilnApi.version` of [`api`]: a Kiln with resident tasks.
pub unsafe extern "C" fn version_with_resident() -> KilnStr {
    kstr(KILN_WITH_RESIDENT)
}
pub unsafe extern "C" fn version_before_resident() -> KilnStr {
    kstr(KILN_BEFORE_RESIDENT)
}
/// A version that is no semantic version at all.
pub unsafe extern "C" fn version_unreadable() -> KilnStr {
    kstr("kiln (local build)")
}

/// A zeroed `KilnApi` of the current size with version, log, service and subscriptions set.
pub fn api(with_tasks: bool) -> Box<KilnApi> {
    let mut api: Box<KilnApi> = Box::new(unsafe { std::mem::zeroed() });
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.abi_version = KILN_ABI_VERSION;
    api.version = Some(version_with_resident);
    api.log = Some(log);
    api.service = Some(if with_tasks {
        service_full
    } else {
        service_no_tasks
    });
    api.subscribe_tick = Some(subscribe_tick);
    api.subscribe_world = Some(subscribe_world);
    api.unsubscribe = Some(unsubscribe);
    api.session = Some(session);
    api.post_world = Some(post_world);
    api.path = Some(path);
    api.build_info = Some(build_info);
    api.side = Some(side);
    api
}
pub fn plugin_info() -> KilnPluginInfo {
    let mut info: KilnPluginInfo = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnPluginInfo>() as u32;
    info
}

// ---- kiln.tasks ----

pub static TASKS: KilnTasksApi = KilnTasksApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnTasksApi>() as u32,
        version: 1,
    },
    submit: Some(submit),
    cancel: Some(cancel),
    status: Some(status),
    release: Some(release),
    drain_owner: Some(drain_owner),
    submit_in: Some(submit_in),
};
pub fn tasks_table() -> *const KilnTasksApi {
    &TASKS
}

#[derive(Clone, Copy)]
pub struct PoolConfig {
    /// Accept `KILN_TASK_RESIDENT`; false = a Kiln older than the bit.
    pub l1: bool,
    /// Refuse every submit with (result, refusal).
    pub refuse: Option<(KilnResult, u32)>,
    /// LongRunning threads free for this owner; 0 keeps a task QUEUED.
    pub threads: usize,
    /// Deliver FAULTED instead of COMPLETED after the body (a quarantine).
    pub fault_done: bool,
}
impl Default for PoolConfig {
    fn default() -> Self {
        Self {
            l1: true,
            refuse: None,
            threads: 2,
            fault_done: false,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Submission {
    pub owner: KilnOwner,
    pub flags: u32,
    pub name: String,
    pub priority: u32,
    pub result: KilnResult,
    pub refusal: u32,
}

/// The run record the fake hands its body; its `internal` points at the task.
struct RunRecord(Box<KilnTaskRun>);
// SAFETY: `internal` points at the Arc'd FakeTask that outlives the record.
unsafe impl Send for RunRecord {}

pub struct FakeTask {
    pub handle: KilnHandle,
    pub flags: u32,
    state: AtomicU32,
    cancel: AtomicBool,
    cancel_at: Mutex<Option<Instant>>,
    body: unsafe extern "C" fn(*const KilnTaskRun, *mut c_void),
    done: unsafe extern "C" fn(KilnHandle, u32, *mut c_void),
    user: usize,
    run: Mutex<Option<RunRecord>>,
    pub body_thread: Mutex<Option<ThreadId>>,
    body_returned: Mutex<Option<Instant>>,
    pub done_calls: AtomicU32,
    pub done_state: AtomicU32,
    pub released: AtomicBool,
    finished: (Mutex<bool>, Condvar),
}
impl FakeTask {
    pub fn state(&self) -> u32 {
        self.state.load(Ordering::SeqCst)
    }
    pub fn cancel_requested(&self) -> bool {
        self.cancel.load(Ordering::SeqCst)
    }
    /// How long the body took to return after its cancellation was asked.
    pub fn leave_latency(&self) -> Option<Duration> {
        let asked = (*self.cancel_at.lock().unwrap())?;
        let returned = (*self.body_returned.lock().unwrap())?;
        Some(returned.saturating_duration_since(asked))
    }
    pub fn body_returned(&self) -> bool {
        self.body_returned.lock().unwrap().is_some()
    }
    fn signal(&self) {
        let mut at = self.cancel_at.lock().unwrap();
        if at.is_none() {
            *at = Some(Instant::now());
        }
        self.cancel.store(true, Ordering::SeqCst);
    }
    fn finish(&self) {
        *self.finished.0.lock().unwrap() = true;
        self.finished.1.notify_all();
    }
    /// Waits until done returned (or, after an exit park, the body returned).
    pub fn wait_finished(&self, timeout: Duration) -> bool {
        let guard = self.finished.0.lock().unwrap();
        let (guard, _) = self
            .finished
            .1
            .wait_timeout_while(guard, timeout, |f| !*f)
            .unwrap();
        *guard
    }
}

pub struct FakePool {
    pub owner: KilnOwner,
    config: Mutex<PoolConfig>,
    tasks: Mutex<Vec<Arc<FakeTask>>>,
    submissions: Mutex<Vec<Submission>>,
    sealed: AtomicBool,
    shutdown: AtomicBool,
    parked: AtomicBool,
    next_handle: AtomicU64,
    drains: Mutex<Vec<u32>>,
    /// AFTER_STEP tasks waiting for a Post edge of (world, session).
    after_step: Mutex<Vec<(Arc<FakeTask>, u32, u32)>>,
    /// Bodies or dones of other tasks of this owner (IO tasks) that `drain_owner`
    /// still reports as running.
    other_running: AtomicU32,
}

static POOLS: LazyLock<Mutex<HashMap<KilnOwner, Arc<FakePool>>>> = LazyLock::new(Default::default);
fn pool(owner: KilnOwner) -> Option<Arc<FakePool>> {
    POOLS.lock().unwrap().get(&owner).cloned()
}

#[derive(Debug)]
pub struct ExitOutcome {
    /// Every running body returned inside the park.
    pub returned: bool,
    pub waited: Duration,
}

impl FakePool {
    pub fn new(owner: KilnOwner, config: PoolConfig) -> Arc<Self> {
        let pool = Arc::new(Self {
            owner,
            config: Mutex::new(config),
            tasks: Mutex::default(),
            submissions: Mutex::default(),
            sealed: AtomicBool::new(false),
            shutdown: AtomicBool::new(false),
            parked: AtomicBool::new(false),
            next_handle: AtomicU64::new(1),
            drains: Mutex::default(),
            after_step: Mutex::default(),
            other_running: AtomicU32::new(0),
        });
        POOLS.lock().unwrap().insert(owner, pool.clone());
        pool
    }
    /// A Post edge of world `kind`: AFTER_STEP tasks of its live session start.
    pub fn post_edge(self: &Arc<Self>, kind: i32) {
        let live = SESSION.lock().unwrap()[kind as usize].1;
        let ready: Vec<_> = {
            let mut waiting = self.after_step.lock().unwrap();
            let (ready, rest) = waiting
                .drain(..)
                .partition(|(_, world, session)| *world == kind as u32 && *session == live);
            *waiting = rest;
            ready
        };
        for (task, _, _) in ready {
            // One cancelled while it waited (cancel, drain) already has its done.
            if task.state() == KILN_TASK_QUEUED as u32 {
                self.start(task);
            }
        }
    }
    /// The session's STOPPING: its waiting AFTER_STEP tasks are CANCELLED, each
    /// with its done (kiln_tasks.h "AFTER_STEP and sessions").
    pub fn end_session(self: &Arc<Self>, kind: i32, session: u32) {
        let ended: Vec<_> = {
            let mut waiting = self.after_step.lock().unwrap();
            let (ended, rest) = waiting
                .drain(..)
                .partition(|(_, world, s)| *world == kind as u32 && *s == session);
            *waiting = rest;
            ended
        };
        for (task, _, _) in ended {
            self.cancel_queued(task);
        }
    }
    /// The `timeout_ms` of every `drain_owner` call made for this owner.
    pub fn drains(&self) -> Vec<u32> {
        self.drains.lock().unwrap().clone()
    }
    /// Other tasks of this owner that `drain_owner` reports as still running.
    pub fn set_other_running(&self, running: u32) {
        self.other_running.store(running, Ordering::SeqCst);
    }
    pub fn submissions(&self) -> Vec<Submission> {
        self.submissions.lock().unwrap().clone()
    }
    pub fn task(&self, index: usize) -> Arc<FakeTask> {
        self.tasks.lock().unwrap()[index].clone()
    }
    pub fn task_count(&self) -> usize {
        self.tasks.lock().unwrap().len()
    }
    pub fn set_fault_done(&self, fault: bool) {
        self.config.lock().unwrap().fault_done = fault;
    }
    /// A LongRunning thread becomes free: start the oldest queued task.
    pub fn free_thread(self: &Arc<Self>) {
        let queued = self
            .tasks
            .lock()
            .unwrap()
            .iter()
            .find(|t| t.state() == KILN_TASK_QUEUED as u32)
            .cloned();
        match queued {
            Some(task) => self.start(task),
            None => self.config.lock().unwrap().threads += 1,
        }
    }
    /// `cancel(owner, task)` as a test would see it.
    pub fn cancel(&self, task: KilnHandle) -> u32 {
        let mut state = 0;
        unsafe { cancel(self.owner, task, &mut state) };
        state
    }
    /// `drain_owner(owner, 0)`: seal, cancel queued, signal running, poll.
    pub fn drain_poll(&self) -> u32 {
        let mut running = 0;
        unsafe { drain_owner(self.owner, 0, &mut running) };
        running
    }
    /// Process exit, normal code 0 (tasks.md "Exit"): step 1 refuses submissions
    /// (SHUTDOWN), step 2 seals and signals, step 4 parks: no body or done starts
    /// any more and the exiting thread waits up to `park` for running bodies.
    pub fn exit(&self, park: Duration) -> ExitOutcome {
        self.shutdown.store(true, Ordering::SeqCst);
        self.sealed.store(true, Ordering::SeqCst);
        let tasks = self.tasks.lock().unwrap().clone();
        for task in &tasks {
            match task.state() as i32 {
                KILN_TASK_QUEUED => task
                    .state
                    .store(KILN_TASK_CANCELLED as u32, Ordering::SeqCst),
                KILN_TASK_RUNNING => task.signal(),
                _ => {}
            }
        }
        self.parked.store(true, Ordering::SeqCst);
        let start = Instant::now();
        let mut returned = true;
        for task in tasks
            .iter()
            .filter(|t| t.state() == KILN_TASK_RUNNING as u32)
        {
            let left = park.saturating_sub(start.elapsed());
            returned &= task.wait_finished(left);
        }
        ExitOutcome {
            returned,
            waited: start.elapsed(),
        }
    }

    fn start(self: &Arc<Self>, task: Arc<FakeTask>) {
        task.state.store(KILN_TASK_RUNNING as u32, Ordering::SeqCst);
        let pool = self.clone();
        std::thread::Builder::new()
            .name(format!("fake-pool-{}", self.owner))
            .spawn(move || {
                *task.body_thread.lock().unwrap() = Some(std::thread::current().id());
                let run = task.run.lock().unwrap().take().expect("run record");
                unsafe { (task.body)(&*run.0, task.user as *mut c_void) };
                *task.body_returned.lock().unwrap() = Some(Instant::now());
                if pool.parked.load(Ordering::SeqCst) {
                    // Parked at exit: done is not guaranteed and does not run.
                    task.finish();
                    return;
                }
                let state = if pool.config.lock().unwrap().fault_done {
                    KILN_TASK_FAULTED
                } else {
                    KILN_TASK_COMPLETED
                } as u32;
                task.state.store(state, Ordering::SeqCst);
                pool.deliver_done(&task, state);
            })
            .expect("spawn fake pool thread");
    }
    fn deliver_done(&self, task: &FakeTask, state: u32) {
        task.done_state.store(state, Ordering::SeqCst);
        unsafe { (task.done)(task.handle, state, task.user as *mut c_void) };
        task.done_calls.fetch_add(1, Ordering::SeqCst);
        task.finish();
    }
    /// A queued task's cancellation: CANCELLED now, done on another thread.
    fn cancel_queued(self: &Arc<Self>, task: Arc<FakeTask>) {
        // Once: a task already cancelled (cancel, drain, its session's end) keeps
        // its one done (kiln_tasks.h: done exactly once).
        if task
            .state
            .compare_exchange(
                KILN_TASK_QUEUED as u32,
                KILN_TASK_CANCELLED as u32,
                Ordering::SeqCst,
                Ordering::SeqCst,
            )
            .is_err()
        {
            return;
        }
        let pool = self.clone();
        std::thread::spawn(move || pool.deliver_done(&task, KILN_TASK_CANCELLED as u32));
    }
}

unsafe extern "C" fn cancelled(run: *const KilnTaskRun) -> u32 {
    let task = unsafe { &*(*run).internal.cast_const().cast::<FakeTask>() };
    task.cancel_requested() as u32
}

unsafe extern "C" fn submit(
    owner: KilnOwner,
    info: *const KilnTaskInfo,
    body: KilnTaskBodyFn,
    done: KilnTaskDoneFn,
    user: *mut c_void,
    task_out: *mut KilnHandle,
    refusal_out: *mut u32,
) -> KilnResult {
    let refuse = |result: KilnResult, refusal: i32| {
        if !task_out.is_null() {
            unsafe { *task_out = 0 };
        }
        if !refusal_out.is_null() {
            unsafe { *refusal_out = refusal as u32 };
        }
        result
    };
    let Some(pool) = pool(owner) else {
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    };
    if info.is_null() || task_out.is_null() {
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    }
    let info = unsafe { *info };
    let record = |result: KilnResult, refusal: i32| {
        pool.submissions.lock().unwrap().push(Submission {
            owner,
            flags: info.flags,
            name: name(info.name),
            priority: info.priority,
            result,
            refusal: refusal as u32,
        });
        refuse(result, refusal)
    };
    let config = *pool.config.lock().unwrap();
    let known = (KILN_TASK_LONG_RUNNING | KILN_TASK_AFTER_STEP) as u32
        | if config.l1 {
            KILN_TASK_RESIDENT as u32
        } else {
            0
        };
    let (Some(body), Some(done)) = (body, done) else {
        return record(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    };
    let resident_alone = info.flags & KILN_TASK_RESIDENT as u32 != 0
        && info.flags & KILN_TASK_LONG_RUNNING as u32 == 0;
    if owner == 0
        || (info.size as usize) < std::mem::size_of::<KilnTaskInfo>()
        || info.flags & !known != 0
        || resident_alone
        || info.priority > 2
        || info.reserved != 0
        || info.name.len == 0
        || info.name.len > 128
    {
        return record(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    }
    if pool.shutdown.load(Ordering::SeqCst) {
        return record(KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_SHUTDOWN);
    }
    if pool.sealed.load(Ordering::SeqCst) {
        return record(KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_OWNER_SEALED);
    }
    if let Some((result, refusal)) = config.refuse {
        return record(result, refusal as i32);
    }
    // At most one resident task per owner (kiln_tasks.h): a second one, while the
    // first is queued or running (a cancelled body that has not returned yet
    // included), is refused ARGUMENT, after the exit and owner-state refusals.
    let resident = KILN_TASK_RESIDENT as u32;
    if info.flags & resident != 0
        && pool.tasks.lock().unwrap().iter().any(|t| {
            t.flags & resident != 0
                && matches!(t.state() as i32, KILN_TASK_QUEUED | KILN_TASK_RUNNING)
        })
    {
        return record(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    }
    let handle = pool.next_handle.fetch_add(1, Ordering::SeqCst);
    let task = Arc::new(FakeTask {
        handle,
        flags: info.flags,
        state: AtomicU32::new(KILN_TASK_QUEUED as u32),
        cancel: AtomicBool::new(false),
        cancel_at: Mutex::new(None),
        body,
        done,
        user: user as usize,
        run: Mutex::new(None),
        body_thread: Mutex::new(None),
        body_returned: Mutex::new(None),
        done_calls: AtomicU32::new(0),
        done_state: AtomicU32::new(0),
        released: AtomicBool::new(false),
        finished: (Mutex::new(false), Condvar::new()),
    });
    *task.run.lock().unwrap() = Some(RunRecord(Box::new(KilnTaskRun {
        size: std::mem::size_of::<KilnTaskRun>() as u32,
        reserved: 0,
        task: handle,
        owner,
        cancelled: Some(cancelled),
        internal: Arc::as_ptr(&task).cast_mut().cast(),
    })));
    pool.tasks.lock().unwrap().push(task.clone());
    record(KILN_OK, KILN_TASK_REFUSAL_NONE);
    unsafe { *task_out = handle };
    let free = {
        let mut config = pool.config.lock().unwrap();
        let free = config.threads > 0;
        if free {
            config.threads -= 1;
        }
        free
    };
    if free {
        pool.start(task);
    }
    KILN_OK
}

unsafe extern "C" fn submit_in(
    owner: KilnOwner,
    info: *const KilnTaskInfo,
    world: u32,
    session: u32,
    body: KilnTaskBodyFn,
    done: KilnTaskDoneFn,
    user: *mut c_void,
    task_out: *mut KilnHandle,
    refusal_out: *mut u32,
) -> KilnResult {
    let refuse = |result: KilnResult, refusal: i32| {
        if !task_out.is_null() {
            unsafe { *task_out = 0 };
        }
        if !refusal_out.is_null() {
            unsafe { *refusal_out = refusal as u32 };
        }
        result
    };
    if world != KILN_WORLD_SERVER as u32 && world != KILN_WORLD_CLIENT as u32 {
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    }
    let live = SESSION.lock().unwrap()[world as usize];
    if live.0 == 0 || (session != 0 && live.1 != session) {
        return refuse(KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_NO_WORLD);
    }
    let Some(pool) = pool(owner) else {
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    };
    let after_step = !info.is_null() && unsafe { (*info).flags } & KILN_TASK_AFTER_STEP as u32 != 0;
    // Hold every new task back from starting, then park it on the edge list.
    let threads = std::mem::replace(&mut pool.config.lock().unwrap().threads, 0);
    let result = unsafe { submit(owner, info, body, done, user, task_out, refusal_out) };
    pool.config.lock().unwrap().threads = threads;
    if result == KILN_OK {
        let task = pool
            .tasks
            .lock()
            .unwrap()
            .last()
            .cloned()
            .expect("accepted");
        if after_step {
            pool.after_step.lock().unwrap().push((task, world, live.1));
        } else {
            pool.start(task);
        }
    }
    result
}

fn find(owner: KilnOwner, handle: KilnHandle) -> Option<(Arc<FakePool>, Arc<FakeTask>)> {
    let pool = pool(owner)?;
    let task = pool
        .tasks
        .lock()
        .unwrap()
        .iter()
        .find(|t| t.handle == handle && !t.released.load(Ordering::SeqCst))
        .cloned()?;
    Some((pool, task))
}

unsafe extern "C" fn cancel(owner: KilnOwner, handle: KilnHandle, state: *mut u32) -> KilnResult {
    let Some((pool, task)) = find(owner, handle) else {
        return KILN_ERR_NOT_FOUND;
    };
    match task.state() as i32 {
        KILN_TASK_QUEUED => pool.cancel_queued(task.clone()),
        KILN_TASK_RUNNING => task.signal(),
        _ => {}
    }
    if !state.is_null() {
        unsafe { *state = task.state() };
    }
    KILN_OK
}

unsafe extern "C" fn status(owner: KilnOwner, handle: KilnHandle, state: *mut u32) -> KilnResult {
    let found = find(owner, handle);
    if !state.is_null() {
        unsafe { *state = found.as_ref().map_or(0, |(_, t)| t.state()) };
    }
    if found.is_some() {
        KILN_OK
    } else {
        KILN_ERR_NOT_FOUND
    }
}

unsafe extern "C" fn release(owner: KilnOwner, handle: KilnHandle) -> KilnResult {
    let Some((_, task)) = find(owner, handle) else {
        return KILN_ERR_NOT_FOUND;
    };
    match task.state() as i32 {
        KILN_TASK_QUEUED | KILN_TASK_RUNNING => KILN_ERR_UNAVAILABLE,
        _ => {
            task.released.store(true, Ordering::SeqCst);
            KILN_OK
        }
    }
}

unsafe extern "C" fn drain_owner(
    owner: KilnOwner,
    timeout_ms: u32,
    running: *mut u32,
) -> KilnResult {
    let Some(pool) = pool(owner) else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    pool.drains.lock().unwrap().push(timeout_ms);
    pool.sealed.store(true, Ordering::SeqCst);
    let tasks = pool.tasks.lock().unwrap().clone();
    for task in &tasks {
        match task.state() as i32 {
            KILN_TASK_QUEUED => pool.cancel_queued(task.clone()),
            KILN_TASK_RUNNING => task.signal(),
            _ => {}
        }
    }
    let outstanding = || {
        tasks
            .iter()
            .filter(|t| t.done_calls.load(Ordering::SeqCst) == 0 && !*t.finished.0.lock().unwrap())
            .count() as u32
    };
    let deadline = Instant::now() + Duration::from_millis(timeout_ms.into());
    while timeout_ms > 0 && outstanding() > 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(1));
    }
    if !running.is_null() {
        unsafe { *running = outstanding() + pool.other_running.load(Ordering::SeqCst) };
    }
    KILN_OK
}

/// Another resident task of `pool`'s owner, running until [`ResidentHold::let_go`] (or the
/// hold is dropped, or it is cancelled): the task owner's one resident task is taken
/// (kiln_tasks.h "Resident tasks"), so the next resident submit is refused ARGUMENT.
pub struct ResidentHold {
    pool: Arc<FakePool>,
    handle: KilnHandle,
    go: &'static AtomicBool,
}
pub const HOLD_NAME: &str = "resident-hold";
unsafe extern "C" fn hold_body(run: *const KilnTaskRun, user: *mut c_void) {
    let go = unsafe { &*user.cast_const().cast::<AtomicBool>() };
    let cancelled = unsafe { (*run).cancelled }.expect("cancelled");
    while unsafe { cancelled(run) } == 0 && !go.load(Ordering::SeqCst) {
        std::thread::sleep(Duration::from_millis(1));
    }
}
unsafe extern "C" fn hold_done(_: KilnHandle, _: u32, _: *mut c_void) {}
/// Submits the hold and waits (up to `wait`) until its body runs.
pub fn hold_resident(pool: &Arc<FakePool>, wait: Duration) -> ResidentHold {
    let go: &'static AtomicBool = Box::leak(Box::new(AtomicBool::new(false)));
    let info = KilnTaskInfo {
        size: std::mem::size_of::<KilnTaskInfo>() as u32,
        flags: (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32,
        name: kstr(HOLD_NAME),
        priority: KILN_TASK_PRIORITY_NORMAL as u32,
        reserved: 0,
    };
    let (mut handle, mut refusal) = (0, KILN_TASK_REFUSAL_NONE as u32);
    let result = unsafe {
        submit(
            pool.owner,
            &info,
            Some(hold_body),
            Some(hold_done),
            std::ptr::from_ref(go).cast_mut().cast(),
            &mut handle,
            &mut refusal,
        )
    };
    assert_eq!((result, refusal), (KILN_OK, KILN_TASK_REFUSAL_NONE as u32));
    let hold = ResidentHold {
        pool: pool.clone(),
        handle,
        go,
    };
    assert!(
        eventually(wait, || hold.task().state() == KILN_TASK_RUNNING as u32),
        "the hold never ran"
    );
    hold
}
impl ResidentHold {
    fn task(&self) -> Arc<FakeTask> {
        let tasks = self.pool.tasks.lock().unwrap();
        let task = tasks.iter().find(|t| t.handle == self.handle);
        task.expect("the hold's task").clone()
    }
    /// Lets the body return and waits (up to `wait`) for its done.
    pub fn let_go(&self, wait: Duration) -> bool {
        self.go.store(true, Ordering::SeqCst);
        self.task().wait_finished(wait)
    }
}
impl Drop for ResidentHold {
    fn drop(&mut self) {
        self.go.store(true, Ordering::SeqCst);
    }
}

/// Polls `condition` every millisecond until it holds or `timeout` passes.
pub fn eventually(timeout: Duration, mut condition: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        if condition() {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}
