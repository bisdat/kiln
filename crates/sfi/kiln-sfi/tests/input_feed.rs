// SPDX-License-Identifier: MIT
//! The input feed (kiln-sfi src/input). The key state is the ShroudForge fork's host feed
//! (modules/hotkeys/observe_keyboard.rs, host_input.rs, host_keyboard.c at 1.7.9 d8a394bc) and its
//! own host test is ported here; the window-thread handoff runs through a real C-ABI fake of kiln.ui
//! v5 (subscribe_window_input, unsubscribe, status) and the runtime's ServiceAdapter.
mod support;

use kiln_abi::services::kiln_ui::*;
use kiln_abi::*;
use kiln_sfi::input::feed::{
    FeedConfig, KeyFeed, PressTracker, Record, StopOutcome, message_scancode, modifier_groups,
    raw_scancode, scancode_of_set1, set1_of_scancode,
};
use kiln_sfi::input::label::{self, Binding, KeyNames, LabelTable};
use kiln_sfi::input::{Desktop, FeedError, InputFeed};
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::worker::Host;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

// ---- records as kiln.ui gives them ----

const WM_INPUT: u32 = 0x00FF;
const WM_KEYDOWN: u32 = 0x0100;
const WM_KEYUP: u32 = 0x0101;
const WM_XBUTTONDOWN: u32 = 0x020B;
const WM_XBUTTONUP: u32 = 0x020C;
const WM_KILLFOCUS: u32 = 0x0008;
const E0: u32 = 2;
const E1: u32 = 4;
const UP: u32 = 1;

fn record(kind: i32, message: u32) -> KilnUiWindowInput {
    KilnUiWindowInput {
        size: size_of::<KilnUiWindowInput>() as u32,
        kind: kind as u32,
        message,
        make_code: 0,
        flags: 0,
        vkey: 0,
        button: 0,
        down: 0,
        wparam: 0,
        lparam: 0,
    }
}
fn raw(make_code: u32, flags: u32) -> KilnUiWindowInput {
    KilnUiWindowInput {
        make_code,
        flags,
        lparam: 0x5151_0000, // an HRAWINPUT: valid during the call only, never kept
        ..record(KILN_UI_INPUT_RAW_KEY, WM_INPUT)
    }
}
/// A key message with scan code `scan` (bit 24: extended), down or up.
fn message(scan: u32, extended: bool, up: bool) -> KilnUiWindowInput {
    let lparam = 1 | (scan << 16) | (u32::from(extended) << 24) | if up { 3 << 30 } else { 0 };
    KilnUiWindowInput {
        lparam: i64::from(lparam as i32) & 0xffff_ffff,
        ..record(
            KILN_UI_INPUT_WINDOW_KEY,
            if up { WM_KEYUP } else { WM_KEYDOWN },
        )
    }
}
fn button(button: u32, down: bool, raw: bool) -> KilnUiWindowInput {
    KilnUiWindowInput {
        button,
        down: u32::from(down),
        ..record(
            KILN_UI_INPUT_MOUSE_BUTTON,
            if raw {
                WM_INPUT
            } else if down {
                WM_XBUTTONDOWN
            } else {
                WM_XBUTTONUP
            },
        )
    }
}
fn focus_lost() -> KilnUiWindowInput {
    record(KILN_UI_INPUT_FOCUS_LOST, WM_KILLFOCUS)
}

fn key(text: &str) -> Binding {
    Binding::parse(text).unwrap().unwrap()
}
fn no_vk(_: u16) -> u16 {
    0
}

// ---- a fake kiln.ui v5, per worker thread ----

type WindowFn = unsafe extern "C" fn(*const KilnUiWindowInput, *mut c_void);

#[derive(Default)]
struct Registration {
    function: Option<WindowFn>,
    user: usize,
    handle: KilnHandle,
    removed: bool,
}

/// kiln.ui as kiln_ui.h describes it: unsubscribe returns KILN_OK once a call in flight has ended,
/// and nothing is called afterwards; another answer leaves the subscription in place.
struct FakeUi {
    table: KilnUiApi,
    registration: Mutex<Registration>,
    in_flight: AtomicU32,
    capturing: AtomicBool,
    subscribe_answer: AtomicI32,
    unsubscribe_answer: AtomicI32,
    unsubscribes: AtomicU32,
}

thread_local! {
    static CURRENT: RefCell<Option<Arc<FakeUi>>> = const { RefCell::new(None) };
    /// The window thread's raw-input probe answer (the delivering thread's).
    static RAW_REGISTERED: Cell<bool> = const { Cell::new(true) };
}
fn current() -> Arc<FakeUi> {
    CURRENT.with(|c| c.borrow().clone().expect("a fake kiln.ui on this thread"))
}

unsafe extern "C" fn fake_status(out: *mut KilnUiStatus) -> KilnResult {
    let ui = current();
    if out.is_null() || unsafe { (*out).size } < size_of::<KilnUiStatus>() as u32 {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    unsafe {
        (*out).capturing = u8::from(ui.capturing.load(Ordering::SeqCst));
    }
    KILN_OK
}
unsafe extern "C" fn fake_subscribe(
    owner: KilnOwner,
    function: KilnUiWindowInputFn,
    user: *mut c_void,
    out: *mut KilnHandle,
) -> KilnResult {
    let ui = current();
    let answer = ui.subscribe_answer.load(Ordering::SeqCst);
    let Some(function) = function else {
        return KILN_ERR_INVALID_ARGUMENT;
    };
    if owner == 0 || out.is_null() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if answer != KILN_OK {
        return answer;
    }
    let mut r = ui.registration.lock().unwrap();
    *r = Registration {
        function: Some(function),
        user: user as usize,
        handle: 77,
        removed: false,
    };
    unsafe { *out = 77 };
    KILN_OK
}
unsafe extern "C" fn fake_unsubscribe(handle: KilnHandle) -> KilnResult {
    let ui = current();
    ui.unsubscribes.fetch_add(1, Ordering::SeqCst);
    let answer = ui.unsubscribe_answer.load(Ordering::SeqCst);
    if answer != KILN_OK {
        return answer;
    }
    {
        let mut r = ui.registration.lock().unwrap();
        if r.handle != handle || r.removed {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        r.removed = true;
    }
    while ui.in_flight.load(Ordering::SeqCst) > 0 {
        std::thread::yield_now();
    }
    KILN_OK
}
unsafe extern "C" fn fake_lookup(name: KilnStr, min_version: u32) -> *const c_void {
    let text = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
    if text != b"kiln.ui" || min_version > 5 {
        return std::ptr::null();
    }
    CURRENT.with(|c| {
        c.borrow()
            .as_ref()
            .map_or(std::ptr::null(), |ui| (&raw const ui.table).cast())
    })
}

impl FakeUi {
    fn new() -> Arc<Self> {
        // SAFETY: a table of plain integers and nullable function pointers.
        let mut table: KilnUiApi = unsafe { std::mem::zeroed() };
        table.header.size = size_of::<KilnUiApi>() as u32;
        table.header.version = 5;
        table.status = Some(fake_status);
        table.subscribe_window_input = Some(fake_subscribe);
        table.unsubscribe = Some(fake_unsubscribe);
        Arc::new(Self {
            table,
            registration: Mutex::new(Registration::default()),
            in_flight: AtomicU32::new(0),
            capturing: AtomicBool::new(false),
            subscribe_answer: AtomicI32::new(KILN_OK),
            unsubscribe_answer: AtomicI32::new(KILN_OK),
            unsubscribes: AtomicU32::new(0),
        })
    }
    /// The window thread: one record through the subscription, as kiln.ui's window procedure does.
    /// `hold` runs while the call counts as in flight, before the function is called.
    fn deliver_with(&self, input: &KilnUiWindowInput, hold: impl FnOnce()) -> bool {
        let (function, user) = {
            let r = self.registration.lock().unwrap();
            let Some(function) = r.function.filter(|_| !r.removed) else {
                return false;
            };
            self.in_flight.fetch_add(1, Ordering::SeqCst);
            (function, r.user)
        };
        hold();
        unsafe { function(input, user as *mut c_void) };
        self.in_flight.fetch_sub(1, Ordering::SeqCst);
        true
    }
    fn deliver(&self, input: KilnUiWindowInput) -> bool {
        self.deliver_with(&input, || {})
    }
}

fn probe(_: u16) -> bool {
    RAW_REGISTERED.with(Cell::get)
}

/// A desktop the test steers: focus, a virtual-key map, a layout and its names.
#[derive(Clone, Default)]
struct FakeDesktop {
    unfocused: Arc<AtomicBool>,
    layout: Arc<AtomicUsize>,
    names: Arc<Mutex<HashMap<usize, Vec<String>>>>,
    builds: Arc<AtomicUsize>,
}
impl Desktop for FakeDesktop {
    fn game_focused(&self) -> bool {
        !self.unfocused.load(Ordering::SeqCst)
    }
    fn scan_of_virtual_key(&self, vk: u16, _: u32) -> u16 {
        match vk {
            0xB3 => 0xe022, // VK_MEDIA_PLAY_PAUSE
            _ => 0,
        }
    }
    fn layout(&self, _: u32) -> usize {
        self.layout.load(Ordering::SeqCst)
    }
    fn key_names(&self, layout: usize) -> Vec<String> {
        self.builds.fetch_add(1, Ordering::SeqCst);
        self.names
            .lock()
            .unwrap()
            .get(&layout)
            .cloned()
            .unwrap_or_default()
    }
}

struct Setup {
    ui: Arc<FakeUi>,
    adapter: ServiceAdapter,
    _api: Box<KilnApi>,
    desktop: FakeDesktop,
}
fn setup() -> Setup {
    let ui = FakeUi::new();
    CURRENT.with(|c| *c.borrow_mut() = Some(ui.clone()));
    let mut api = support::api(false);
    api.service = Some(fake_lookup);
    let host = unsafe { Host::read(&*api) };
    let adapter = ServiceAdapter::new(host, Rc::new(Watchdog::new(process_clock())));
    Setup {
        ui,
        adapter,
        _api: api,
        desktop: FakeDesktop::default(),
    }
}
fn config(capacity: usize) -> FeedConfig {
    FeedConfig { capacity, probe }
}
fn feed(s: &Setup, capacity: usize) -> InputFeed {
    InputFeed::subscribe(
        &s.adapter,
        support::next_owner(),
        Box::new(s.desktop.clone()),
        config(capacity),
    )
    .unwrap()
}

// ---- labels ----

struct Names(HashMap<u16, String>);
impl KeyNames for Names {
    fn key_name(&self, scancode: u16) -> Option<&str> {
        self.0.get(&scancode).map(String::as_str)
    }
}

/// SF fixture parity for binding.label(): every case of tests/fixtures/input/labels.json.
#[test]
fn keybind_labels_match_shroudforge_s_label_rules() {
    let text = include_str!("fixtures/input/labels.json");
    let fixture: serde_json::Value = serde_json::from_str(text).unwrap();
    let mut checked = 0;
    for case in fixture["cases"].as_array().unwrap() {
        let binding = case["binding"].as_str().unwrap();
        let names = Names(
            case["names"]
                .as_object()
                .unwrap()
                .iter()
                .map(|(k, v)| (k.parse().unwrap(), v.as_str().unwrap().to_owned()))
                .collect(),
        );
        let parsed = Binding::parse(binding).unwrap_or_else(|e| panic!("{binding}: {e}"));
        let label = parsed.as_ref().map(|b| b.label(&names));
        assert_eq!(label.as_deref(), case["label"].as_str(), "{binding}");
        if let Some(b) = &parsed {
            assert_eq!(
                b.normalized(),
                binding,
                "the canonical text is the signature"
            );
        }
        checked += 1;
    }
    for case in fixture["invalid"].as_array().unwrap() {
        let binding = case["binding"].as_str().unwrap();
        assert!(
            Binding::parse(binding).is_err(),
            "{binding} is no canonical binding"
        );
        checked += 1;
    }
    assert_eq!(checked, 29);
}

#[test]
fn a_label_table_counts_a_new_revision_only_when_its_names_change() {
    let mut table = LabelTable::default();
    assert_eq!(table.revision(), 0);
    assert!(table.replace(vec!["".into(), "A".into()]));
    assert_eq!(table.revision(), 1);
    assert!(
        !table.replace(vec!["".into(), "A".into()]),
        "the same names: no new revision"
    );
    assert_eq!(table.revision(), 1);
    assert!(table.replace(vec!["".into(), "Q".into()]));
    assert_eq!(table.revision(), 2);
    assert_eq!(table.key_name(1), Some("Q"));
    assert_eq!(label::key_label(&table, 2), "SDL:2");
}

#[test]
fn the_feed_labels_in_the_window_layout_and_rebuilds_on_a_change_or_a_focus_loss() {
    let s = setup();
    let mut us = vec![String::new(); label::SCANCODE_COUNT];
    us[66] = "F9".into();
    us[4] = "A".into();
    let mut fr = us.clone();
    fr[4] = "Q".into();
    s.desktop.names.lock().unwrap().extend([(1, us), (2, fr)]);
    s.desktop.layout.store(1, Ordering::SeqCst);
    let mut f = feed(&s, 64);
    assert_eq!(f.label(&key("Ctrl+SDL:4|Mouse:4")), "Ctrl+A / Mouse X1");
    assert_eq!(f.layout_revision(), 1);
    let builds = s.desktop.builds.load(Ordering::SeqCst);
    assert_eq!(f.label(&key("SDL:66")), "F9");
    assert_eq!(
        s.desktop.builds.load(Ordering::SeqCst),
        builds,
        "same layout: the table is kept"
    );
    s.desktop.layout.store(2, Ordering::SeqCst);
    assert_eq!(f.label(&key("SDL:4")), "Q", "the window's layout changed");
    assert_eq!(f.layout_revision(), 2);
    s.ui.deliver(focus_lost());
    f.pump(&s.adapter);
    let builds = s.desktop.builds.load(Ordering::SeqCst);
    assert_eq!(f.label(&key("SDL:4")), "Q");
    assert_eq!(
        s.desktop.builds.load(Ordering::SeqCst),
        builds + 1,
        "rebuilt after a focus loss"
    );
    assert_eq!(f.layout_revision(), 2, "same names: same revision");
    f.stop(&s.adapter);
}

// ---- scancodes (the fork's own test, host_input.rs window_key_records_get_sdl_scancodes) ----

fn raw_code(make_code: u16, flags: u16) -> u16 {
    let mut pending = false;
    raw_scancode(make_code, flags, 0, &mut pending, &no_vk)
}
fn message_code(scan: u32, extended: bool) -> u16 {
    message_scancode(0, 1 | (scan << 16) | (u32::from(extended) << 24), &no_vk)
}

#[test]
fn window_key_records_get_shroudforge_s_sdl_scancodes() {
    assert_eq!(raw_code(0x43, 0), 66, "F9");
    assert_eq!(raw_code(0x1e, 0), 4, "A");
    assert_eq!(raw_code(0x1d, 0), 224, "left Ctrl");
    assert_eq!(raw_code(0x1d, E0 as u16), 228, "right Ctrl");
    assert_eq!(raw_code(0x38, E0 as u16), 230, "right Alt");
    assert_eq!(raw_code(0x48, E0 as u16), 82, "Up");
    assert_eq!(raw_code(0x48, 0), 96, "keypad 8");
    assert_eq!(raw_code(0x45, 0), 83, "Num Lock");
    assert_eq!(
        raw_code(0x2a, E0 as u16),
        0,
        "the fake shift before Print Screen is no key"
    );
    assert_eq!(raw_code(0x37, E0 as u16), 70, "Print Screen");
    let mut pending = false;
    assert_eq!(
        raw_scancode(0x1d, E1 as u16, 0, &mut pending, &no_vk),
        0,
        "the first record of Pause is a prefix"
    );
    assert!(pending);
    assert_eq!(raw_scancode(0x45, 0, 0, &mut pending, &no_vk), 72, "Pause");
    assert!(!pending);
    assert_eq!(message_code(0x43, false), 66, "F9");
    assert_eq!(message_code(0x1d, true), 228, "right Ctrl");
    assert_eq!(
        message_code(0x45, false),
        72,
        "Pause has no extended bit in a window message"
    );
    assert_eq!(message_code(0x45, true), 83, "Num Lock has it");
    // A record without a make code takes the virtual key's (MapVirtualKey, the window's layout).
    let vk = |vk: u16| if vk == 0xB3 { 0xe022 } else { 0 };
    let mut pending = false;
    assert_eq!(
        raw_scancode(0, E0 as u16, 0xB3, &mut pending, &vk),
        scancode_of_set1(0xA2)
    );
    assert_eq!(
        raw_scancode(0, 0, 0x07, &mut pending, &vk),
        0,
        "no make code at all: no key"
    );
    assert_eq!(
        message_scancode(0x13, 0, &|_| 0xe11d),
        72,
        "a message without a scan code: Pause by its virtual key"
    );
}

#[test]
fn every_named_scancode_has_a_set1_make_code_that_maps_back() {
    let mut named = 0;
    for scancode in 1..label::SCANCODE_COUNT as u16 {
        if let Some(index) = set1_of_scancode(scancode) {
            assert_eq!(scancode_of_set1(index), scancode);
            named += 1;
        }
    }
    assert!(named >= 120, "{named}");
    assert_eq!(set1_of_scancode(0), None);
}

#[test]
fn modifier_keys_fold_into_shroudforge_s_groups() {
    assert_eq!(modifier_groups(0), 0);
    assert_eq!(modifier_groups(1), label::CTRL, "LCTRL");
    assert_eq!(modifier_groups(1 << 4), label::CTRL, "RCTRL");
    assert_eq!(modifier_groups(1 << 1), label::SHIFT, "LSHIFT");
    assert_eq!(modifier_groups(1 << 6), label::ALT, "RALT");
    assert_eq!(
        modifier_groups(1 << 3 | 1 << 5),
        label::META | label::SHIFT,
        "LGUI, RSHIFT"
    );
}

// ---- the key state: the fork's host test (host_input.rs a_host_feeds_the_shortcut_state...) ----

fn apply(feed: &mut KeyFeed, input: KilnUiWindowInput) {
    if let Some(record) = Record::copy_of(&input) {
        feed.apply(record, false, &no_vk);
    }
}

#[test]
fn the_key_state_follows_the_fork_s_host_feed() {
    let f9 = key("SDL:66");
    let ctrl_f9 = key("Ctrl+SDL:66");
    let right_ctrl = key("SDL:228");
    let mouse4 = key("Mouse:4");
    let shift_mouse4 = key("Shift+Mouse:4");
    let mut feed = KeyFeed::default();

    apply(&mut feed, raw(0x43, 0));
    assert!(
        !feed.is_down(&f9),
        "nothing is recorded before the feed begins"
    );
    feed.begin();

    // A held key: down once, auto-repeat does not count as a new press.
    let before = feed.press_revision(&f9);
    apply(&mut feed, raw(0x43, 0));
    assert!(feed.is_down(&f9));
    assert!(!feed.is_down(&ctrl_f9));
    let pressed = feed.press_revision(&f9);
    assert!(pressed > before);
    apply(&mut feed, raw(0x43, 0));
    assert_eq!(feed.press_revision(&f9), pressed);
    apply(&mut feed, raw(0x43, UP));
    assert!(!feed.is_down(&f9));
    assert_eq!(feed.press_revision(&f9), pressed, "a release is no press");

    // A chord, through window messages; either Ctrl key counts.
    apply(&mut feed, message(0x1d, true, false));
    assert!(
        feed.is_down(&right_ctrl),
        "a Ctrl key alone is a binding too"
    );
    apply(&mut feed, message(0x43, false, false));
    assert!(feed.is_down(&ctrl_f9));
    assert!(!feed.is_down(&f9), "F9 alone is another binding");
    assert!(feed.press_revision(&ctrl_f9) > pressed);
    assert_eq!(feed.press_revision(&f9), pressed);
    apply(&mut feed, message(0x1d, true, true));
    assert!(!feed.is_down(&ctrl_f9), "the chord ends with its modifier");
    apply(&mut feed, message(0x43, false, true));

    // Mouse buttons take the modifiers held at their press.
    apply(&mut feed, button(4, true, true));
    assert!(feed.is_down(&mouse4));
    apply(&mut feed, button(4, false, true));
    assert!(!feed.is_down(&mouse4));
    apply(&mut feed, raw(0x36, 0));
    apply(&mut feed, button(4, true, true));
    assert!(feed.is_down(&shift_mouse4));
    assert!(!feed.is_down(&mouse4));

    // Focus loss: the releases never arrive.
    apply(&mut feed, focus_lost());
    assert!(!feed.is_down(&shift_mouse4));
    assert_eq!(feed.held(), 0);
    apply(&mut feed, raw(0x43, 0));
    assert!(
        feed.is_down(&f9),
        "the Shift held before the focus loss is forgotten"
    );

    // Print Screen in window messages: Windows sends its release only.
    let print_screen = key("SDL:70");
    let before = feed.press_revision(&print_screen);
    let mut release = message(0x37, true, true);
    release.wparam = 0x2c;
    apply(&mut feed, release);
    let pressed_once = feed.press_revision(&print_screen);
    assert!(
        pressed_once > before,
        "the release alone counts as one press"
    );
    assert!(!feed.is_down(&print_screen));
    // Through raw input both arrive, and the release adds no second press.
    apply(&mut feed, raw(0x37, E0));
    let pressed_twice = feed.press_revision(&print_screen);
    assert!(pressed_twice > pressed_once);
    apply(&mut feed, release);
    assert_eq!(feed.press_revision(&print_screen), pressed_twice);

    let last_press = feed.press_revision(&f9);
    assert!(last_press > 0);
    feed.end();
    assert!(!feed.is_down(&f9));
    assert_eq!(feed.press_revision(&f9), 0);
    apply(&mut feed, raw(0x43, 0));
    assert!(
        !feed.is_down(&f9),
        "nothing is recorded after the feed ended"
    );

    feed.begin();
    assert!(!feed.is_down(&f9), "a new feed starts with nothing held");
    assert_eq!(
        feed.press_revision(&f9),
        0,
        "a press from before the end is not a new press"
    );
    apply(&mut feed, raw(0x43, 0));
    assert!(feed.is_down(&f9));
    assert!(
        feed.press_revision(&f9) > last_press,
        "a new press is newer than any before"
    );
}

#[test]
fn a_press_while_kiln_captures_gets_no_revision_but_is_held() {
    let f9 = key("SDL:66");
    let mut feed = KeyFeed::default();
    feed.begin();
    feed.apply(Record::copy_of(&raw(0x43, 0)).unwrap(), true, &no_vk);
    assert!(feed.is_down(&f9), "the key state still sees it");
    assert_eq!(
        feed.press_revision(&f9),
        0,
        "observe_keyboard.rs: no revision while capture is active"
    );
}

// ---- was_keybind_pressed (create_mod_api.rs:150-178) ----

#[test]
fn was_keybind_pressed_reports_each_press_revision_once() {
    let f9 = key("SDL:66");
    let ctrl_f9 = key("Ctrl+SDL:66");
    let mut t = PressTracker::default();
    assert!(
        !t.was_pressed("open", Some(&f9), 5, true),
        "the first call only remembers"
    );
    assert!(!t.was_pressed("open", Some(&f9), 5, true), "no newer press");
    assert!(
        t.was_pressed("open", Some(&f9), 6, true),
        "a tap between two polls survives"
    );
    assert!(!t.was_pressed("open", Some(&f9), 6, true), "once");
    assert!(
        t.was_pressed("open", Some(&f9), 9, true),
        "several presses between polls: one true"
    );
    assert!(
        !t.was_pressed("open", Some(&f9), 10, false),
        "unfocused or captured: not pressed..."
    );
    assert!(
        !t.was_pressed("open", Some(&f9), 10, true),
        "...and that press is used up"
    );
    assert!(
        !t.was_pressed("open", Some(&ctrl_f9), 11, true),
        "a changed binding starts over"
    );
    assert!(t.was_pressed("open", Some(&ctrl_f9), 12, true));
    assert!(
        !t.was_pressed("open", None, 13, true),
        "disabled or empty: false and forgotten"
    );
    assert!(
        !t.was_pressed("open", Some(&ctrl_f9), 14, true),
        "after a disable the first call only remembers"
    );
    assert!(
        !t.was_pressed("other", Some(&ctrl_f9), 14, true),
        "each setting key has its own memory"
    );
    assert!(t.was_pressed("open", Some(&ctrl_f9), 15, true));
    assert!(!t.was_pressed("other", Some(&ctrl_f9), 14, true));
}

// ---- the feed through kiln.ui ----

#[test]
fn the_feed_needs_kiln_ui_v5_and_its_members() {
    let s = setup();
    let mut table = s.ui.table;
    table.header.version = 4;
    let ui = Arc::new(FakeUi {
        table,
        registration: Mutex::new(Registration::default()),
        in_flight: AtomicU32::new(0),
        capturing: AtomicBool::new(false),
        subscribe_answer: AtomicI32::new(KILN_OK),
        unsubscribe_answer: AtomicI32::new(KILN_OK),
        unsubscribes: AtomicU32::new(0),
    });
    CURRENT.with(|c| *c.borrow_mut() = Some(ui));
    let error = InputFeed::subscribe(&s.adapter, 5, Box::new(s.desktop.clone()), config(8))
        .err()
        .unwrap();
    assert!(matches!(error, FeedError::Service(_)), "{error}");
    CURRENT.with(|c| *c.borrow_mut() = Some(s.ui.clone()));
    s.ui.subscribe_answer
        .store(KILN_ERR_UNAVAILABLE, Ordering::SeqCst);
    let error = InputFeed::subscribe(&s.adapter, 5, Box::new(s.desktop.clone()), config(8))
        .err()
        .unwrap();
    assert_eq!(error, FeedError::Refused(KILN_ERR_UNAVAILABLE));
    s.ui.subscribe_answer.store(KILN_OK, Ordering::SeqCst);
    let mut f = feed(&s, 8);
    assert!(f.is_live());
    assert_eq!(f.stop(&s.adapter), StopOutcome::Unsubscribed);
    assert_eq!(f.stop(&s.adapter), StopOutcome::AlreadyStopped);
}

/// raw+window duplicate records counted once.
#[test]
fn a_key_that_arrives_raw_and_as_a_message_counts_once() {
    let s = setup();
    let mut f = feed(&s, 64);
    let f9 = key("SDL:66");
    // Raw input, then the messages (the window may take WM_INPUT ahead of the messages).
    for input in [
        raw(0x43, 0),
        raw(0x43, UP),
        message(0x43, false, false),
        message(0x43, false, true),
    ] {
        assert!(s.ui.deliver(input));
    }
    f.pump(&s.adapter);
    assert_eq!(f.press_revision(&f9), 1, "a quick tap: one press, not two");
    assert!(!f.keys().key_is_down(66), "and released");
    assert_eq!(f.counters().duplicates, 2);
    // Buttons alike: a raw button record, then its button message.
    let mouse4 = key("Mouse:4");
    for input in [
        button(4, true, true),
        button(4, true, false),
        button(4, false, false),
        button(4, false, true),
    ] {
        s.ui.deliver(input);
    }
    f.pump(&s.adapter);
    assert_eq!(f.press_revision(&mouse4), 2);
    assert!(!f.is_keybind_down(&mouse4));
    assert_eq!(f.counters().duplicates, 4);
    // The raw registration is gone: the messages count again, and release a raw-held key.
    s.ui.deliver(raw(0x43, 0));
    RAW_REGISTERED.with(|r| r.set(false));
    s.ui.deliver(message(0x43, false, true));
    f.pump(&s.adapter);
    assert!(!f.keys().key_is_down(66), "the message released it");
    s.ui.deliver(message(0x43, false, false));
    s.ui.deliver(message(0x43, false, true));
    f.pump(&s.adapter);
    assert_eq!(
        f.press_revision(&f9),
        4,
        "messages alone count while no raw input comes"
    );
    RAW_REGISTERED.with(|r| r.set(true));
    assert_eq!(f.counters().duplicates, 4);
    f.stop(&s.adapter);
}

#[test]
fn without_raw_input_the_window_messages_count() {
    let s = setup();
    let mut f = feed(&s, 64);
    let ctrl_f9 = key("Ctrl+SDL:66");
    s.ui.deliver(message(0x1d, false, false));
    s.ui.deliver(message(0x43, false, false));
    s.ui.deliver(message(0x43, false, false)); // auto-repeat
    f.pump(&s.adapter);
    assert!(f.is_keybind_down(&ctrl_f9));
    assert_eq!(
        f.press_revision(&ctrl_f9),
        2,
        "Ctrl was the first press, the chord the second"
    );
    assert_eq!(f.counters().duplicates, 0);
    f.stop(&s.adapter);
}

/// focus loss clears held keys (KILN_UI_INPUT_FOCUS_LOST).
#[test]
fn a_focus_loss_releases_every_held_key_and_button() {
    let s = setup();
    let mut f = feed(&s, 64);
    let shift_f9 = key("Shift+SDL:66");
    s.ui.deliver(raw(0x2a, 0));
    s.ui.deliver(raw(0x43, 0));
    s.ui.deliver(button(5, true, true));
    f.pump(&s.adapter);
    assert!(f.is_keybind_down(&shift_f9));
    assert_eq!(f.keys().held(), 3);
    let revision = f.press_revision(&shift_f9);
    s.ui.deliver(focus_lost());
    f.pump(&s.adapter);
    assert!(!f.is_keybind_down(&shift_f9));
    assert_eq!(f.keys().held(), 0);
    assert_eq!(f.keys().modifiers(), 0);
    assert_eq!(f.press_revision(&shift_f9), revision, "presses are kept");
    s.ui.deliver(raw(0x43, 0));
    f.pump(&s.adapter);
    assert!(
        f.is_keybind_down(&key("SDL:66")),
        "Shift's release never came, and it no longer counts"
    );
    f.stop(&s.adapter);
}

/// captured/text_input = SF is_capture_active.
#[test]
fn while_kiln_captures_input_nothing_is_pressed_or_held() {
    let s = setup();
    let mut f = feed(&s, 64);
    let f9 = key("SDL:66");
    let mut t = PressTracker::default();
    assert!(!f.was_keybind_pressed(&mut t, "k", Some(&f9)));
    s.ui.capturing.store(true, Ordering::SeqCst);
    s.ui.deliver(raw(0x43, 0));
    f.pump(&s.adapter);
    assert!(f.captured());
    assert!(!f.focused());
    assert!(!f.is_keybind_down(&f9), "held, but kiln.ui has the input");
    assert!(!f.was_keybind_pressed(&mut t, "k", Some(&f9)));
    assert_eq!(
        f.press_revision(&f9),
        0,
        "a press during the capture gets no revision"
    );
    s.ui.capturing.store(false, Ordering::SeqCst);
    f.pump(&s.adapter);
    assert!(f.is_keybind_down(&f9), "still held once the capture ends");
    assert!(
        !f.was_keybind_pressed(&mut t, "k", Some(&f9)),
        "but it was never a press"
    );
    s.ui.deliver(raw(0x43, UP));
    s.ui.deliver(raw(0x43, 0));
    f.pump(&s.adapter);
    assert!(f.was_keybind_pressed(&mut t, "k", Some(&f9)));
    // An unfocused game: the same answers.
    s.desktop.unfocused.store(true, Ordering::SeqCst);
    assert!(!f.is_keybind_down(&f9));
    s.ui.deliver(raw(0x43, UP));
    s.ui.deliver(raw(0x43, 0));
    f.pump(&s.adapter);
    assert!(!f.was_keybind_pressed(&mut t, "k", Some(&f9)));
    s.desktop.unfocused.store(false, Ordering::SeqCst);
    assert!(
        !f.was_keybind_pressed(&mut t, "k", Some(&f9)),
        "a press seen unfocused is used up"
    );
    f.stop(&s.adapter);
}

/// owned copies only on the window thread.
#[test]
fn the_window_thread_copies_the_record_before_kiln_ui_reuses_it() {
    let s = setup();
    let mut f = feed(&s, 64);
    let mut lent = Box::new(raw(0x43, 0));
    s.ui.deliver_with(&lent, || {});
    // kiln.ui reuses its record (and the HRAWINPUT is gone) once the call returned.
    *lent = KilnUiWindowInput {
        make_code: 0x1e,
        flags: UP,
        lparam: -1,
        ..*lent
    };
    drop(lent);
    f.pump(&s.adapter);
    assert!(f.keys().key_is_down(66), "the copy is F9's press");
    assert!(!f.keys().key_is_down(4));
    // The copy holds no lent pointer: two raw records that differ only in their HRAWINPUT are one.
    let mut other = raw(0x43, 0);
    other.lparam = 0x1234;
    assert_eq!(Record::copy_of(&raw(0x43, 0)), Record::copy_of(&other));
    assert_eq!(size_of::<Record>(), 8);
    // A short, null or unknown record is not read.
    let mut short = raw(0x43, UP);
    short.size = 40;
    s.ui.deliver(short);
    s.ui.deliver(record(9, 0));
    unsafe { kiln_sfi::input::feed::on_window_input(std::ptr::null(), std::ptr::null_mut()) };
    f.pump(&s.adapter);
    assert!(f.keys().key_is_down(66), "the short release was not read");
    assert_eq!(f.counters().ignored, 2);
    f.stop(&s.adapter);
}

#[test]
fn a_full_ring_never_leaves_a_key_held() {
    let s = setup();
    let mut f = feed(&s, 2);
    assert_eq!(f.capacity(), 2);
    let (a, b) = (key("SDL:4"), key("SDL:5"));
    // Down A, down B fill the ring; both releases are lost and nothing comes after them.
    for input in [raw(0x1e, 0), raw(0x30, 0), raw(0x1e, UP), raw(0x30, UP)] {
        s.ui.deliver(input);
    }
    assert_eq!(f.counters().full, 2);
    f.pump(&s.adapter);
    assert!(
        !f.is_keybind_down(&a) && !f.is_keybind_down(&b),
        "the worker took the loss itself"
    );
    assert_eq!(f.keys().held(), 0);
    assert_eq!((f.counters().losses, f.counters().marked), (1, 0));
    assert_eq!(f.press_revision(&a), 1, "presses that were applied stay");
    // A loss with records after it: the marker goes in before the newer record, so the worker drops
    // the held keys at the loss and then applies the newer press.
    let mut f = feed(&s, 4);
    let c_key = key("SDL:6");
    for input in [
        raw(0x1e, 0),
        raw(0x30, 0),
        raw(0x2e, 0),
        raw(0x1e, UP),
        raw(0x30, UP),
    ] {
        s.ui.deliver(input); // A, B, C, A's release fill the ring; B's release is lost
    }
    assert_eq!(
        f.pump_at_most(&s.adapter, 3),
        3,
        "A, B and C down; A's release still waits"
    );
    assert!(f.is_keybind_down(&b));
    s.ui.deliver(raw(0x2e, UP)); // C up, after the loss: the marker, then C's release
    s.ui.deliver(raw(0x1e, 0)); // A again
    assert_eq!(f.counters().marked, 1);
    f.pump(&s.adapter);
    assert!(
        !f.is_keybind_down(&b),
        "B's lost release: B no longer counts as held"
    );
    assert!(!f.is_keybind_down(&c_key));
    assert!(f.is_keybind_down(&a), "the press after the loss counts");
    assert_eq!(f.keys().held(), 1);
    let c = f.counters();
    assert_eq!((c.losses, c.marked, c.full), (1, 1, 1));
    assert_eq!(
        c.received,
        c.queued + c.full + c.duplicates + c.ignored + c.after_stop
    );
    f.stop(&s.adapter);
}

/// unsubscribe in flight.
#[test]
fn stop_waits_for_a_record_in_flight_and_nothing_counts_after_it() {
    let s = setup();
    let mut f = feed(&s, 64);
    let f9 = key("SDL:66");
    let ui = s.ui.clone();
    let entered = Arc::new(AtomicBool::new(false));
    let release = Arc::new(AtomicBool::new(false));
    let window = {
        let (entered, release) = (entered.clone(), release.clone());
        std::thread::spawn(move || {
            ui.deliver_with(&raw(0x43, 0), || {
                entered.store(true, Ordering::SeqCst);
                while !release.load(Ordering::SeqCst) {
                    std::thread::yield_now();
                }
            })
        })
    };
    while !entered.load(Ordering::SeqCst) {
        std::thread::yield_now();
    }
    let opener = {
        let release = release.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(50));
            release.store(true, Ordering::SeqCst);
        })
    };
    assert_eq!(f.stop(&s.adapter), StopOutcome::Unsubscribed);
    assert!(
        release.load(Ordering::SeqCst),
        "unsubscribe returned only after the call in flight ended"
    );
    assert!(window.join().unwrap());
    opener.join().unwrap();
    assert_eq!(
        f.counters().after_stop,
        1,
        "the record in flight was dropped, not applied"
    );
    assert!(
        !s.ui.deliver(raw(0x43, 0)),
        "kiln.ui calls nothing after KILN_OK"
    );
    assert_eq!(f.pump(&s.adapter), 0);
    assert!(!f.is_keybind_down(&f9));
    assert_eq!(f.press_revision(&f9), 0, "an ended feed answers revision 0");
}

#[test]
fn a_removal_kiln_ui_did_not_confirm_keeps_the_ring_alive_for_late_records() {
    let s = setup();
    let mut f = feed(&s, 8);
    s.ui.unsubscribe_answer
        .store(KILN_ERR_WRONG_THREAD, Ordering::SeqCst);
    assert_eq!(f.stop(&s.adapter), StopOutcome::Kept(KILN_ERR_WRONG_THREAD));
    assert!(s.ui.deliver(raw(0x43, 0)), "still subscribed");
    assert_eq!(f.counters().after_stop, 1);
    drop(f);
    // The registration's reference keeps the ring: a late record is dropped, nothing is freed under it.
    for _ in 0..100 {
        assert!(s.ui.deliver(raw(0x43, 0)));
    }
}

#[test]
fn a_feed_dropped_without_stop_drops_later_records() {
    let s = setup();
    let f = feed(&s, 8);
    drop(f);
    assert_eq!(
        s.ui.unsubscribes.load(Ordering::SeqCst),
        0,
        "no adapter at drop: no unsubscribe"
    );
    for _ in 0..10 {
        assert!(s.ui.deliver(raw(0x43, 0)));
    }
}

// ---- the handoff under load ----

fn rounds() -> usize {
    std::env::var("SFI_INPUT_STRESS_ROUNDS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(40)
}

/// A window thread floods presses and releases of 12 keys (and focus losses) while the worker pumps,
/// sometimes slower than the window delivers; at the end every key is released. Whatever the ring
/// dropped: no key stays held, the counts add up, and a run without a loss equals the same records
/// applied in order.
#[test]
fn the_window_thread_never_waits_and_no_key_stays_held_under_load() {
    let s = setup();
    let (mut lossy, mut marked, mut taken, mut whole) = (0u64, 0u64, 0u64, 0u64);
    for round in 0..rounds() {
        let capacity = [4usize, 16, 64, 1024][round % 4];
        // Odd rounds: the window pauses now and then and the worker never sleeps (mostly no loss);
        // even rounds: a flood against a worker that sleeps.
        let paced = round % 2 == 1;
        let mut f = feed(&s, capacity);
        let ui = s.ui.clone();
        let done = Arc::new(AtomicBool::new(false));
        let window = {
            let done = done.clone();
            std::thread::spawn(move || {
                let mut sent = Vec::new();
                let mut seed = 0x9e37_79b9_u32.wrapping_mul(round as u32 + 1);
                let mut held = [false; 12];
                let mut slowest = Duration::ZERO;
                for i in 0..20_000 {
                    if paced && i % 16 == 15 {
                        std::thread::sleep(Duration::from_micros(50));
                    }
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    let k = (seed % 12) as usize;
                    let input = if seed.is_multiple_of(97) {
                        held = [false; 12];
                        focus_lost()
                    } else {
                        held[k] = !held[k];
                        raw(0x10 + k as u32, if held[k] { 0 } else { UP })
                    };
                    let started = std::time::Instant::now();
                    ui.deliver(input);
                    slowest = slowest.max(started.elapsed());
                    sent.push(input);
                }
                for (k, h) in held.iter().enumerate() {
                    if *h {
                        ui.deliver(raw(0x10 + k as u32, UP));
                        sent.push(raw(0x10 + k as u32, UP));
                    }
                }
                done.store(true, Ordering::SeqCst);
                (sent, slowest)
            })
        };
        let mut pumps = 0u32;
        while !done.load(Ordering::SeqCst) {
            f.pump(&s.adapter);
            pumps += 1;
            if !paced && pumps.is_multiple_of(3) {
                std::thread::sleep(Duration::from_micros(200));
            }
        }
        let (sent, slowest) = window.join().unwrap();
        while f.pump(&s.adapter) > 0 {}
        let c = f.counters();
        assert_eq!(c.received, sent.len() as u64);
        assert_eq!(
            c.received,
            c.queued + c.full,
            "round {round}: every record queued or refused"
        );
        assert_eq!(
            f.keys().held(),
            0,
            "round {round}: a key stayed held ({c:?})"
        );
        if c.full == 0 {
            let mut reference = KeyFeed::default();
            reference.begin();
            for input in &sent {
                reference.apply(Record::copy_of(input).unwrap(), false, &no_vk);
            }
            for k in 0..12u16 {
                let binding = Binding::parse(&format!("SDL:{}", scancode_of_set1(0x10 + k as u8)))
                    .unwrap()
                    .unwrap();
                assert_eq!(
                    f.press_revision(&binding),
                    reference.press_revision(&binding),
                    "round {round}"
                );
            }
            whole += 1;
        } else {
            assert!(c.losses >= 1, "round {round}: a loss was applied ({c:?})");
            lossy += 1;
            marked += c.marked;
            taken += c.losses - c.marked;
        }
        assert!(
            slowest < Duration::from_secs(2),
            "round {round}: a delivery took {slowest:?}"
        );
        assert_eq!(f.stop(&s.adapter), StopOutcome::Unsubscribed);
    }
    eprintln!(
        "load: {whole} rounds without a loss, {lossy} with ({marked} losses marked, {taken} taken by the worker)"
    );
    assert!(lossy > 0, "the small rings overflowed at least once");
}

/// Without a loss the feed is exactly its records applied in order (after the raw/message rule): random
/// raw keys, key messages, buttons both ways and focus losses, delivered in bursts shorter than the ring
/// and pumped between bursts.
#[test]
fn without_a_loss_the_feed_equals_its_records_applied_in_order() {
    let s = setup();
    let mut f = feed(&s, 64);
    let mut reference = KeyFeed::default();
    reference.begin();
    let (mut raw_keys, mut raw_buttons) = (false, false);
    let mut seed = 0x2545_f491_u32;
    for burst in 0..2_000u32 {
        for _ in 0..(burst % 60 + 1) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            let up = seed & 64 != 0;
            let input = match seed % 41 {
                0 => focus_lost(),
                1..=4 => button(1 + (seed >> 8) % 5, !up, seed & 128 != 0),
                5..=9 => message(0x10 + (seed >> 8) % 12, false, up),
                _ => raw(0x10 + (seed >> 8) % 12, if up { UP } else { 0 }),
            };
            s.ui.deliver(input);
            // The same rule by hand: a message is a duplicate once raw records came.
            let counts = match input.kind as i32 {
                KILN_UI_INPUT_RAW_KEY => {
                    raw_keys = true;
                    true
                }
                KILN_UI_INPUT_WINDOW_KEY => !raw_keys,
                KILN_UI_INPUT_MOUSE_BUTTON if input.message == WM_INPUT => {
                    raw_buttons = true;
                    true
                }
                KILN_UI_INPUT_MOUSE_BUTTON => !raw_buttons,
                _ => true,
            };
            if counts {
                reference.apply(Record::copy_of(&input).unwrap(), false, &no_vk);
            }
        }
        f.pump(&s.adapter);
        if burst.is_multiple_of(100) {
            // Raw input goes away now and then: messages count again.
            raw_keys = false;
            raw_buttons = false;
            RAW_REGISTERED.with(|r| r.set(false));
            s.ui.deliver(message(0x10, false, true));
            s.ui.deliver(button(1, false, false));
            RAW_REGISTERED.with(|r| r.set(true));
            reference.apply(
                Record::copy_of(&message(0x10, false, true)).unwrap(),
                false,
                &no_vk,
            );
            reference.apply(
                Record::copy_of(&button(1, false, false)).unwrap(),
                false,
                &no_vk,
            );
            f.pump(&s.adapter);
        }
        assert_eq!(f.keys().held(), reference.held(), "burst {burst}");
    }
    let c = f.counters();
    assert_eq!(c.full, 0);
    assert_eq!(c.received, c.queued + c.duplicates);
    assert!(c.duplicates > 100, "{c:?}");
    let mut compared = 0;
    for k in 0..12u8 {
        let binding = key(&format!("SDL:{}", scancode_of_set1(0x10 + k)));
        assert_eq!(
            f.press_revision(&binding),
            reference.press_revision(&binding),
            "key {k}"
        );
        assert_eq!(
            f.keys().is_down(&binding),
            reference.is_down(&binding),
            "key {k}"
        );
        compared += 1;
    }
    for b in 1..=5 {
        let binding = key(&format!("Mouse:{b}"));
        assert_eq!(
            f.press_revision(&binding),
            reference.press_revision(&binding),
            "button {b}"
        );
        assert_eq!(
            f.keys().is_down(&binding),
            reference.is_down(&binding),
            "button {b}"
        );
        compared += 1;
    }
    assert_eq!(compared, 17);
    f.stop(&s.adapter);
}

/// Unsubscribe while the window thread delivers without a pause: stop returns, nothing is applied after
/// it, and the window thread never touches freed memory (the registration's reference is released only
/// after KILN_OK).
#[test]
fn stop_during_a_flood_of_records_is_clean() {
    let s = setup();
    for round in 0..rounds() {
        let mut f = feed(&s, 256);
        let ui = s.ui.clone();
        let window = std::thread::spawn(move || {
            let mut delivered = 0u32;
            let mut i = 0u32;
            while ui.deliver(raw(0x10 + i % 12, if i.is_multiple_of(2) { 0 } else { UP })) {
                delivered += 1;
                i += 1;
            }
            delivered
        });
        for _ in 0..(round % 5) {
            f.pump(&s.adapter);
            std::thread::yield_now();
        }
        assert_eq!(f.stop(&s.adapter), StopOutcome::Unsubscribed);
        let delivered = window.join().unwrap();
        let c = f.counters();
        assert_eq!(c.received, u64::from(delivered));
        assert_eq!(f.pump(&s.adapter), 0);
        assert_eq!(f.keys().held(), 0);
    }
}

// ---- Windows: the system's key names, layout and raw-input probe (native proof on Windows) ----

#[cfg(windows)]
#[test]
fn windows_names_the_keys_in_the_thread_s_layout_and_restores_it() {
    use kiln_sfi::input::SystemDesktop;
    let desktop = SystemDesktop;
    let layout = desktop.layout(0);
    assert_ne!(layout, 0);
    let names = desktop.key_names(layout);
    assert_eq!(names.len(), label::SCANCODE_COUNT);
    assert_eq!(names[66], "F9", "the fork's own host test");
    for (code, name) in names.iter().enumerate().take(30).skip(4) {
        assert!(!name.is_empty(), "letter {code}");
    }
    assert_ne!(names[72], names[83], "Pause and Num Lock are told apart");
    assert_eq!(desktop.layout(0), layout, "the thread's layout is back");
    // Another layout loaded in this session (a runner may have only one): the names are built in it, and
    // the thread's own layout is back afterwards.
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetKeyboardLayoutList(count: i32, list: *mut usize) -> i32;
    }
    let mut loaded = [0usize; 16];
    // SAFETY: a stack array of the length passed.
    let count = unsafe { GetKeyboardLayoutList(loaded.len() as i32, loaded.as_mut_ptr()) };
    let loaded = &loaded[..usize::try_from(count).unwrap_or(0).min(16)];
    if let Some(&other) = loaded.iter().find(|&&other| other != layout) {
        let other_names = desktop.key_names(other);
        assert_eq!(other_names.len(), label::SCANCODE_COUNT);
        assert_eq!(
            desktop.layout(0),
            layout,
            "the thread's own layout is back after a build in {other:#x}"
        );
        eprintln!("built in {other:#x} as well: A={:?}", other_names[4]);
    } else {
        eprintln!(
            "{} layout(s) loaded: the switch to another layout is not exercised here",
            loaded.len()
        );
    }
    let mut table = LabelTable::default();
    table.replace(names);
    assert_eq!(
        key("Ctrl+SDL:66|Mouse:5").label(&table),
        "Ctrl+F9 / Mouse X2"
    );
    eprintln!(
        "layout {layout:#x}: F9={:?} A={:?} Pause={:?} NumLock={:?}; VK_UP -> {:#x}, VK_MEDIA_PLAY_PAUSE -> {:#x}",
        table.key_name(66),
        table.key_name(4),
        table.key_name(72),
        table.key_name(83),
        desktop.scan_of_virtual_key(0x26, 0),
        desktop.scan_of_virtual_key(0xB3, 0)
    );
    assert_eq!(desktop.scan_of_virtual_key(0x78, 0), 0x43, "VK_F9");
    assert_eq!(
        desktop.scan_of_virtual_key(0xA3, 0),
        0xe01d,
        "VK_RCONTROL is extended"
    );
    assert!(
        !kiln_sfi::input::feed::raw_input_registered(6),
        "this process registered no raw keyboard input"
    );
}

// ---- the keybind members of shroudforge.input on this feed (runtime/input.rs) ----

/// The keybind members over the real feed: kiln.ui's records reach a package's keybind setting at the turn start; an
/// auto-repeat is no new press; kiln.ui's capture and a focus loss answer "not held"; the label uses the desktop's key
/// names; the runtime's stop unsubscribes from kiln.ui.
#[test]
fn the_keybind_members_read_the_feed_at_each_turn() {
    use kiln_sfi::runtime::input::{self, InputState};
    let s = setup();
    let mut names = vec![String::new(); label::SCANCODE_COUNT];
    names[66] = "F9".into();
    s.desktop.names.lock().unwrap().insert(0, names);
    let Setup {
        ui,
        adapter,
        _api,
        desktop,
    } = s;
    let adapter = Rc::new(adapter);
    let open = input::open_feed(
        Rc::new(move || Box::new(desktop.clone()) as Box<dyn Desktop>),
        config(64),
    );
    let state = InputState::new();
    state.set_side(sfi_core::host::Side::Client);
    state.opened(open(&adapter, support::next_owner()));
    assert!(state.is_live());
    let manifest = sfi_core::package::Package::parse(
        br#"{"id":"keys-mod","name":"Keys","version":"1.0.0","capabilities":["runtime"]}"#,
        Some(br#"{"schemaVersion":1,"enabled":true,"settings":{"toggleKey":{"value":"SDL:66","control":"keybind"}}}"#),
        sfi_core::package::ParseOptions::default(),
    )
    .unwrap()
    .manifest;
    let lua = mlua::Lua::new();
    let api = lua.create_table().unwrap();
    let settings = sfi_core::settings::install(
        &lua,
        &api,
        "keys-mod",
        &manifest.setting_values,
        &manifest.settings,
    )
    .unwrap();
    input::install_members(&lua, &api, &settings, &state).unwrap();
    lua.globals().raw_set("sf", api).unwrap();
    let q = |member: &str| {
        lua.load(format!("return tostring(sf.input.{member}('toggleKey'))"))
            .eval::<String>()
            .unwrap()
    };
    assert_eq!(q("keybind_label"), "F9");
    assert_eq!(
        q("was_keybind_pressed"),
        "false",
        "the first call only remembers"
    );
    ui.deliver(raw(0x43, 0));
    assert_eq!(
        q("is_keybind_down"),
        "false",
        "not before the turn start takes it in"
    );
    state.turn_start();
    assert_eq!(q("is_keybind_down"), "true");
    assert_eq!(q("was_keybind_pressed"), "true");
    ui.deliver(raw(0x43, 0));
    state.turn_start();
    assert_eq!(
        q("was_keybind_pressed"),
        "false",
        "an auto-repeat is no new press"
    );
    ui.capturing.store(true, Ordering::SeqCst);
    state.turn_start();
    assert_eq!(q("is_keybind_down"), "false", "kiln.ui has the input");
    ui.capturing.store(false, Ordering::SeqCst);
    ui.deliver(focus_lost());
    state.turn_start();
    assert_eq!(q("is_keybind_down"), "false", "a focus loss releases it");
    ui.deliver(raw(0x43, 0));
    state.turn_start();
    assert_eq!(
        q("was_keybind_pressed"),
        "true",
        "pressed anew after the focus loss"
    );
    state.stop();
    assert_eq!(ui.unsubscribes.load(Ordering::SeqCst), 1);
    assert_eq!(
        state.take_lines(),
        vec![(KILN_LOG_INFO, input::STARTED.to_owned())]
    );
    assert_eq!(q("is_keybind_down"), "false");
}
