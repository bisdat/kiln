// SPDX-License-Identifier: MIT
//! kiln-sfi's native do-not-save banner (src/notice_native.rs) against a fake kiln.ui that keeps ui.cpp's
//! subscription rules (a frame snapshots the subscribers under the list lock and calls each under its own gate;
//! unsubscribe marks the subscriber inactive, then waits on the gate for a call in flight; a frame with the Kiln menu
//! open draws the fallback menu only when it found no subscriber, ui.cpp:321). The control step runs with a test
//! clock, so the 10 s grace and the 30 s showing are the production values.
//!
//! What each test proves is in its name. The two thread tests (`unsubscribe_waits_...`, `frames_on_the_render_thread_...`)
//! are the frame-callback/unsubscribe path the plan runs once under ASan and TSan.
mod support;

use kiln_abi::services::kiln_ui::*;
use kiln_abi::*;
use kiln_sfi::notice_native::{
    self, BANNER_CHARS, Env, GRACE, HINT, Notice, NoticeHooks, OVERLAY_OFF, SHOW_FOR, TITLE,
};
use kiln_sfi::state::*;
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Condvar, LazyLock, Mutex, MutexGuard};
use std::time::{Duration, Instant};

/// A liveness bound, not a latency claim (the wave's 300 s): a wait for something that must happen.
const WAIT: Duration = Duration::from_secs(300);

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|p| p.into_inner())
}

// ---- the fake kiln.ui: one per process (kiln.ui's entry points carry no context), so every test takes TEST ----

static TEST: Mutex<()> = Mutex::new(());

type FrameFn = unsafe extern "C" fn(*const KilnUiFrame, *mut c_void);

struct Sub {
    handle: KilnHandle,
    owner: KilnOwner,
    callback: FrameFn,
    user: usize,
    active: AtomicBool,
    /// ui.cpp's CallbackLifetime gate: held for the whole call.
    gate: Mutex<()>,
}

#[derive(Default)]
struct Fake {
    subs: Mutex<Vec<Arc<Sub>>>,
    retiring: Mutex<HashMap<KilnHandle, Arc<Sub>>>,
    next: AtomicU64,
    subscribes: AtomicU32,
    demand: AtomicU32,
    demand_calls: AtomicU32,
    menu_open: AtomicBool,
    presents: AtomicU64,
    /// KilnUiScreen.ui_state; `screens` false: screen() answers KILN_ERR_UNAVAILABLE.
    ui_state: AtomicU32,
    screens: AtomicBool,
    error: Mutex<String>,
    /// The core's lookup gives the table for "kiln.ui".
    table: AtomicBool,
    asked: AtomicU32,
    /// Kiln.Ui's ready event exists.
    ready: AtomicBool,
    /// unsubscribe refuses (answers KILN_ERR_UNAVAILABLE) while set.
    refuse_unsubscribe: AtomicBool,
    /// The client world's session (0: no world).
    world: AtomicU32,
    // drawing
    lines: Mutex<Vec<String>>,
    begins: AtomicU32,
    ends: AtomicU32,
    outside: AtomicU32,
    calls: AtomicU64,
    /// begin blocks here while set (the in-flight tests), and says so on `entered`.
    hold: Mutex<bool>,
    held: Condvar,
    entered: AtomicBool,
    /// A frame blocks right after its active check, before it calls the subscriber, while set; says so on
    /// `at_call`.
    hold_call: Mutex<bool>,
    at_call: AtomicBool,
    /// unsubscribe is about to wait on a subscriber's gate.
    unsub_waiting: AtomicBool,
    frames: AtomicU64,
}
static FAKE: LazyLock<Fake> = LazyLock::new(Fake::default);

thread_local! {
    static IN_FRAME: Cell<bool> = const { Cell::new(false) };
}

fn in_frame() -> bool {
    let inside = IN_FRAME.with(Cell::get);
    if !inside {
        FAKE.outside.fetch_add(1, Ordering::SeqCst);
    }
    inside
}
fn text(s: KilnStr) -> String {
    let bytes = unsafe { std::slice::from_raw_parts(s.ptr.cast::<u8>(), s.len) };
    String::from_utf8_lossy(bytes).into_owned()
}

unsafe extern "C" fn ui_status(out: *mut KilnUiStatus) -> KilnResult {
    let out = unsafe { &mut *out };
    if (out.size as usize) < std::mem::size_of::<KilnUiStatus>() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    out.menu_open = u8::from(FAKE.menu_open.load(Ordering::SeqCst));
    out.presents = FAKE.presents.load(Ordering::SeqCst);
    out.subscribers = lock(&FAKE.subs).len() as u32;
    let error = lock(&FAKE.error).clone();
    for (i, b) in error.bytes().take(out.error.len() - 1).enumerate() {
        out.error[i] = b as _;
    }
    KILN_OK
}
unsafe extern "C" fn ui_subscribe_frame(
    owner: KilnOwner,
    f: KilnUiFrameFn,
    user: *mut c_void,
    out: *mut KilnHandle,
) -> KilnResult {
    let Some(callback) = f else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    let handle = FAKE.next.fetch_add(1, Ordering::SeqCst) + 1;
    lock(&FAKE.subs).push(Arc::new(Sub {
        handle,
        owner,
        callback,
        user: user as usize,
        active: AtomicBool::new(true),
        gate: Mutex::new(()),
    }));
    FAKE.subscribes.fetch_add(1, Ordering::SeqCst);
    if !out.is_null() {
        unsafe { *out = handle };
    }
    KILN_OK
}
unsafe extern "C" fn ui_unsubscribe(h: KilnHandle) -> KilnResult {
    if FAKE.refuse_unsubscribe.load(Ordering::SeqCst) {
        return KILN_ERR_UNAVAILABLE;
    }
    let mut removed = None;
    {
        let mut subs = lock(&FAKE.subs);
        subs.retain(|s| {
            if s.handle != h {
                return true;
            }
            s.active.store(false, Ordering::Release);
            lock(&FAKE.retiring).insert(h, Arc::clone(s));
            removed = Some(Arc::clone(s));
            false
        });
        if removed.is_none() {
            removed = lock(&FAKE.retiring).get(&h).cloned();
        }
    }
    // never waits under the list lock; waits for a call in flight on the gate
    let Some(sub) = removed else {
        return KILN_ERR_NOT_FOUND;
    };
    FAKE.unsub_waiting.store(true, Ordering::SeqCst);
    drop(lock(&sub.gate));
    lock(&FAKE.retiring).remove(&h);
    KILN_OK
}
unsafe extern "C" fn ui_set_demand(n: u32) {
    FAKE.demand.store(n, Ordering::SeqCst);
    FAKE.demand_calls.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn ui_begin(title: KilnStr, _open: *mut bool, flags: u32) -> bool {
    if !in_frame() {
        return false;
    }
    FAKE.begins.fetch_add(1, Ordering::SeqCst);
    assert_eq!(
        flags & KILN_UI_WINDOW_NO_INPUTS as u32,
        KILN_UI_WINDOW_NO_INPUTS as u32,
        "the banner takes no input"
    );
    lock(&FAKE.lines).push(format!("begin {}", text(title)));
    FAKE.entered.store(true, Ordering::SeqCst);
    let mut hold = lock(&FAKE.hold);
    while *hold {
        hold = FAKE.held.wait(hold).unwrap_or_else(|p| p.into_inner());
    }
    true
}
unsafe extern "C" fn ui_end() {
    if in_frame() {
        FAKE.ends.fetch_add(1, Ordering::SeqCst);
        lock(&FAKE.lines).push("end".into());
    }
}
unsafe extern "C" fn ui_text_wrapped(s: KilnStr) {
    if in_frame() {
        lock(&FAKE.lines).push(text(s));
    }
}
unsafe extern "C" fn ui_pos(_: f32, _: f32, _: u32, _: f32, _: f32) {
    in_frame();
}
unsafe extern "C" fn ui_size(w: f32, h: f32, _: u32) {
    if in_frame() {
        assert!(w > 0.0 && h == 0.0, "a width, and the height of the text");
    }
}
unsafe extern "C" fn ui_alpha(_: f32) {
    in_frame();
}
unsafe extern "C" fn ui_screen(out: *mut KilnUiScreen) -> KilnResult {
    if !FAKE.screens.load(Ordering::SeqCst) {
        return KILN_ERR_UNAVAILABLE;
    }
    let out = unsafe { &mut *out };
    out.available = 1;
    out.ui_state = FAKE.ui_state.load(Ordering::SeqCst) as u8;
    KILN_OK
}

struct Table(KilnUiApi);
// SAFETY: function pointers only.
unsafe impl Sync for Table {}
static TABLE: LazyLock<Table> = LazyLock::new(|| {
    // SAFETY: every member is an Option of a function pointer: zero is None.
    let mut t: KilnUiApi = unsafe { std::mem::zeroed() };
    t.header = KilnServiceHeader {
        size: std::mem::size_of::<KilnUiApi>() as u32,
        version: KILN_UI_VERSION,
    };
    t.status = Some(ui_status);
    t.subscribe_frame = Some(ui_subscribe_frame);
    t.unsubscribe = Some(ui_unsubscribe);
    t.set_demand = Some(ui_set_demand);
    t.begin = Some(ui_begin);
    t.end = Some(ui_end);
    t.text_wrapped = Some(ui_text_wrapped);
    t.set_next_window_pos = Some(ui_pos);
    t.set_next_window_size = Some(ui_size);
    t.set_next_window_bg_alpha = Some(ui_alpha);
    t.screen = Some(ui_screen);
    Table(t)
});

/// One overlay frame on the calling thread (the render thread's part): every subscriber found now, each under its
/// gate. True when the fallback Kiln menu would draw (menu open, no subscriber), as ui.cpp:321.
fn frame() -> bool {
    let snapshot: Vec<Arc<Sub>> = lock(&FAKE.subs).clone();
    let n = FAKE.frames.fetch_add(1, Ordering::SeqCst) + 1;
    let menu_open = FAKE.menu_open.load(Ordering::SeqCst);
    let f = KilnUiFrame {
        size: std::mem::size_of::<KilnUiFrame>() as u32,
        width: 1920.0,
        height: 1080.0,
        dpi_scale: 1.0,
        delta_seconds: 0.016,
        menu_open: u8::from(menu_open),
        capturing: 0,
        reserved: [0; 2],
        frame: n,
    };
    IN_FRAME.with(|c| c.set(true));
    for s in &snapshot {
        let _call = lock(&s.gate);
        if !s.active.load(Ordering::Acquire) {
            continue;
        }
        FAKE.at_call.store(true, Ordering::SeqCst);
        let mut hold = lock(&FAKE.hold_call);
        while *hold {
            hold = FAKE.held.wait(hold).unwrap_or_else(|p| p.into_inner());
        }
        drop(hold);
        FAKE.calls.fetch_add(1, Ordering::SeqCst);
        unsafe { (s.callback)(&f, s.user as *mut c_void) };
    }
    IN_FRAME.with(|c| c.set(false));
    menu_open && snapshot.is_empty()
}

// ---- the core's side: log, service lookup, session ----

static LOG: Mutex<Vec<(i32, String)>> = Mutex::new(Vec::new());
unsafe extern "C" fn log(_: KilnOwner, level: i32, s: KilnStr) {
    lock(&LOG).push((level, text(s)));
}
unsafe extern "C" fn service(name: KilnStr, _: u32) -> *const c_void {
    if text(name) != "kiln.ui" {
        return std::ptr::null();
    }
    FAKE.asked.fetch_add(1, Ordering::SeqCst);
    if FAKE.table.load(Ordering::SeqCst) {
        (&TABLE.0 as *const KilnUiApi).cast()
    } else {
        std::ptr::null()
    }
}
unsafe extern "C" fn session(info: *mut KilnSessionInfo) -> KilnResult {
    let info = unsafe { &mut *info };
    let world = FAKE.world.load(Ordering::SeqCst);
    if world != 0 {
        info.worlds[KILN_WORLD_CLIENT as usize] = 0x1000;
        info.sessions[KILN_WORLD_CLIENT as usize] = world;
    }
    KILN_OK
}

const OWNER: KilnOwner = 77;
fn env() -> Env {
    Env {
        log,
        service,
        session: Some(session),
        owner: OWNER,
        pid: 4242,
    }
}
fn ready(_: u32) -> bool {
    FAKE.ready.load(Ordering::SeqCst)
}
const HOOKS: NoticeHooks = NoticeHooks {
    kiln_ui_ready: ready,
    grace: GRACE,
    show_for: SHOW_FOR,
};

/// A fresh fake (defaults: a table, the game's screens at the title, no Kiln.Ui, overlay on) and the test's turn.
fn reset() -> MutexGuard<'static, ()> {
    let turn = TEST.lock().unwrap_or_else(|p| p.into_inner());
    lock(&FAKE.subs).clear();
    lock(&FAKE.retiring).clear();
    for a in [
        &FAKE.subscribes,
        &FAKE.demand,
        &FAKE.demand_calls,
        &FAKE.ui_state,
        &FAKE.asked,
        &FAKE.world,
        &FAKE.begins,
        &FAKE.ends,
        &FAKE.outside,
    ] {
        a.store(0, Ordering::SeqCst);
    }
    FAKE.presents.store(0, Ordering::SeqCst);
    FAKE.calls.store(0, Ordering::SeqCst);
    FAKE.ui_state
        .store(KILN_UI_STATE_PRELOAD as u32, Ordering::SeqCst);
    for (b, v) in [
        (&FAKE.menu_open, false),
        (&FAKE.screens, true),
        (&FAKE.table, true),
        (&FAKE.ready, false),
        (&FAKE.refuse_unsubscribe, false),
        (&FAKE.entered, false),
        (&FAKE.at_call, false),
        (&FAKE.unsub_waiting, false),
    ] {
        b.store(v, Ordering::SeqCst);
    }
    lock(&FAKE.error).clear();
    lock(&FAKE.lines).clear();
    *lock(&FAKE.hold) = false;
    *lock(&FAKE.hold_call) = false;
    lock(&LOG).clear();
    turn
}

fn logged() -> Vec<(i32, String)> {
    lock(&LOG).clone()
}
fn drawn() -> Vec<String> {
    lock(&FAKE.lines).clone()
}

// ---- the record ----

static DIRS: AtomicU64 = AtomicU64::new(0);
fn fresh() -> PathBuf {
    let n = DIRS.fetch_add(1, Ordering::Relaxed);
    let base = PathBuf::from(env!("CARGO_TARGET_TMPDIR"));
    let base = if base.is_dir() {
        base
    } else {
        std::env::temp_dir()
    };
    let dir = base
        .join("kiln-sfi-notice")
        .join(format!("{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn identity(side: Side) -> Identity {
    Identity {
        launch_id: Some(0x3f2b_8c1e_5a7d_4e2f),
        plan_hash: Some([0xaa; 32]),
        pid: 4242,
        process_start_ms: 1_763_520_000_000,
        side,
        profile: Some(("main".into(), 7)),
        backend: Backend::Kiln,
        reason_code: "plan".into(),
        reason_text: Some("kiln.json shroudforge.backend = kiln".into()),
        frozen_at_ms: Some(1_763_520_000_100),
    }
}
fn new_hub(side: Side) -> Arc<StateHub> {
    let hub = Arc::new(StateHub::new());
    hub.set_dir(Some(fresh()));
    assert_eq!(
        hub.begin(identity(side), Phase::Starting, None),
        Begun::Written
    );
    hub
}
/// A save-sensitive package that is skipped for its DLL: a warning (a new revision) that says "do not save".
fn skipped(hub: &StateHub, id: &str) {
    hub.update(|r| {
        let mut init = PackageInit::selected(id, Some("1.0.0"), Some(&format!("Mod {id}")));
        init.save_sensitive = SaveSensitive::policy(id, "1.0.0", None);
        r.add_package(init).max(r.skip(
            id,
            Skip::new(
                "dll-gate",
                Part::Dll,
                "its DLL was refused by the native load gate",
            ),
        ))
    });
}
fn deliveries(hub: &StateHub) -> Vec<Delivery> {
    hub.read(|r| r.deliveries().to_vec()).unwrap()
}
fn warning_text(hub: &StateHub, id: &str) -> String {
    hub.read(|r| r.package(id).unwrap().warning.clone().unwrap().text)
        .unwrap()
}
fn notice(hub: &Arc<StateHub>) -> Notice {
    Notice::new(Arc::clone(hub), env(), HOOKS)
}
fn row(revision: u64, state: DeliveryState) -> (u64, DeliveryState) {
    (revision, state)
}
fn rows(hub: &StateHub) -> Vec<(u64, DeliveryState)> {
    deliveries(hub)
        .iter()
        .map(|d| (d.revision, d.state))
        .collect()
}
fn sec(s: f64) -> Duration {
    Duration::from_secs_f64(s)
}

/// The main menu reached at `t` and the grace over: the banner is due.
fn past_grace(n: &Notice, t: Instant) -> Instant {
    FAKE.ui_state
        .store(KILN_UI_STATE_PREGAME as u32, Ordering::SeqCst);
    n.step(t);
    let due = t + GRACE;
    n.step(due);
    due
}

// ---- when it draws ----

#[test]
fn with_no_warning_kiln_ui_is_never_looked_up_and_nothing_subscribes() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    let n = notice(&hub);
    let t = Instant::now();
    FAKE.ui_state
        .store(KILN_UI_STATE_PREGAME as u32, Ordering::SeqCst);
    for s in [0.0, 1.0, 11.0, 60.0, 600.0] {
        assert_eq!(n.step(t + sec(s)), Some(notice_native::IDLE));
    }
    assert_eq!(FAKE.asked.load(Ordering::SeqCst), 0, "no kiln.ui lookup");
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 0);
    assert_eq!(FAKE.demand_calls.load(Ordering::SeqCst), 0);
    assert!(rows(&hub).is_empty());
}

#[test]
fn the_grace_starts_at_the_first_main_menu_frame_never_at_the_process_start() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let t0 = Instant::now();
    // two minutes of title screens: nothing, however long the process has run
    for s in (0..=480).map(|k| k as f64 * 0.25) {
        assert_eq!(n.step(t0 + sec(s)), Some(notice_native::TICK));
    }
    assert!(!n.subscribed() && !n.demanding());
    FAKE.ui_state
        .store(KILN_UI_STATE_PREGAME as u32, Ordering::SeqCst);
    let menu = t0 + sec(120.0);
    n.step(menu);
    n.step(menu + GRACE - sec(0.01));
    assert!(!n.subscribed(), "Kiln.Ui still has its 10 s");
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 0);
    n.step(menu + GRACE);
    assert!(n.subscribed() && n.demanding());
    assert_eq!(FAKE.demand.load(Ordering::SeqCst), 1);
    assert_eq!(lock(&FAKE.subs)[0].owner, OWNER, "kiln-sfi's plugin owner");
    assert_eq!(
        rows(&hub),
        [row(1, DeliveryState::Pending)],
        "pending, not shown"
    );
    // a frame draws it: the title, the summary, the package's own text and the hint, inside begin/end
    assert!(!frame());
    let lines = drawn();
    let text = warning_text(&hub, "ammo-slots");
    assert_eq!(
        lines.first().map(String::as_str),
        Some(format!("begin {TITLE}").as_str())
    );
    assert_eq!(lines[1], "Do not save: 1 mod is off this start");
    assert_eq!(
        lines[2], text,
        "short enough to show whole ({BANNER_CHARS} characters)"
    );
    assert!(text.ends_with(reducer::DO_NOT_SAVE) || text.contains(reducer::DO_NOT_SAVE));
    assert_eq!(lines[3], HINT);
    assert_eq!(lines.last().map(String::as_str), Some("end"));
    assert_eq!(
        (
            FAKE.begins.load(Ordering::SeqCst),
            FAKE.ends.load(Ordering::SeqCst),
            FAKE.outside.load(Ordering::SeqCst)
        ),
        (1, 1, 0),
        "drawing only inside the frame callback, end after begin"
    );
    assert_eq!(
        rows(&hub),
        [row(1, DeliveryState::Pending)],
        "shown only once the control step saw the frame"
    );
    n.step(menu + GRACE + sec(0.25));
    let d = deliveries(&hub);
    assert_eq!(
        (d[0].revision, d[0].state, d[0].world_session),
        (1, DeliveryState::Shown, None),
        "shown at the main menu"
    );
    assert!(d[0].at_ms.is_some());
    let log = logged();
    assert!(
        log.iter()
            .any(|(l, s)| *l == KILN_LOG_WARN && s.contains("Kiln.Ui did not start within 10 s")),
        "{log:?}"
    );
    assert!(
        log.iter()
            .any(|(_, s)| s == &format!("kiln-sfi: WARNING: {text}")),
        "kiln.log has the whole text"
    );
    assert!(
        log.iter()
            .any(|(_, s)| s.contains("banner was shown (warning revision 1, main menu)")),
        "{log:?}"
    );
}

#[test]
fn without_the_games_screens_the_first_presented_frame_starts_the_grace() {
    let _t = reset();
    FAKE.screens.store(false, Ordering::SeqCst);
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let t0 = Instant::now();
    n.step(t0 + sec(30.0));
    assert!(!n.subscribed());
    FAKE.presents.store(1, Ordering::SeqCst);
    let first = t0 + sec(40.0);
    n.step(first);
    n.step(first + GRACE - sec(0.25));
    assert!(!n.subscribed());
    n.step(first + GRACE);
    assert!(n.subscribed());
}

#[test]
fn kiln_ui_ready_keeps_the_banner_off_and_takes_it_down() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let t = Instant::now();
    FAKE.ready.store(true, Ordering::SeqCst);
    let due = past_grace(&n, t);
    for s in [0.0, 5.0, 60.0] {
        n.step(due + sec(s));
    }
    assert_eq!(
        FAKE.subscribes.load(Ordering::SeqCst),
        0,
        "Kiln.Ui shows it"
    );
    assert!(rows(&hub).is_empty(), "Kiln.Ui keeps its own record");
    assert!(
        logged()
            .iter()
            .any(|(_, s)| s.contains("Kiln.Ui shows the do-not-save notice"))
    );
    // a Kiln.Ui that comes up late (after the grace) takes over a showing
    let _t2 = (drop(_t), reset()).1;
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, t);
    assert!(n.subscribed());
    FAKE.ready.store(true, Ordering::SeqCst);
    n.step(due + sec(1.0));
    assert!(!n.subscribed() && !n.demanding());
    assert_eq!(
        (lock(&FAKE.subs).len(), FAKE.demand.load(Ordering::SeqCst)),
        (0, 0)
    );
    assert_eq!(rows(&hub), [row(1, DeliveryState::Pending)]);
}

#[test]
fn a_showing_ends_after_its_time_and_gives_the_fallback_menu_back() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    // not drawn yet: the showing waits for a frame however long it takes
    n.step(due + sec(120.0));
    assert!(n.subscribed());
    frame();
    let drawn_at = due + sec(120.25);
    n.step(drawn_at);
    assert_eq!(rows(&hub), [row(1, DeliveryState::Shown)]);
    n.step(drawn_at + SHOW_FOR - sec(0.25));
    assert!(n.subscribed());
    n.step(drawn_at + SHOW_FOR);
    assert!(!n.subscribed() && !n.demanding());
    assert_eq!(FAKE.demand.load(Ordering::SeqCst), 0);
    // no subscriber of kiln-sfi's: the Kiln menu's own fallback draws (ui.cpp:321)
    FAKE.menu_open.store(true, Ordering::SeqCst);
    assert!(frame(), "the fallback menu draws");
    FAKE.menu_open.store(false, Ordering::SeqCst);
    for s in [1.0, 60.0, 600.0] {
        n.step(drawn_at + SHOW_FOR + sec(s));
    }
    assert_eq!(
        FAKE.subscribes.load(Ordering::SeqCst),
        1,
        "the same revision does not come back by itself"
    );
    assert_eq!(rows(&hub), [row(1, DeliveryState::Shown)]);
}

#[test]
fn the_open_kiln_menu_pauses_the_banner_so_its_fallback_draws() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    assert!(n.subscribed());
    FAKE.menu_open.store(true, Ordering::SeqCst);
    n.step(due + sec(0.25));
    assert!(!n.subscribed() && !n.demanding());
    assert!(frame(), "the fallback menu draws while it is open");
    FAKE.menu_open.store(false, Ordering::SeqCst);
    n.step(due + sec(0.5));
    assert!(n.subscribed() && n.demanding(), "back once it closes");
    assert!(!frame());
    n.step(due + sec(0.75));
    assert_eq!(rows(&hub), [row(1, DeliveryState::Shown)]);
}

// ---- no overlay: not shown, never shown ----

const SETTING_OFF: &str =
    "the overlay's drawing is off: turned off in Kiln's settings (kiln.json ui.overlay)";

#[test]
fn overlay_off_records_not_shown_and_never_subscribes() {
    let _t = reset();
    assert!(SETTING_OFF.starts_with(OVERLAY_OFF));
    *lock(&FAKE.error) = SETTING_OFF.into();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let t = Instant::now();
    // no wait for a menu frame: no frame can draw it at all
    n.step(t);
    let d = deliveries(&hub);
    assert_eq!(
        (d.len(), d[0].revision, d[0].state, d[0].world_session),
        (1, 1, DeliveryState::NotShown, None)
    );
    past_grace(&n, t);
    for s in [11.0, 60.0] {
        n.step(t + sec(s));
    }
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 0);
    assert_eq!(FAKE.demand_calls.load(Ordering::SeqCst), 0);
    assert_eq!(
        rows(&hub),
        [row(1, DeliveryState::NotShown)],
        "once per revision"
    );
    let errors: Vec<String> = logged()
        .into_iter()
        .filter(|(l, _)| *l == KILN_LOG_ERROR)
        .map(|(_, s)| s)
        .collect();
    assert_eq!(
        errors.len(),
        2,
        "one loud line and the warning's text: {errors:?}"
    );
    assert!(
        errors[0].contains(SETTING_OFF) && errors[0].contains("next Play"),
        "{errors:?}"
    );
    assert_eq!(
        errors[1],
        format!("kiln-sfi: WARNING: {}", warning_text(&hub, "ammo-slots"))
    );
    // a new warning: not shown either, said again
    skipped(&hub, "other-mod");
    n.step(t + sec(61.0));
    assert_eq!(
        rows(&hub),
        [
            row(1, DeliveryState::NotShown),
            row(2, DeliveryState::NotShown)
        ]
    );
}

#[test]
fn drawing_that_turns_off_during_a_showing_is_not_shown_unless_a_frame_drew_it() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    assert!(n.subscribed());
    *lock(&FAKE.error) = "the overlay's drawing is off: its Vulkan submit failed".into();
    n.step(due + sec(0.25));
    assert!(!n.subscribed() && !n.demanding());
    assert_eq!(rows(&hub), [row(1, DeliveryState::NotShown)]);
    // drawn first: it stays shown
    let _t2 = (drop(_t), reset()).1;
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    frame();
    n.step(due + sec(0.25));
    *lock(&FAKE.error) = "the overlay's drawing is off: its Vulkan submit failed".into();
    n.step(due + sec(0.5));
    assert!(!n.subscribed());
    assert_eq!(rows(&hub), [row(1, DeliveryState::Shown)]);
}

#[test]
fn drawing_off_after_kiln_ui_made_its_event_is_not_shown_too() {
    // a side case: the event stays once made, but no surface can draw any more
    let _t = reset();
    FAKE.ready.store(true, Ordering::SeqCst);
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    past_grace(&n, Instant::now());
    assert!(rows(&hub).is_empty());
    *lock(&FAKE.error) = "the overlay's drawing is off: a fault in the overlay".into();
    n.step(Instant::now() + sec(20.0));
    assert_eq!(rows(&hub), [row(1, DeliveryState::NotShown)]);
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 0);
}

#[test]
fn no_kiln_ui_table_is_not_shown() {
    let _t = reset();
    FAKE.table.store(false, Ordering::SeqCst);
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    n.step(Instant::now());
    n.step(Instant::now() + sec(30.0));
    assert_eq!(rows(&hub), [row(1, DeliveryState::NotShown)]);
    assert_eq!(FAKE.asked.load(Ordering::SeqCst), 1, "looked up once");
    assert!(
        logged()
            .iter()
            .any(|(l, s)| *l == KILN_LOG_ERROR && s.contains("kiln.ui is not available"))
    );
}

// ---- again ----

#[test]
fn a_new_revision_and_entering_a_world_show_it_again() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    frame();
    n.step(due + sec(0.25));
    let ended = due + sec(0.25) + SHOW_FOR;
    n.step(ended);
    assert!(!n.subscribed());
    // a new warning (revision 2): a new showing with both packages
    skipped(&hub, "other-mod");
    n.step(ended + sec(5.0));
    assert!(n.subscribed());
    assert_eq!(
        rows(&hub),
        [row(1, DeliveryState::Shown), row(2, DeliveryState::Pending)]
    );
    lock(&FAKE.lines).clear();
    frame();
    assert_eq!(drawn()[1], "Do not save: 2 mods are off this start");
    n.step(ended + sec(5.25));
    let again = ended + sec(5.25) + SHOW_FOR;
    n.step(again);
    assert!(!n.subscribed());
    // entering a world (client world session 3): shown again, the row says where
    FAKE.world.store(3, Ordering::SeqCst);
    FAKE.ui_state
        .store(KILN_UI_STATE_PLAY as u32, Ordering::SeqCst);
    n.step(again + sec(10.0));
    assert!(n.subscribed());
    frame();
    n.step(again + sec(10.25));
    let d = deliveries(&hub);
    assert_eq!(
        (d[1].revision, d[1].state, d[1].world_session),
        (2, DeliveryState::Shown, Some(3))
    );
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 3);
    // staying in the world shows nothing more
    n.step(again + sec(10.25) + SHOW_FOR);
    n.step(again + sec(200.0));
    assert!(!n.subscribed());
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 3);
}

#[test]
fn a_mod_that_cannot_lose_save_data_is_listed_without_do_not_save_until_one_can() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    hub.update(|r| {
        r.add_package(PackageInit::selected("plain", Some("1.0.0"), None))
            .max(r.skip(
                "plain",
                Skip::new(
                    "dll-gate",
                    Part::Dll,
                    "its DLL was refused by the native load gate",
                ),
            ))
    });
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    frame();
    let lines = drawn();
    let text = warning_text(&hub, "plain");
    assert_eq!(lines[1], "1 mod is off this start");
    assert_eq!(lines[2], text, "which mod and why");
    assert!(!text.contains(reducer::DO_NOT_SAVE), "{text}");
    // a runtime that stops during the start (its data still applied): saving can lose data now
    hub.update(|r| {
        r.add_package(PackageInit::selected("late-one", Some("1.0.0"), None))
            .max(r.late_loss("late-one", "runtime-error", "runtime error in on_update"))
    });
    n.step(due + sec(0.5));
    lock(&FAKE.lines).clear();
    frame();
    assert_eq!(drawn()[1], "Do not save: 2 mods are off this start");
}

#[test]
fn many_warnings_show_the_first_three_and_count_the_rest() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    for k in 0..5 {
        skipped(&hub, &format!("mod-{k}"));
    }
    hub.update(|r| {
        r.add_package(PackageInit::selected("long-one", Some("1.0.0"), None))
            .max(r.skip(
                "long-one",
                Skip::new("dll-gate", Part::Dll, "x".repeat(2000)),
            ))
    });
    let n = notice(&hub);
    past_grace(&n, Instant::now());
    frame();
    let lines = drawn();
    assert_eq!(lines[1], "Do not save: 6 mods are off this start");
    assert_eq!(lines[5], "and 3 more");
    assert_eq!(lines.len(), 1 + 1 + 3 + 1 + 1 + 1, "{lines:?}");
}

#[test]
fn a_long_warning_is_cut_on_the_banner_and_whole_in_kiln_log() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    hub.update(|r| {
        r.add_package(PackageInit::selected("long-one", Some("1.0.0"), None))
            .max(r.skip(
                "long-one",
                Skip::new("dll-gate", Part::Dll, "é".repeat(2000)),
            ))
    });
    let n = notice(&hub);
    past_grace(&n, Instant::now());
    frame();
    let text = warning_text(&hub, "long-one");
    let shown = &drawn()[2];
    assert!(
        shown.ends_with("...") && shown.chars().count() <= BANNER_CHARS + 3,
        "{shown}"
    );
    assert!(text.starts_with(shown.trim_end_matches("...")));
    assert!(
        logged()
            .iter()
            .any(|(_, s)| s == &format!("kiln-sfi: WARNING: {text}"))
    );
}

// ---- a dedicated server ----

#[test]
fn a_dedicated_server_logs_each_warning_once_and_never_looks_up_kiln_ui() {
    let _t = reset();
    let hub = new_hub(Side::Server);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let t = Instant::now();
    for s in [0.0, 1.0, 30.0, 300.0] {
        assert_eq!(n.step(t + sec(s)), Some(notice_native::IDLE));
    }
    skipped(&hub, "other-mod");
    n.step(t + sec(301.0));
    let lines: Vec<String> = logged()
        .into_iter()
        .filter(|(l, _)| *l == KILN_LOG_ERROR)
        .map(|(_, s)| s)
        .collect();
    assert_eq!(
        lines,
        [
            format!("kiln-sfi: WARNING: {}", warning_text(&hub, "ammo-slots")),
            format!("kiln-sfi: WARNING: {}", warning_text(&hub, "other-mod")),
        ]
    );
    assert_eq!(FAKE.asked.load(Ordering::SeqCst), 0);
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 0);
    assert!(rows(&hub).is_empty(), "no surface on a server");
    // a final record: logged, then the step ends
    let _ = hub.end();
    assert_eq!(n.step(t + sec(302.0)), None);
}

// ---- lifetime ----

#[test]
fn close_unsubscribes_clears_the_demand_and_ends_the_step() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    assert!(n.subscribed());
    n.close();
    assert!(!n.subscribed() && !n.demanding());
    assert_eq!(
        (lock(&FAKE.subs).len(), FAKE.demand.load(Ordering::SeqCst)),
        (0, 0)
    );
    let calls = FAKE.calls.load(Ordering::SeqCst);
    frame();
    assert_eq!(
        FAKE.calls.load(Ordering::SeqCst),
        calls,
        "no call after close"
    );
    assert_eq!(n.step(due + sec(1.0)), None);
    assert_eq!(FAKE.subscribes.load(Ordering::SeqCst), 1);
}

#[test]
fn a_refused_unsubscribe_keeps_the_banner_state_alive_for_later_frames() {
    // kiln.ui answers something other than OK: the subscription keeps its count of the render-side state, so frames
    // that still call it after the Notice is gone find live memory (ASan: no use after free) and draw nothing
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    past_grace(&n, Instant::now());
    frame();
    FAKE.refuse_unsubscribe.store(true, Ordering::SeqCst);
    n.close();
    assert!(n.subscribed(), "kiln.ui still has it");
    assert!(
        logged()
            .iter()
            .any(|(_, s)| s.contains("unsubscribe of the do-not-save banner answered"))
    );
    drop(n);
    drop(hub);
    let (begins, calls) = (
        FAKE.begins.load(Ordering::SeqCst),
        FAKE.calls.load(Ordering::SeqCst),
    );
    for _ in 0..100 {
        frame();
    }
    assert_eq!(
        FAKE.calls.load(Ordering::SeqCst),
        calls + 100,
        "kiln.ui still calls it"
    );
    assert_eq!(
        FAKE.begins.load(Ordering::SeqCst),
        begins,
        "and it draws nothing"
    );
    FAKE.refuse_unsubscribe.store(false, Ordering::SeqCst);
    lock(&FAKE.subs).clear();
}

fn wait_for(flag: &AtomicBool, what: &str) {
    let end = Instant::now() + WAIT;
    while !flag.load(Ordering::SeqCst) {
        assert!(Instant::now() < end, "{what}");
        std::thread::yield_now();
    }
}
/// Closes `n` on another thread once kiln.ui's unsubscribe waits on the gate; the receiver gets close's return.
fn close_on_another_thread(n: &Arc<Notice>) -> (std::thread::JoinHandle<()>, mpsc::Receiver<()>) {
    let (tx, rx) = mpsc::channel();
    let n = Arc::clone(n);
    let closer = std::thread::spawn(move || {
        n.close();
        tx.send(()).unwrap();
    });
    wait_for(
        &FAKE.unsub_waiting,
        "close never reached kiln.ui's unsubscribe",
    );
    (closer, rx)
}

#[test]
fn unsubscribe_waits_for_a_frame_that_is_drawing() {
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = Arc::new(notice(&hub));
    past_grace(&n, Instant::now());
    *lock(&FAKE.hold) = true;
    let render = std::thread::spawn(frame);
    wait_for(&FAKE.entered, "the frame never reached begin");
    // the callback is inside begin: close (the control lock, then kiln.ui's unsubscribe) waits for it
    let (closer, rx) = close_on_another_thread(&n);
    std::thread::sleep(Duration::from_millis(50));
    assert!(
        rx.try_recv().is_err(),
        "close returned while the callback ran"
    );
    *lock(&FAKE.hold) = false;
    FAKE.held.notify_all();
    rx.recv_timeout(WAIT)
        .expect("close never returned: deadlock");
    render.join().unwrap();
    closer.join().unwrap();
    assert!(!n.subscribed());
    assert_eq!(
        (
            FAKE.begins.load(Ordering::SeqCst),
            FAKE.ends.load(Ordering::SeqCst)
        ),
        (1, 1),
        "the frame in flight finished its window"
    );
}

#[test]
fn a_callback_that_starts_while_close_waits_needs_no_lock_close_holds() {
    // ui.cpp's window: the callback passed its active check, then unsubscribe marks it inactive and waits on the
    // gate. The callback runs now; if it needed the control lock or the banner lock while close held one across
    // unsubscribe, neither thread would go on.
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = Arc::new(notice(&hub));
    past_grace(&n, Instant::now());
    *lock(&FAKE.hold_call) = true;
    let render = std::thread::spawn(frame);
    wait_for(&FAKE.at_call, "the frame never reached the call");
    let (closer, rx) = close_on_another_thread(&n);
    assert!(rx.try_recv().is_err());
    *lock(&FAKE.hold_call) = false;
    FAKE.held.notify_all();
    rx.recv_timeout(WAIT)
        .expect("close never returned: deadlock");
    render.join().unwrap();
    closer.join().unwrap();
    assert_eq!(FAKE.calls.load(Ordering::SeqCst), 1, "the call went ahead");
    assert_eq!(
        FAKE.begins.load(Ordering::SeqCst),
        0,
        "close had taken the banner away first"
    );
}

#[test]
fn frames_on_the_render_thread_while_the_banner_turns_on_and_off_then_close_and_drop() {
    // the frame-callback/unsubscribe path under load: a render thread draws as fast as it can while the control step
    // subscribes and unsubscribes (the Kiln menu opening and closing), then close and drop the Notice while frames
    // go on; after close no call reaches kiln-sfi
    let _t = reset();
    let hub = new_hub(Side::Client);
    skipped(&hub, "ammo-slots");
    let n = notice(&hub);
    let due = past_grace(&n, Instant::now());
    let stop = Arc::new(AtomicBool::new(false));
    let render = {
        let stop = Arc::clone(&stop);
        std::thread::spawn(move || {
            let mut frames = 0u64;
            while !stop.load(Ordering::SeqCst) {
                frame();
                frames += 1;
            }
            frames
        })
    };
    let turns = std::env::var("SFI_NOTICE_TURNS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(2000u32);
    for k in 0..turns {
        FAKE.menu_open.store(k % 2 == 1, Ordering::SeqCst);
        // the clock stays inside the showing's time
        n.step(due);
    }
    FAKE.menu_open.store(false, Ordering::SeqCst);
    n.step(due);
    n.close();
    let calls = FAKE.calls.load(Ordering::SeqCst);
    drop(n);
    drop(hub);
    let more = FAKE.frames.load(Ordering::SeqCst) + 200;
    let end = Instant::now() + WAIT;
    while FAKE.frames.load(Ordering::SeqCst) < more {
        assert!(Instant::now() < end, "the render thread stopped");
        std::thread::yield_now();
    }
    stop.store(true, Ordering::SeqCst);
    let frames = render.join().unwrap();
    assert_eq!(
        FAKE.calls.load(Ordering::SeqCst),
        calls,
        "a call after close"
    );
    assert!(frames > 200);
    assert!(FAKE.subscribes.load(Ordering::SeqCst) > 1);
    assert_eq!(lock(&FAKE.subs).len(), 0);
    assert_eq!(
        FAKE.begins.load(Ordering::SeqCst),
        FAKE.ends.load(Ordering::SeqCst)
    );
    assert_eq!(FAKE.outside.load(Ordering::SeqCst), 0);
}

// ---- sfi.json's delivery rows (the reducer's rules) ----

fn reducer_with(revisions: u64) -> Reducer {
    let mut r = Reducer::new(
        identity(Side::Client),
        PreviousStart::NONE,
        1_763_520_000_200,
        Phase::Starting,
        None,
    );
    for k in 0..revisions {
        let id = format!("mod-{k}");
        r.add_package(PackageInit::selected(&id, Some("1.0.0"), None));
        r.skip(&id, Skip::new("dll-gate", Part::Dll, "refused"));
    }
    r
}
fn d(
    revision: u64,
    state: DeliveryState,
    world_session: Option<u64>,
    at_ms: Option<u64>,
) -> Delivery {
    Delivery {
        revision,
        state,
        world_session,
        at_ms,
    }
}

#[test]
fn delivery_rows_keep_shown_final_and_only_name_revisions_the_record_has() {
    let mut r = reducer_with(2);
    assert_eq!(r.warning_revision(), 2);
    assert_eq!(
        r.set_delivery(d(0, DeliveryState::Pending, None, None)),
        Change::None
    );
    assert_eq!(
        r.set_delivery(d(3, DeliveryState::Pending, None, None)),
        Change::None,
        "no such revision"
    );
    assert_eq!(
        r.set_delivery(d(1, DeliveryState::Pending, Some(9), None)),
        Change::Warning
    );
    assert_eq!(
        r.deliveries()[0].world_session,
        None,
        "pending has no world session"
    );
    assert_eq!(
        r.set_delivery(d(1, DeliveryState::Pending, None, None)),
        Change::None,
        "the same"
    );
    assert_eq!(
        r.set_delivery(d(1, DeliveryState::Shown, None, None)),
        Change::Warning
    );
    assert_eq!(r.deliveries()[0].at_ms, Some(0), "shown always has a time");
    for later in [DeliveryState::Pending, DeliveryState::NotShown] {
        assert_eq!(
            r.set_delivery(d(1, later, None, Some(5))),
            Change::None,
            "shown is final"
        );
    }
    assert_eq!(
        r.set_delivery(d(1, DeliveryState::Shown, Some(3), Some(7))),
        Change::Warning,
        "a showing again says where and when"
    );
    assert_eq!(
        r.set_delivery(d(2, DeliveryState::NotShown, Some(3), Some(8))),
        Change::Warning
    );
    assert_eq!(
        r.deliveries()[1].world_session,
        None,
        "not-shown has no world session"
    );
    assert_eq!(
        r.set_delivery(d(2, DeliveryState::Pending, None, None)),
        Change::None
    );
    let v = r.to_value(1);
    assert_eq!(
        v["delivery"],
        serde_json::json!([
            {"surface": "native", "revision": 1, "state": "shown", "worldSession": 3, "atMs": 7},
            {"surface": "native", "revision": 2, "state": "not-shown", "worldSession": null, "atMs": 8},
        ])
    );
    assert_eq!(
        r.set_delivery(d(2, DeliveryState::Shown, None, Some(9))),
        Change::Warning,
        "not-shown, then a frame"
    );
    r.set_phase(Phase::Ended, None);
    assert_eq!(
        r.set_delivery(d(2, DeliveryState::Shown, Some(1), Some(10))),
        Change::None,
        "sealed"
    );
    assert_eq!(
        r.to_value(1)["delivery"].as_array().unwrap().len(),
        2,
        "kept in the final record"
    );
}

#[test]
fn delivery_rows_are_absent_until_the_banner_did_something_and_bounded() {
    let mut r = reducer_with(70);
    assert!(
        r.to_value(1).get("delivery").is_none(),
        "no key: the writer's older documents stay byte-identical"
    );
    for k in 1..=70 {
        r.set_delivery(d(k, DeliveryState::NotShown, None, Some(k)));
    }
    let rows = r.deliveries();
    assert_eq!(rows.len(), reducer::MAX_DELIVERY);
    assert_eq!(
        (rows[0].revision, rows[63].revision),
        (7, 70),
        "the oldest went"
    );
    assert_eq!(
        r.set_delivery(d(3, DeliveryState::Shown, None, Some(1))),
        Change::None,
        "older than every kept row"
    );
}

// ---- the boundary: on_ready starts it on kiln-sfi's IO thread, on_unload takes it down ----

#[test]
fn on_ready_starts_the_banner_on_the_io_thread_and_on_unload_closes_it() {
    use kiln_abi::services::kiln_launch::*;
    use kiln_sfi::worker::{Worker, empty_runtime};
    use kiln_sfi::{Boundary, InactiveReason};
    use support::*;
    let _t = reset();
    set_launch(Some((KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)));
    let dir = fresh_dir("notice-boundary");
    set_state_dir(dir.clone());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(empty_runtime)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_notice(HOOKS),
    ));
    let (early, normal) = (next_owner(), next_owner());
    let _pool = FakePool::new(normal, PoolConfig::default());
    let _early_pool = FakePool::new(early, PoolConfig::default());
    let api = Box::leak(api(true));
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    unsafe {
        assert_eq!(
            boundary.enter(kiln_sfi::Phase::Early, &*api, early, &mut early_info),
            KILN_OK
        );
        assert_eq!(
            boundary.enter(kiln_sfi::Phase::Normal, &*api, normal, &mut info),
            KILN_OK
        );
    }
    assert!(
        boundary.notice().is_none(),
        "nothing before Kiln's plugin step"
    );
    unsafe { (info.on_ready.unwrap())(info.user) };
    let notice = boundary.notice().expect("on_ready starts it");
    // a warning arrives; this host has no kiln.ui, so kiln-sfi's IO thread records it not shown, loudly
    skipped(boundary.status(), "ammo-slots");
    let end = Instant::now() + WAIT;
    while !boundary
        .status()
        .read(|r| {
            r.deliveries()
                .iter()
                .any(|d| d.state == DeliveryState::NotShown)
        })
        .unwrap()
    {
        assert!(
            Instant::now() < end,
            "the IO thread never ran the banner's step"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    assert!(
        logs(normal)
            .iter()
            .any(|(l, s)| *l == KILN_LOG_ERROR && s.contains("kiln.ui is not available"))
    );
    unsafe { (info.on_unload.unwrap())(info.user) };
    assert!(!notice.subscribed());
    assert_eq!(notice.step(Instant::now()), None, "closed by on_unload");
    let v: serde_json::Value =
        serde_json::from_slice(&std::fs::read(dir.join("sfi.json")).unwrap()).unwrap();
    assert_eq!(v["phase"], "ended");
    assert_eq!(
        v["delivery"][0]["state"], "not-shown",
        "in the final record"
    );
}
