// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), the Kiln layer of the fork (1.7.9 d8a394bc):
// Source: modules/hotkeys/observe_keyboard.rs:528-650 (record, is_down, press_revision) and :712-835 (host_begin,
//         host_key, host_mouse_button, host_reset, host_end, host_forget_presses, host_clear, host_modifier_groups);
//         modules/hotkeys/host_input.rs:58-140 (the host entry points); modules/hotkeys/host_keyboard.c:24-69 (the
//         raw-record and window-message scancode rules); modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:136-180
//         (is_keybind_down, was_keybind_pressed) and :313-333 (is_game_focused).
// The raw/window de-duplication follows kiln-shroudforge's key_feed.cpp and kiln.ui's raw_input_source.h (Kiln, MIT).
//! The input feed, the key state behind `shroudforge.input` (the key and keybind functions build on it).
//!
//! Kiln's interpreter has no ShroudForge runtime and no `--keyboard-input` helper: it keeps ShroudForge's
//! shortcut state itself, fed by the game window's own records from kiln.ui (`subscribe_window_input`,
//! kiln_ui.h v5), as the fork's host feed is fed by kiln-shroudforge.
//!
//! **Window thread (the producer).** kiln.ui calls [`on_window_input`] inside the game's window procedure.
//! It copies the lent record into an owned [`Record`] at once (the record and a WM_INPUT's HRAWINPUT are
//! valid for the call only; no pointer is kept), drops a legacy duplicate of a raw record, and pushes the
//! copy into a bounded lock-free ring that was allocated at the subscription. Nothing there allocates,
//! takes a lock, waits for the worker, translates a key or reads a label: a push is one CAS and one
//! release store. A full ring refuses the record at once and the feed remembers that records were lost
//! (under a number of their own); the next record that fits is preceded by a [`Record::Lost`] marker, and a
//! worker that finds the ring empty with that loss still unmarked takes it itself. Either way the worker
//! drops every held key at the point of the loss, as at a focus loss: a lost release can never leave a key
//! held. A press lost with the ring full is not seen; a key still held at the loss counts as released, so
//! its next auto-repeat counts as a new press (as after a focus loss in the fork's host feed).
//!
//! **Duplicates.** The game registers raw input without RIDEV_NOLEGACY, so a key usually arrives twice: a
//! raw record and a window message, not always press by press. As kiln-shroudforge does for the fork's
//! feed: once the window has given a raw keyboard record, a window key message is dropped while raw
//! keyboard input stays registered for the focused window; buttons likewise with raw mouse records.
//! Without raw input the window messages count. A key is counted once either way.
//!
//! **Worker (the consumer).** The interpreter worker drains the ring in [`InputFeed::pump`] (once per turn,
//! before the mods' update) and applies each record to [`KeyFeed`], the port of the fork's host feed
//! state: SDL scancodes by ShroudForge's (SDL's Windows) rules, modifier groups, held keys and buttons,
//! and the press revisions `was_keybind_pressed` compares ([`PressTracker`]). A focus loss
//! (`KILN_UI_INPUT_FOCUS_LOST`) drops every held key, as ShroudForge's `host_reset`.
//!
//! **Capture.** ShroudForge answers "not pressed, not held" while a shortcut is being recorded in its
//! Modloader (`is_capture_active`), and records no press revision then. Kiln's counterpart is kiln.ui
//! capturing the input (the Kiln menu, a Kiln screen or text field): [`InputFeed::pump`] samples
//! `KilnUiStatus.capturing` once per drain, applies it to the records of that drain, and the queries use
//! it until the next drain.
//!
//! **Unsubscribe.** [`InputFeed::stop`] shuts the producer first (a record in flight is dropped), then
//! calls kiln.ui's `unsubscribe`, which returns `KILN_OK` only once a call in flight has ended: only then is
//! the registration's reference to the ring given back. Any other answer (or an `InputFeed` dropped
//! without `stop`) keeps that reference for the process, so a late record still finds valid memory and
//! is dropped.

use crate::input::label::{Binding, LabelTable, SCANCODE_COUNT, Trigger};
use crate::runtime::services::{Member, ServiceAdapter, ServiceError};
use crate::service_member;
use kiln_abi::services::kiln_ui::*;
use kiln_abi::*;
use std::cell::UnsafeCell;
use std::collections::{BTreeMap, HashMap};
use std::ffi::c_void;
use std::mem::MaybeUninit;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};

// ---- SDL scancodes of Windows set-1 make codes ----

/// `RAWKEYBOARD.Flags`.
const RI_KEY_BREAK: u16 = 1;
const RI_KEY_E0: u16 = 2;
const RI_KEY_E1: u16 = 4;
/// The extended-key bit of a key message's lParam high word.
const KF_EXTENDED: u32 = 0x0100;
const WM_INPUT: u32 = 0x00FF;
pub const SCANCODE_PAUSE: u16 = 72;
pub const SCANCODE_PRINTSCREEN: u16 = 70;

/// (index, SDL scancode): the index is a set-1 make code, with 0x80 for an E0-prefixed one. SDL's
/// scancodes below 256 are the USB HID Keyboard/Keypad usages (page 0x07); the pairs are those of
/// Microsoft's USB HID to PS/2 scan code translation table for that page. Consumer-page keys (media and
/// browser keys) have SDL-only numbers and are not mapped: such a key never matches a binding here
/// (a parity difference; ShroudForge's runtime maps them through SDL).
const SET1: &[(u8, u16)] = &[
    (0x01, 41),
    (0x02, 30),
    (0x03, 31),
    (0x04, 32),
    (0x05, 33),
    (0x06, 34),
    (0x07, 35),
    (0x08, 36),
    (0x09, 37),
    (0x0A, 38),
    (0x0B, 39),
    (0x0C, 45),
    (0x0D, 46),
    (0x0E, 42),
    (0x0F, 43),
    (0x10, 20),
    (0x11, 26),
    (0x12, 8),
    (0x13, 21),
    (0x14, 23),
    (0x15, 28),
    (0x16, 24),
    (0x17, 12),
    (0x18, 18),
    (0x19, 19),
    (0x1A, 47),
    (0x1B, 48),
    (0x1C, 40),
    (0x1D, 224),
    (0x1E, 4),
    (0x1F, 22),
    (0x20, 7),
    (0x21, 9),
    (0x22, 10),
    (0x23, 11),
    (0x24, 13),
    (0x25, 14),
    (0x26, 15),
    (0x27, 51),
    (0x28, 52),
    (0x29, 53),
    (0x2A, 225),
    (0x2B, 49),
    (0x2C, 29),
    (0x2D, 27),
    (0x2E, 6),
    (0x2F, 25),
    (0x30, 5),
    (0x31, 17),
    (0x32, 16),
    (0x33, 54),
    (0x34, 55),
    (0x35, 56),
    (0x36, 229),
    (0x37, 85),
    (0x38, 226),
    (0x39, 44),
    (0x3A, 57),
    (0x3B, 58),
    (0x3C, 59),
    (0x3D, 60),
    (0x3E, 61),
    (0x3F, 62),
    (0x40, 63),
    (0x41, 64),
    (0x42, 65),
    (0x43, 66),
    (0x44, 67),
    (0x45, 83),
    (0x46, 71),
    (0x47, 95),
    (0x48, 96),
    (0x49, 97),
    (0x4A, 86),
    (0x4B, 92),
    (0x4C, 93),
    (0x4D, 94),
    (0x4E, 87),
    (0x4F, 89),
    (0x50, 90),
    (0x51, 91),
    (0x52, 98),
    (0x53, 99),
    (0x56, 100),
    (0x57, 68),
    (0x58, 69),
    (0x59, 103),
    (0x64, 104),
    (0x65, 105),
    (0x66, 106),
    (0x67, 107),
    (0x68, 108),
    (0x69, 109),
    (0x6A, 110),
    (0x6B, 111),
    (0x6C, 112),
    (0x6D, 113),
    (0x6E, 114),
    (0x70, 136),
    (0x73, 135),
    (0x76, 115),
    (0x79, 138),
    (0x7B, 139),
    (0x7D, 137),
    (0x7E, 133),
    (0x5C, 140),
    (0x71, 145),
    (0x72, 144),
    (0x77, 147),
    (0x78, 146),
    // E0-prefixed
    (0x87, 121),
    (0x88, 122),
    (0x8A, 125),
    (0x97, 123),
    (0x98, 124),
    (0xB6, 229),
    (0xBB, 117),
    (0x9C, 88),
    (0x9D, 228),
    (0xA0, 127),
    (0xAE, 129),
    (0xB0, 128),
    (0xB5, 84),
    (0xB7, 70),
    (0xB8, 230),
    (0xC5, 83),
    (0xC6, 72),
    (0xC7, 74),
    (0xC8, 82),
    (0xC9, 75),
    (0xCB, 80),
    (0xCD, 79),
    (0xCF, 77),
    (0xD0, 81),
    (0xD1, 78),
    (0xD2, 73),
    (0xD3, 76),
    (0xDB, 227),
    (0xDC, 231),
    (0xDD, 101),
    (0xDE, 102),
];

const fn set1_table() -> [u16; 256] {
    let mut table = [0u16; 256];
    let mut i = 0;
    while i < SET1.len() {
        table[SET1[i].0 as usize] = SET1[i].1;
        i += 1;
    }
    table
}
static SET1_TABLE: [u16; 256] = set1_table();

/// The SDL scancode of a set-1 index (make code, 0x80 for E0), 0 for none.
pub fn scancode_of_set1(index: u8) -> u16 {
    SET1_TABLE[usize::from(index)]
}

/// The first set-1 index (from 1) whose SDL scancode is `scancode` (host_keyboard.c's label lookup).
pub fn set1_of_scancode(scancode: u16) -> Option<u8> {
    (1..=255u8).find(|&index| scancode != 0 && SET1_TABLE[usize::from(index)] == scancode)
}

/// One keyboard record of WM_INPUT (`RAWKEYBOARD.MakeCode`, `.Flags`, `.VKey`) as an SDL scancode, 0 for
/// a prefix record and for a record that names no key. `pending_e1` carries Pause's E1 prefix from one
/// record to the next. `scan_of_vk` is `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX)` in the window's layout
/// (some keys, media keys, arrive without a make code). host_keyboard.c:24-49.
pub fn raw_scancode(
    make_code: u16,
    flags: u16,
    vkey: u16,
    pending_e1: &mut bool,
    scan_of_vk: &dyn Fn(u16) -> u16,
) -> u16 {
    if flags & RI_KEY_E1 != 0 {
        *pending_e1 = true;
        return 0;
    }
    if flags & RI_KEY_E0 != 0 && make_code == 0x2a {
        return 0; // the fake shift before Print Screen
    }
    let make_code = if make_code == 0 {
        scan_of_vk(vkey)
    } else {
        make_code
    };
    if make_code == 0 {
        return 0;
    }
    if *pending_e1 {
        *pending_e1 = false;
        return if make_code == 0x45 { SCANCODE_PAUSE } else { 0 };
    }
    // As in SDL, only the record's own E0 flag marks an extended key.
    let mut index = (make_code & 0xff) as u8;
    if flags & RI_KEY_E0 != 0 {
        index |= 0x80;
    }
    scancode_of_set1(index)
}

/// `wParam`/`lParam` of WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN or WM_SYSKEYUP as an SDL scancode, 0 for none.
/// host_keyboard.c:52-69.
pub fn message_scancode(wparam: u16, lparam: u32, scan_of_vk: &dyn Fn(u16) -> u16) -> u16 {
    let flags = lparam >> 16;
    let mut scan_code = (flags & 0xff) & !0x80;
    if scan_code != 0 {
        if flags & KF_EXTENDED != 0 {
            scan_code |= 0xe000;
        } else if scan_code == 0x45 {
            scan_code = 0xe046; // Pause
        }
    } else {
        scan_code = u32::from(scan_of_vk(wparam));
        if scan_code == 0xe11d {
            scan_code = 0xe046;
        }
    }
    let high = (scan_code >> 8) & 0xff != 0;
    scancode_of_set1((scan_code & 0xff) as u8 | if high { 0x80 } else { 0 })
}

// ---- the window thread's owned copy ----

/// One window record, copied on the window thread inside the callback. Plain data: no pointer, no
/// HRAWINPUT, nothing borrowed from kiln.ui.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Record {
    /// `RAWKEYBOARD.MakeCode`, `.Flags`, `.VKey` (16 bits wide in RAWKEYBOARD).
    RawKey {
        make_code: u16,
        flags: u16,
        vkey: u16,
    },
    /// A key message: `LOWORD(wParam)` (the virtual key) and the low 32 bits of `lParam`.
    WindowKey {
        wparam: u16,
        lparam: u32,
    },
    /// SDL button 1..5 (kiln.ui's numbering).
    MouseButton {
        button: u8,
        down: bool,
    },
    FocusLost,
    /// Records were lost here (the ring was full).
    Lost,
}

impl Record {
    /// The owned copy of a lent kiln.ui record, or `None` for a kind the feed does not use.
    pub fn copy_of(input: &KilnUiWindowInput) -> Option<Self> {
        Some(match input.kind as i32 {
            KILN_UI_INPUT_RAW_KEY => Self::RawKey {
                make_code: input.make_code as u16,
                flags: input.flags as u16,
                vkey: input.vkey as u16,
            },
            KILN_UI_INPUT_WINDOW_KEY => Self::WindowKey {
                wparam: input.wparam as u16,
                lparam: input.lparam as u32,
            },
            KILN_UI_INPUT_MOUSE_BUTTON => Self::MouseButton {
                button: u8::try_from(input.button).unwrap_or(0),
                down: input.down != 0,
            },
            KILN_UI_INPUT_FOCUS_LOST => Self::FocusLost,
            _ => return None,
        })
    }
}

/// Whether raw input of usage page 1, `usage` (6 keyboard, 2 mouse) is registered for the focused
/// window (the window thread's question; it runs only there).
pub type RawProbe = fn(usage: u16) -> bool;

#[derive(Clone, Copy)]
pub struct FeedConfig {
    /// Records the ring holds (rounded up to a power of two). At human input rates 1,024 records are a
    /// long worker stall.
    pub capacity: usize,
    pub probe: RawProbe,
}
impl Default for FeedConfig {
    fn default() -> Self {
        Self {
            capacity: 1024,
            probe: raw_input_registered,
        }
    }
}

/// What the feed counted (status and tests). Producer counts are the window thread's.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FeedCounters {
    /// Records kiln.ui delivered.
    pub received: u64,
    /// Records pushed into the ring.
    pub queued: u64,
    /// Window messages and button messages dropped as legacy duplicates of raw records.
    pub duplicates: u64,
    /// Records of a size or kind the feed does not read.
    pub ignored: u64,
    /// Records refused because the ring was full.
    pub full: u64,
    /// Records that arrived once the feed was stopping.
    pub after_stop: u64,
    /// Losses the worker applied (a marker, or one it took itself).
    pub losses: u64,
    /// `Lost` markers the window thread pushed (the rest of `losses` the worker took itself).
    pub marked: u64,
    /// Records the worker applied (markers included).
    pub applied: u64,
}

const RECEIVED: usize = 0;
const QUEUED: usize = 1;
const DUPLICATES: usize = 2;
const IGNORED: usize = 3;
const FULL: usize = 4;
const AFTER_STOP: usize = 5;
const LOSSES: usize = 6;
const APPLIED: usize = 7;
const MARKED: usize = 8;

/// What the window thread and the worker share. The registration's `user` is one reference to it.
pub struct FeedShared {
    ring: Ring,
    /// Records were dropped and no `Lost` marker went into the ring yet: 0, or that loss's number.
    /// Each new loss gets a new number, so a worker that saw the ring empty under one loss can never
    /// take a later one with it (see [`Self::drain`]).
    lost: AtomicU64,
    /// The last loss number given out (window thread).
    loss_numbers: AtomicU64,
    stopping: AtomicBool,
    /// The game window's thread (`GetCurrentThreadId` of the first record), 0 before one came.
    window_thread: AtomicU32,
    /// Window thread only: the window has given a raw keyboard / raw mouse button record.
    raw_keys: AtomicBool,
    raw_buttons: AtomicBool,
    probe: RawProbe,
    counts: [AtomicU64; 9],
}

impl FeedShared {
    fn new(config: FeedConfig) -> Self {
        Self {
            ring: Ring::new(config.capacity),
            lost: AtomicU64::new(0),
            loss_numbers: AtomicU64::new(0),
            stopping: AtomicBool::new(false),
            window_thread: AtomicU32::new(0),
            raw_keys: AtomicBool::new(false),
            raw_buttons: AtomicBool::new(false),
            probe: config.probe,
            counts: Default::default(),
        }
    }

    fn count(&self, index: usize) {
        self.counts[index].fetch_add(1, Ordering::Relaxed);
    }

    /// The window thread's whole turn: check, copy, de-duplicate, push.
    fn window_input(&self, input: *const KilnUiWindowInput) {
        self.count(RECEIVED);
        if self.stopping.load(Ordering::Acquire) {
            self.count(AFTER_STOP);
            return;
        }
        if input.is_null() || !input.is_aligned() {
            self.count(IGNORED);
            return;
        }
        // SAFETY: kiln.ui lends a readable record for the call; its first field is its size.
        let size = unsafe { input.cast::<u32>().read() } as usize;
        if size < core::mem::size_of::<KilnUiWindowInput>() {
            self.count(IGNORED);
            return;
        }
        // SAFETY: the size covers the whole record. This is the owned copy; nothing lent is kept.
        let lent = unsafe { input.read() };
        let Some(record) = Record::copy_of(&lent) else {
            self.count(IGNORED);
            return;
        };
        if self.window_thread.load(Ordering::Relaxed) == 0 {
            self.window_thread
                .store(current_thread(), Ordering::Relaxed);
        }
        // key_feed.cpp: the raw record once the window gives raw input, the message while it gives none.
        match record {
            Record::RawKey { .. } => self.raw_keys.store(true, Ordering::Relaxed),
            Record::WindowKey { .. } => {
                if self.raw_keys.load(Ordering::Relaxed) && (self.probe)(6) {
                    self.count(DUPLICATES);
                    return;
                }
                self.raw_keys.store(false, Ordering::Relaxed);
            }
            Record::MouseButton { .. } => {
                if lent.message == WM_INPUT {
                    self.raw_buttons.store(true, Ordering::Relaxed);
                } else {
                    if self.raw_buttons.load(Ordering::Relaxed) && (self.probe)(2) {
                        self.count(DUPLICATES);
                        return;
                    }
                    self.raw_buttons.store(false, Ordering::Relaxed);
                }
            }
            Record::FocusLost | Record::Lost => {}
        }
        self.produce(record);
    }

    /// Push with the loss rule: an unmarked loss is marked before the next record that fits.
    fn produce(&self, record: Record) {
        let lost = self.lost.load(Ordering::Acquire);
        if lost != 0
            && self
                .lost
                .compare_exchange(lost, 0, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
        {
            if self.ring.push(Record::Lost).is_err() {
                self.begin_loss();
                self.count(FULL);
                return;
            }
            self.count(MARKED);
        }
        if self.ring.push(record).is_err() {
            self.begin_loss();
            self.count(FULL);
        } else {
            self.count(QUEUED);
        }
    }

    /// A record was refused: an unmarked loss under a new number (published after every record pushed
    /// before it, by the release). One already unmarked keeps its number.
    fn begin_loss(&self) {
        let number = self.loss_numbers.fetch_add(1, Ordering::Relaxed) + 1;
        let _ = self
            .lost
            .compare_exchange(0, number, Ordering::Release, Ordering::Relaxed);
    }

    /// The single consumer's drain (the worker, through its one [`InputFeed`]). Returns the records it
    /// applied. A loss with no marker in the ring is taken here once the ring is empty. The loss is read
    /// before the ring: the window thread (the one producer) published every record it pushed before the
    /// loss ahead of the loss's number, so a ring then found empty holds none of them. It is taken only
    /// under the number read: if the window thread delivered in between (the marker, newer records, a
    /// newer loss under a new number), the exchange fails and the marker or the newer loss comes later, in
    /// its place in the ring. Taken under a mere "lost" flag, a newer loss could be applied ahead of the
    /// records before it, and a press among them would stay held.
    fn drain(&self, budget: usize, apply: impl FnMut(Record)) -> usize {
        self.drain_with(budget, apply, || {})
    }

    /// [`Self::drain`], running `between` where the worker has found the ring empty and has not taken
    /// the loss yet (the window thread may deliver right there; a test delivers from it).
    fn drain_with(
        &self,
        budget: usize,
        mut apply: impl FnMut(Record),
        mut between: impl FnMut(),
    ) -> usize {
        let mut applied = 0;
        while applied < budget {
            let lost = self.lost.load(Ordering::Acquire);
            // SAFETY: the one InputFeed owning this ring is its only consumer.
            match unsafe { self.ring.pop() } {
                Some(record) => {
                    if record == Record::Lost {
                        self.count(LOSSES);
                    }
                    apply(record);
                }
                None => {
                    between();
                    if lost != 0
                        && self
                            .lost
                            .compare_exchange(lost, 0, Ordering::AcqRel, Ordering::Relaxed)
                            .is_ok()
                    {
                        self.count(LOSSES);
                        apply(Record::Lost);
                    } else {
                        break;
                    }
                }
            }
            applied += 1;
            self.count(APPLIED);
        }
        applied
    }

    fn counters(&self) -> FeedCounters {
        let c = |i: usize| self.counts[i].load(Ordering::Relaxed);
        FeedCounters {
            received: c(RECEIVED),
            queued: c(QUEUED),
            duplicates: c(DUPLICATES),
            ignored: c(IGNORED),
            full: c(FULL),
            after_stop: c(AFTER_STOP),
            losses: c(LOSSES),
            marked: c(MARKED),
            applied: c(APPLIED),
        }
    }
}

/// The function registered with kiln.ui's `subscribe_window_input`. Window thread, inside the window
/// procedure: no allocation, no lock, no wait, no panic across the C boundary.
///
/// # Safety
/// `user` is the reference [`InputFeed::subscribe`] registered (never released while kiln.ui may call).
pub unsafe extern "C" fn on_window_input(input: *const KilnUiWindowInput, user: *mut c_void) {
    if user.is_null() {
        return;
    }
    // SAFETY: the caller's contract.
    let shared = unsafe { &*user.cast_const().cast::<FeedShared>() };
    let _ = catch_unwind(AssertUnwindSafe(|| shared.window_input(input)));
}

#[cfg(windows)]
fn current_thread() -> u32 {
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentThreadId() -> u32;
    }
    // SAFETY: no arguments.
    unsafe { GetCurrentThreadId() }
}
#[cfg(not(windows))]
fn current_thread() -> u32 {
    0
}

/// kiln.ui's `raw_input::registered_for(usage, GetFocus())` (raw_input_source.h): a list that cannot be
/// read is no proof that raw input stopped, so it counts as registered.
#[cfg(windows)]
pub fn raw_input_registered(usage: u16) -> bool {
    #[repr(C)]
    #[derive(Clone, Copy)]
    struct RawInputDevice {
        usage_page: u16,
        usage: u16,
        flags: u32,
        target: *mut c_void,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetRegisteredRawInputDevices(
            devices: *mut RawInputDevice,
            count: *mut u32,
            size: u32,
        ) -> u32;
        fn GetFocus() -> *mut c_void;
        fn IsWindow(window: *mut c_void) -> i32;
    }
    const RIDEV_EXCLUDE: u32 = 0x10;
    const RIDEV_PAGEONLY: u32 = 0x20;
    const RIDEV_NOLEGACY: u32 = 0x30;
    let mut devices = [RawInputDevice {
        usage_page: 0,
        usage: 0,
        flags: 0,
        target: core::ptr::null_mut(),
    }; 64];
    let mut count = devices.len() as u32;
    // SAFETY: a stack array of `count` entries of the size passed.
    let n = unsafe {
        GetRegisteredRawInputDevices(
            devices.as_mut_ptr(),
            &mut count,
            core::mem::size_of::<RawInputDevice>() as u32,
        )
    };
    if n == u32::MAX {
        return true;
    }
    // SAFETY: no arguments.
    let window = unsafe { GetFocus() };
    // SAFETY: IsWindow accepts any value.
    let targets = |target: *mut c_void| {
        target.is_null()
            || target == window
            || (window.is_null() && unsafe { IsWindow(target) } != 0)
    };
    let mut page = false;
    for device in &devices[..(n as usize).min(devices.len())] {
        if device.usage_page != 1 {
            continue;
        }
        if device.usage == usage {
            if device.flags & RIDEV_NOLEGACY == RIDEV_EXCLUDE {
                return false;
            }
            return targets(device.target);
        }
        if device.usage == 0 && device.flags & RIDEV_PAGEONLY != 0 {
            page = targets(device.target);
        }
    }
    page
}
/// Off Windows there is no raw input list: keep the raw source.
#[cfg(not(windows))]
pub fn raw_input_registered(_usage: u16) -> bool {
    true
}

// ---- the bounded ring (Vyukov's bounded MPMC queue used single-consumer; plain records) ----

struct Slot {
    sequence: AtomicUsize,
    value: UnsafeCell<MaybeUninit<Record>>,
}

struct Ring {
    slots: Box<[Slot]>,
    mask: usize,
    head: AtomicUsize,
    tail: AtomicUsize,
}
// SAFETY: a value moves between threads only through its slot's sequence (release on publish, acquire on
// read); `Record` is plain data.
unsafe impl Send for Ring {}
unsafe impl Sync for Ring {}

impl Ring {
    fn new(capacity: usize) -> Self {
        let capacity = capacity.clamp(2, 1 << 20).next_power_of_two();
        Self {
            slots: (0..capacity)
                .map(|i| Slot {
                    sequence: AtomicUsize::new(i),
                    value: UnsafeCell::new(MaybeUninit::uninit()),
                })
                .collect(),
            mask: capacity - 1,
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
        }
    }
    fn capacity(&self) -> usize {
        self.mask + 1
    }
    /// Never waits: a full ring is an immediate `Err`.
    fn push(&self, value: Record) -> Result<(), Record> {
        let mut position = self.head.load(Ordering::Relaxed);
        loop {
            let slot = &self.slots[position & self.mask];
            let sequence = slot.sequence.load(Ordering::Acquire);
            let lag = sequence.wrapping_sub(position) as isize;
            if lag == 0 {
                match self.head.compare_exchange_weak(
                    position,
                    position.wrapping_add(1),
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => {
                        // SAFETY: the CAS gave this producer the slot; the consumer reads it only after
                        // the release below.
                        unsafe { (*slot.value.get()).write(value) };
                        slot.sequence
                            .store(position.wrapping_add(1), Ordering::Release);
                        return Ok(());
                    }
                    Err(current) => position = current,
                }
            } else if lag < 0 {
                return Err(value);
            } else {
                position = self.head.load(Ordering::Relaxed);
            }
        }
    }
    /// # Safety
    /// One consumer at a time.
    unsafe fn pop(&self) -> Option<Record> {
        let position = self.tail.load(Ordering::Relaxed);
        let slot = &self.slots[position & self.mask];
        if slot.sequence.load(Ordering::Acquire) != position.wrapping_add(1) {
            return None;
        }
        // SAFETY: the acquire above saw the producer's release of this value.
        let value = unsafe { (*slot.value.get()).assume_init_read() };
        slot.sequence
            .store(position.wrapping_add(self.mask + 1), Ordering::Release);
        self.tail.store(position.wrapping_add(1), Ordering::Relaxed);
        Some(value)
    }
}

// ---- the key state (the fork's host feed, on the worker) ----

/// ShroudForge's shortcut state as the fork's host feed keeps it, for one feed. Worker only.
#[derive(Clone, Debug, Default)]
pub struct KeyFeed {
    /// `ACTIVE` with `HOST`: between [`Self::begin`] and [`Self::end`].
    active: bool,
    /// Held keys and buttons, with the modifier groups held at their press (`KEYS`).
    keys: BTreeMap<Trigger, u8>,
    /// The modifier groups of the last record (`MODIFIERS`).
    modifiers: u8,
    /// LCTRL..RGUI held, one bit each (`HOST_MODIFIER_KEYS`).
    modifier_keys: u8,
    /// `PRESS_SEQUENCE`: never reset, so a later press is newer than any before.
    sequence: u64,
    /// The newest press revision per (trigger, modifier groups) (`PRESS_REVISIONS`).
    presses: BTreeMap<(Trigger, u8), u64>,
    /// Pause's E1 prefix (host_input.rs `Feed.pending_e1`).
    pending_e1: bool,
}

impl KeyFeed {
    /// host_begin: nothing held, no press from before counts, and the feed answers.
    pub fn begin(&mut self) {
        self.pending_e1 = false;
        self.clear();
        self.presses.clear();
        self.active = true;
    }
    /// host_end: "not held" and revision 0 from here on.
    pub fn end(&mut self) {
        if self.active {
            self.active = false;
            self.clear();
            self.presses.clear();
        }
    }
    pub fn is_active(&self) -> bool {
        self.active
    }
    /// host_reset (and a lost run of records): every key and button counts as released. Press
    /// revisions stay.
    pub fn reset(&mut self) {
        self.pending_e1 = false;
        if self.active {
            self.clear();
        }
    }
    fn clear(&mut self) {
        self.modifier_keys = 0;
        self.modifiers = 0;
        self.keys.clear();
    }

    /// One record. `captured`: kiln.ui captured the input when it came (no press revision then).
    pub fn apply(&mut self, record: Record, captured: bool, scan_of_vk: &dyn Fn(u16) -> u16) {
        match record {
            Record::RawKey {
                make_code,
                flags,
                vkey,
            } => {
                let scancode =
                    raw_scancode(make_code, flags, vkey, &mut self.pending_e1, scan_of_vk);
                self.key(scancode, flags & RI_KEY_BREAK == 0, captured);
            }
            Record::WindowKey { wparam, lparam } => {
                let scancode = message_scancode(wparam, lparam, scan_of_vk);
                let down = lparam & (1 << 31) == 0;
                // Windows sends a window only the release of Print Screen: count the press that never came.
                if scancode == SCANCODE_PRINTSCREEN && !down && !self.key_is_down(scancode) {
                    self.key(scancode, true, captured);
                }
                self.key(scancode, down, captured);
            }
            Record::MouseButton { button, down } => self.mouse_button(button, down, captured),
            Record::FocusLost | Record::Lost => self.reset(),
        }
    }

    /// host_key: an SDL scancode went down or up.
    pub fn key(&mut self, scancode: u16, down: bool, captured: bool) {
        if !self.active || !(1..SCANCODE_COUNT as u16).contains(&scancode) {
            return;
        }
        // A modifier key counts for its own press and no longer for its release, as in SDL's events.
        if (224..=231).contains(&scancode) {
            let bit = 1u8 << (scancode - 224);
            if down {
                self.modifier_keys |= bit;
            } else {
                self.modifier_keys &= !bit;
            }
        }
        let modifiers = modifier_groups(self.modifier_keys);
        self.record(Trigger::Keyboard(scancode), down, modifiers, captured);
    }

    /// host_mouse_button: SDL button 1..=32.
    pub fn mouse_button(&mut self, button: u8, down: bool, captured: bool) {
        if !self.active || !(1..=32).contains(&button) {
            return;
        }
        let modifiers = modifier_groups(self.modifier_keys);
        self.record(Trigger::Mouse(button), down, modifiers, captured);
    }

    /// observe_keyboard.rs `record`, a Key message (never a repeat from the host).
    fn record(&mut self, trigger: Trigger, down: bool, modifiers: u8, captured: bool) {
        self.modifiers = modifiers;
        if down && self.keys.contains_key(&trigger) {
            return; // held: an auto-repeat is no new press
        }
        if down {
            self.keys.insert(trigger, modifiers);
        } else {
            self.keys.remove(&trigger);
        }
        if down && !captured {
            self.sequence = self.sequence.wrapping_add(1);
            self.presses.insert((trigger, modifiers), self.sequence);
        }
    }

    /// observe_keyboard.rs `is_down`: a chord is held with exactly its modifier groups.
    pub fn is_down(&self, binding: &Binding) -> bool {
        self.active
            && binding.chords().iter().any(|chord| {
                let required = chord.required_modifiers();
                self.modifiers == required && self.keys.get(&chord.trigger) == Some(&required)
            })
    }

    /// observe_keyboard.rs `press_revision`: the newest press of any of the chords, 0 for none.
    pub fn press_revision(&self, binding: &Binding) -> u64 {
        if !self.active {
            return 0;
        }
        binding
            .chords()
            .iter()
            .filter_map(|chord| {
                self.presses
                    .get(&(chord.trigger, chord.required_modifiers()))
                    .copied()
            })
            .max()
            .unwrap_or(0)
    }

    /// Whether the feed has this key as held (host_key_is_down).
    pub fn key_is_down(&self, scancode: u16) -> bool {
        self.keys.contains_key(&Trigger::Keyboard(scancode))
    }
    /// The modifier groups of the last record.
    pub fn modifiers(&self) -> u8 {
        self.modifiers
    }
    /// Keys and buttons held now.
    pub fn held(&self) -> usize {
        self.keys.len()
    }
}

/// Bits 0..=7 LCTRL, LSHIFT, LALT, LGUI, RCTRL, RSHIFT, RALT, RGUI as groups Ctrl 1, Alt 2, Shift 4, Meta 8.
pub fn modifier_groups(keys: u8) -> u8 {
    let either = (keys | keys >> 4) & 0x0f;
    (either & 1) | ((either & 4) >> 1) | ((either & 2) << 1) | (either & 8)
}

// ---- was_keybind_pressed ----

/// One mod's `was_keybind_pressed` memory: per setting key, the binding's canonical text and the press
/// revision it last saw (create_mod_api.rs:149-180). Worker only.
#[derive(Clone, Debug, Default)]
pub struct PressTracker {
    revisions: HashMap<String, (String, u64)>,
}
impl PressTracker {
    /// `binding` is the setting as `read_keybind` gave it (`None`: disabled or empty), `revision` its
    /// press revision now, `focused` the game focused and no capture. The first call for a key, and the
    /// first after its binding changed, only remembers: a press from before is never reported.
    pub fn was_pressed(
        &mut self,
        key: &str,
        binding: Option<&Binding>,
        revision: u64,
        focused: bool,
    ) -> bool {
        let Some(binding) = binding else {
            self.revisions.remove(key);
            return false;
        };
        let signature = binding.normalized();
        match self.revisions.get_mut(key) {
            Some((previous_signature, previous_revision)) if *previous_signature == signature => {
                let pressed = focused && revision > *previous_revision;
                *previous_revision = revision;
                pressed
            }
            _ => {
                self.revisions.insert(key.to_owned(), (signature, revision));
                false
            }
        }
    }
}

// ---- the desktop: focus, layout and key names ----

/// What the worker asks Windows (a test gives its own).
pub trait Desktop {
    /// create_mod_api.rs `is_game_focused`: the foreground window belongs to this process.
    fn game_focused(&self) -> bool;
    /// `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX)` in the keyboard layout of `window_thread` (0: unknown).
    fn scan_of_virtual_key(&self, vk: u16, window_thread: u32) -> u16;
    /// The keyboard layout of `window_thread` (0: unknown).
    fn layout(&self, window_thread: u32) -> usize;
    /// Key names by SDL scancode in `layout` ([`SCANCODE_COUNT`] entries, or none).
    fn key_names(&self, layout: usize) -> Vec<String>;
}

/// The real desktop. Off Windows: never focused, no names (ShroudForge's own answer there).
pub struct SystemDesktop;

#[cfg(windows)]
mod system {
    #[link(name = "user32")]
    unsafe extern "system" {
        pub fn GetForegroundWindow() -> *mut core::ffi::c_void;
        pub fn GetWindowThreadProcessId(window: *mut core::ffi::c_void, process: *mut u32) -> u32;
        pub fn GetKeyboardLayout(thread: u32) -> usize;
        pub fn MapVirtualKeyExW(code: u32, map: u32, layout: usize) -> u32;
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        pub fn GetCurrentProcessId() -> u32;
    }
}

impl Desktop for SystemDesktop {
    #[cfg(windows)]
    fn game_focused(&self) -> bool {
        // SAFETY: plain Win32 queries; a NULL window is checked.
        unsafe {
            let foreground = system::GetForegroundWindow();
            if foreground.is_null() {
                return false;
            }
            let mut process = 0;
            system::GetWindowThreadProcessId(foreground, &mut process);
            process == system::GetCurrentProcessId()
        }
    }
    #[cfg(not(windows))]
    fn game_focused(&self) -> bool {
        false
    }
    #[cfg(windows)]
    fn scan_of_virtual_key(&self, vk: u16, window_thread: u32) -> u16 {
        const MAPVK_VK_TO_VSC_EX: u32 = 4;
        // SAFETY: plain Win32 queries.
        unsafe {
            let layout = system::GetKeyboardLayout(window_thread);
            system::MapVirtualKeyExW(u32::from(vk), MAPVK_VK_TO_VSC_EX, layout) as u16
        }
    }
    #[cfg(not(windows))]
    fn scan_of_virtual_key(&self, _: u16, _: u32) -> u16 {
        0
    }
    #[cfg(windows)]
    fn layout(&self, window_thread: u32) -> usize {
        // SAFETY: plain Win32 query (thread 0: this thread).
        unsafe { system::GetKeyboardLayout(window_thread) }
    }
    #[cfg(not(windows))]
    fn layout(&self, _: u32) -> usize {
        0
    }
    #[cfg(windows)]
    fn key_names(&self, layout: usize) -> Vec<String> {
        crate::input::label::system_key_names(layout)
    }
    #[cfg(not(windows))]
    fn key_names(&self, _: usize) -> Vec<String> {
        Vec::new()
    }
}

// ---- the subscription ----

type SubscribeFn = unsafe extern "C" fn(
    KilnOwner,
    KilnUiWindowInputFn,
    *mut c_void,
    *mut KilnHandle,
) -> KilnResult;
type UnsubscribeFn = unsafe extern "C" fn(KilnHandle) -> KilnResult;
type StatusFn = unsafe extern "C" fn(*mut KilnUiStatus) -> KilnResult;

/// Why there is no feed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FeedError {
    /// kiln.ui v5 or one of its members is not there.
    Service(ServiceError),
    /// `subscribe_window_input` refused (or gave no handle).
    Refused(KilnResult),
}
impl std::fmt::Display for FeedError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Service(error) => write!(f, "{error}"),
            Self::Refused(code) => {
                write!(f, "kiln.ui refused the window input subscription ({code})")
            }
        }
    }
}
impl std::error::Error for FeedError {}

/// What [`InputFeed::stop`] did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StopOutcome {
    /// Unsubscribed; the registration's reference is back.
    Unsubscribed,
    /// kiln.ui did not confirm the removal: the registration keeps its reference for the process.
    Kept(KilnResult),
    /// Stopped before.
    AlreadyStopped,
}

struct Registration {
    handle: KilnHandle,
    user: *const FeedShared,
    unsubscribe: Member<UnsubscribeFn>,
}

/// The worker's input feed: the subscription, the key state and the labels. `!Send`: it lives on the
/// worker thread with the [`ServiceAdapter`] it was made with.
pub struct InputFeed {
    shared: Arc<FeedShared>,
    registration: Option<Registration>,
    status: Option<Member<StatusFn>>,
    keys: KeyFeed,
    labels: LabelTable,
    labels_layout: Option<usize>,
    desktop: Box<dyn Desktop>,
    captured: bool,
}

impl InputFeed {
    /// Subscribes to kiln.ui's window input (v5) under `owner`. kiln.ui's `status` is optional (without it
    /// the feed never counts as captured).
    pub fn subscribe(
        adapter: &ServiceAdapter,
        owner: KilnOwner,
        desktop: Box<dyn Desktop>,
        config: FeedConfig,
    ) -> Result<Self, FeedError> {
        let table = adapter
            .bind::<KilnUiApi>(KILN_UI_SERVICE, 5)
            .map_err(FeedError::Service)?;
        let subscribe: Member<SubscribeFn> =
            service_member!(table, KilnUiApi, subscribe_window_input)
                .map_err(FeedError::Service)?;
        let unsubscribe: Member<UnsubscribeFn> =
            service_member!(table, KilnUiApi, unsubscribe).map_err(FeedError::Service)?;
        let status: Option<Member<StatusFn>> = service_member!(table, KilnUiApi, status).ok();
        let shared = Arc::new(FeedShared::new(config));
        let user = Arc::into_raw(shared.clone());
        let mut handle: KilnHandle = 0;
        let function: KilnUiWindowInputFn = Some(on_window_input);
        // SAFETY: kiln_ui.h subscribe_window_input: a function, a user pointer it hands back, an out handle.
        let result = unsafe {
            adapter.call(
                subscribe,
                (
                    owner,
                    function,
                    user.cast_mut().cast::<c_void>(),
                    &raw mut handle,
                ),
            )
        };
        if result != KILN_OK {
            // SAFETY: refused: kiln.ui keeps nothing, so the reference handed over is back.
            drop(unsafe { Arc::from_raw(user) });
            return Err(FeedError::Refused(result));
        }
        if handle == 0 {
            // Registered without a handle to remove it with: the reference stays with kiln.ui.
            shared.stopping.store(true, Ordering::Release);
            return Err(FeedError::Refused(result));
        }
        let mut keys = KeyFeed::default();
        keys.begin();
        Ok(Self {
            shared,
            registration: Some(Registration {
                handle,
                user,
                unsubscribe,
            }),
            status,
            keys,
            labels: LabelTable::default(),
            labels_layout: None,
            desktop,
            captured: false,
        })
    }

    /// Drains the window records into the key state (worker, once per turn before the mods' update) and
    /// samples kiln.ui's capture. Returns the records applied (at most four ring lengths per call, so a
    /// window thread that never pauses cannot hold the worker here).
    pub fn pump(&mut self, adapter: &ServiceAdapter) -> usize {
        let budget = self.shared.ring.capacity() * 4;
        self.pump_at_most(adapter, budget)
    }

    /// [`Self::pump`] applying at most `budget` records; the rest wait for the next call.
    pub fn pump_at_most(&mut self, adapter: &ServiceAdapter, budget: usize) -> usize {
        if self.registration.is_none() {
            return 0;
        }
        self.captured = self.sample_captured(adapter);
        let window_thread = self.shared.window_thread.load(Ordering::Relaxed);
        let desktop = &*self.desktop;
        let keys = &mut self.keys;
        let captured = self.captured;
        let mut relabel = false;
        let applied = self.shared.drain(budget, |record| {
            relabel |= matches!(record, Record::FocusLost | Record::Lost);
            keys.apply(record, captured, &|vk| {
                desktop.scan_of_virtual_key(vk, window_thread)
            });
        });
        if relabel {
            // host_reset sends the labels again: a layout changed while the window was away shows now.
            self.labels_layout = None;
        }
        applied
    }

    fn sample_captured(&self, adapter: &ServiceAdapter) -> bool {
        let Some(status) = self.status else {
            return false;
        };
        // SAFETY: KilnUiStatus is plain data; zero is a valid value.
        let mut out: KilnUiStatus = unsafe { core::mem::zeroed() };
        out.size = core::mem::size_of::<KilnUiStatus>() as u32;
        // SAFETY: kiln_ui.h status: a record with its size set, any thread.
        let result = unsafe { adapter.call(status, (&raw mut out,)) };
        result == KILN_OK && out.capturing != 0
    }

    /// ShroudForge's `is_game_focused() && !is_capture_active()`.
    pub fn focused(&self) -> bool {
        self.desktop.game_focused() && !self.captured
    }
    /// kiln.ui captured the input at the last [`Self::pump`].
    pub fn captured(&self) -> bool {
        self.captured
    }
    /// `is_keybind_down` for a binding `read_keybind` gave.
    pub fn is_keybind_down(&self, binding: &Binding) -> bool {
        self.focused() && self.keys.is_down(binding)
    }
    /// The binding's newest press revision (`hotkey_press_revision`).
    pub fn press_revision(&self, binding: &Binding) -> u64 {
        self.keys.press_revision(binding)
    }
    /// `was_keybind_pressed` through a mod's [`PressTracker`].
    pub fn was_keybind_pressed(
        &self,
        tracker: &mut PressTracker,
        key: &str,
        binding: Option<&Binding>,
    ) -> bool {
        let revision = binding.map_or(0, |binding| self.press_revision(binding));
        tracker.was_pressed(key, binding, revision, self.focused())
    }
    /// `keybind_label`: the label in the game window's current keyboard layout.
    pub fn label(&mut self, binding: &Binding) -> String {
        self.refresh_labels();
        binding.label(&self.labels)
    }
    /// The label table's revision (a new one for each layout whose names differ).
    pub fn layout_revision(&mut self) -> u64 {
        self.refresh_labels();
        self.labels.revision()
    }
    fn refresh_labels(&mut self) {
        let window_thread = self.shared.window_thread.load(Ordering::Relaxed);
        let layout = self.desktop.layout(window_thread);
        if self.labels_layout != Some(layout) || self.labels.is_empty() {
            self.labels.replace(self.desktop.key_names(layout));
            self.labels_layout = Some(layout);
        }
    }

    pub fn keys(&self) -> &KeyFeed {
        &self.keys
    }
    pub fn counters(&self) -> FeedCounters {
        self.shared.counters()
    }
    /// Records the ring holds.
    pub fn capacity(&self) -> usize {
        self.shared.ring.capacity()
    }
    pub fn is_live(&self) -> bool {
        self.registration.is_some()
    }

    /// Ends the feed: the window thread drops every record from here on, kiln.ui removes the
    /// subscription (waiting for a record in flight), and the key state answers "not held" and
    /// revision 0. Worker thread, never from inside a kiln.ui callback.
    pub fn stop(&mut self, adapter: &ServiceAdapter) -> StopOutcome {
        let Some(registration) = self.registration.take() else {
            return StopOutcome::AlreadyStopped;
        };
        self.shared.stopping.store(true, Ordering::Release);
        // SAFETY: kiln_ui.h unsubscribe with the handle subscribe gave.
        let result = unsafe { adapter.call(registration.unsubscribe, (registration.handle,)) };
        self.keys.end();
        if result == KILN_OK {
            // SAFETY: KILN_OK: the call in flight has ended and the function is not called again, so the
            // reference the registration held is no longer used.
            drop(unsafe { Arc::from_raw(registration.user) });
            StopOutcome::Unsubscribed
        } else {
            StopOutcome::Kept(result)
        }
    }
}

impl Drop for InputFeed {
    /// Without [`Self::stop`] (no adapter here) the subscription stays: the window thread drops its
    /// records, and the registration keeps its reference for the process.
    fn drop(&mut self) {
        if self.registration.is_some() {
            self.shared.stopping.store(true, Ordering::Release);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(make_code: u16, down: bool) -> Record {
        Record::RawKey {
            make_code,
            flags: if down { 0 } else { RI_KEY_BREAK },
            vkey: 0,
        }
    }
    fn no_vk(_: u16) -> u16 {
        0
    }

    /// The worker found the ring empty with a loss still unmarked, and right then, before it took that
    /// loss, the window thread delivered: the marker and C's press filled the ring, and C's release
    /// was lost (a second loss). The worker must not take the second loss ahead of the press before
    /// it: C's press would come after the loss and C would stay held.
    #[test]
    fn a_worker_that_saw_the_ring_empty_never_takes_a_later_loss_early() {
        let shared = FeedShared::new(FeedConfig {
            capacity: 2,
            probe: |_| true,
        });
        let (a, b, c) = (0x1e, 0x30, 0x2e);
        for record in [key(a, true), key(b, true), key(a, false)] {
            shared.produce(record); // A and B down fill the ring; A's release is lost
        }
        assert_eq!(shared.counters().full, 1);
        let mut keys = KeyFeed::default();
        keys.begin();
        let mut applied = Vec::new();
        let mut window = Some(|| {
            shared.produce(key(c, true)); // the marker, then C's press: the ring is full again
            shared.produce(key(c, false)); // C's release is lost
        });
        shared.drain_with(
            usize::MAX,
            |record| {
                applied.push(record);
                keys.apply(record, false, &no_vk);
            },
            || {
                if let Some(deliver) = window.take() {
                    deliver();
                }
            },
        );
        while shared.drain(usize::MAX, |record| {
            applied.push(record);
            keys.apply(record, false, &no_vk);
        }) > 0
        {}
        assert!(
            !keys.key_is_down(scancode_of_set1(c as u8)),
            "C's release was lost: C must not stay held ({applied:?})"
        );
        assert_eq!(keys.held(), 0, "{applied:?}");
        let counts = shared.counters();
        assert_eq!(
            (counts.full, counts.marked, counts.losses),
            (2, 1, 2),
            "{applied:?}"
        );
    }
}
