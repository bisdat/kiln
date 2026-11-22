// SPDX-License-Identifier: MIT
//! kiln-sfi's own do-not-save banner in game: the fallback for a kiln start whose
//! Kiln.Ui is absent. Kiln.Ui shows the persistent notice itself and, once it is up and can draw, creates the
//! named event `Local\Kiln.Sfi.UiReady.<pid>`; kiln-sfi only looks whether that name exists ([`kiln_ui_ready`]:
//! `OpenEventW`, never a wait, so an unsignalled event a squatter made counts as present too).
//!
//! **When it draws.** Only while a warning is pending (sfi.json has warnings and the current revision has not had its
//! showing, or a world was entered since the last one) **and** Kiln.Ui is absent: no ready event [`GRACE`] (10 s) after
//! the first main-menu frame (`KilnUiScreen.ui_state` at the main menu or later; `KilnUiStatus.presents` when the
//! game's screens are unknown), never timed from the process start (Kiln.Ui's managed boot can take seconds). A showing
//! lasts [`SHOW_FOR`] from its first drawn frame (the banner takes no input, so the player cannot close it); a new
//! warning revision and entering a world show it again. Otherwise kiln-sfi has **no frame subscription and no demand**:
//! kiln.ui draws its own fallback Kiln menu only while no frame subscriber exists (native/services/ui/ui.cpp,
//! `if (f.menu_open && subs.empty()) fallback_menu()`), so the banner also steps aside while the Kiln menu is open.
//!
//! **Delivery** (sfi.json `delivery`, `surface: native`, [`crate::state::Delivery`]): `pending` when the banner takes a
//! revision, `shown` (with the time and the world session) only after a frame callback drew it, and `not-shown` with a
//! loud kiln.log line when no frame can draw it: the overlay's drawing is off (kiln.json `ui.overlay` off, the
//! overlay's crash guard, or a session that turned it off: `KilnUiStatus.error` begins with "the overlay's drawing is
//! off:"), there is no kiln.ui, or the banner's frame callback failed. Never shown without a drawn frame. The overlay
//! check comes before the ready event: drawing that turned off after Kiln.Ui made its event leaves no surface that can
//! draw, so that is not-shown too. A record that is final already (refused) takes no rows; kiln.log has the lines.
//!
//! **Dedicated server:** kiln.log only, each warning once at ERROR; kiln.ui is never looked up, nothing subscribes.
//!
//! **Threads and lifetime** (the part that must not go wrong):
//! - The control step ([`Notice::step`]) runs on kiln-sfi's IO thread ([`StateHub::add_io_job`]), every
//!   [`TICK`] while a warning needs it, every [`IDLE`] otherwise. [`Notice::close`] (on_unload) is the only other
//!   caller of kiln.ui's subscribe/unsubscribe/set_demand. Both hold the control lock across those calls.
//! - The frame callback runs on the game's render thread. It never takes the control lock: it reads the immutable call
//!   table, clones the published banner under its own short lock (never held across a kiln.ui call by anyone) and
//!   stores the showing it drew in an atomic. So kiln.ui's unsubscribe, which waits for a callback in flight
//!   (ui.cpp `CallbackLifetime::drain`), can never wait on a lock its caller holds: no deadlock.
//! - The callback's `user` is the render-side state ([`Shared`]) behind an `Arc`. Each subscription holds one strong
//!   count of it (`Arc::into_raw`), given back only after kiln.ui's unsubscribe answered `KILN_OK` (no call in flight,
//!   none later). Any other answer keeps the count: the state then lives for the process, never freed under a frame.
//! - The callback never unwinds into kiln.ui (a panic is caught; the banner then counts as faulted, not shown) and
//!   always pairs `end` with `begin`.

use crate::state::{Delivery, DeliveryState, IoJob, IoNext, Side, StateHub};
use kiln_abi::services::kiln_ui::*;
use kiln_abi::*;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

/// Kiln.Ui is absent when its ready event is missing this long after the first main-menu frame.
pub const GRACE: Duration = Duration::from_secs(10);
/// One showing of the banner, from its first drawn frame.
pub const SHOW_FOR: Duration = Duration::from_secs(30);
/// The control step's period while a warning needs the banner (waiting, showing).
pub const TICK: Duration = Duration::from_millis(250);
/// The control step's period otherwise.
pub const IDLE: Duration = Duration::from_secs(1);
/// How `KilnUiStatus.error` begins while the overlay's drawing is off (docs/kiln/services/ui.md; Kiln.Ui's
/// `UiNative.DrawingOffReason` reads it the same way).
pub const OVERLAY_OFF: &str = "the overlay's drawing is off:";
/// The banner shows the first this many warnings; the rest are counted.
pub const BANNER_PACKAGES: usize = 3;
/// A warning's text on the banner is cut after this many characters (kiln.log has it whole).
pub const BANNER_CHARS: usize = 360;
/// kiln.log gets the text of this many warnings when the banner takes a revision or cannot show it.
pub const LOG_WARNINGS: usize = 16;
/// The window's id (the title shown is the part before `###`).
pub const TITLE: &str = "Kiln: mods off this start###kiln.sfi.native.notice";
/// The banner's last line.
pub const HINT: &str = "Kiln's interface (Kiln.Ui) is not running, so this notice shows for a short while, again on \
entering a world and on a new warning. Every warning is in kiln.log.";

type LogFn = unsafe extern "C" fn(KilnOwner, i32, KilnStr);
type ServiceFn = unsafe extern "C" fn(KilnStr, u32) -> *const c_void;
type SessionFn = unsafe extern "C" fn(*mut KilnSessionInfo) -> KilnResult;
type StatusFn = unsafe extern "C" fn(*mut KilnUiStatus) -> KilnResult;
type SubscribeFrameFn =
    unsafe extern "C" fn(KilnOwner, KilnUiFrameFn, *mut c_void, *mut KilnHandle) -> KilnResult;
type UnsubscribeFn = unsafe extern "C" fn(KilnHandle) -> KilnResult;
type DemandFn = unsafe extern "C" fn(u32);
type BeginFn = unsafe extern "C" fn(KilnStr, *mut bool, u32) -> bool;
type EndFn = unsafe extern "C" fn();
type TextFn = unsafe extern "C" fn(KilnStr);
type PosFn = unsafe extern "C" fn(f32, f32, u32, f32, f32);
type SizeFn = unsafe extern "C" fn(f32, f32, u32);
type AlphaFn = unsafe extern "C" fn(f32);
type ScreenFn = unsafe extern "C" fn(*mut KilnUiScreen) -> KilnResult;

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|p| p.into_inner())
}

fn kstr(text: &str) -> KilnStr {
    KilnStr {
        ptr: text.as_ptr().cast(),
        len: text.len(),
    }
}

/// What the banner needs from the core, copied once (any thread).
#[derive(Clone, Copy)]
pub struct Env {
    pub log: LogFn,
    pub service: ServiceFn,
    /// `KilnApi.session` (`None`: an older core), for the world session a showing was in.
    pub session: Option<SessionFn>,
    /// kiln-sfi's plugin owner: kiln.ui's subscription and kiln.log's lines are its.
    pub owner: KilnOwner,
    /// This process: the ready event's name.
    pub pid: u32,
}

/// What tests replace: the ready-event probe and the two durations.
#[derive(Clone, Copy)]
pub struct NoticeHooks {
    /// Kiln.Ui created `Local\Kiln.Sfi.UiReady.<pid>` (existence only).
    pub kiln_ui_ready: fn(u32) -> bool,
    pub grace: Duration,
    pub show_for: Duration,
}
impl NoticeHooks {
    pub const PRODUCTION: Self = Self {
        kiln_ui_ready,
        grace: GRACE,
        show_for: SHOW_FOR,
    };
}

/// The ready event's name for `pid` (Kiln.Ui's `SfiSafetyNotice.ReadyEventName`).
pub fn ready_event_name(pid: u32) -> String {
    format!("Local\\Kiln.Sfi.UiReady.{pid}")
}

/// Whether Kiln.Ui's ready event exists: `OpenEventW(SYNCHRONIZE)` succeeds. Existence only, never a wait (a squatted
/// event may be unsignalled). A failed open of any kind (missing; the name held by another object type, which also
/// keeps Kiln.Ui from making its own; denied) is "absent": the banner may then show next to Kiln.Ui's, never neither.
#[cfg(windows)]
pub fn kiln_ui_ready(pid: u32) -> bool {
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn OpenEventW(access: u32, inherit: i32, name: *const u16) -> *mut c_void;
        fn CloseHandle(handle: *mut c_void) -> i32;
    }
    const SYNCHRONIZE: u32 = 0x0010_0000;
    let name: Vec<u16> = ready_event_name(pid)
        .encode_utf16()
        .chain(Some(0))
        .collect();
    // SAFETY: a NUL-terminated name; the handle is closed at once.
    let handle = unsafe { OpenEventW(SYNCHRONIZE, 0, name.as_ptr()) };
    if handle.is_null() {
        return false;
    }
    unsafe { CloseHandle(handle) };
    true
}
/// No Kiln.Ui outside Windows.
#[cfg(not(windows))]
pub fn kiln_ui_ready(_pid: u32) -> bool {
    false
}

/// The kiln.ui members the banner uses, checked against the table's declared size.
#[derive(Clone, Copy)]
struct Calls {
    status: StatusFn,
    subscribe_frame: SubscribeFrameFn,
    unsubscribe: UnsubscribeFn,
    set_demand: DemandFn,
    begin: BeginFn,
    end: EndFn,
    text_wrapped: TextFn,
    set_next_window_pos: Option<PosFn>,
    set_next_window_size: Option<SizeFn>,
    set_next_window_bg_alpha: Option<AlphaFn>,
    screen: Option<ScreenFn>,
}
impl Calls {
    /// # Safety
    /// `table` is NULL or a kiln.ui table readable for its declared size.
    unsafe fn read(table: *const KilnUiApi) -> Option<Self> {
        if table.is_null() || !table.is_aligned() {
            return None;
        }
        // SAFETY: the table starts with its header; a member is read only when the declared size covers it.
        let header = unsafe { table.cast::<KilnServiceHeader>().read() };
        if header.version < 1 {
            return None;
        }
        let size = header.size as usize;
        macro_rules! member {
            ($name:ident) => {
                if size >= core::mem::offset_of!(KilnUiApi, $name) + core::mem::size_of::<usize>() {
                    unsafe { core::ptr::addr_of!((*table).$name).read() }
                } else {
                    None
                }
            };
        }
        Some(Self {
            status: member!(status)?,
            subscribe_frame: member!(subscribe_frame)?,
            unsubscribe: member!(unsubscribe)?,
            set_demand: member!(set_demand)?,
            begin: member!(begin)?,
            end: member!(end)?,
            text_wrapped: member!(text_wrapped)?,
            set_next_window_pos: member!(set_next_window_pos),
            set_next_window_size: member!(set_next_window_size),
            set_next_window_bg_alpha: member!(set_next_window_bg_alpha),
            screen: member!(screen),
        })
    }
}

/// What one showing draws: published whole, never changed.
struct Banner {
    /// The showing it belongs to (from 1).
    showing: u64,
    lines: Vec<String>,
}

/// The render thread's side: the frame callback's `user`.
struct Shared {
    calls: Calls,
    /// The banner to draw now (`None`: nothing). Locked briefly by the callback (a clone) and by the control step (a
    /// replace); never held across a kiln.ui call.
    banner: Mutex<Option<Arc<Banner>>>,
    /// The highest showing a frame callback drew whole (0: none).
    drawn: AtomicU64,
    /// The callback panicked: it draws nothing more, and the control step ends the banner.
    faulted: AtomicBool,
}

/// Ends the window at the end of the scope, also when a call in between panics (ImGui needs `End` after every
/// `Begin`, whatever `Begin` returned).
struct EndGuard(EndFn);
impl Drop for EndGuard {
    fn drop(&mut self) {
        // SAFETY: kiln.ui's end, inside the frame callback that called begin.
        unsafe { (self.0)() };
    }
}

impl Shared {
    /// One frame (render thread, inside kiln.ui's frame callback).
    fn frame(&self, frame: Option<&KilnUiFrame>) {
        if self.faulted.load(Ordering::Acquire) {
            return;
        }
        let Some(banner) = lock(&self.banner).clone() else {
            return;
        };
        match catch_unwind(AssertUnwindSafe(|| self.paint(&banner, frame))) {
            Ok(true) => {
                self.drawn.fetch_max(banner.showing, Ordering::AcqRel);
            }
            Ok(false) => {}
            Err(_) => self.faulted.store(true, Ordering::Release),
        }
    }

    /// Draws the banner: true when its window and every line were drawn.
    fn paint(&self, banner: &Banner, frame: Option<&KilnUiFrame>) -> bool {
        let (width, dpi) = frame
            .filter(|f| f.width > 0.0 && f.dpi_scale > 0.0)
            .map_or((1920.0, 1.0), |f| (f.width, f.dpi_scale));
        let c = &self.calls;
        // SAFETY: kiln.ui's drawing members, inside its frame callback (their one valid place).
        unsafe {
            if let Some(pos) = c.set_next_window_pos {
                pos(
                    width * 0.5,
                    32.0 * dpi,
                    KILN_UI_COND_ALWAYS as u32,
                    0.5,
                    0.0,
                );
            }
            if let Some(size) = c.set_next_window_size {
                // height 0: as tall as the text
                size(
                    (width * 0.9).min(560.0 * dpi),
                    0.0,
                    KILN_UI_COND_ALWAYS as u32,
                );
            }
            if let Some(alpha) = c.set_next_window_bg_alpha {
                alpha(0.92);
            }
            let flags = (KILN_UI_WINDOW_NO_COLLAPSE
                | KILN_UI_WINDOW_NO_RESIZE
                | KILN_UI_WINDOW_NO_MOVE
                | KILN_UI_WINDOW_NO_SCROLLBAR
                | KILN_UI_WINDOW_NO_SAVED_SETTINGS
                | KILN_UI_WINDOW_NO_INPUTS) as u32;
            let open = (c.begin)(kstr(TITLE), core::ptr::null_mut(), flags);
            let _end = EndGuard(c.end);
            if !open {
                return false;
            }
            for line in &banner.lines {
                (c.text_wrapped)(kstr(line));
            }
        }
        true
    }

    fn status(&self) -> Option<KilnUiStatus> {
        // SAFETY: an all-zero record is valid; kiln.ui fills it (any thread).
        let mut status: KilnUiStatus = unsafe { core::mem::zeroed() };
        status.size = core::mem::size_of::<KilnUiStatus>() as u32;
        (unsafe { (self.calls.status)(&mut status) } == KILN_OK).then_some(status)
    }

    fn screen(&self) -> Option<KilnUiScreen> {
        let screen = self.calls.screen?;
        // SAFETY: as for status (any thread).
        let mut out: KilnUiScreen = unsafe { core::mem::zeroed() };
        out.size = core::mem::size_of::<KilnUiScreen>() as u32;
        (unsafe { screen(&mut out) } == KILN_OK && out.available != 0).then_some(out)
    }
}

/// kiln.ui's frame callback. `user` is a [`Shared`] whose strong count the subscription holds.
unsafe extern "C" fn on_frame(frame: *const KilnUiFrame, user: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the subscription keeps `user` alive until kiln.ui's unsubscribe answered KILN_OK; kiln.ui passes its
        // frame record for the call.
        let shared = unsafe { &*user.cast_const().cast::<Shared>() };
        shared.frame(unsafe { frame.as_ref() });
    }));
}

/// The status text without its NUL padding.
fn error_text(status: &KilnUiStatus) -> String {
    let bytes: Vec<u8> = status
        .error
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/// One warning as the banner and kiln.log say it.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Line {
    id: String,
    revision: u64,
    text: String,
    /// Saving now can lose data: a save-sensitive package, or one whose runtime stopped during this start (Kiln.Ui's
    /// `MustNotSave`). Only then does the banner's first line say "Do not save".
    must_not_save: bool,
}

/// `text` cut after `max` characters (at a character boundary), with "..." when cut; NULs as spaces.
fn cut(text: &str, max: usize) -> String {
    let clean = text.replace('\0', " ");
    if clean.chars().count() <= max {
        return clean;
    }
    let mut out: String = clean.chars().take(max).collect();
    out.truncate(out.trim_end().len());
    out.push_str("...");
    out
}

/// The banner's lines for these warnings: as Kiln.Ui's banner, "Do not save: " only when saving can lose data.
fn banner_lines(warnings: &[Line]) -> Vec<String> {
    let n = warnings.len();
    let prefix = if warnings.iter().any(|w| w.must_not_save) {
        "Do not save: "
    } else {
        ""
    };
    let mut lines = vec![if n == 1 {
        format!("{prefix}1 mod is off this start")
    } else {
        format!("{prefix}{n} mods are off this start")
    }];
    lines.extend(
        warnings
            .iter()
            .take(BANNER_PACKAGES)
            .map(|w| cut(&w.text, BANNER_CHARS)),
    );
    if n > BANNER_PACKAGES {
        lines.push(format!("and {} more", n - BANNER_PACKAGES));
    }
    lines.push(HINT.to_owned());
    lines
}

/// Where kiln.ui stands for this process.
enum Ui {
    /// Not looked up yet (nothing needed it).
    Unknown,
    /// No kiln.ui table, or one without the members the banner needs.
    Absent,
    Found(Arc<Shared>),
}

/// One showing: a warning revision on the banner until [`SHOW_FOR`] after its first drawn frame.
#[derive(Clone, Copy)]
struct Showing {
    id: u64,
    revision: u64,
    drawn_at: Option<Instant>,
}

/// The control step's state (control lock).
struct Control {
    ui: Ui,
    /// kiln.ui's handle while subscribed; the subscription holds one strong count of the [`Shared`].
    sub: Option<KilnHandle>,
    demand: bool,
    closed: bool,
    /// The hub's `warningRevision` the warnings were read at, and those warnings (rising revision).
    revision: u64,
    warnings: Vec<Line>,
    /// The first main-menu frame (or game frame, when the screens are unknown).
    first_menu: Option<Instant>,
    in_world: bool,
    /// A world was entered: the next turn starts a new showing.
    reshow: bool,
    showing: Option<Showing>,
    showings: u64,
    /// The highest revision whose showing ended.
    finished: u64,
    /// Kiln.Ui's ready event was seen.
    kiln_ui: bool,
    /// The last delivery each revision was given here (no repeated hub update or log line).
    recorded: Vec<(u64, DeliveryState)>,
    /// (package, revision) pairs a dedicated server logged.
    server_logged: Vec<(String, u64)>,
    /// One-time lines.
    told_subscribe: bool,
    told_unsubscribe: bool,
}

/// kiln-sfi's native do-not-save banner for this process.
pub struct Notice {
    hub: Arc<StateHub>,
    env: Env,
    hooks: NoticeHooks,
    control: Mutex<Control>,
}

impl Notice {
    pub fn new(hub: Arc<StateHub>, env: Env, hooks: NoticeHooks) -> Self {
        Self {
            hub,
            env,
            hooks,
            control: Mutex::new(Control {
                ui: Ui::Unknown,
                sub: None,
                demand: false,
                closed: false,
                revision: 0,
                warnings: Vec::new(),
                first_menu: None,
                in_world: false,
                reshow: false,
                showing: None,
                showings: 0,
                finished: 0,
                kiln_ui: false,
                recorded: Vec::new(),
                server_logged: Vec::new(),
                told_subscribe: false,
                told_unsubscribe: false,
            }),
        }
    }

    /// Runs [`step`](Self::step) on kiln-sfi's IO thread from now on. False when the hub has no IO thread (after its
    /// end, or no thread could be made).
    pub fn start(self: &Arc<Self>) -> bool {
        self.hub.add_io_job(Box::new(Job(Arc::clone(self))))
    }

    /// Subscribed to kiln.ui's frames now (tests, support).
    pub fn subscribed(&self) -> bool {
        lock(&self.control).sub.is_some()
    }
    /// Frame demand set now (tests, support).
    pub fn demanding(&self) -> bool {
        lock(&self.control).demand
    }

    fn log(&self, level: i32, text: &str) {
        // SAFETY: the core's log copies the text during the call (any thread).
        unsafe { (self.env.log)(self.env.owner, level, kstr(text)) };
    }

    /// on_unload (or a test): no subscription and no demand from here on; the step does nothing more. A subscription
    /// kiln.ui did not let go of keeps its count of the render-side state (logged), so no frame can find it freed.
    pub fn close(&self) {
        let mut c = lock(&self.control);
        c.closed = true;
        self.end_showing(&mut c);
    }

    /// No showing: nothing published to draw (a frame kiln.ui still calls draws nothing), no subscription, no demand.
    fn end_showing(&self, c: &mut Control) {
        c.showing = None;
        if let Ui::Found(shared) = &c.ui {
            *lock(&shared.banner) = None;
        }
        self.off(c);
    }

    /// One control turn at `now` (kiln-sfi's IO thread): the wait until the next, or `None` when there is nothing more
    /// to do (closed, or a dedicated server whose record is final and logged).
    pub fn step(&self, now: Instant) -> Option<Duration> {
        let mut c = lock(&self.control);
        if c.closed {
            return None;
        }
        // the record, in memory: the warnings are read again only when the revision moved
        let known = c.revision;
        let Some((side, sealed, revision, fresh)) = self.hub.read(|r| {
            let fresh = (r.warning_revision() != known).then(|| {
                let mut lines: Vec<Line> = r
                    .packages()
                    .iter()
                    .filter_map(|p| {
                        p.warning.as_ref().map(|w| Line {
                            id: p.id.clone(),
                            revision: w.revision,
                            text: w.text.clone(),
                            must_not_save: w.save_sensitive
                                || p.skip.as_ref().is_some_and(|s| s.late),
                        })
                    })
                    .collect();
                lines.sort_by_key(|l| l.revision);
                lines
            });
            (r.identity().side, r.sealed(), r.warning_revision(), fresh)
        }) else {
            return Some(IDLE);
        };
        if let Some(fresh) = fresh {
            c.warnings = fresh;
            c.revision = revision;
        }
        if side == Side::Server {
            self.server(&mut c);
            return (!sealed).then_some(IDLE);
        }
        if c.warnings.is_empty() {
            self.end_showing(&mut c);
            // a final record gets no warning any more
            return (!sealed).then_some(IDLE);
        }
        let shared = match self.ui(&mut c) {
            Some(shared) => shared,
            None => {
                self.not_shown(&mut c, "kiln.ui is not available");
                return Some(IDLE);
            }
        };
        if shared.faulted.load(Ordering::Acquire) {
            self.end_showing(&mut c);
            self.not_shown(&mut c, "the banner's frame callback failed");
            return Some(IDLE);
        }
        let status = shared.status();
        let error = status.as_ref().map(error_text).unwrap_or_default();
        if error.starts_with(OVERLAY_OFF) {
            self.end_showing(&mut c);
            self.not_shown(&mut c, &error);
            return Some(IDLE);
        }
        // the first main-menu frame and entering a world
        let screen = shared.screen();
        let menu = match &screen {
            Some(s) => i32::from(s.ui_state) >= KILN_UI_STATE_PREGAME,
            None => status.as_ref().is_some_and(|s| s.presents > 0),
        };
        if menu && c.first_menu.is_none() {
            c.first_menu = Some(now);
        }
        let in_world = screen
            .as_ref()
            .is_some_and(|s| i32::from(s.ui_state) == KILN_UI_STATE_PLAY);
        if in_world && !c.in_world {
            c.reshow = true;
        }
        c.in_world = in_world;
        if (self.hooks.kiln_ui_ready)(self.env.pid) {
            if !c.kiln_ui {
                c.kiln_ui = true;
                self.log(
                    KILN_LOG_INFO,
                    "kiln-sfi: Kiln.Ui shows the do-not-save notice; kiln-sfi's own banner stays off",
                );
            }
            self.end_showing(&mut c);
            return Some(IDLE);
        }
        let Some(first) = c.first_menu else {
            return Some(TICK);
        };
        if now.saturating_duration_since(first) < self.hooks.grace {
            return Some(TICK);
        }
        // Kiln.Ui is absent: the banner's turn
        let revision = c.revision;
        let renew = match c.showing {
            None => revision > c.finished || c.reshow,
            Some(s) => s.revision != revision || c.reshow,
        };
        if renew {
            c.reshow = false;
            c.showings += 1;
            let id = c.showings;
            c.showing = Some(Showing {
                id,
                revision,
                drawn_at: None,
            });
            *lock(&shared.banner) = Some(Arc::new(Banner {
                showing: id,
                lines: banner_lines(&c.warnings),
            }));
            if self.record(&mut c, revision, DeliveryState::Pending, None) {
                self.log(
                    KILN_LOG_WARN,
                    &format!(
                        "kiln-sfi: kiln-sfi shows its own do-not-save banner (warning revision {revision}): Kiln.Ui did not start within {} s of the main menu",
                        self.hooks.grace.as_secs()
                    ),
                );
                self.log_warnings(&c.warnings, KILN_LOG_WARN);
            }
        }
        let Some(mut showing) = c.showing else {
            self.off(&mut c);
            return Some(IDLE);
        };
        if showing.drawn_at.is_none() && shared.drawn.load(Ordering::Acquire) >= showing.id {
            showing.drawn_at = Some(now);
            c.showing = Some(showing);
            let session = self.world_session();
            if self.record(
                &mut c,
                showing.revision,
                DeliveryState::Shown,
                Some((session, crate::state::now_ms())),
            ) {
                self.log(
                    KILN_LOG_INFO,
                    &format!(
                        "kiln-sfi: the do-not-save banner was shown (warning revision {}{})",
                        showing.revision,
                        session.map_or_else(
                            || ", main menu".to_owned(),
                            |s| format!(", world session {s}")
                        )
                    ),
                );
            }
        }
        if let Some(at) = showing.drawn_at
            && now.saturating_duration_since(at) >= self.hooks.show_for
        {
            c.finished = c.finished.max(showing.revision);
            self.end_showing(&mut c);
            return Some(TICK);
        }
        // the Kiln menu's own fallback draws only with no frame subscriber: step aside while it is open
        if status.is_some_and(|s| s.menu_open != 0) {
            self.off(&mut c);
        } else {
            self.on(&mut c, &shared);
        }
        Some(TICK)
    }

    /// kiln.ui's table, looked up once.
    fn ui(&self, c: &mut Control) -> Option<Arc<Shared>> {
        if let Ui::Unknown = c.ui {
            let name = KILN_UI_SERVICE
                .strip_suffix(b"\0")
                .unwrap_or(KILN_UI_SERVICE);
            let name = KilnStr {
                ptr: name.as_ptr().cast(),
                len: name.len(),
            };
            // SAFETY: the core's lookup; a table it gives is readable for its declared size for the process.
            let table = unsafe { (self.env.service)(name, 1) }.cast::<KilnUiApi>();
            c.ui = match unsafe { Calls::read(table) } {
                Some(calls) => Ui::Found(Arc::new(Shared {
                    calls,
                    banner: Mutex::new(None),
                    drawn: AtomicU64::new(0),
                    faulted: AtomicBool::new(false),
                })),
                None => Ui::Absent,
            };
        }
        match &c.ui {
            Ui::Found(shared) => Some(Arc::clone(shared)),
            _ => None,
        }
    }

    /// Subscribed with demand (control lock held; see the module's notes on why that cannot deadlock).
    fn on(&self, c: &mut Control, shared: &Arc<Shared>) {
        if c.sub.is_none() {
            let user = Arc::into_raw(Arc::clone(shared))
                .cast_mut()
                .cast::<c_void>();
            let mut handle: KilnHandle = 0;
            // SAFETY: kiln.ui's subscribe (any thread); `user` stays valid while the subscription holds its count.
            let result = unsafe {
                (shared.calls.subscribe_frame)(self.env.owner, Some(on_frame), user, &mut handle)
            };
            if result != KILN_OK {
                // no subscription: give the count back
                unsafe { Arc::decrement_strong_count(user.cast_const().cast::<Shared>()) };
                if !c.told_subscribe {
                    c.told_subscribe = true;
                    self.log(
                        KILN_LOG_WARN,
                        &format!("kiln-sfi: kiln.ui refused the do-not-save banner's frame callback ({result})"),
                    );
                }
                return;
            }
            c.sub = Some(handle);
        }
        if !c.demand {
            // SAFETY: kiln.ui's set_demand (any thread; kiln-sfi's own count, summed with the others').
            unsafe { (shared.calls.set_demand)(1) };
            c.demand = true;
        }
    }

    /// No subscription and no demand (control lock held). The subscription's count of the render-side state is given
    /// back only after kiln.ui's unsubscribe answered KILN_OK: then no call is in flight and none comes later.
    fn off(&self, c: &mut Control) {
        let Ui::Found(shared) = &c.ui else {
            return;
        };
        let shared = Arc::clone(shared);
        if c.demand {
            unsafe { (shared.calls.set_demand)(0) };
            c.demand = false;
        }
        let Some(handle) = c.sub else {
            return;
        };
        // SAFETY: kiln.ui's unsubscribe, from a thread that is in no kiln.ui callback: it waits for a call in flight.
        let result = unsafe { (shared.calls.unsubscribe)(handle) };
        match result {
            KILN_OK => {
                c.sub = None;
                // SAFETY: the count `on` took with Arc::into_raw for this subscription.
                unsafe { Arc::decrement_strong_count(Arc::as_ptr(&shared)) };
            }
            _ => {
                // NOT_FOUND (kiln.ui dropped it after a fault) or a refusal: keep the count for good (the state then
                // outlives every frame); a refusal is tried again next turn
                if result == KILN_ERR_NOT_FOUND {
                    c.sub = None;
                }
                if !c.told_unsubscribe {
                    c.told_unsubscribe = true;
                    self.log(
                        KILN_LOG_WARN,
                        &format!("kiln-sfi: kiln.ui's unsubscribe of the do-not-save banner answered {result}"),
                    );
                }
            }
        }
    }

    /// Records `state` for `revision` once (sfi.json's delivery row; `shown`: the world session and the time). True
    /// when this is new here (the caller logs it).
    fn record(
        &self,
        c: &mut Control,
        revision: u64,
        state: DeliveryState,
        shown: Option<(Option<u64>, u64)>,
    ) -> bool {
        let before = c
            .recorded
            .iter()
            .find(|(r, _)| *r == revision)
            .map(|(_, s)| *s);
        let new = match (before, state) {
            (None, _) => true,
            (Some(DeliveryState::Shown), DeliveryState::Shown) => true,
            (Some(DeliveryState::Shown), _) => false,
            (Some(DeliveryState::NotShown), DeliveryState::Pending | DeliveryState::NotShown) => {
                false
            }
            (Some(DeliveryState::Pending), DeliveryState::Pending) => false,
            _ => true,
        };
        if !new {
            return false;
        }
        c.recorded.retain(|(r, _)| *r != revision);
        c.recorded.push((revision, state));
        let (world_session, at_ms) = match shown {
            Some((session, at)) => (session, Some(at)),
            None if state == DeliveryState::NotShown => (None, Some(crate::state::now_ms())),
            None => (None, None),
        };
        self.hub.update(|r| {
            r.set_delivery(Delivery {
                revision,
                state,
                world_session,
                at_ms,
            })
        });
        true
    }

    /// No frame can draw the banner: the current revision is not-shown (unless a frame drew it already), once, loudly.
    fn not_shown(&self, c: &mut Control, why: &str) {
        let revision = c.revision;
        if revision == 0 || !self.record(c, revision, DeliveryState::NotShown, None) {
            return;
        }
        self.log(
            KILN_LOG_ERROR,
            &format!(
                "kiln-sfi: WARNING: the do-not-save notice cannot show in game ({why}); the next Play from the launcher repeats it (warning revision {revision})"
            ),
        );
        self.log_warnings(&c.warnings, KILN_LOG_ERROR);
    }

    fn log_warnings(&self, warnings: &[Line], level: i32) {
        for w in warnings.iter().take(LOG_WARNINGS) {
            self.log(level, &format!("kiln-sfi: WARNING: {}", w.text));
        }
        if warnings.len() > LOG_WARNINGS {
            self.log(
                level,
                &format!(
                    "kiln-sfi: WARNING: and {} more (kiln/state/sfi.json lists every one)",
                    warnings.len() - LOG_WARNINGS
                ),
            );
        }
    }

    /// A dedicated server: each warning once in kiln.log; kiln.ui is never looked up.
    fn server(&self, c: &mut Control) {
        let fresh: Vec<Line> = c
            .warnings
            .iter()
            .filter(|w| {
                !c.server_logged
                    .iter()
                    .any(|(id, r)| *id == w.id && *r == w.revision)
            })
            .cloned()
            .collect();
        for w in fresh {
            self.log(KILN_LOG_ERROR, &format!("kiln-sfi: WARNING: {}", w.text));
            c.server_logged.push((w.id, w.revision));
        }
    }

    /// The world session a showing is in now: the client world's, else the server world's; `None` at the main menu or
    /// when the core cannot say.
    fn world_session(&self) -> Option<u64> {
        let session = self.env.session?;
        // SAFETY: zeroed POD; the core fills it (any thread).
        let mut info: KilnSessionInfo = unsafe { core::mem::zeroed() };
        info.size = core::mem::size_of::<KilnSessionInfo>() as u32;
        if unsafe { session(&mut info) } != KILN_OK {
            return None;
        }
        [KILN_WORLD_CLIENT, KILN_WORLD_SERVER]
            .into_iter()
            .find_map(|kind| {
                let kind = kind as usize;
                (info.worlds[kind] != 0).then(|| u64::from(info.sessions[kind]))
            })
    }
}

/// The control step as a job of kiln-sfi's IO thread.
struct Job(Arc<Notice>);
impl IoJob for Job {
    fn run(&mut self) -> IoNext {
        match self.0.step(Instant::now()) {
            Some(wait) => IoNext::After(wait),
            None => IoNext::Done,
        }
    }
}
