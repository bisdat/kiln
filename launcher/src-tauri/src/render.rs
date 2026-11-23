//! How the window is drawn on Linux: with the GPU (WebKitGTK's default) or without it ("software").
//!
//! A player on an NVIDIA / X11 machine saw only a black window (0.2.0-dev.23): the webview never painted the page, and
//! nothing said so. So:
//! - the page says it started: `app_started` after React's first commit and two frames (launcher/src/shell/started.ts);
//! - software mode: before GTK starts, [`SOFTWARE_ENV`] (a value the user set stays); the webview's hardware
//!   acceleration policy Never and no WebGL (the page draws the CSS mouth instead of the fire: `app_info.noWebgl`);
//! - when the page has not said so [`TIMEOUT`] after the window was made, a start that drew with the GPU by default
//!   writes the marker (`<data>/state/software-render.json`, [`marker`]), logs why and starts itself again in software
//!   mode (`$APPIMAGE` when set, else this executable, with the same arguments). A start in software mode, or with a
//!   mode the user forced, only says so loudly: at most one restart, never a loop;
//! - `KILN_LAUNCHER_RENDER=software|gpu` and `--software-render` / `--gpu-render` win over the marker; Settings' "Draw the
//!   launcher without the GPU" sets or clears it (`app_render_set`), for the next start.
//!
//! WebKit's DMA-BUF renderer (0.2.0-dev.29.19): on by default (WebKitGTK composites on the GPU; up to 29.18 the launcher
//! always set [`DMABUF_VAR`]`=1`, for "Gdk Error 71 (Protocol error)" at the start on GNOME + NVIDIA, and every change
//! was painted again on the CPU with the whole window: its lag in large windows). So:
//! - a start that uses it by default writes `<data>/state/dmabuf-pending.json` ([`dmabuf_pending`]) right before Tauri
//!   ([`watch`]) and removes it [`SETTLE`] after the page said it started, at a clean exit, or before a restart. Left
//!   behind, the next start draws without it, says so loudly and writes the sticky `state/dmabuf-off.json`
//!   ([`dmabuf_marker`], once the beta copy's gate opens: [`after_gate`]);
//! - the watchdog's steps: DMA-BUF on, then the GPU without it ([`Late::NoDmabuf`], [`DMABUF_RESTARTED`]), then software
//!   ([`Late::Restart`]); one restart per step, the restart's variable says its step, never a loop;
//! - the user's own [`DMABUF_VAR`] always wins; software mode keeps it off as before; `--dmabuf` / `--no-dmabuf` and
//!   `KILN_LAUNCHER_DMABUF=on|off` win over the markers; Settings' switch turned off clears the sticky marker too;
//! - a [`DMABUF_VAR`] the launcher set is named in [`SET_BY_US`] like software mode's variables: host programs never get it.
//!
//! X11 on Wayland (0.2.0-dev.30): where WebKit turns hardware acceleration off by itself on a Wayland session with
//! XWayland, one restart through X11 ([`X11_VAR`]`=x11`, [`X11_RESTARTED`]); X11 is remembered ([`x11_marker`], also
//! when WebKit composites nothing there too), a page late through X11 goes back to Wayland first ([`x11_no_help`]),
//! the user's own `GDK_BACKEND` always wins, Settings can turn it off ([`x11_auto_off`]); never a loop.
//!
//! Ubuntu WebKit's NVIDIA block (0.2.0-dev.30.7): the AppImage bundles Ubuntu's WebKitGTK, whose DMA-BUF renderer is off
//! for every NVIDIA GL driver (Debian's disable-nvidia-dmabuf.patch), so it composites nothing there and says nothing.
//! launcher.log names that cause ([`nvidia_block_reason`]); GLib's warnings go into launcher.log from the start
//! ([`glib_line`]); Settings' "Force GPU drawing on NVIDIA" (off by default, [`FORCE_DEFAULT`]) sets [`FORCE_VAR`]
//! with [`SHM_VAR`], and a forced start that dies or is late turns the switch off again ([`force_gave_up`]).
//! docs/kiln/launcher/app.md, "Drawing without the GPU".

use kiln_manage::paths::DataDirs;
use kiln_manage::Error;
use serde::{Deserialize, Serialize};
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

/// How long the page has, from the window's creation, to say it started. A cold start on a slow machine (spinning disk,
/// AppImage mount, WebKit's processes) takes a few seconds; the page's first screen is the splash.
pub const TIMEOUT: Duration = Duration::from_secs(15);
/// When the window is never made (GTK or WebKit hangs before the setup), the watchdog acts this long after the start.
pub const NO_WINDOW_TIMEOUT: Duration = Duration::from_secs(30);

pub const ENV: &str = "KILN_LAUNCHER_RENDER";
pub const ARG_SOFTWARE: &str = "--software-render";
pub const ARG_GPU: &str = "--gpu-render";
/// Set only for the process the fallback starts: it draws without the GPU even when the marker could not be written.
pub(crate) const RESTARTED: &str = "KILN_LAUNCHER_RENDER_RESTARTED";
/// The [`SOFTWARE_ENV`] names this process set itself (comma-separated), so a launcher started from it (the fallback, an
/// update's restart) tells them from the user's own.
/// Programs of the host never get these (kiln_manage::hostenv: a Steam the launcher starts draws with its GPU).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const SET_BY_US: &str = kiln_manage::hostenv::RENDER_SET;
/// Debug and QA builds (`dev-eval`) only: `gpu`, `software` or `all`: `app_started` is ignored in that mode, so the
/// fallback can be tested on a machine where the page renders.
#[cfg(any(debug_assertions, feature = "dev-eval"))]
const TEST_WITHHOLD: &str = "KILN_LAUNCHER_TEST_WITHHOLD_READY";

/// What a start that draws without the GPU because of the marker says, on stderr and in launcher.log.
pub const UNDO_MARKER: &str =
    "it stays so until Settings > Appearance > \"Draw the launcher without the GPU\" is turned off (--gpu-render: one start with the GPU)";

/// What software mode sets before GTK starts.
pub const SOFTWARE_ENV: [(&str, &str); 3] =
    [("WEBKIT_DISABLE_COMPOSITING_MODE", "1"), ("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1"), ("LIBGL_ALWAYS_SOFTWARE", "1")];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Mode {
    Gpu,
    Software,
}

/// Why a start draws the way it does.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Why {
    /// Nothing asked: the GPU.
    Default,
    /// The marker of an earlier start (the fallback, or Settings).
    Marker,
    /// This process is the fallback's restart.
    Restart,
    /// `KILN_LAUNCHER_RENDER`.
    Environment,
    /// `--software-render` / `--gpu-render`.
    Argument,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Choice {
    pub mode: Mode,
    pub why: Why,
    /// A `KILN_LAUNCHER_RENDER` value that is neither mode (said in the log).
    pub ignored: Option<String>,
}

/// Why WebKit's DMA-BUF renderer is on or off in this start.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DmabufWhy {
    /// Nothing asked: on (WebKitGTK's default, with compositing).
    Default,
    /// The user's own `WEBKIT_DISABLE_DMABUF_RENDERER` (any value but "0" is off, as WebKit reads it).
    Yours,
    /// Software mode: off, as before 0.2.0-dev.29.19 (compositing is off there anyway).
    Software,
    /// `--dmabuf` / `--no-dmabuf`.
    Argument,
    /// `KILN_LAUNCHER_DMABUF=on|off`.
    Environment,
    /// This process is the watchdog's first restart (`KILN_LAUNCHER_DMABUF_RESTARTED`).
    Restart,
    /// The start before this one used it and died before its page had started (the pending marker was left).
    Crashed,
    /// The sticky marker (`state/dmabuf-off.json`) of an earlier start.
    Marker,
}

/// WebKit's DMA-BUF renderer in this start.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Dmabuf {
    pub on: bool,
    pub why: DmabufWhy,
    /// A `KILN_LAUNCHER_DMABUF` value that is neither on nor off, or an override something stronger outranked.
    pub ignored: Option<String>,
}

impl Dmabuf {
    /// Only a start that uses it because nothing asked otherwise is guarded (the pending marker) and may step down:
    /// a choice of the user's is theirs.
    pub fn guarded(&self) -> bool {
        self.on && self.why == DmabufWhy::Default
    }
    /// The launcher sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` itself (never over the user's own value).
    pub fn sets_var(&self) -> bool {
        !self.on && self.why != DmabufWhy::Yours
    }
}

/// WebKitGTK's own switch for its DMA-BUF renderer: set (to anything but "0"), WebKit draws without it and without
/// compositing on the GPU.
pub const DMABUF_VAR: &str = "WEBKIT_DISABLE_DMABUF_RENDERER";
pub const DMABUF_ENV: &str = "KILN_LAUNCHER_DMABUF";
pub const ARG_DMABUF: &str = "--dmabuf";
pub const ARG_NO_DMABUF: &str = "--no-dmabuf";
/// Set only for the process the watchdog's first step starts: the GPU without DMA-BUF, even when the marker could not
/// be written; that start is never guarded, so it cannot take this step again.
pub(crate) const DMABUF_RESTARTED: &str = "KILN_LAUNCHER_DMABUF_RESTARTED";
/// How long a guarded start keeps its pending marker after the page said it started: the DMA-BUF failure seen on
/// GNOME with NVIDIA ("Gdk Error 71 (Protocol error)") ends the process as the first frames are shown, well within
/// this; a launcher quit sooner (`RunEvent::Exit`) clears it then.
pub const SETTLE: Duration = Duration::from_secs(10);
/// What a start without DMA-BUF because of its marker (or a start that died) says, on stderr and in launcher.log.
pub const UNDO_DMABUF: &str =
    "it stays so until Settings > Appearance > \"Draw the launcher without the GPU\" is turned on and off again, which tries it again (--dmabuf: one start with it)";
/// Debug and QA builds (`dev-eval`) only: `dmabuf` (a start that uses the DMA-BUF renderer) or `all`: the process
/// ends 2 s after the window was made, without any exit handler, as the Gdk Error 71 crash does.
#[cfg(any(debug_assertions, feature = "dev-eval"))]
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const TEST_EXIT: &str = "KILN_LAUNCHER_TEST_EXIT_AFTER_WINDOW";

/// WebKit's DMA-BUF renderer for this start. Precedence: the user's own `WEBKIT_DISABLE_DMABUF_RENDERER` (`yours`),
/// software mode (off, as before), the arguments (the last of `--dmabuf` / `--no-dmabuf`), `KILN_LAUNCHER_DMABUF`,
/// the watchdog's restart (off), a start that died with it (`crashed`: the pending marker was left; off), the sticky
/// marker (off), then on.
pub fn decide_dmabuf(args: &[String], yours: Option<&str>, env: Option<&str>, software: bool, restarted: bool, crashed: bool, marker: bool) -> Dmabuf {
    let arg = args.iter().skip(1).rev().find_map(|a| match a.as_str() {
        ARG_DMABUF => Some(true),
        ARG_NO_DMABUF => Some(false),
        _ => None,
    });
    let mut ignored = None;
    let env = match env.map(|v| v.trim().to_ascii_lowercase()) {
        Some(v) if v == "on" => Some(true),
        Some(v) if v == "off" => Some(false),
        Some(v) if !v.is_empty() => {
            ignored = Some(format!("{DMABUF_ENV}={v} is neither on nor off: ignored"));
            None
        }
        _ => None,
    };
    let asked = arg.is_some() || env.is_some();
    let (on, why) = match (arg, env) {
        _ if yours.is_some() => (yours == Some("0"), DmabufWhy::Yours),
        _ if software => (false, DmabufWhy::Software),
        (Some(on), _) => (on, DmabufWhy::Argument),
        (None, Some(on)) => (on, DmabufWhy::Environment),
        _ if restarted => (false, DmabufWhy::Restart),
        _ if crashed => (false, DmabufWhy::Crashed),
        _ if marker => (false, DmabufWhy::Marker),
        _ => (true, DmabufWhy::Default),
    };
    if asked && matches!(why, DmabufWhy::Yours | DmabufWhy::Software) {
        let by = if why == DmabufWhy::Yours { format!("your {DMABUF_VAR} decides") } else { "software mode draws without it".into() };
        ignored = Some(format!("{ARG_DMABUF} / {ARG_NO_DMABUF} / {DMABUF_ENV} ignored: {by}"));
    }
    Dmabuf { on, why, ignored }
}

/// `<data>/state/dmabuf-pending.json`: written before Tauri starts in a guarded start, removed once its page has
/// started (plus [`SETTLE`]), at a clean exit, or before a restart. Left behind: that start died.
pub fn dmabuf_pending(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("dmabuf-pending.json")
}

/// `<data>/state/dmabuf-off.json`: there, a start without an override draws without the DMA-BUF renderer.
pub fn dmabuf_marker(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("dmabuf-off.json")
}

/// The sticky marker's sentence for a pending marker left behind: what happened and when.
pub fn dmabuf_crash_why(pending: &Path) -> String {
    match kiln_manage::util::read_json::<Marker>(pending).ok().flatten() {
        Some(m) => format!(
            "The launcher turned this on by itself: its start at {} (Kiln Launcher {}) with WebKit's DMA-BUF renderer ended before its page had started",
            m.at, m.version),
        None => format!("The launcher turned this on by itself: a start with WebKit's DMA-BUF renderer ended before its page had started ({} was left)", pending.display()),
    }
}

// ---------------------------------------------------------------- Ubuntu WebKit's NVIDIA block

/// The AppImage bundles Ubuntu's WebKitGTK (libwebkit2gtk-4.1-0 2.50.4-0ubuntu0.22.04.1 in 0.2.0-dev.30.5), which
/// carries Debian's disable-nvidia-dmabuf.patch: `isNVIDIA()` in AcceleratedBackingStore::rendererBufferTransportMode
/// makes a GL context, and when GL_VENDOR names NVIDIA the DMA-BUF renderer gets no transport at all, so WebKit
/// composites nothing (the webview's policy reads Never) and prints nothing about it. The patch's own way past it is
/// this variable, set (not "0"). A player with an RTX 5080 on Wayland, 0.2.0-dev.30.6.
pub const FORCE_VAR: &str = "WEBKIT_FORCE_DMABUF_RENDERER";
/// WebKit's DMA-BUF renderer with shared-memory buffers only (no GBM buffers between its processes): always set with
/// [`FORCE_VAR`] ([`Force::Shm`]).
pub const SHM_VAR: &str = "WEBKIT_DMABUF_RENDERER_FORCE_SHM";
/// Settings' "Force GPU drawing on NVIDIA" for a player who never set it. Off: real NVIDIA driver bugs (a crash at the
/// start, a black window) sit behind the patch's guard. One constant: the owner decides it before a build.
pub const FORCE_DEFAULT: bool = false;
/// Set only for a start the watchdog made after a forced start's page was late: "off" (nothing forced).
pub(crate) const FORCE_RESTARTED: &str = "KILN_LAUNCHER_FORCE_DMABUF_RESTARTED";
/// The loaded libwebkit2gtk carries the patch when it has this string: upstream WebKit reads no such variable.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const PATCH_MARKER: &[u8] = FORCE_VAR.as_bytes();
/// The library is read in pieces this big when it is searched for [`PATCH_MARKER`].
const MARKER_CHUNK: usize = 1 << 20;

/// What a start forces past the NVIDIA block.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Force {
    /// Nothing: WebKit as it is.
    Off,
    /// [`FORCE_VAR`]=1 with [`SHM_VAR`]=1. The plain forced renderer (shared GBM buffers) is not offered: it can
    /// fail with a Wayland protocol error before the page starts. Shared memory is the variant on X11 too.
    Shm,
}

/// Why a start forces what it does.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ForceWhy {
    /// The switch is off (or was never set and [`FORCE_DEFAULT`] is off).
    Off,
    /// The switch is on.
    On,
    /// The switch is on, but the start before with it failed (its guard was left, or its page was late): nothing is
    /// forced, and the launcher turns the switch off ([`force_gave_up`]).
    Failed,
    /// An earlier normal NVIDIA start never confirmed a visible painted frame: try shared memory once.
    Recovery,
    /// Shared memory was the last working automatic renderer for this installation and display backend.
    Recovered,
    /// The startup guard could not be written: no override is safe to try.
    Unguarded,
    /// The user's own [`FORCE_VAR`] or [`SHM_VAR`]: theirs, never changed.
    Yours,
    /// This start draws without the GPU or without WebKit's DMA-BUF renderer: nothing to force.
    Unused,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ForceDmabuf {
    pub step: Force,
    pub why: ForceWhy,
}

/// The NVIDIA-block step for this start. Precedence: the user's own variables (`yours`), Settings' switch (`chosen`),
/// WebKit's DMA-BUF renderer in this start (`dmabuf_on`, [`decide_dmabuf`]), a forced start before this one that
/// failed (`failed`: its guard was left, or the watchdog's restart says so), then on.
pub fn decide_force(chosen: bool, yours: bool, dmabuf_on: bool, failed: bool) -> ForceDmabuf {
    let (step, why) = match () {
        _ if yours => (Force::Off, ForceWhy::Yours),
        _ if !chosen => (Force::Off, ForceWhy::Off),
        _ if !dmabuf_on => (Force::Off, ForceWhy::Unused),
        _ if failed => (Force::Off, ForceWhy::Failed),
        _ => (Force::Shm, ForceWhy::On),
    };
    ForceDmabuf { step, why }
}

/// The variables a step sets before GTK starts.
pub fn force_env(step: Force) -> Vec<(&'static str, &'static str)> {
    match step {
        Force::Off => vec![],
        Force::Shm => vec![(FORCE_VAR, "1"), (SHM_VAR, "1")],
    }
}

/// The DMA-BUF start guard ([`Dmabuf::guarded`]) is not a forced start's: that one has its own guard, whose failure
/// turns the switch off instead of turning WebKit's DMA-BUF renderer off.
pub fn guarded(d: &Dmabuf, f: &ForceDmabuf) -> bool {
    d.guarded() && f.step == Force::Off
}

/// [`when_late_x11`] with the forced renderer first: the newest change the player asked for goes first.
pub fn when_late_forced(c: &Choice, d: &Dmabuf, x: &X11, f: &ForceDmabuf) -> Late {
    if f.step != Force::Off { Late::NoForce } else { when_late_x11(c, d, x) }
}

/// `<data>/state/force-dmabuf.json`: Settings' switch (absent: [`FORCE_DEFAULT`]).
pub fn force_choice(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("force-dmabuf.json")
}

/// `<data>/state/force-dmabuf-failed.json`: why the launcher turned the switch off by itself; gone when the player sets
/// the switch again.
pub fn force_failed(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("force-dmabuf-failed.json")
}

/// `<data>/state/force-dmabuf-pending.json`: written before any forced renderer variables or WebKit use, removed once
/// a visible frame has settled, at a successful clean exit, or before a normal restart. A failed fallback keeps it
/// until the off preference is durable. Left behind with a visible window expected: that start failed.
pub fn force_pending(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("force-dmabuf-pending.json")
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ForceChoice {
    format: u32,
    on: bool,
    at: String,
    version: String,
}

/// Settings' switch as its file says, or [`FORCE_DEFAULT`] without one.
pub fn force_chosen(path: &Path) -> bool {
    force_setting(path).unwrap_or(FORCE_DEFAULT)
}

/// None differs from an explicit manual off: only an unset preference may recover automatically.
fn force_setting(path: &Path) -> Option<bool> {
    kiln_manage::util::read_json::<ForceChoice>(path).ok().flatten().map(|c| c.on)
}

pub fn normal_pending(d: &DataDirs) -> PathBuf { d.root.join("state/nvidia-frame-pending.json") }
pub fn force_recovered(d: &DataDirs) -> PathBuf { d.root.join("state/force-dmabuf-recovered.json") }

#[derive(Serialize, Deserialize)]
struct StartupGuard {
    #[serde(flatten)]
    marker: Marker,
    context: String,
    visible: bool,
}

/// An installation's stable executable path (not its transient AppImage mount) and the GTK display backend.
#[cfg(target_os = "linux")]
fn startup_context(x11: bool) -> String {
    let install = std::env::var_os("APPIMAGE").map(PathBuf::from).or_else(|| std::env::current_exe().ok()).unwrap_or_default();
    let backend = std::env::var(X11_VAR).unwrap_or_else(|_| if x11 { "x11" } else if std::env::var_os("WAYLAND_DISPLAY").is_some() { "wayland" } else { "x11" }.into());
    format!("{}|{backend}", install.display())
}

/// The attempt and the off preference must survive a process crash: flush the file and its rename, including a newly
/// created state directory's entry in the data root. Failure prevents enabling the override at startup.
fn write_force_state(path: &Path, value: &impl Serialize) -> Result<(), Error> {
    let dir = path.parent().ok_or_else(|| Error::invalid("No renderer state directory."))?;
    std::fs::create_dir_all(dir).map_err(kiln_manage::util::io_at(dir, "creating renderer state"))?;
    #[cfg(unix)]
    if let Some(parent) = dir.parent() {
        std::fs::File::open(parent).and_then(|d| d.sync_all()).map_err(kiln_manage::util::io_at(parent, "flushing renderer state directory"))?;
    }
    let mut text = serde_json::to_vec_pretty(value)?;
    text.push(b'\n');
    kiln_manage::util::write_atomic_durable_exclusive(path, &text)
}

fn write_startup_guard(path: &Path, context: &str, visible: bool, why: &str) -> Result<(), Error> {
    write_force_state(path, &StartupGuard {
        marker: Marker { format: 1, why: why.into(), at: kiln_manage::util::now_rfc3339(), version: env!("CARGO_PKG_VERSION").into() },
        context: context.into(), visible,
    })?;
    Ok(())
}

/// Hidden/minimized starts and a different installation/backend are not failed visible attempts. A legacy or damaged
/// SHM guard still counts, conservatively: it must never cause a persistent manual setting to lock the player out.
fn expected_frame(path: &Path, context: &str) -> bool {
    if !path.is_file() { return false; }
    kiln_manage::util::read_json::<StartupGuard>(path).ok().flatten().is_none_or(|g| g.context == context && g.visible)
}

pub fn write_force_choice(path: &Path, on: bool) -> Result<(), Error> {
    let c = ForceChoice { format: 1, on, at: kiln_manage::util::now_rfc3339(), version: env!("CARGO_PKG_VERSION").into() };
    write_force_state(path, &c)
}

/// The sentence for a forced start's guard that was left behind: what happened and when.
pub fn force_crash_why(pending: &Path) -> String {
    match kiln_manage::util::read_json::<Marker>(pending).ok().flatten() {
        Some(m) => format!(
            "The launcher turned this off by itself: its start at {} (Kiln Launcher {}) with GPU drawing forced on NVIDIA ended before its page had started",
            m.at, m.version),
        None => format!("The launcher turned this off by itself: a start with GPU drawing forced on NVIDIA ended before its page had started ({} was left)",
            pending.display()),
    }
}

/// A forced start failed: the switch goes off and its note says why (Settings shows it), so a setting that ends the
/// launcher at its start can never lock the player out of the setting itself.
pub fn force_gave_up(d: &DataDirs, why: &str) -> Result<(), Error> {
    write_marker_file(&force_failed(d), why)?;
    write_force_choice(&force_choice(d), false)?;
    clear_marker_file(&force_recovered(d))?;
    clear_marker_file(&normal_pending(d))
}

/// What Settings' switch does to the files: the choice, and the note of a failure goes (the player decides again). On also
/// takes away the sticky DMA-BUF marker, without which there is no renderer to force, and the X11 marker, written because
/// WebKit drew without the GPU on Wayland: with the GPU the window stays sharp on Wayland (the X11 switch still acts by
/// itself if WebKit composites nothing).
pub fn force_switch_files(d: &DataDirs, on: bool) -> Result<(), Error> {
    write_force_choice(&force_choice(d), on)?;
    clear_marker_file(&force_failed(d))?;
    clear_marker_file(&force_recovered(d))?;
    clear_marker_file(&normal_pending(d))?;
    if on {
        clear_marker_file(&dmabuf_marker(d))?;
        clear_marker_file(&x11_marker(d))?;
    }
    Ok(())
}

/// The why of WebKit's Never when Ubuntu's NVIDIA block explains it: `patched`, the loaded libwebkit2gtk carries the
/// patch ([`has_marker`]; None: unknown); `vendor`, GL_VENDOR of GTK's GL context (None: not asked); `driver`, NVIDIA's
/// kernel driver is loaded; `forced`, [`FORCE_VAR`] is set (the block is lifted already). None: not this cause.
pub fn nvidia_block_reason(patched: Option<bool>, vendor: Option<&str>, driver: bool, forced: bool) -> Option<String> {
    if forced || patched != Some(true) {
        return None;
    }
    let gl = match vendor {
        Some(v) if v.contains("NVIDIA") => format!("GL vendor {v}"),
        Some(_) => return None,
        None if driver => "NVIDIA's driver is loaded".to_string(),
        None => return None,
    };
    Some(format!(
        "this WebKitGTK is Ubuntu's build (the AppImage bundles it), which turns its DMA-BUF renderer off for every NVIDIA GL \
         driver (Debian's disable-nvidia-dmabuf.patch; {gl}), so WebKit composites nothing. Settings > Advanced > Drawing \
         the launcher > \"Force GPU drawing on NVIDIA\" lifts that block from the next start (off by default: some NVIDIA \
         drivers crash or show a black window with it; the launcher then turns it off by itself)"
    ))
}

/// `marker` occurs in what `r` gives, read in [`MARKER_CHUNK`] pieces (a hit across two pieces counts).
pub fn has_marker(mut r: impl std::io::Read, marker: &[u8]) -> std::io::Result<bool> {
    fn find(h: &[u8], n: &[u8]) -> bool {
        let Some(&first) = n.first() else { return true };
        let mut i = 0;
        while let Some(p) = h.get(i..).and_then(|rest| rest.iter().position(|&b| b == first)) {
            let at = i + p;
            if h.get(at..at + n.len()) == Some(n) {
                return true;
            }
            i = at + 1;
        }
        false
    }
    let keep = marker.len().saturating_sub(1);
    let mut buf = vec![0u8; MARKER_CHUNK + keep];
    let mut have = 0;
    loop {
        let n = r.read(&mut buf[have..])?;
        if n == 0 {
            return Ok(false);
        }
        let end = have + n;
        if find(&buf[..end], marker) {
            return Ok(true);
        }
        let start = end.saturating_sub(keep);
        buf.copy_within(start..end, 0);
        have = end - start;
    }
}

/// The loaded libwebkit2gtk's path and whether it carries the patch ([`PATCH_MARKER`]); None when it cannot be read.
#[cfg(target_os = "linux")]
fn webkit_patched() -> Option<(String, bool)> {
    use std::ffi::{CStr, c_void};
    let f = webkit2gtk::ffi::webkit_get_major_version as *const c_void;
    // SAFETY: dladdr only reads the dynamic loader's tables for the address of a function of a loaded library.
    let path = unsafe {
        let mut info: libc::Dl_info = std::mem::zeroed();
        if libc::dladdr(f, &mut info) == 0 || info.dli_fname.is_null() {
            return None;
        }
        CStr::from_ptr(info.dli_fname).to_string_lossy().into_owned()
    };
    let file = std::fs::File::open(&path).ok()?;
    has_marker(file, PATCH_MARKER).ok().map(|p| (path, p))
}

/// GLib's lines of this start that go into launcher.log at most (WebKitGTK, GTK and GDK say what they found only there).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const GLIB_LINES: usize = 40;
/// A GLib line is cut to this many bytes.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const GLIB_LINE_MAX: usize = 300;
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
static GLIB_SAID: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// A GLib message as launcher.log's line ("<domain> WARNING: <text>", one line, short); None for info and debug.
#[cfg(target_os = "linux")]
pub fn glib_line(domain: Option<&str>, level: gtk::glib::LogLevel, message: &str) -> Option<String> {
    use gtk::glib::LogLevel as L;
    let lvl = match level {
        L::Error => "ERROR",
        L::Critical => "CRITICAL",
        L::Warning => "WARNING",
        L::Message => "MESSAGE",
        L::Info | L::Debug => return None,
    };
    let text: String = message.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    let mut text = text.trim().to_string();
    if text.len() > GLIB_LINE_MAX {
        let mut cut = GLIB_LINE_MAX;
        while !text.is_char_boundary(cut) {
            cut -= 1;
        }
        text.truncate(cut);
        text.push_str("...");
    }
    Some(match domain.filter(|d| !d.is_empty()) {
        Some(d) => format!("{d} {lvl}: {text}"),
        None => format!("{lvl}: {text}"),
    })
}

/// From the start of `main`, before GTK: GLib's warnings (GTK's, GDK's, WebKit's own process) go on to stderr as before
/// and into launcher.log too, [`GLIB_LINES`] at most.
#[cfg(target_os = "linux")]
fn capture_glib() {
    gtk::glib::log_set_default_handler(|domain, level, message| {
        gtk::glib::log_default_handler(domain, level, Some(message));
        if let Some(line) = glib_line(domain, level, message)
            && GLIB_SAID.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < GLIB_LINES
        {
            crate::commands::app::launcher_log("glib", &line);
        }
    });
}

/// GTK's own choice of display server: `x11` on a Wayland session draws through XWayland.
pub const X11_VAR: &str = "GDK_BACKEND";
/// Set only for a start the launcher made itself: "1", the switch to X11 (that start draws through XWayland even when
/// its marker could not be written yet); "0", X11 was tried in this chain of restarts and did not help (no switch again).
pub(crate) const X11_RESTARTED: &str = "KILN_LAUNCHER_X11_RESTARTED";

/// Why this start draws through X11 (XWayland) or not (0.2.0-dev.30, Oryx593 on KDE Wayland + NVIDIA: WebKit turned
/// hardware acceleration off by itself on Wayland, and with GDK_BACKEND=x11 the lag was gone).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum X11Why {
    /// Nothing asked: GTK's default (Wayland); the launcher may switch once if WebKit composites nothing.
    Default,
    /// The user's own `GDK_BACKEND` (any value): theirs, never changed.
    Yours,
    /// Not a Wayland session with XWayland (`WAYLAND_DISPLAY` and `DISPLAY` both set): nothing to switch to.
    Unavailable,
    /// Software mode: draws without the GPU anyway, X11 would change nothing.
    Software,
    /// Settings: the automatic switch is off.
    Off,
    /// This process is the switch's restart (`KILN_LAUNCHER_X11_RESTARTED=1`).
    Restart,
    /// The marker of an earlier start where X11 helped (`state/x11.json`).
    Marker,
    /// X11 was tried and did not help (`state/x11-no-help.json`, or `KILN_LAUNCHER_X11_RESTARTED=0`).
    Tried,
}

/// X11 in this start: `on` means the launcher sets `GDK_BACKEND=x11` itself.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct X11 {
    pub on: bool,
    pub why: X11Why,
}

/// X11 for this start, before GTK. Precedence: the user's own `GDK_BACKEND` (`yours`, after the launcher's own
/// variables were taken back), no Wayland session with XWayland, software mode, the restart that said X11 did not help
/// (`restarted` "0"), Settings' switch off, the switch's restart ("1"), the marker that X11 did not help, the marker
/// that it did, then GTK's default.
#[allow(clippy::too_many_arguments)]
pub fn decide_x11(yours: Option<&str>, wayland: bool, display: bool, software: bool, off: bool, restarted: Option<&str>,
    marker: bool, no_help: bool) -> X11 {
    let why = match restarted {
        _ if yours.is_some() => X11Why::Yours,
        _ if !wayland || !display => X11Why::Unavailable,
        _ if software => X11Why::Software,
        Some("0") => X11Why::Tried,
        _ if off => X11Why::Off,
        Some("1") => X11Why::Restart,
        _ if no_help => X11Why::Tried,
        _ if marker => X11Why::Marker,
        _ => X11Why::Default,
    };
    X11 { on: matches!(why, X11Why::Restart | X11Why::Marker), why }
}

/// What a start does about X11 once it knows WebKit's hardware acceleration (the webview's policy, [`window_created`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum X11Step {
    /// Wayland, and WebKit composites nothing: start again once through X11.
    Switch,
    /// The switch's X11 start: remember it (`state/x11.json`), later starts go straight to X11. Also when WebKit still
    /// composites nothing there (work-cm 2026-11-20: Oryx593's lag was gone under GDK_BACKEND=x11 although the torch
    /// stood still, so X11's painting on the CPU was far better than Wayland's); only a page late through X11 goes back.
    Keep,
    /// Nothing to do.
    Nothing,
}

/// `off`: WebKit turned hardware acceleration off by itself. `by_vars`: a variable explains it
/// (`WEBKIT_DISABLE_COMPOSITING_MODE` / `WEBKIT_DISABLE_DMABUF_RENDERER`): X11 would not change that, so no switch.
/// `scale`: the screens' largest GDK scale factor. X11 does not bring the GPU back (Oryx593's X11 starts: policy Never,
/// "WebKit composites: no" too); it is faster only because XWayland draws a HiDPI window at scale 1 (1024 x 640 at
/// devicePixelRatio 1 against 1280 x 800 at 2: p50 16 ms against 43-73 ms a frame on Play), so only scale > 1 switches:
/// at scale 1 it would paint as many pixels on the CPU and stick for nothing.
pub fn x11_step(x: &X11, off: bool, by_vars: bool, scale: i32) -> X11Step {
    match x.why {
        X11Why::Default if off && !by_vars && scale > 1 => X11Step::Switch,
        X11Why::Restart => X11Step::Keep,
        _ => X11Step::Nothing,
    }
}

/// `<data>/state/x11.json`: X11 helped in an earlier start; a Wayland start goes straight to X11.
pub fn x11_marker(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("x11.json")
}

/// `<data>/state/x11-no-help.json`: X11 was tried and did not help; no automatic switch while it is there.
pub fn x11_no_help(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("x11-no-help.json")
}

/// `<data>/state/x11-auto-off.json`: Settings turned the automatic switch off.
pub fn x11_auto_off(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("x11-auto-off.json")
}

/// What a start does when the page has not said it started in time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Late {
    /// This start forces WebKit's DMA-BUF renderer past Ubuntu's NVIDIA block: note the step as failed and start again
    /// with the next one (shared memory, then nothing forced).
    NoForce,
    /// This start draws through X11 (the launcher's switch): X11 made it worse; start again on Wayland, X11 marked as no help.
    NoX11,
    /// Write the DMA-BUF marker and start again with the GPU, without the DMA-BUF renderer.
    NoDmabuf,
    /// Write the marker and start again without the GPU.
    Restart,
    /// Say so loudly and go on (software mode already, or a mode the user forced).
    Report,
}

/// The mode for this start. Precedence: the arguments (the last of `--software-render` / `--gpu-render`), then
/// `KILN_LAUNCHER_RENDER`, then the fallback's restart, then the marker, then the GPU.
pub fn decide(args: &[String], env: Option<&str>, restarted: bool, marker: bool) -> Choice {
    let arg = args.iter().skip(1).rev().find_map(|a| match a.as_str() {
        ARG_SOFTWARE => Some(Mode::Software),
        ARG_GPU => Some(Mode::Gpu),
        _ => None,
    });
    let mut ignored = None;
    let env = match env.map(|v| v.trim().to_ascii_lowercase()) {
        Some(v) if v == "software" => Some(Mode::Software),
        Some(v) if v == "gpu" => Some(Mode::Gpu),
        Some(v) if !v.is_empty() => {
            ignored = Some(format!("{ENV}={v} is neither software nor gpu: ignored"));
            None
        }
        _ => None,
    };
    let (mode, why) = match (arg, env) {
        (Some(m), _) => (m, Why::Argument),
        (None, Some(m)) => (m, Why::Environment),
        _ if restarted => (Mode::Software, Why::Restart),
        _ if marker => (Mode::Software, Why::Marker),
        _ => (Mode::Gpu, Why::Default),
    };
    Choice { mode, why, ignored }
}

/// Only a start that drew with the GPU because nothing asked otherwise falls back: software mode has nowhere to go
/// (no loop), and a mode the user forced is theirs.
pub fn when_late(c: &Choice, d: &Dmabuf) -> Late {
    match (c.mode, c.why) {
        (Mode::Gpu, Why::Default) if d.guarded() => Late::NoDmabuf,
        (Mode::Gpu, Why::Default) => Late::Restart,
        _ => Late::Report,
    }
}

/// [`when_late`] with X11 first: a start the launcher switched to X11 goes back to Wayland before any other step (the
/// newest change goes first); that start then takes the steps [`when_late`] says, never X11 again.
pub fn when_late_x11(c: &Choice, d: &Dmabuf, x: &X11) -> Late {
    if x.on { Late::NoX11 } else { when_late(c, d) }
}

/// What the fallback starts: the AppImage file when `$APPIMAGE` is set (the running binary sits in the AppImage's
/// mount, which goes away with this process), else this executable. `true`: an AppImage.
pub fn restart_target(appimage: Option<OsString>, exe: Option<PathBuf>) -> Option<(PathBuf, bool)> {
    match appimage.filter(|a| !a.is_empty()) {
        Some(a) => Some((PathBuf::from(a), true)),
        None => exe.map(|e| (e, false)),
    }
}

/// What a restart changes for the new start: the arguments it leaves out and the variables it removes (a choice a
/// click replaces), and the variables it sets (why the new start is what it is).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Relaunch {
    pub drop_args: Vec<&'static str>,
    pub unset: Vec<&'static str>,
    pub set: Vec<(&'static str, String)>,
}

impl Relaunch {
    /// The fallback's restart: the same arguments, [`RESTARTED`].
    pub fn software_fallback() -> Relaunch {
        Relaunch { set: vec![(RESTARTED, "1".into())], ..Default::default() }
    }
    /// The watchdog's first step: the same arguments, [`DMABUF_RESTARTED`] (the GPU without the DMA-BUF renderer).
    pub fn dmabuf_fallback() -> Relaunch {
        Relaunch { set: vec![(DMABUF_RESTARTED, "1".into())], ..Default::default() }
    }
    /// The switch to X11: the same arguments, [`X11_RESTARTED`]=1 (the new start sets `GDK_BACKEND=x11` itself).
    pub fn x11_switch() -> Relaunch {
        Relaunch { set: vec![(X11_RESTARTED, "1".into())], ..Default::default() }
    }
    /// A forced start failed: [`FORCE_RESTARTED`]=off (the new start forces nothing even before the switch is written off).
    pub fn force_fallback() -> Relaunch {
        Relaunch { set: vec![(FORCE_RESTARTED, "off".into())], ..Default::default() }
    }
    /// Back from X11 to Wayland: [`X11_RESTARTED`]=0 (X11 is not tried again in this chain), without our `GDK_BACKEND`.
    pub fn x11_back() -> Relaunch {
        Relaunch { unset: vec![X11_VAR], set: vec![(X11_RESTARTED, "0".into())], ..Default::default() }
    }
    /// The player clicked "Start without the GPU" (crate::sandbox's message): their click wins over a GPU they asked
    /// for with an argument or `KILN_LAUNCHER_RENDER`, for this restart.
    pub fn software_by_click() -> Relaunch {
        Relaunch { drop_args: vec![ARG_SOFTWARE, ARG_GPU], unset: vec![ENV], set: vec![(RESTARTED, "1".into())] }
    }
    /// What this start's render and sandbox choices took from a restart's variable (their markers may not have been
    /// written) goes along to the next restart, whatever it is for.
    pub fn keeping(mut self) -> Relaunch {
        if choice().is_some_and(|c| c.why == Why::Restart) {
            self.add(RESTARTED, "1");
        }
        if dmabuf().is_some_and(|d| d.why == DmabufWhy::Restart) {
            self.add(DMABUF_RESTARTED, "1");
        }
        match x11().map(|x| x.why) {
            Some(X11Why::Restart) => self.add(X11_RESTARTED, "1"),
            Some(X11Why::Tried) => self.add(X11_RESTARTED, "0"),
            _ => {}
        }
        if let Some(v) = FORCE_RESTARTED_SEEN.get() {
            self.add(FORCE_RESTARTED, v);
        }
        crate::sandbox::keep(&mut self);
        self
    }

    /// Sets `k` unless this restart already sets or removes it.
    pub fn add(&mut self, k: &'static str, v: &str) {
        if !self.set.iter().any(|(x, _)| *x == k) && !self.unset.contains(&k) {
            self.set.push((k, v.into()));
        }
    }
}

/// The restart: `target` with this start's arguments (without argv[0]) as `how` says. An AppImage is a program of the
/// host: it gets `vars` (this process's environment) without the running image's entries, as a fresh start does.
pub fn restart_command<I>(target: &Path, appimage: bool, args: &[OsString], vars: I, how: &Relaunch) -> Command
where
    I: IntoIterator<Item = (OsString, OsString)>,
{
    let mut cmd = Command::new(target);
    if appimage {
        kiln_manage::hostenv::clean_with(&mut cmd, vars);
    }
    cmd.args(args.iter().filter(|a| !how.drop_args.iter().any(|d| a.as_os_str() == std::ffi::OsStr::new(d))));
    for k in &how.unset {
        cmd.env_remove(k);
    }
    for (k, v) in &how.set {
        cmd.env(k, v);
    }
    cmd
}

/// Which of [`SOFTWARE_ENV`] software mode sets: those the environment (`get`) does not have.
pub fn software_env(get: impl Fn(&str) -> Option<OsString>) -> Vec<(&'static str, &'static str)> {
    SOFTWARE_ENV.iter().copied().filter(|(k, _)| get(k).is_none()).collect()
}

/// The names in a [`SET_BY_US`] value that are ours to take back (only [`SOFTWARE_ENV`] names, [`DMABUF_VAR`],
/// [`X11_VAR`], [`FORCE_VAR`] and [`SHM_VAR`], never anything else).
pub fn set_by_us(list: Option<&str>) -> Vec<&'static str> {
    let list = list.unwrap_or_default();
    SOFTWARE_ENV.iter().map(|(k, _)| *k).chain([DMABUF_VAR, X11_VAR, FORCE_VAR, SHM_VAR])
        .filter(|k| list.split(',').any(|x| x.trim() == *k)).collect()
}

// ---------------------------------------------------------------- the marker

/// `<data>/state/software-render.json`: there, a start without an override draws without the GPU.
pub fn marker(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("software-render.json")
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Marker {
    format: u32,
    /// Who set it and why, in a sentence.
    why: String,
    at: String,
    version: String,
}

pub fn write_marker(d: &DataDirs, why: &str) -> Result<(), Error> {
    write_marker_file(&marker(d), why)
}

pub fn clear_marker(d: &DataDirs) -> Result<(), Error> {
    clear_marker_file(&marker(d))
}

/// The marker's sentence (None: no marker, or not readable as one).
fn marker_why(d: &DataDirs) -> Option<String> {
    marker_file_why(&marker(d))
}

/// A start's marker file (`{format, why, at, version}`): this one, and crate::sandbox's `state/webkit-sandbox.json`.
pub(crate) fn write_marker_file(path: &Path, why: &str) -> Result<(), Error> {
    let m = Marker { format: 1, why: why.into(), at: kiln_manage::util::now_rfc3339(), version: env!("CARGO_PKG_VERSION").into() };
    kiln_manage::util::write_json_atomic(path, &m)?;
    Ok(())
}

pub(crate) fn clear_marker_file(path: &Path) -> Result<(), Error> {
    match std::fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(Error::from(e).with_detail(path.display().to_string())),
        _ => Ok(()),
    }
}

/// "<why> (<at>)" of a marker file (None: none, or not readable as one).
pub(crate) fn marker_file_why(path: &Path) -> Option<String> {
    kiln_manage::util::read_json::<Marker>(path).ok().flatten().map(|m| format!("{} ({})", m.why, m.at))
}

// ---------------------------------------------------------------- the watchdog

/// The page's ready call and the window's creation, as the watchdog sees them.
pub struct Watch {
    state: Mutex<WatchState>,
    cv: Condvar,
}

#[derive(Default)]
struct WatchState {
    started: Option<Instant>,
    window: Option<Instant>,
    painted: bool,
}

/// What the watchdog saw.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Seen {
    /// The page said it started, this long after the window was made (or after the watchdog began, without one).
    Started(Duration),
    /// Time ran out; `window`: the window had been made.
    Late { window: bool },
}

impl Watch {
    pub const fn new() -> Watch {
        Watch { state: Mutex::new(WatchState { started: None, window: None, painted: false }), cv: Condvar::new() }
    }
    pub fn window_created(&self) {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        s.window.get_or_insert_with(Instant::now);
        self.cv.notify_all();
    }
    /// The page's ready call; true the first time.
    pub fn page_started(&self) -> bool {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let first = s.started.is_none();
        s.started.get_or_insert_with(Instant::now);
        self.cv.notify_all();
        first
    }
    /// The page has said it started.
    pub fn has_started(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).started.is_some()
    }
    pub fn page_painted(&self) -> bool {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let first = !s.painted;
        s.painted = true;
        first
    }
    pub fn has_painted(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).painted
    }
    /// Waits for the ready call: `timeout` from the window's creation, or `no_window` from `begun` while there is none.
    pub fn wait(&self, begun: Instant, timeout: Duration, no_window: Duration) -> Seen {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if let Some(t) = s.started {
                return Seen::Started(t.saturating_duration_since(s.window.unwrap_or(begun)));
            }
            let due = match s.window { Some(w) => w + timeout, None => begun + no_window };
            let now = Instant::now();
            if now >= due {
                return Seen::Late { window: s.window.is_some() };
            }
            s = self.cv.wait_timeout(s, due - now).unwrap_or_else(|e| e.into_inner()).0;
        }
    }
    /// After [`Seen::Late`]: waits (without end) for a late ready call and how long after the window it came.
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    fn wait_late(&self, begun: Instant) -> Duration {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if let Some(t) = s.started {
                return t.saturating_duration_since(s.window.unwrap_or(begun));
            }
            s = self.cv.wait(s).unwrap_or_else(|e| e.into_inner());
        }
    }
}

impl Default for Watch {
    fn default() -> Self { Watch::new() }
}

static WATCH: Watch = Watch::new();
static CHOICE: OnceLock<Choice> = OnceLock::new();
static DMABUF: OnceLock<Dmabuf> = OnceLock::new();
static X11_CHOICE: OnceLock<X11> = OnceLock::new();
/// What X11 taught this start, for the data folder: written once the beta copy's gate is open (`.0`), whichever
/// comes first ([`x11_note`]).
static X11_NOTE: Mutex<(bool, Option<X11Note>)> = Mutex::new((false, None));

#[derive(Debug, Clone)]
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
enum X11Note {
    /// X11 helped: `state/x11.json`, the no-help marker goes.
    Helped,
    /// X11 did not help (the sentence): `state/x11-no-help.json`, the X11 marker goes.
    NoHelp(String),
}
/// The pending marker this start wrote (a guarded start, [`watch`]); removed at most once.
static PENDING: Mutex<Option<PathBuf>> = Mutex::new(None);
/// This start's step past Ubuntu WebKit's NVIDIA block ([`prepare`]).
static FORCE: OnceLock<ForceDmabuf> = OnceLock::new();
/// The guard a forced start wrote before setting its variables ([`prepare`]); removed at most once.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
static FORCE_PENDING: Mutex<Option<PathBuf>> = Mutex::new(None);
/// A forced start's guard an earlier start left: its path and the note's sentence, for [`after_gate`].
static FORCE_LEFT: OnceLock<(PathBuf, String)> = OnceLock::new();
/// Why this start could not arm its guard: the switch goes off once the data folder opens.
static FORCE_GUARD_ERROR: OnceLock<String> = OnceLock::new();
static RENDER_CONTEXT: OnceLock<String> = OnceLock::new();
static NORMAL_PENDING: Mutex<Option<PathBuf>> = Mutex::new(None);
/// The [`FORCE_RESTARTED`] value this start was given: a later restart keeps it ([`Relaunch::keeping`]).
static FORCE_RESTARTED_SEEN: OnceLock<String> = OnceLock::new();
/// This start found Ubuntu WebKit's NVIDIA block behind WebKit's Never ([`window_created`]).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
static NVIDIA_BLOCK: OnceLock<bool> = OnceLock::new();
/// A pending marker an earlier start left: its path and the sticky marker's sentence, for [`after_gate`].
static LEFT: OnceLock<(PathBuf, String)> = OnceLock::new();

/// This start's choice (Linux, after [`prepare`]).
pub fn choice() -> Option<&'static Choice> {
    CHOICE.get()
}

/// This start's DMA-BUF renderer (Linux, after [`prepare`]).
pub fn dmabuf() -> Option<&'static Dmabuf> {
    DMABUF.get()
}

/// This start's X11 choice (Linux, after [`prepare`]).
pub fn x11() -> Option<&'static X11> {
    X11_CHOICE.get()
}

/// This start's step past Ubuntu WebKit's NVIDIA block (Linux, after [`prepare`]).
pub fn force() -> Option<&'static ForceDmabuf> {
    FORCE.get()
}

/// Removes the guard this forced start wrote (the page started and settled, a clean exit, a restart).
fn clear_force_pending(when: &str) {
    let Some(p) = FORCE_PENDING.lock().unwrap_or_else(|e| e.into_inner()).take() else { return };
    match clear_marker_file(&p) {
        Ok(()) => log(&format!("forced DMA-BUF start guard cleared ({when}): {}", p.display())),
        Err(e) => log(&format!("could not remove {} ({when}): {}", p.display(), e.message)),
    }
}

/// Keeps `note` for the data folder and writes the newest one once the gate is open (`gate`: it opened now).
fn x11_note(note: Option<X11Note>, gate: bool) {
    let mut g = X11_NOTE.lock().unwrap_or_else(|e| e.into_inner());
    g.0 |= gate;
    if note.is_some() {
        g.1 = note;
    }
    if !g.0 {
        return;
    }
    let Some(note) = g.1.take() else { return };
    drop(g);
    let Ok(dirs) = crate::state::dirs() else { return };
    let (write, clear, why) = match note {
        X11Note::Helped => (x11_marker(&dirs), x11_no_help(&dirs),
            "The launcher turned this on by itself: WebKit drew without the GPU on Wayland, so it switched to X11 (XWayland)".to_string()),
        X11Note::NoHelp(why) => (x11_no_help(&dirs), x11_marker(&dirs), why),
    };
    match write_marker_file(&write, &why) {
        Ok(()) => log(&format!("X11: wrote {} ({why})", write.display())),
        Err(e) => log(&format!("X11: could not write {}: {}", write.display(), e.message)),
    }
    if let Err(e) = clear_marker_file(&clear) {
        log(&format!("X11: could not remove {}: {}", clear.display(), e.message));
    }
}

/// Removes the pending marker this start wrote (the page started and settled, a clean exit, a restart).
fn clear_pending(when: &str) {
    let Some(p) = PENDING.lock().unwrap_or_else(|e| e.into_inner()).take() else { return };
    match clear_marker_file(&p) {
        Ok(()) => log(&format!("DMA-BUF start guard cleared ({when}): {}", p.display())),
        Err(e) => log(&format!("could not remove {} ({when}): {}", p.display(), e.message)),
    }
}

/// After the beta copy's gate opened (lib.rs `after_snapshot`): the sticky DMA-BUF marker for a pending marker an
/// earlier start left (then that one goes), or for this start's watchdog restart when its marker could not be written
/// then. Before the gate a start only reads the data folder (and writes its own pending marker).
pub fn after_gate() {
    x11_note(None, true);
    force_after_gate();
    if WATCH.has_painted() { finish_frame("the data folder opened after a visible frame"); }
    let Some(d) = dmabuf() else { return };
    let Ok(dirs) = crate::state::dirs() else { return };
    let off = dmabuf_marker(&dirs);
    if let Some((pending, why)) = LEFT.get() {
        if !off.is_file() {
            match write_marker_file(&off, why) {
                Ok(()) => log(&format!("wrote {} ({why})", off.display())),
                // the pending marker stays: the next start finds it again
                Err(e) => { log(&format!("could not write {}: {}", off.display(), e.message)); return; }
            }
        }
        if let Err(e) = clear_marker_file(pending) {
            log(&format!("could not remove {}: {}", pending.display(), e.message));
        }
    } else if d.why == DmabufWhy::Restart && !off.is_file() {
        let why = "The launcher turned this on by itself: the page did not start in time with WebKit's DMA-BUF renderer (the start before this one)";
        match write_marker_file(&off, why) {
            Ok(()) => log(&format!("wrote {} (the watchdog's restart)", off.display())),
            Err(e) => log(&format!("could not write {}: {}", off.display(), e.message)),
        }
    }
}

/// After the gate: a forced start's guard an earlier start left, or this start's restart after a late page, turns the
/// switch off with its note ([`force_gave_up`]; then the guard goes).
fn force_after_gate() {
    let Ok(dirs) = crate::state::dirs() else { return };
    let why = match (FORCE_LEFT.get(), FORCE_GUARD_ERROR.get(), FORCE_RESTARTED_SEEN.get()) {
        (Some((_, why)), _, _) => why.clone(),
        (_, Some(why), _) => why.clone(),
        (None, None, Some(_)) if !force_failed(&dirs).is_file() =>
            "The launcher turned this off by itself: the page did not start in time with GPU drawing forced on NVIDIA".into(),
        _ => return,
    };
    match force_gave_up(&dirs, &why) {
        Ok(()) => log(&format!("Settings' \"Force GPU drawing on NVIDIA\" is off now: wrote {} ({why})", force_failed(&dirs).display())),
        // the guard stays: the next start finds it again
        Err(e) => { log(&format!("could not turn \"Force GPU drawing on NVIDIA\" off: {}", e.message)); return; }
    }
    if let Some((p, _)) = FORCE_LEFT.get()
        && let Err(e) = clear_marker_file(p)
    {
        log(&format!("could not remove {}: {}", p.display(), e.message));
    }
}

/// `RunEvent::Exit`: a clean exit after the page started is no crash.
pub fn leaving() {
    if WATCH.has_started() {
        clear_pending("the launcher quit");
    }
    if WATCH.has_painted() { finish_frame("the launcher quit"); }
}

/// Page navigation/readiness is not proof of painting. Visibility updates suspend the failure breadcrumb for an
/// intentionally hidden page; the frontend acknowledges painting only after two visible animation frames.
pub fn frame_painted(painted: bool, visible: bool) -> Option<String> {
    choice()?; // Only Linux has renderer startup guards.
    if let Some(context) = RENDER_CONTEXT.get() {
        for pending in [&NORMAL_PENDING, &FORCE_PENDING] {
            let pending = pending.lock().unwrap_or_else(|e| e.into_inner());
            if let Some(p) = pending.as_ref()
                && let Err(e) = write_startup_guard(p, context, visible, "Waiting for a confirmed visible frame")
            { log(&format!("could not update startup visibility: {}", e.message)); }
        }
    }
    if !painted || !visible || !WATCH.page_painted() { return None; }
    WATCH.page_started();
    std::thread::Builder::new().name("kiln-render-settle".into()).spawn(|| {
        std::thread::sleep(SETTLE);
        clear_pending("a visible frame settled");
        finish_frame("a visible frame settled");
    }).ok();
    force().filter(|f| f.why == ForceWhy::Recovery).map(|_| {
        "The previous normal NVIDIA startup never confirmed a visible frame. Kiln is trying shared-memory GPU drawing; if this attempt fails, it will turn that recovery off.".into()
    })
}

fn finish_frame(when: &str) {
    let Ok(dirs) = crate::state::dirs() else { return };
    if force().is_some_and(|f| matches!(f.why, ForceWhy::Recovery | ForceWhy::Recovered))
        && force_setting(&force_choice(&dirs)).is_none()
        && let Some(context) = RENDER_CONTEXT.get()
        && let Err(e) = write_startup_guard(&force_recovered(&dirs), context, true, "Shared-memory GPU drawing confirmed a visible frame")
    {
        log(&format!("could not remember the working automatic renderer: {}", e.message));
        return;
    }
    let mut normal = NORMAL_PENDING.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(p) = normal.as_ref() {
        match clear_marker_file(p) {
            Ok(()) => { normal.take(); }
            Err(e) => log(&format!("could not clear normal startup guard: {}", e.message)),
        }
    }
    clear_force_pending(when);
}

// ---------------------------------------------------------------- does WebKit composite (inbox 94)

/// Where WebKitGTK composites nothing although the launcher draws with the GPU (inbox 94, an RTX 5080 on KDE Wayland:
/// "WebKit's DMA-BUF renderer on" in launcher.log, yet the page answered (-webkit-transform-3d) with false, scrolling and
/// page changes lagged and the torch stood still). WebKitGTK 2.50 turns hardware acceleration off for the whole process
/// by itself, before any page, when one of its checks fails (HardwareAccelerationManager, AcceleratedBackingStore::
/// checkRequirements): then the webview's policy reads Never and every change is painted again on the CPU. Its only
/// trace is a GLib warning on stderr, so launcher.log names the checks here, the same ones WebKit makes, in its order:
/// - `WEBKIT_DISABLE_COMPOSITING_MODE` set (not "0");
/// - `WEBKIT_DISABLE_DMABUF_RENDERER` set (not "0"): no way to hand frames over, so no compositing at all;
/// - EGL's client extensions without `EGL_KHR_platform_gbm` and `EGL_MESA_platform_surfaceless` (on an NVIDIA-only
///   system the egl-gbm library provides the first);
/// - GTK 3 cannot make a GL context for a window (`gdk_window_create_gl_context`: GTK's own EGL on Wayland, GLX on X11).
///
/// `gtk_gl` is None when it was not tried. Empty: none of them failed here.
pub fn webkit_off_reasons(no_compositing: Option<&str>, no_dmabuf: Option<&str>, egl_client: Option<&str>, gtk_gl: Option<Result<(), String>>) -> Vec<String> {
    let set = |v: Option<&str>| v.filter(|v| *v != "0").map(str::to_string);
    let mut out = Vec::new();
    if let Some(v) = set(no_compositing) {
        out.push(format!("WEBKIT_DISABLE_COMPOSITING_MODE={v}"));
    }
    if let Some(v) = set(no_dmabuf) {
        out.push(format!("{DMABUF_VAR}={v} (WebKitGTK then has no way to hand composited frames over)"));
    }
    match egl_client {
        None => out.push("EGL answers no client extensions (libEGL.so.1 missing, or it predates EGL_EXT_client_extensions)".into()),
        Some(ext) => {
            let has = |e: &str| ext.split_ascii_whitespace().any(|x| x == e);
            if !has("EGL_KHR_platform_gbm") && !has("EGL_MESA_platform_surfaceless") {
                out.push("EGL offers neither EGL_KHR_platform_gbm nor EGL_MESA_platform_surfaceless (NVIDIA: is egl-gbm installed? Mesa's EGL gives the second)".into());
            }
        }
    }
    if let Some(Err(e)) = gtk_gl {
        out.push(format!("GTK could not make a GL context: {}", e.trim()));
    }
    out
}

/// What the page measured about its drawing (`app_render_report`, launcher/src/shell/renderReport.ts): the facts once
/// after its start, the frame sample once on Play. Every field is optional: each call sends what it has.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PageRender {
    /// `(-webkit-transform-3d)`: WebKit composites (the frontend's flatDrawing is its negation).
    #[serde(default)]
    pub composited: Option<bool>,
    /// WebGL's version and renderer, or "none".
    #[serde(default)]
    pub webgl: Option<String>,
    #[serde(default)]
    pub dpr: Option<f64>,
    /// The window's CSS size.
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub height: Option<u32>,
    #[serde(default)]
    pub reduced_motion: Option<bool>,
    /// The fire burns (WebGL, Animated fire on).
    #[serde(default)]
    pub fire: Option<bool>,
    #[serde(default)]
    pub frames: Option<FrameSample>,
}

/// requestAnimationFrame over a stretch of Play: how often the page got a frame.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FrameSample {
    pub ms: f64,
    pub frames: u32,
    pub p50: f64,
    pub p95: f64,
    pub max: f64,
    /// Frames that came more than 50 ms after the one before.
    pub slow: u32,
}

/// The page's line in launcher.log ("the page: ..."); a short, plain text whatever the page sent.
pub fn says_page(r: &PageRender) -> String {
    let yn = |b: bool| if b { "yes" } else { "no" };
    let mut parts = Vec::new();
    if let Some(c) = r.composited {
        parts.push(if c { "WebKit composites: yes".to_string() } else {
            "WebKit composites: no ((-webkit-transform-3d) is false: every change is painted again on the CPU; the page's flat mode)".to_string()
        });
    }
    if let Some(w) = &r.webgl {
        let w: String = w.chars().filter(|c| !c.is_control()).take(160).collect();
        parts.push(format!("WebGL: {w}"));
    }
    if let (Some(w), Some(h)) = (r.width, r.height) {
        parts.push(format!("window {w} x {h} CSS px"));
    }
    if let Some(d) = r.dpr.filter(|d| d.is_finite()) {
        parts.push(format!("devicePixelRatio {d}"));
    }
    if let Some(m) = r.reduced_motion {
        parts.push(format!("prefers-reduced-motion: {}", yn(m)));
    }
    if let Some(f) = r.fire {
        parts.push(format!("the fire: {}", if f { "on" } else { "off" }));
    }
    if let Some(s) = &r.frames {
        let rate = if s.ms > 0.0 { s.frames as f64 * 1000.0 / s.ms } else { 0.0 };
        parts.push(format!("frames on Play over {:.0} ms: {} ({rate:.1} a second), p50 {:.1} ms, p95 {:.1} ms, max {:.1} ms, {} over 50 ms",
            s.ms, s.frames, s.p50, s.p95, s.max, s.slow));
    }
    format!("the page: {}", if parts.is_empty() { "nothing".to_string() } else { parts.join("; ") })
}

/// How many `app_render_report` lines a start writes at most (the facts and the frame sample).
const PAGE_LINES: usize = 2;
static PAGE_SAID: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
/// WebKit's hardware acceleration in this start (Linux, [`window_created`]), and the page's facts: the support report.
static ACCEL: OnceLock<String> = OnceLock::new();
static PAGE: Mutex<Option<String>> = Mutex::new(None);

/// `app_render_report`: one line into launcher.log, at most [`PAGE_LINES`] a start.
pub fn page_report(r: &PageRender) {
    if PAGE_SAID.fetch_add(1, std::sync::atomic::Ordering::Relaxed) >= PAGE_LINES {
        return;
    }
    let line = says_page(r);
    if r.composited.is_some() {
        *PAGE.lock().unwrap_or_else(|e| e.into_inner()) = Some(line.trim_start_matches("the page: ").to_string());
    }
    log(&line);
}

/// The support report's lines: WebKit's hardware acceleration as applied, and what the page said about its drawing.
pub fn compositing_report() -> Vec<(String, String)> {
    let mut out = Vec::new();
    if let Some(a) = ACCEL.get() {
        out.push(("Launcher compositing".into(), a.clone()));
    }
    if let Some(p) = PAGE.lock().unwrap_or_else(|e| e.into_inner()).clone() {
        out.push(("Launcher page".into(), p));
    }
    out
}

/// EGL's client extensions (`eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`), from the libEGL WebKit loaded.
#[cfg(target_os = "linux")]
fn egl_client_extensions() -> Option<String> {
    use std::ffi::{CStr, c_char, c_void};
    const EGL_EXTENSIONS: i32 = 0x3055;
    // SAFETY: dlopen/dlsym of a system library by name; eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) is defined by
    // EGL_EXT_client_extensions (NULL where unsupported) and returns a static string. The handle is kept (WebKit has the
    // library loaded anyway).
    unsafe {
        let lib = libc::dlopen(c"libEGL.so.1".as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
        if lib.is_null() {
            return None;
        }
        let f = libc::dlsym(lib, c"eglQueryString".as_ptr());
        if f.is_null() {
            return None;
        }
        let query: extern "C" fn(*mut c_void, i32) -> *const c_char = std::mem::transmute(f);
        let p = query(std::ptr::null_mut(), EGL_EXTENSIONS);
        (!p.is_null()).then(|| CStr::from_ptr(p).to_string_lossy().into_owned())
    }
}

/// GTK 3's GL check, as WebKitGTK makes it: a GL context for a realized popup window; and that context's GL_VENDOR
/// (what Ubuntu's NVIDIA block reads, [`gl_vendor`]). Main thread only.
#[cfg(target_os = "linux")]
fn gtk_gl_check() -> (Result<(), String>, Option<String>) {
    use gtk::prelude::*;
    let w = gtk::Window::new(gtk::WindowType::Popup);
    w.realize();
    let r = match w.window() {
        Some(gw) => match gw.create_gl_context() {
            Ok(ctx) => (Ok(()), gl_vendor(&ctx)),
            Err(e) => (Err(e.to_string()), None),
        },
        None => (Err("the test window has no GdkWindow".into()), None),
    };
    // SAFETY: our own toplevel, never shown, referenced nowhere else.
    unsafe { w.destroy() };
    r
}

/// GL_VENDOR of a GTK GL context: made current, asked through GLVND's dispatch (libOpenGL, libGL, libGLESv2: whichever
/// is there), released again. None when any step fails.
#[cfg(target_os = "linux")]
fn gl_vendor(ctx: &gtk::gdk::GLContext) -> Option<String> {
    use std::ffi::{CStr, c_char};
    const GL_VENDOR: u32 = 0x1F00;
    ctx.realize().ok()?;
    ctx.make_current();
    // SAFETY: dlopen/dlsym of system GL libraries by name; glGetString(GL_VENDOR) with a context current on this thread
    // returns a static string or NULL (a dispatch without a context returns NULL). The handles are kept.
    let vendor = unsafe {
        let mut f = std::ptr::null_mut();
        for lib in [c"libOpenGL.so.0", c"libGL.so.1", c"libGLESv2.so.2"] {
            let h = libc::dlopen(lib.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
            if !h.is_null() {
                f = libc::dlsym(h, c"glGetString".as_ptr());
                if !f.is_null() {
                    break;
                }
            }
        }
        if f.is_null() {
            None
        } else {
            let get: extern "C" fn(u32) -> *const c_char = std::mem::transmute(f);
            let p = get(GL_VENDOR);
            (!p.is_null()).then(|| CStr::from_ptr(p).to_string_lossy().into_owned())
        }
    };
    gtk::gdk::GLContext::clear_current();
    vendor.filter(|v| !v.trim().is_empty())
}

// ---------------------------------------------------------------- what the UI sees

/// `app_info.render` (Linux only).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RenderInfo {
    /// This start.
    pub mode: Mode,
    pub why: Why,
    /// The marker is there: a start without an override draws without the GPU.
    pub marker: bool,
    /// The marker's sentence: who set it, why, when.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub marker_why: Option<String>,
    /// WebKit's DMA-BUF renderer in this start.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dmabuf: Option<DmabufInfo>,
    /// X11 (XWayland) in this start; only on a Wayland session with XWayland.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub x11: Option<X11Info>,
    /// Settings' "Force GPU drawing on NVIDIA" and this start's step past Ubuntu WebKit's NVIDIA block.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub force: Option<ForceInfo>,
}

/// `app_info.render.force`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ForceInfo {
    /// Settings' switch (its file, or [`FORCE_DEFAULT`]).
    pub on: bool,
    /// This start.
    pub step: Force,
    pub why: ForceWhy,
    /// Why the launcher turned the switch off by itself, and when (a forced start failed).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failed_why: Option<String>,
    /// This start found Ubuntu WebKit's NVIDIA block behind WebKit's choice to draw without the GPU.
    pub blocked: bool,
}

/// `app_info.render.x11`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct X11Info {
    /// This start draws through X11 (the launcher set `GDK_BACKEND=x11`).
    pub on: bool,
    pub why: X11Why,
    /// Settings' switch: the launcher may switch to X11 by itself (no `state/x11-auto-off.json`).
    pub auto: bool,
    /// The user's own `GDK_BACKEND`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yours: Option<String>,
}

/// `app_info.render.dmabuf`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DmabufInfo {
    /// This start.
    pub on: bool,
    pub why: DmabufWhy,
    /// The sticky marker is there: a start without an override draws without it.
    pub marker: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub marker_why: Option<String>,
}

pub fn info() -> Option<RenderInfo> {
    let c = choice()?;
    let d = crate::state::dirs().ok();
    Some(RenderInfo {
        mode: c.mode,
        why: c.why,
        marker: d.as_ref().is_some_and(|d| marker(d).is_file()),
        marker_why: d.as_ref().and_then(marker_why),
        dmabuf: dmabuf().map(|m| DmabufInfo {
            on: m.on,
            why: m.why,
            marker: d.as_ref().is_some_and(|d| dmabuf_marker(d).is_file()),
            marker_why: d.as_ref().and_then(|d| marker_file_why(&dmabuf_marker(d))),
        }),
        x11: x11().filter(|x| x.why != X11Why::Unavailable).map(|x| X11Info {
            on: x.on,
            why: x.why,
            auto: !d.as_ref().is_some_and(|d| x11_auto_off(d).is_file()),
            yours: if x.why == X11Why::Yours { std::env::var(X11_VAR).ok() } else { None },
        }),
        force: force().map(|f| ForceInfo {
            on: d.as_ref().and_then(|d| force_setting(&force_choice(d)))
                .unwrap_or(FORCE_DEFAULT || matches!(f.why, ForceWhy::Recovery | ForceWhy::Recovered)),
            step: f.step,
            why: f.why,
            failed_why: d.as_ref().and_then(|d| marker_file_why(&force_failed(d))),
            blocked: NVIDIA_BLOCK.get().copied().unwrap_or(false),
        }),
    })
}

/// Settings, "Force GPU drawing on NVIDIA": the switch for the next start ([`force_switch_files`]).
pub fn set_force(on: bool) -> Result<RenderInfo, Error> {
    if choice().is_none() {
        return Err(Error::new(kiln_manage::ErrorCode::Unsupported, "Only the Linux launcher has this."));
    }
    let d = crate::state::dirs()?;
    force_switch_files(&d, on)?;
    log(&format!("Settings: \"Force GPU drawing on NVIDIA\" is {}", if on {
        "on: the next start sets WEBKIT_FORCE_DMABUF_RENDERER=1 and WEBKIT_DMABUF_RENDERER_FORCE_SHM=1 past Ubuntu WebKit's NVIDIA block (and turns this off by itself if that start fails)"
    } else {
        "off: the next start leaves WebKit's NVIDIA block as it is"
    }));
    info().ok_or_else(|| Error::new(kiln_manage::ErrorCode::Internal, "no render state"))
}

/// Settings, "Switch to X11 when drawing is slow on Wayland": off writes `state/x11-auto-off.json` and removes the X11
/// marker (the next start stays on Wayland); on removes it and the no-help marker (X11 may be tried again).
pub fn set_x11_auto(on: bool) -> Result<RenderInfo, Error> {
    if choice().is_none() {
        return Err(Error::new(kiln_manage::ErrorCode::Unsupported, "Only the Linux launcher has this."));
    }
    let d = crate::state::dirs()?;
    if on {
        clear_marker_file(&x11_auto_off(&d))?;
        clear_marker_file(&x11_no_help(&d))?;
    } else {
        write_marker_file(&x11_auto_off(&d), "Turned off in Settings")?;
        clear_marker_file(&x11_marker(&d))?;
    }
    log(&format!("Settings: the automatic switch to X11 is {}", if on { "on (X11 may be tried again)" } else { "off (the next start stays on Wayland)" }));
    info().ok_or_else(|| Error::new(kiln_manage::ErrorCode::Internal, "no render state"))
}

/// Settings, "Draw the launcher without the GPU": the marker for the next start.
pub fn set_next(software: bool) -> Result<RenderInfo, Error> {
    if choice().is_none() {
        return Err(Error::new(kiln_manage::ErrorCode::Unsupported, "Only the Linux launcher has this."));
    }
    let d = crate::state::dirs()?;
    if software {
        write_marker(&d, "Turned on in Settings")?;
    } else {
        clear_marker(&d)?;
        // GPU drawing back on means "try the default again": WebKit's DMA-BUF renderer too
        clear_marker_file(&dmabuf_marker(&d))?;
    }
    log(&format!("Settings: the next start draws {}", if software { "without the GPU" } else { "with the GPU (and tries WebKit's DMA-BUF renderer again)" }));
    info().ok_or_else(|| Error::new(kiln_manage::ErrorCode::Internal, "no render state"))
}

/// The page's ready call (`app_started`).
pub fn page_started() {
    #[cfg(any(debug_assertions, feature = "dev-eval"))]
    if let (Some(c), Ok(v)) = (choice(), std::env::var(TEST_WITHHOLD)) {
        let mode = match c.mode { Mode::Gpu => "gpu", Mode::Software => "software" };
        if v == "all" || v == mode {
            log(&format!("test: the page's ready call is withheld ({TEST_WITHHOLD}={v})"));
            return;
        }
    }
    WATCH.page_started();
}

fn log(msg: &str) {
    crate::commands::app::launcher_log("render", msg);
}

/// "drawing with the GPU: the default", for the logs and the support report.
pub fn says(c: &Choice) -> String {
    let how = match c.mode { Mode::Gpu => "with the GPU", Mode::Software => "without the GPU (software mode)" };
    let why = match c.why {
        Why::Default => "the default".to_string(),
        Why::Marker => "the marker of an earlier start".to_string(),
        Why::Restart => "restarted after the page did not start with the GPU".to_string(),
        Why::Environment => format!("{ENV} is set"),
        Why::Argument => format!("{} was given", if c.mode == Mode::Software { ARG_SOFTWARE } else { ARG_GPU }),
    };
    format!("drawing {how}: {why}")
}

/// "WebKit's DMA-BUF renderer on: the default", for the logs and the support report.
pub fn says_dmabuf(d: &Dmabuf) -> String {
    let why = match d.why {
        DmabufWhy::Default => "the default".to_string(),
        DmabufWhy::Yours => format!("your {DMABUF_VAR}"),
        DmabufWhy::Software => "software mode".to_string(),
        DmabufWhy::Argument => format!("{} was given", if d.on { ARG_DMABUF } else { ARG_NO_DMABUF }),
        DmabufWhy::Environment => format!("{DMABUF_ENV}={}", if d.on { "on" } else { "off" }),
        DmabufWhy::Restart => "restarted after the page did not start with it".to_string(),
        DmabufWhy::Crashed => "the last start with it ended before its page had started".to_string(),
        DmabufWhy::Marker => "the marker of an earlier start".to_string(),
    };
    format!("WebKit's DMA-BUF renderer {}: {why}", if d.on { "on" } else { "off" })
}

/// "WebKit's DMA-BUF renderer forced past Ubuntu WebKit's NVIDIA block on shared memory (...): Settings' ... is on", for
/// the logs and the support report.
pub fn says_force(f: &ForceDmabuf) -> String {
    let how = match f.step {
        Force::Off => "not forced past Ubuntu WebKit's NVIDIA block".to_string(),
        Force::Shm => format!("forced past Ubuntu WebKit's NVIDIA block on shared memory ({FORCE_VAR}=1, {SHM_VAR}=1)"),
    };
    let why = match f.why {
        ForceWhy::Off => "Settings' \"Force GPU drawing on NVIDIA\" is off",
        ForceWhy::On => "Settings' \"Force GPU drawing on NVIDIA\" is on",
        ForceWhy::Failed => "the start before with it failed, so the launcher turns the switch off",
        ForceWhy::Recovery => "the previous normal NVIDIA start never confirmed a visible frame: trying shared memory once",
        ForceWhy::Recovered => "shared memory is the last working automatic renderer for this installation and display backend",
        ForceWhy::Unguarded => "the startup crash guard could not be written, so no override is applied",
        ForceWhy::Yours => "your own variables decide",
        ForceWhy::Unused => "WebKit's DMA-BUF renderer is off in this start",
    };
    format!("WebKit's DMA-BUF renderer {how}: {why}")
}

/// "X11 (XWayland) on: switched once after ...", for the logs.
pub fn says_x11(x: &X11) -> String {
    let why = match x.why {
        X11Why::Default => "the default (Wayland); the launcher switches once if WebKit draws without the GPU here",
        X11Why::Yours => "your GDK_BACKEND decides",
        X11Why::Unavailable => "not a Wayland session with XWayland",
        X11Why::Software => "software mode",
        X11Why::Off => "the automatic switch is off in Settings",
        X11Why::Restart => "restarted because WebKit drew without the GPU on Wayland",
        X11Why::Marker => "X11 helped in an earlier start",
        X11Why::Tried => "X11 was tried and did not help",
    };
    format!("X11 (XWayland) {}: {why}", if x.on { "on, GDK_BACKEND=x11" } else { "off" })
}

/// The support report's line: [`says_dmabuf`] and the sticky marker's sentence when there is one.
pub fn dmabuf_report() -> Option<String> {
    let d = dmabuf()?;
    let mut s = says_dmabuf(d);
    if let Some(w) = crate::state::dirs().ok().and_then(|dirs| marker_file_why(&dmabuf_marker(&dirs))) {
        s += &format!("; marker: {w}");
    }
    if let Some(f) = force().filter(|f| f.why != ForceWhy::Off) {
        s += &format!("; {}", says_force(f));
    }
    Some(s)
}

// ---------------------------------------------------------------- Linux: before GTK, and at the setup

/// Linux, in `main` before any thread exists and before Tauri: decides the mode, sets software mode's variables and
/// prints the log's path and the mode to stderr. [`watch`] starts the watchdog after crate::sandbox's start.
#[cfg(target_os = "linux")]
pub fn prepare(args: &[String]) {
    prepare_for_driver(args, Path::new("/proc/driver/nvidia/version").is_file());
}

#[cfg(target_os = "linux")]
fn prepare_for_driver(args: &[String], nvidia: bool) {
    // GLib's warnings into launcher.log from here on, before GTK and WebKit start (they say what they found only there)
    capture_glib();
    // a launcher started from another Kiln (the fallback, an update's restart): its variables are not the user's
    let ours = set_by_us(std::env::var(SET_BY_US).ok().as_deref());
    let restarted = std::env::var_os(RESTARTED).is_some();
    let dmabuf_restarted = std::env::var_os(DMABUF_RESTARTED).is_some();
    let x11_restarted = std::env::var(X11_RESTARTED).ok();
    let force_restarted = std::env::var(FORCE_RESTARTED).ok().filter(|v| v == "off");
    // SAFETY: main, before any thread exists (the callers' contract).
    unsafe {
        for k in &ours {
            std::env::remove_var(k);
        }
        std::env::remove_var(SET_BY_US);
        std::env::remove_var(RESTARTED);
        std::env::remove_var(DMABUF_RESTARTED);
        std::env::remove_var(X11_RESTARTED);
        std::env::remove_var(FORCE_RESTARTED);
    }
    // read only, before the beta copy's gate opens (state::dirs() refuses until then: 0.2.0-dev.29 to 29.17 never saw the marker)
    let dirs = crate::folder::found_dirs();
    let has_marker = dirs.as_ref().is_some_and(|d| marker(d).is_file());
    let c = decide(args, std::env::var(ENV).ok().as_deref(), restarted, has_marker);
    // WebKit's DMA-BUF renderer (the same read-only folder): a pending marker left behind means the last start that
    // used it died before its page had started; the sticky marker is written once the gate opens (after_gate)
    let left = dirs.as_ref().map(dmabuf_pending).filter(|p| p.is_file());
    let has_off = dirs.as_ref().is_some_and(|d| dmabuf_marker(d).is_file());
    let yours = std::env::var(DMABUF_VAR).ok();
    let mut d = decide_dmabuf(args, yours.as_deref(), std::env::var(DMABUF_ENV).ok().as_deref(), c.mode == Mode::Software,
        dmabuf_restarted, left.is_some(), has_off);
    if let Some(p) = left {
        let why = dmabuf_crash_why(&p);
        let _ = LEFT.set((p, why));
    }
    // X11 (XWayland) on a Wayland session: the user's own GDK_BACKEND (ours were taken back above) always wins
    let present = |k: &str| std::env::var_os(k).is_some_and(|v| !v.is_empty());
    let has = |f: fn(&DataDirs) -> PathBuf| dirs.as_ref().is_some_and(|dd| f(dd).is_file());
    let x = decide_x11(std::env::var(X11_VAR).ok().as_deref(), present("WAYLAND_DISPLAY"), present("DISPLAY"), c.mode == Mode::Software,
        has(x11_auto_off), x11_restarted.as_deref(), has(x11_marker), has(x11_no_help));
    // Ubuntu WebKit's NVIDIA block: Settings' switch; a forced start before this one that died (its guard was left) or
    // whose page was late (the watchdog's restart) forces nothing now and turns the switch off once the data folder opens;
    // the user's own variables (ours were taken back above) always win
    let context = startup_context(x.on);
    let _ = RENDER_CONTEXT.set(context.clone());
    let force_left = dirs.as_ref().map(force_pending).filter(|p| expected_frame(p, &context)).map(|p| { let why = force_crash_why(&p); (p, why) });
    let force_yours = [FORCE_VAR, SHM_VAR].iter().any(|k| std::env::var_os(k).is_some());
    let setting = dirs.as_ref().and_then(|dd| force_setting(&force_choice(dd)));
    let recovered = dirs.as_ref().is_some_and(|dd| expected_frame(&force_recovered(dd), &context));
    let normal_failed = dirs.as_ref().is_some_and(|dd| expected_frame(&normal_pending(dd), &context));
    let automatic = nvidia && setting.is_none() && (normal_failed || recovered) && !force_yours
        && force_left.is_none() && force_restarted.is_none() && c.mode == Mode::Gpu;
    // Internal sticky/restart DMA-BUF fallbacks must not prevent the one SHM attempt. Explicit user choices still win.
    if automatic {
        d = decide_dmabuf(args, yours.as_deref(), std::env::var(DMABUF_ENV).ok().as_deref(), false, false, false, false);
    }
    let chosen = setting.unwrap_or(FORCE_DEFAULT) || automatic;
    let mut f = decide_force(chosen, force_yours, d.on && c.mode == Mode::Gpu && nvidia, force_left.is_some() || force_restarted.is_some());
    if automatic && f.step == Force::Shm {
        f.why = if recovered { ForceWhy::Recovered } else { ForceWhy::Recovery };
    }
    // A previous normal start belongs to this attempt too; clear it only after a confirmed, settled frame.
    if normal_failed && let Some(dd) = &dirs {
        *NORMAL_PENDING.lock().unwrap_or_else(|e| e.into_inner()) = Some(normal_pending(dd));
    }
    if f.step != Force::Off {
        // Arm before setting variables: sandbox::prepare can call WebKit or GTK too. If the breadcrumb cannot be
        // written, never apply a persistent setting whose crash would leave the next start with no way to recover.
        let armed = dirs.as_ref().ok_or_else(|| Error::invalid("No data folder is available for the startup crash guard.")).and_then(|dirs| {
            let p = force_pending(dirs);
            let why = format!("A start with GPU drawing forced on NVIDIA (pid {}) has not said yet that its page started", std::process::id());
            write_startup_guard(&p, &context, true, &why)?;
            Ok(p)
        });
        match armed {
            Ok(p) => *FORCE_PENDING.lock().unwrap_or_else(|e| e.into_inner()) = Some(p),
            Err(e) => {
                let why = format!("The launcher turned this off by itself: it could not write the startup crash guard for GPU drawing forced on NVIDIA: {}", e.message);
                log(&why);
                let _ = FORCE_GUARD_ERROR.set(why);
                f = ForceDmabuf { step: Force::Off, why: ForceWhy::Unguarded };
            }
        }
    }
    if nvidia && setting.is_none() && !force_yours && c.mode == Mode::Gpu && d.on && f.why == ForceWhy::Off
        && let Some(dd) = &dirs
    {
        let p = normal_pending(dd);
        match write_startup_guard(&p, &context, true, "A normal NVIDIA startup has not confirmed a visible frame") {
            Ok(()) => *NORMAL_PENDING.lock().unwrap_or_else(|e| e.into_inner()) = Some(p),
            Err(e) => log(&format!("could not write normal NVIDIA startup guard: {}", e.message)),
        }
    }
    let mut set = Vec::new();
    if c.mode == Mode::Software {
        for (k, v) in software_env(|k| std::env::var_os(k)) {
            // SAFETY: as above.
            unsafe { std::env::set_var(k, v) };
            set.push(k);
        }
    }
    if d.sets_var() {
        // SAFETY: as above.
        unsafe { std::env::set_var(DMABUF_VAR, "1") };
        set.push(DMABUF_VAR);
    }
    for (k, v) in force_env(f.step) {
        // SAFETY: as above.
        unsafe { std::env::set_var(k, v) };
        set.push(k);
    }
    if let Some(left) = force_left {
        let _ = FORCE_LEFT.set(left);
    }
    if let Some(v) = force_restarted {
        let _ = FORCE_RESTARTED_SEEN.set(v);
    }
    if x.on {
        // SAFETY: as above.
        unsafe { std::env::set_var(X11_VAR, "x11") };
        set.push(X11_VAR);
    }
    if x.why == X11Why::Tried && x11_restarted.as_deref() == Some("0") && !has(x11_no_help) {
        // the start before went back from X11 (its page did not start in time there) and could not write this yet
        x11_note(Some(X11Note::NoHelp("The launcher turned this on by itself: the page did not start in time through X11 (XWayland)".into())), false);
    }
    if !set.is_empty() {
        // SAFETY: as above.
        unsafe { std::env::set_var(SET_BY_US, set.join(",")) };
    }
    let log_path = dirs.as_ref().map(|d| d.logs().join("launcher.log").display().to_string()).unwrap_or_else(|| "(no data folder)".into());
    let hint = match c.why {
        _ if c.mode == Mode::Gpu => format!("; a black window: {ARG_SOFTWARE}"),
        Why::Marker => format!("; {UNDO_MARKER}"),
        _ => String::new(),
    };
    eprintln!("Kiln Launcher: log {log_path}; {}{hint}; {}", says(&c), says_dmabuf(&d));
    let mut line = says(&c);
    if c.why == Why::Marker || c.why == Why::Restart {
        if let Some(w) = dirs.as_ref().and_then(marker_why) {
            line += &format!(" ({}: {w})", marker(dirs.as_ref().unwrap()).display());
        }
    }
    if c.why == Why::Marker {
        // a marker can be old: 0.2.0-dev.29 to 29.17 never read it at the start
        line += &format!("; {UNDO_MARKER}");
    }
    if let Some(i) = &c.ignored {
        line += &format!("; {i}");
    }
    if c.mode == Mode::Software {
        let vars: Vec<String> = SOFTWARE_ENV.iter().map(|(k, _)| {
            let v = std::env::var(k).unwrap_or_default();
            format!("{k}={v}{}", if set.contains(k) { "" } else { " (yours)" })
        }).collect();
        line += &format!("; {}", vars.join(", "));
    }
    log(&line);
    // the DMA-BUF line: the decision, why, the variable, and how to undo a marker
    let mut line = format!("{}; {DMABUF_VAR}={}", says_dmabuf(&d), match std::env::var(DMABUF_VAR) {
        Ok(v) if d.why == DmabufWhy::Yours => format!("{v} (yours)"),
        Ok(v) => v,
        Err(_) => "(not set)".into(),
    });
    if let Some((p, why)) = LEFT.get() {
        line += &format!("; found {} ({why}); the sticky marker is written once the data folder opens", p.display());
    }
    if d.why == DmabufWhy::Marker
        && let Some(w) = dirs.as_ref().and_then(|dd| marker_file_why(&dmabuf_marker(dd)))
    {
        line += &format!(" ({}: {w})", dmabuf_marker(dirs.as_ref().unwrap()).display());
    }
    if matches!(d.why, DmabufWhy::Marker | DmabufWhy::Crashed) {
        line += &format!("; {UNDO_DMABUF}");
    }
    if let Some(i) = &d.ignored {
        line += &format!("; {i}");
    }
    if d.why == DmabufWhy::Crashed {
        eprintln!("Kiln Launcher: PROBLEM: the last start with WebKit's DMA-BUF renderer ended before its page had started: \
            this start and the next ones draw without it ({DMABUF_VAR}=1); {UNDO_DMABUF}");
        log(&format!("PROBLEM: {line}"));
    } else {
        log(&line);
    }
    log(&says_x11(&x));
    let mut line = says_force(&f);
    if let Some((p, why)) = FORCE_LEFT.get() {
        line += &format!("; found {} ({why}); the switch is turned off once the data folder opens", p.display());
    }
    if f.why == ForceWhy::Off
        && let Some(w) = dirs.as_ref().and_then(|dd| marker_file_why(&force_failed(dd)))
    {
        line += &format!(" ({}: {w})", force_failed(dirs.as_ref().unwrap()).display());
    }
    if f.why == ForceWhy::Yours {
        let v = |k: &str| std::env::var(k).map_or("(not set)".to_string(), |v| format!("{v} (yours)"));
        line += &format!("; {FORCE_VAR}={}, {SHM_VAR}={}", v(FORCE_VAR), v(SHM_VAR));
    }
    if f.why == ForceWhy::Failed {
        eprintln!("Kiln Launcher: PROBLEM: the last start with GPU drawing forced on NVIDIA failed: this start forces nothing, and \
            Settings > Advanced > Drawing the launcher > \"Force GPU drawing on NVIDIA\" goes off");
        log(&format!("PROBLEM: {line}"));
    } else {
        log(&line);
    }
    let _ = CHOICE.set(c);
    let _ = DMABUF.set(d);
    let _ = X11_CHOICE.set(x);
    let _ = FORCE.set(f);
}

#[cfg(not(target_os = "linux"))]
pub fn prepare(_args: &[String]) {}

/// Linux, in `main` after [`prepare`] and crate::sandbox's start (its message may have been up): the watchdog. Its
/// clock for a start without a window begins here.
/// A guarded start (WebKit's DMA-BUF renderer by default) writes its pending marker here, right before Tauri, GTK's
/// window and WebKit start: after crate::sandbox's message (whose restart or Quit is no crash).
#[cfg(target_os = "linux")]
pub fn watch() {
    let Some(c) = choice().cloned() else { return };
    let d = dmabuf().cloned().unwrap_or(Dmabuf { on: false, why: DmabufWhy::Software, ignored: None });
    let f = force().cloned().unwrap_or(ForceDmabuf { step: Force::Off, why: ForceWhy::Unused });
    if guarded(&d, &f)
        && let Some(dirs) = crate::folder::found_dirs()
    {
        // read-only use of the folder before the beta copy's gate, except this one file of our own
        let p = dmabuf_pending(&dirs);
        match write_marker_file(&p, &format!("A start with WebKit's DMA-BUF renderer (pid {}) has not said yet that its page started", std::process::id())) {
            Ok(()) => *PENDING.lock().unwrap_or_else(|e| e.into_inner()) = Some(p),
            Err(e) => log(&format!("could not write the DMA-BUF start guard {}: {}", p.display(), e.message)),
        }
    }
    let x = x11().cloned().unwrap_or(X11 { on: false, why: X11Why::Unavailable });
    let begun = Instant::now();
    std::thread::Builder::new().name("kiln-render-watch".into()).spawn(move || watchdog(c, d, x, f, begun)).ok();
}

#[cfg(not(target_os = "linux"))]
pub fn watch() {}

/// The watchdog: the page's ready call in time, or the fallback.
#[cfg(target_os = "linux")]
fn watchdog(c: Choice, d: Dmabuf, x: X11, f: ForceDmabuf, begun: Instant) {
    let window = match WATCH.wait(begun, TIMEOUT, NO_WINDOW_TIMEOUT) {
        Seen::Started(after) => {
            log(&format!("the page started {} ms after the window was made ({:?}, dmabuf {})", after.as_millis(), c.mode,
                if d.on { "on" } else { "off" }).to_lowercase());
            if guarded(&d, &f) {
                std::thread::sleep(SETTLE);
                let when = format!("the page started {} s ago", SETTLE.as_secs());
                clear_pending(&when);
            }
            return;
        }
        Seen::Late { window } => window,
    };
    let what = if window { format!("the page did not start within {} s of the window", TIMEOUT.as_secs()) } else { format!("no window within {} s", NO_WINDOW_TIMEOUT.as_secs()) };
    match when_late_forced(&c, &d, &x, &f) {
        Late::NoForce => {
            let why = format!("The launcher turned this off by itself: {what} with GPU drawing forced on NVIDIA");
            let off = crate::state::dirs().and_then(|dd| force_gave_up(&dd, &why).map(|_| force_failed(&dd)));
            match &off {
                Ok(p) => log(&format!("{what} ({}); the switch is off now, wrote {}; starting again without forcing it", says_force(&f), p.display())),
                Err(e) => log(&format!("{what} ({}); could not turn the switch off yet ({}); starting again without forcing it", says_force(&f), e.message)),
            }
            eprintln!("Kiln Launcher: {what} with GPU drawing forced on NVIDIA; starting again without it");
            let err = restart(&Relaunch::force_fallback().keeping());
            log(&format!("PROBLEM: could not start again without forcing it: {err}"));
            eprintln!("Kiln Launcher: {what}; could not start again: {err}. Turn off Settings > Advanced > Drawing the launcher > \
                \"Force GPU drawing on NVIDIA\".");
            crate::dialog::late(&c, &what, Some(&err.to_string()));
        }
        Late::NoX11 => {
            let why = format!("The launcher turned this on by itself: {what} through X11 (XWayland)");
            log(&format!("X11: {what} through X11 ({}); X11 did not help: starting again on Wayland, X11 is not tried again", says_x11(&x)));
            eprintln!("Kiln Launcher: {what} through X11; starting again on Wayland");
            // written now when the gate is open; else the restart's variable says so and the next start writes it
            x11_note(Some(X11Note::NoHelp(why)), false);
            let err = restart(&Relaunch::x11_back().keeping());
            log(&format!("PROBLEM: could not start again on Wayland: {err}"));
            eprintln!("Kiln Launcher: {what}; could not start again on Wayland: {err}.");
            crate::dialog::late(&c, &what, Some(&err.to_string()));
        }
        Late::NoDmabuf => {
            let why = format!("The launcher turned this on by itself: {what} with WebKit's DMA-BUF renderer");
            let wrote = crate::state::dirs().and_then(|dd| write_marker_file(&dmabuf_marker(&dd), &why).map(|_| dmabuf_marker(&dd)));
            match &wrote {
                Ok(p) => log(&format!("{what} ({}, {}); wrote {}; starting again with the GPU, without the DMA-BUF renderer", says(&c), says_dmabuf(&d), p.display())),
                Err(e) => log(&format!("{what} ({}, {}); could not write the DMA-BUF marker ({}); starting again with the GPU, without the DMA-BUF renderer",
                    says(&c), says_dmabuf(&d), e.message)),
            }
            eprintln!("Kiln Launcher: {what} with WebKit's DMA-BUF renderer; starting again without it ({UNDO_DMABUF})");
            let err = restart(&Relaunch::dmabuf_fallback().keeping());
            log(&format!("PROBLEM: could not start again without the DMA-BUF renderer: {err}"));
            eprintln!("Kiln Launcher: {what}; could not start again without the DMA-BUF renderer: {err}. Start it with {ARG_NO_DMABUF}.");
            crate::dialog::late(&c, &what, Some(&err.to_string()));
        }
        Late::Restart => {
            let why = format!("The launcher turned this on by itself: {what} with the GPU");
            let wrote = crate::state::dirs().and_then(|d| write_marker(&d, &why).map(|_| marker(&d)));
            match &wrote {
                Ok(p) => log(&format!("{what} ({}); wrote {}; starting again without the GPU", says(&c), p.display())),
                Err(e) => log(&format!("{what} ({}); could not write the marker ({}); starting again without the GPU", says(&c), e.message)),
            }
            let err = restart(&Relaunch::software_fallback().keeping());
            log(&format!("PROBLEM: could not start again without the GPU: {err}"));
            eprintln!("Kiln Launcher: {what}; could not start again without the GPU: {err}. Start it with {ARG_SOFTWARE}.");
            // nothing left the launcher can try by itself: the player's message (crate::dialog)
            crate::dialog::late(&c, &what, Some(&err.to_string()));
        }
        Late::Report => {
            let next = match c.mode {
                Mode::Software => "not starting again (once is enough). Send launcher.log and a support report".to_string(),
                Mode::Gpu => format!("not falling back to software mode: the GPU was asked for ({}). Try {ARG_SOFTWARE}", says(&c)),
            };
            log(&format!("PROBLEM: {what} ({}); {next}", says(&c)));
            eprintln!("Kiln Launcher: PROBLEM: {what} ({}); {next}.", says(&c));
            // instead of a blank window: what was tried, where the log is, what the player can still try (crate::dialog)
            crate::dialog::late(&c, &what, None);
        }
    }
    let after = WATCH.wait_late(begun);
    log(&format!("the page started late, {} ms after the window was made", after.as_millis()));
}

/// Prepare a restart's breadcrumbs independently of exec, so failure to persist the off setting is testable.
fn before_restart(how: &Relaunch) {
    clear_pending("a restart");
    // A failed SHM start keeps its breadcrumb until the next start durably turns the preference off. Otherwise a
    // failed off-setting write followed by exec would erase the only persistent evidence and could retry forever.
    if !how.set.iter().any(|(k, v)| *k == FORCE_RESTARTED && v == "off") {
        clear_force_pending("a restart");
    }
}

/// Replaces this process with a new start as `how` says (the fallback's software mode, or the player's choice in a
/// message, crate::dialog); returns only when that failed.
#[cfg(target_os = "linux")]
pub(crate) fn restart(how: &Relaunch) -> std::io::Error {
    use std::os::unix::process::CommandExt;
    let Some((target, appimage)) = restart_target(std::env::var_os("APPIMAGE"), std::env::current_exe().ok()) else {
        return std::io::Error::other("neither $APPIMAGE nor this executable's path is known");
    };
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    // A normal restart is no crash; a failed forced start retains its guard until the off choice is durable.
    before_restart(how);
    log(&format!("restart: {}{}", target.display(), if appimage { " (the AppImage)" } else { "" }));
    // exec: the same process (a terminal keeps waiting for it), and the instance lock goes with the old image
    restart_command(&target, appimage, &args, std::env::vars_os(), how).exec()
}

/// Linux, in Tauri's setup (the window exists): the watchdog's clock, the webview's acceleration policy in software
/// mode, the web process's end, and the facts a black window needs in launcher.log.
#[cfg(target_os = "linux")]
pub fn window_created(app: &tauri::AppHandle) {
    use tauri::Manager;
    WATCH.window_created();
    // a late message goes up through this window's main loop (crate::dialog)
    crate::dialog::set_app(app);
    let Some(c) = choice().cloned() else { return };
    let webkit = {
        use webkit2gtk::ffi;
        // SAFETY: plain getters of the loaded library's version.
        unsafe { format!("{}.{}.{}", ffi::webkit_get_major_version(), ffi::webkit_get_minor_version(), ffi::webkit_get_micro_version()) }
    };
    let var = |k: &str| match std::env::var_os(k) { Some(v) if !v.is_empty() => "set", Some(_) => "empty", None => "not set" };
    let nvidia_version = std::fs::read_to_string("/proc/driver/nvidia/version").ok();
    let nvidia_driver = nvidia_version.is_some();
    let nvidia = nvidia_version.as_deref().and_then(nvidia_line)
        .map_or("no /proc/driver/nvidia/version".to_string(), |l| format!("/proc/driver/nvidia/version: {l}"));
    let sandbox = crate::sandbox::state().map(|s| format!("; WebKit's sandbox: {}", s.says())).unwrap_or_default();
    let dmabuf_says = dmabuf().map(|d| format!(" ({})", says_dmabuf(d))).unwrap_or_default();
    log(&format!(
        "WebKitGTK {webkit}; XDG_SESSION_TYPE={}, DISPLAY {}, WAYLAND_DISPLAY {}; {nvidia}; {DMABUF_VAR}={}{dmabuf_says}{sandbox}",
        std::env::var("XDG_SESSION_TYPE").unwrap_or_else(|_| "(not set)".into()), var("DISPLAY"), var("WAYLAND_DISPLAY"),
        std::env::var(DMABUF_VAR).unwrap_or_else(|_| "(not set)".into())
    ));
    #[cfg(any(debug_assertions, feature = "dev-eval"))]
    if let Ok(v) = std::env::var(TEST_EXIT)
        && (v == "all" || (v == "dmabuf" && dmabuf().is_some_and(|d| d.on)))
    {
        log(&format!("test: the process ends in 2 s without exit handlers ({TEST_EXIT}={v})"));
        std::thread::spawn(|| {
            std::thread::sleep(Duration::from_secs(2));
            eprintln!("Kiln Launcher: test: ending now ({TEST_EXIT})");
            // SAFETY: _exit only ends the process (as a crash would, nothing runs after it).
            unsafe { libc::_exit(71) }
        });
    }
    let Some(w) = app.get_webview_window("main") else { return };
    frame_painted(false, w.is_visible().unwrap_or(true) && !w.is_minimized().unwrap_or(false));
    let software = c.mode == Mode::Software;
    let x = x11().cloned();
    let _ = w.with_webview(move |pw| {
        use webkit2gtk::{HardwareAccelerationPolicy, SettingsExt, WebViewExt};
        let wv = pw.inner();
        if let Some(s) = WebViewExt::settings(&wv) {
            if software {
                s.set_hardware_acceleration_policy(HardwareAccelerationPolicy::Never);
                // WebKitGTK 2.54 still offers WebGL without compositing (through EGL: the GPU's driver); the page then
                // draws its CSS mouth (app_info.noWebgl), the admin map says WebGL2 is missing
                s.set_enable_webgl(false);
            }
            let policy = s.hardware_acceleration_policy();
            log(&format!("the webview's hardware acceleration policy: {policy:?}, WebGL: {}", if s.enables_webgl() { "on" } else { "off" }));
            // with the GPU, Never is WebKit's own answer: it composites nothing (inbox 94); say which check failed
            let off = !software && policy == HardwareAccelerationPolicy::Never;
            let scale = screen_scale(&wv);
            let env = |k: &str| std::env::var(k).ok();
            // a variable that turns compositing off explains it whatever the display server: X11 would not help
            let by_vars = [env("WEBKIT_DISABLE_COMPOSITING_MODE"), env(DMABUF_VAR)].iter().any(|v| v.as_deref().is_some_and(|v| v != "0"));
            let forced = force().filter(|f| f.step != Force::Off);
            let accel = if off {
                let (gl, vendor) = gtk_gl_check();
                let mut reasons = webkit_off_reasons(env("WEBKIT_DISABLE_COMPOSITING_MODE").as_deref(), env(DMABUF_VAR).as_deref(),
                    egl_client_extensions().as_deref(), Some(gl));
                // Ubuntu WebKit's NVIDIA block comes right after the variables, where WebKit checks it
                let patched = webkit_patched();
                let lifted = env(FORCE_VAR).is_some_and(|v| v != "0");
                let block = nvidia_block_reason(patched.as_ref().map(|(_, p)| *p), vendor.as_deref(), nvidia_driver, lifted);
                let at = reasons.iter().take_while(|r| r.starts_with("WEBKIT_")).count();
                if let Some(b) = &block {
                    reasons.insert(at, b.clone());
                } else if let Some(f) = forced {
                    reasons.insert(at, format!("{}, and WebKit still composites nothing", says_force(f)));
                }
                let _ = NVIDIA_BLOCK.set(block.is_some());
                let (nodes, modeset) = drm_state();
                reasons.extend(drm_reasons(&nodes, modeset.as_deref()));
                let why = if reasons.is_empty() { "none of the checks the launcher knows failed here".to_string() } else { reasons.join("; ") };
                let lib = match &patched {
                    Some((path, true)) => format!("; WebKitGTK library {path}: Ubuntu's build with the NVIDIA block"),
                    Some((path, false)) => format!("; WebKitGTK library {path}: without the NVIDIA block"),
                    None => String::new(),
                };
                log(&format!("PROBLEM: WebKit turned hardware acceleration off by itself (the launcher did not ask for it): it composites \
                    nothing, every change of the page is painted again on the CPU (slow in a large window). Why: {why}. {}; scale {scale}; \
                    GL vendor: {}{lib}", says_drm(&nodes, modeset.as_deref()), vendor.as_deref().unwrap_or("(not known)")));
                format!("off (WebKit's own choice): {why}")
            } else {
                if let Some(f) = forced {
                    log(&format!("{}: WebKit composites (policy {policy:?})", says_force(f)));
                }
                format!("{policy:?}")
            };
            let _ = ACCEL.set(accel);
            if let Some(x) = &x {
                x11_after_policy(x, off, by_vars, scale);
            }
        }
        // what the web context itself says about its sandbox: crate::sandbox read the variables, this is WebKit's own
        // answer (a WebKitGTK that ran the sandbox by default would show here)
        if let Some(ctx) = WebViewExt::context(&wv) {
            use webkit2gtk::WebContextExt;
            let said = ctx.is_sandbox_enabled();
            // before 2.46 the variables act later (when a process starts), so only "on" without any asking is news
            let unused = crate::sandbox::state().is_some_and(|s| s.using.webkit == crate::sandbox::Webkit::Unused);
            let note = if said && unused { " although nothing asked for it: this WebKitGTK runs it by default (sandbox.rs expects not)" } else { "" };
            crate::commands::app::launcher_log("sandbox", &format!("the web context says its sandbox is {}{note}", if said { "on" } else { "off" }));
        }
        wv.connect_web_process_terminated(|_, reason| log(&format!("PROBLEM: the page's web process ended ({reason:?})")));
    });
}

#[cfg(not(target_os = "linux"))]
pub fn window_created(_app: &tauri::AppHandle) {}

/// Linux, once the webview's acceleration policy is known (`off`: WebKit's own Never): the switch to X11, or what an X11
/// start learned ([`x11_step`]); one line in launcher.log for each decision.
#[cfg(target_os = "linux")]
fn x11_after_policy(x: &X11, off: bool, by_vars: bool, scale: i32) {
    match x11_step(x, off, by_vars, scale) {
        X11Step::Switch => {
            log(&format!("X11: WebKit draws without the GPU on Wayland at scale {scale}; starting again once through X11 (XWayland, \
                GDK_BACKEND=x11), which draws at scale 1: far fewer pixels to paint on the CPU, less sharp. \
                Settings > Appearance > \"Switch to X11 when drawing is slow on Wayland\" turns this off"));
            eprintln!("Kiln Launcher: WebKit draws without the GPU on Wayland; starting again once through X11 (GDK_BACKEND=x11)");
            let err = restart(&Relaunch::x11_switch().keeping());
            log(&format!("PROBLEM: could not start again through X11: {err}; this start goes on on Wayland"));
        }
        X11Step::Keep => {
            log(if off {
                "X11: WebKit draws without the GPU through X11 (XWayland) too, which still paints far faster than Wayland: \
                    staying on X11, later starts go straight to X11 (a page late through X11 goes back to Wayland)"
            } else {
                "X11: WebKit draws with the GPU through X11 (XWayland): later starts go straight to X11"
            });
            x11_note(Some(X11Note::Helped), false);
        }
        X11Step::Nothing if off => {
            let why = match x.why {
                X11Why::Default if by_vars => "a variable turns WebKit's compositing off, X11 would not change that".to_string(),
                X11Why::Default => format!("scale {scale}: X11 would paint as many pixels on the CPU"),
                _ => says_x11(x),
            };
            log(&format!("X11: not switching ({why})"));
        }
        X11Step::Nothing => {}
    }
}

/// The largest GDK scale factor of the screens and the webview (GTK 3: whole numbers; KDE's 150 % reads 2).
#[cfg(target_os = "linux")]
fn screen_scale(wv: &webkit2gtk::WebView) -> i32 {
    use gtk::prelude::*;
    let own = wv.scale_factor();
    let screens = gtk::gdk::Display::default()
        .map(|d| (0..d.n_monitors()).filter_map(|i| d.monitor(i)).map(|m| m.scale_factor()).max().unwrap_or(1))
        .unwrap_or(1);
    own.max(screens).max(1)
}

/// The DRM render nodes (`/dev/dri/renderD*`, readable and writable by this process or not) and NVIDIA's
/// `nvidia_drm modeset` parameter: WebKitGTK 2.50 hands its frames over as DMA-BUFs from a GBM device on a render
/// node, a check the launcher cannot repeat from the outside (EGL's extension names alone pass on Oryx593's machine).
#[cfg(target_os = "linux")]
fn drm_state() -> (Vec<(String, bool)>, Option<String>) {
    let mut nodes: Vec<(String, bool)> = std::fs::read_dir("/dev/dri").into_iter().flatten().flatten()
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|n| n.starts_with("renderD"))
        .map(|n| {
            let ok = std::fs::OpenOptions::new().read(true).write(true).open(format!("/dev/dri/{n}")).is_ok();
            (n, ok)
        })
        .collect();
    nodes.sort();
    let modeset = std::fs::read_to_string("/sys/module/nvidia_drm/parameters/modeset").ok().map(|v| v.trim().to_string());
    (nodes, modeset)
}

/// What [`drm_state`] found that explains WebKit's Never: no render node it can open, NVIDIA's DRM without modesetting.
pub fn drm_reasons(nodes: &[(String, bool)], modeset: Option<&str>) -> Vec<String> {
    let mut out = Vec::new();
    if !nodes.iter().any(|(_, ok)| *ok) {
        out.push(if nodes.is_empty() { "no DRM render node (/dev/dri/renderD*)".to_string() }
            else { "no DRM render node this user can open (is the user in the render/video group?)".to_string() });
    }
    if modeset == Some("N") {
        out.push("nvidia_drm modeset=N (NVIDIA's driver offers no GBM device without it: nvidia_drm.modeset=1)".into());
    }
    out
}

/// "DRM: renderD128 (opens), nvidia_drm modeset=Y", for the PROBLEM line.
pub fn says_drm(nodes: &[(String, bool)], modeset: Option<&str>) -> String {
    let n: Vec<String> = nodes.iter().map(|(n, ok)| format!("{n} ({})", if *ok { "opens" } else { "cannot open" })).collect();
    format!("DRM: {}{}", if n.is_empty() { "no render node".to_string() } else { n.join(", ") },
        modeset.map(|m| format!(", nvidia_drm modeset={m}")).unwrap_or_default())
}

/// The first line of `/proc/driver/nvidia/version`, without the driver build's `(user@host)` (a module built on the
/// player's machine names it; launcher.log goes into support reports).
pub fn nvidia_line(text: &str) -> Option<String> {
    let l = text.lines().next()?;
    Some(l.split_whitespace().filter(|w| !w.contains('@')).collect::<Vec<_>>().join(" "))
}

#[cfg(test)]
mod tests {
    use super::*;

    const EGL_NVIDIA: &str = "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_gbm EGL_KHR_platform_wayland";

    #[test]
    fn webkit_off_reasons_follow_webkits_own_checks() {
        // all passed: nothing to say
        assert!(webkit_off_reasons(None, None, Some(EGL_NVIDIA), Some(Ok(()))).is_empty());
        assert!(webkit_off_reasons(Some("0"), Some("0"), Some("EGL_MESA_platform_surfaceless"), None).is_empty(), "0 is unset, as WebKit reads it");
        // GTK's GL failing (the likely inbox 94 case) is named with GTK's message
        let r = webkit_off_reasons(None, None, Some(EGL_NVIDIA), Some(Err("Unable to create a GL context ".into())));
        assert_eq!(r, vec!["GTK could not make a GL context: Unable to create a GL context".to_string()]);
        // EGL without either platform, or without client extensions at all
        let r = webkit_off_reasons(None, None, Some("EGL_EXT_client_extensions EGL_KHR_platform_wayland"), Some(Ok(())));
        assert!(r.len() == 1 && r[0].contains("neither EGL_KHR_platform_gbm nor EGL_MESA_platform_surfaceless"), "{r:?}");
        assert!(webkit_off_reasons(None, None, None, None)[0].contains("no client extensions"));
        // a token is a whole word: EGL_KHR_platform_gbm_x is not EGL_KHR_platform_gbm
        assert_eq!(webkit_off_reasons(None, None, Some("EGL_KHR_platform_gbm_x"), None).len(), 1);
        // the variables, in WebKit's order
        let r = webkit_off_reasons(Some("1"), Some("1"), Some(EGL_NVIDIA), None);
        assert!(r[0].starts_with("WEBKIT_DISABLE_COMPOSITING_MODE=1") && r[1].starts_with("WEBKIT_DISABLE_DMABUF_RENDERER=1"), "{r:?}");
    }

    #[test]
    fn the_pages_line_says_what_it_measured() {
        let facts = PageRender { composited: Some(false), webgl: Some("WebGL 1.0 / Apple GPU\n".into()), dpr: Some(1.25), width: Some(2048),
            height: Some(1152), reduced_motion: Some(false), fire: Some(true), frames: None };
        let s = says_page(&facts);
        assert!(s.starts_with("the page: WebKit composites: no ("), "{s}");
        assert!(s.contains("WebGL: WebGL 1.0 / Apple GPU;") && s.contains("window 2048 x 1152 CSS px") && s.contains("devicePixelRatio 1.25")
            && s.contains("prefers-reduced-motion: no") && s.contains("the fire: on"), "{s}");
        let frames = PageRender { frames: Some(FrameSample { ms: 2000.0, frames: 30, p50: 66.4, p95: 81.0, max: 120.0, slow: 20 }), ..Default::default() };
        assert_eq!(says_page(&frames), "the page: frames on Play over 2000 ms: 30 (15.0 a second), p50 66.4 ms, p95 81.0 ms, max 120.0 ms, 20 over 50 ms");
        assert_eq!(says_page(&PageRender::default()), "the page: nothing");
        // a long or odd renderer string stays one short line
        let long = PageRender { webgl: Some("x".repeat(500)), ..Default::default() };
        assert_eq!(says_page(&long).len(), "the page: WebGL: ".len() + 160);
    }

    #[test]
    fn the_pages_report_parses_from_the_frontends_json() {
        let r: PageRender = serde_json::from_value(serde_json::json!({ "composited": true, "reducedMotion": true,
            "frames": { "ms": 2000, "frames": 120, "p50": 16.7, "p95": 17.5, "max": 20, "slow": 0 } })).unwrap();
        assert_eq!(r.composited, Some(true));
        assert_eq!(r.reduced_motion, Some(true));
        assert_eq!(r.frames.unwrap().frames, 120);
    }

    fn args(tail: &[&str]) -> Vec<String> {
        std::iter::once("kiln-launcher").chain(tail.iter().copied()).map(str::to_string).collect()
    }
    fn c(mode: Mode, why: Why) -> Choice {
        Choice { mode, why, ignored: None }
    }
    fn dm(on: bool, why: DmabufWhy) -> Dmabuf {
        Dmabuf { on, why, ignored: None }
    }
    /// The software step, as before the DMA-BUF step came (a start already without the DMA-BUF renderer).
    fn late(c: &Choice) -> Late {
        when_late(c, &dm(false, DmabufWhy::Restart))
    }
    fn dmabuf_of(tail: &[&str], yours: Option<&str>, env: Option<&str>) -> Dmabuf {
        decide_dmabuf(&args(tail), yours, env, false, false, false, false)
    }

    #[test]
    fn nothing_asked_draws_with_the_gpu_and_may_fall_back() {
        let first = decide(&args(&[]), None, false, false);
        assert_eq!(first, c(Mode::Gpu, Why::Default));
        assert_eq!(late(&first), Late::Restart);
        // other arguments are not render arguments
        assert_eq!(decide(&args(&["--deck", "--software-renderer"]), None, false, false), c(Mode::Gpu, Why::Default));
    }

    #[test]
    fn a_marker_from_an_earlier_start_draws_without_the_gpu_from_the_start() {
        let m = decide(&args(&[]), None, false, true);
        assert_eq!(m, c(Mode::Software, Why::Marker));
        assert_eq!(late(&m), Late::Report, "software mode never restarts");
    }

    #[test]
    fn the_environment_and_the_arguments_win_over_the_marker() {
        // the environment over the marker
        let e = decide(&args(&[]), Some("gpu"), false, true);
        assert_eq!(e, c(Mode::Gpu, Why::Environment));
        assert_eq!(late(&e), Late::Report, "a GPU the user asked for is not switched off by the launcher");
        assert_eq!(decide(&args(&[]), Some(" Software "), false, false), c(Mode::Software, Why::Environment));
        // the arguments over the environment and the marker; the last one given wins
        assert_eq!(decide(&args(&[ARG_GPU]), Some("software"), false, true), c(Mode::Gpu, Why::Argument));
        assert_eq!(decide(&args(&[ARG_SOFTWARE]), Some("gpu"), false, false), c(Mode::Software, Why::Argument));
        assert_eq!(decide(&args(&[ARG_SOFTWARE, "--deck", ARG_GPU]), None, false, false), c(Mode::Gpu, Why::Argument));
        assert_eq!(late(&decide(&args(&[ARG_GPU]), None, false, false)), Late::Report);
        // the arguments and the environment also win over a restart's flag
        assert_eq!(decide(&args(&[ARG_GPU]), None, true, true), c(Mode::Gpu, Why::Argument));
        // an empty value is no value; an unknown one is said and ignored
        assert_eq!(decide(&args(&[]), Some(""), false, true), c(Mode::Software, Why::Marker));
        let odd = decide(&args(&[]), Some("fast"), false, false);
        assert_eq!((odd.mode, odd.why), (Mode::Gpu, Why::Default));
        assert!(odd.ignored.as_deref().is_some_and(|i| i.contains("KILN_LAUNCHER_RENDER=fast")), "{odd:?}");
    }

    #[test]
    fn the_fallback_restarts_once_and_never_loops() {
        let t = std::env::temp_dir().join(format!("kl-render-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let d = DataDirs::at(&t);
        let start = |restarted: bool| decide(&args(&["--deck"]), None, restarted, marker(&d).is_file());
        // a first start draws with the GPU; the page is late: the marker, then the restart (same arguments, the flag)
        let first = start(false);
        assert_eq!(late(&first), Late::Restart);
        write_marker(&d, "test: the page did not start").unwrap();
        assert!(marker(&d).is_file());
        // the restart draws without the GPU and, late again, only reports
        let second = start(true);
        assert_eq!(second, c(Mode::Software, Why::Restart));
        assert_eq!(late(&second), Late::Report);
        // a marker that could not be written: the restart's flag alone still keeps it from looping
        let unwritten = decide(&args(&["--deck"]), None, true, false);
        assert_eq!(unwritten.mode, Mode::Software);
        assert_eq!(late(&unwritten), Late::Report);
        // the next start by hand: software mode from the start
        assert_eq!(start(false), c(Mode::Software, Why::Marker));
        assert!(marker_why(&d).is_some_and(|w| w.starts_with("test: the page did not start")));
        // Settings clears it: the GPU again (and a second clear is no error)
        clear_marker(&d).unwrap();
        clear_marker(&d).unwrap();
        assert_eq!(start(false), c(Mode::Gpu, Why::Default));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn the_restart_runs_the_appimage_file_when_appimage_is_set() {
        let exe = PathBuf::from("/tmp/.mount_KilnAB12/usr/bin/kiln-launcher");
        let image = OsString::from("/games/Kiln Launcher_0.2.0-dev.24_amd64.AppImage");
        assert_eq!(restart_target(Some(image.clone()), Some(exe.clone())), Some((PathBuf::from(&image), true)));
        assert_eq!(restart_target(None, Some(exe.clone())), Some((exe.clone(), false)), "a .deb or a plain build: this executable");
        assert_eq!(restart_target(Some(OsString::new()), Some(exe.clone())), Some((exe.clone(), false)), "an empty APPIMAGE is none");
        assert_eq!(restart_target(Some(image.clone()), None), Some((PathBuf::from(&image), true)));
        assert_eq!(restart_target(None, None), None);
    }

    #[test]
    fn the_restart_keeps_the_arguments_and_starts_the_appimage_like_the_host_would() {
        let os = |s: &str| OsString::from(s);
        let mount = "/tmp/.mount_KilnAB12";
        let vars = vec![
            (os("APPDIR"), os(mount)),
            (os("APPIMAGE"), os("/games/Kiln.AppImage")),
            (os("LD_LIBRARY_PATH"), os(&format!("{mount}/usr/lib/:/opt/host/lib"))),
            (os("HOME"), os("/home/player")),
        ];
        let a = [os("--deck"), os("--nxm"), os("nxm://enshrouded/mods/1/files/2")];
        let envs = |cmd: &Command| cmd.get_envs().map(|(k, v)| (k.to_os_string(), v.map(|v| v.to_os_string()))).collect::<Vec<_>>();
        let cmd = restart_command(Path::new("/games/Kiln.AppImage"), true, &a, vars.clone(), &Relaunch::software_fallback());
        assert_eq!(cmd.get_program(), "/games/Kiln.AppImage");
        assert_eq!(cmd.get_args().collect::<Vec<_>>(), a.iter().map(|x| x.as_os_str()).collect::<Vec<_>>());
        let e = envs(&cmd);
        assert!(e.contains(&(os(RESTARTED), Some(os("1")))));
        // the running image's entries do not go along (its mount is not the new start's)
        assert!(e.contains(&(os("APPDIR"), None)) && e.contains(&(os("APPIMAGE"), None)), "{e:?}");
        assert!(e.contains(&(os("LD_LIBRARY_PATH"), Some(os("/opt/host/lib")))), "{e:?}");
        // a plain executable inherits the environment as it is
        let plain = restart_command(Path::new("/usr/bin/kiln-launcher"), false, &a, vars, &Relaunch::software_fallback());
        assert_eq!(envs(&plain), vec![(os(RESTARTED), Some(os("1")))]);
    }

    /// The player's click in a message (crate::dialog) wins over a mode they asked for: the restart leaves the render
    /// arguments and KILN_LAUNCHER_RENDER out; everything else goes along.
    #[test]
    fn a_click_restart_replaces_the_choice_it_answers_and_keeps_the_rest() {
        let os = |s: &str| OsString::from(s);
        let a = [os("--deck"), os(ARG_GPU), os("--no-webkit-sandbox")];
        let cmd = restart_command(Path::new("/usr/bin/kiln-launcher"), false, &a, Vec::new(), &Relaunch::software_by_click());
        assert_eq!(cmd.get_args().collect::<Vec<_>>(), ["--deck", "--no-webkit-sandbox"]);
        let envs: Vec<_> = cmd.get_envs().map(|(k, v)| (k.to_os_string(), v.map(|v| v.to_os_string()))).collect();
        assert!(envs.contains(&(os(ENV), None)) && envs.contains(&(os(RESTARTED), Some(os("1")))), "{envs:?}");
        // the restart's own start decides software mode from the flag, whatever the arguments said before
        let next: Vec<String> = std::iter::once("kiln-launcher".to_string()).chain(cmd.get_args().map(|a| a.to_string_lossy().into_owned())).collect();
        assert_eq!(decide(&next, None, true, false), c(Mode::Software, Why::Restart));
    }

    #[test]
    fn software_mode_keeps_the_users_own_values() {
        let user = |k: &str| (k == "WEBKIT_DISABLE_COMPOSITING_MODE").then(|| OsString::from("0"));
        assert_eq!(software_env(user), vec![("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1"), ("LIBGL_ALWAYS_SOFTWARE", "1")]);
        assert_eq!(software_env(|_| None).len(), 3);
        // what an earlier Kiln set is taken back, nothing else ever is
        assert_eq!(set_by_us(Some("WEBKIT_DISABLE_COMPOSITING_MODE,PATH, LIBGL_ALWAYS_SOFTWARE")), vec!["WEBKIT_DISABLE_COMPOSITING_MODE", "LIBGL_ALWAYS_SOFTWARE"]);
        assert!(set_by_us(None).is_empty());
    }

    #[test]
    fn host_programs_know_every_variable_software_mode_sets() {
        let names: Vec<&str> = SOFTWARE_ENV.iter().map(|(k, _)| *k).chain([DMABUF_VAR, X11_VAR, FORCE_VAR, SHM_VAR]).collect();
        assert_eq!(names, kiln_manage::hostenv::RENDER_VARS, "kiln_manage::hostenv takes back exactly these");
    }


    // ------------------------------------------------------------ X11 (XWayland) on Wayland (0.2.0-dev.30)

    /// decide_x11 on a Wayland session with XWayland: (yours, software, off, restarted, marker, no_help).
    fn x11_of(yours: Option<&str>, software: bool, off: bool, restarted: Option<&str>, marker: bool, no_help: bool) -> X11 {
        decide_x11(yours, true, true, software, off, restarted, marker, no_help)
    }

    #[test]
    fn wayland_without_acceleration_switches_to_x11_once() {
        // a plain Wayland start: GTK's default, and WebKit's own Never there means one switch
        let first = x11_of(None, false, false, None, false, false);
        assert_eq!(first, X11 { on: false, why: X11Why::Default });
        assert_eq!(x11_step(&first, true, false, 2), X11Step::Switch);
        assert_eq!(x11_step(&first, false, false, 2), X11Step::Nothing, "WebKit composites: nothing to do");
        // scale 1: X11 paints as many pixels on the CPU (it never brings the GPU back): no switch, nothing sticks
        assert_eq!(x11_step(&first, true, false, 1), X11Step::Nothing);
        // the switch's restart says which start it is: X11, without a marker yet
        let r = Relaunch::x11_switch();
        assert_eq!(r.set, vec![(X11_RESTARTED, "1".to_string())]);
        let second = x11_of(None, false, false, Some("1"), false, false);
        assert_eq!(second, X11 { on: true, why: X11Why::Restart });
        // X11 is remembered, with the GPU or still without it (X11's CPU painting beat Wayland's: Oryx593), and an X11
        // start never switches again
        assert_eq!(x11_step(&second, false, false, 1), X11Step::Keep, "XWayland's own scale is 1");
        assert_eq!(x11_step(&second, true, false, 1), X11Step::Keep, "still without the GPU through X11: stays on X11");
        assert_eq!(x11_step(&second, true, true, 2), X11Step::Keep);
    }

    #[test]
    fn a_users_own_gdk_backend_always_wins() {
        for v in ["wayland", "x11", "", "wayland,x11"] {
            for (restarted, marker) in [(None, false), (Some("1"), false), (None, true)] {
                let x = x11_of(Some(v), false, false, restarted, marker, false);
                assert_eq!(x, X11 { on: false, why: X11Why::Yours }, "{v:?} {restarted:?} {marker}");
                assert_eq!(x11_step(&x, true, false, 2), X11Step::Nothing);
            }
        }
    }

    #[test]
    fn the_remembered_choice_goes_straight_to_x11() {
        let x = x11_of(None, false, false, None, true, false);
        assert_eq!(x, X11 { on: true, why: X11Why::Marker });
        assert_eq!(x11_step(&x, false, false, 1), X11Step::Nothing, "already remembered: nothing to write");
        assert_eq!(x11_step(&x, true, false, 2), X11Step::Nothing, "without the GPU through X11 too: stays on X11, never a restart");
        // Settings' switch off: the marker no longer counts (set_x11_auto removes it too)
        assert_eq!(x11_of(None, false, true, None, true, false), X11 { on: false, why: X11Why::Off });
        // no help beats an old X11 marker
        assert_eq!(x11_of(None, false, false, None, true, true).why, X11Why::Tried);
    }

    #[test]
    fn x11_never_loops() {
        // X11 did not help (marker, or the way back's variable): Wayland, and the PROBLEM only reports
        for (restarted, no_help) in [(None, true), (Some("0"), false), (Some("0"), true)] {
            let x = x11_of(None, false, false, restarted, false, no_help);
            assert_eq!(x, X11 { on: false, why: X11Why::Tried });
            assert_eq!(x11_step(&x, true, false, 2), X11Step::Nothing);
        }
        // the way back's "0" beats even a stale X11 marker and Settings
        assert_eq!(x11_of(None, false, false, Some("0"), true, false).why, X11Why::Tried);
        // a late page through X11 goes back to Wayland first, before the DMA-BUF and software steps; the way back is "0"
        let gpu = c(Mode::Gpu, Why::Default);
        let on = dm(true, DmabufWhy::Default);
        let x11 = x11_of(None, false, false, Some("1"), false, false);
        assert_eq!(when_late_x11(&gpu, &on, &x11), Late::NoX11);
        let back = Relaunch::x11_back();
        assert_eq!(back.set, vec![(X11_RESTARTED, "0".to_string())]);
        assert_eq!(back.unset, vec![X11_VAR]);
        let wayland = x11_of(None, false, false, Some("0"), false, false);
        assert_eq!(when_late_x11(&gpu, &on, &wayland), Late::NoDmabuf, "then the steps as before");
        assert_eq!(when_late_x11(&gpu, &dm(false, DmabufWhy::Restart), &wayland), Late::Restart);
        // software mode never draws through X11
        let sw = x11_of(None, true, false, Some("1"), true, false);
        assert_eq!(sw, X11 { on: false, why: X11Why::Software });
        assert_eq!(when_late_x11(&c(Mode::Software, Why::Restart), &dm(false, DmabufWhy::Software), &sw), Late::Report);
        // the restart command carries the variable and leaves our GDK_BACKEND out
        let a = [OsString::from("--deck")];
        let vars = vec![(OsString::from(X11_VAR), OsString::from("x11")), (OsString::from(SET_BY_US), OsString::from(X11_VAR))];
        let cmd = restart_command(Path::new("/games/Kiln.AppImage"), true, &a, vars.clone(), &back);
        let envs: Vec<_> = cmd.get_envs().map(|(k, v)| (k.to_os_string(), v.map(|v| v.to_os_string()))).collect();
        assert!(envs.contains(&(OsString::from(X11_VAR), None)), "{envs:?}");
        assert!(envs.contains(&(OsString::from(X11_RESTARTED), Some(OsString::from("0")))), "{envs:?}");
    }

    #[test]
    fn x11_needs_a_wayland_session_with_xwayland() {
        assert_eq!(decide_x11(None, false, true, false, false, None, true, false).why, X11Why::Unavailable, "X11 session");
        assert_eq!(decide_x11(None, true, false, false, false, Some("1"), true, false).why, X11Why::Unavailable, "no XWayland");
        assert!(!decide_x11(None, true, false, false, false, Some("1"), true, false).on);
    }

    #[test]
    fn a_variable_that_turns_compositing_off_is_no_reason_to_switch() {
        let x = x11_of(None, false, false, None, false, false);
        assert_eq!(x11_step(&x, true, true, 2), X11Step::Nothing);
    }

    #[test]
    fn the_drm_facts_name_what_webkit_needs() {
        let ok = vec![("renderD128".to_string(), true)];
        assert!(drm_reasons(&ok, Some("Y")).is_empty());
        assert!(drm_reasons(&ok, None).is_empty(), "no NVIDIA module: nothing about it");
        assert!(drm_reasons(&ok, Some("N"))[0].contains("nvidia_drm modeset=N"));
        assert!(drm_reasons(&[], None)[0].contains("no DRM render node"));
        assert!(drm_reasons(&[("renderD128".into(), false)], None)[0].contains("this user can open"));
        assert_eq!(says_drm(&[("renderD128".into(), true), ("renderD129".into(), false)], Some("Y")),
            "DRM: renderD128 (opens), renderD129 (cannot open), nvidia_drm modeset=Y");
        assert_eq!(says_drm(&[], None), "DRM: no render node");
    }

    #[test]
    fn x11_is_ours_to_take_back_only_when_we_named_it() {
        assert_eq!(set_by_us(Some("WEBKIT_DISABLE_DMABUF_RENDERER,GDK_BACKEND")), vec![DMABUF_VAR, X11_VAR]);
        assert!(set_by_us(Some("WEBKIT_DISABLE_DMABUF_RENDERER")).iter().all(|k| *k != X11_VAR), "the user's own stays");
    }

    // ------------------------------------------------------------ WebKit's DMA-BUF renderer

    #[test]
    fn dmabuf_is_on_by_default_and_that_start_is_guarded() {
        let d = dmabuf_of(&[], None, None);
        assert_eq!(d, dm(true, DmabufWhy::Default));
        assert!(d.guarded(), "a default DMA-BUF start writes the pending marker");
        assert!(!d.sets_var(), "the default sets no variable");
        assert_eq!(dmabuf_of(&["--deck", "--dmabuf-renderer"], None, None), dm(true, DmabufWhy::Default));
    }

    #[test]
    fn the_users_own_webkit_variable_always_wins() {
        // WebKit reads any value but "0" as off (strcmp with "0"), an empty one too
        for (v, on) in [("1", false), ("0", true), ("", false), ("yes", false)] {
            let d = decide_dmabuf(&args(&[ARG_DMABUF, ARG_NO_DMABUF]), Some(v), Some("on"), true, true, true, true);
            assert_eq!((d.on, d.why), (on, DmabufWhy::Yours), "{v:?}");
            assert!(!d.guarded() && !d.sets_var(), "the user's value is theirs: no guard, nothing set over it");
        }
    }

    #[test]
    fn a_start_that_died_turns_it_off_and_the_sticky_marker_keeps_it_off() {
        let crashed = decide_dmabuf(&args(&[]), None, None, false, false, true, false);
        assert_eq!(crashed, dm(false, DmabufWhy::Crashed));
        assert!(crashed.sets_var() && !crashed.guarded());
        assert_eq!(decide_dmabuf(&args(&[]), None, None, false, false, false, true), dm(false, DmabufWhy::Marker));
        assert_eq!(decide_dmabuf(&args(&[]), None, None, false, false, true, true), dm(false, DmabufWhy::Crashed));
        assert!(decide_dmabuf(&args(&[]), None, None, false, false, false, true).sets_var());
    }

    #[test]
    fn the_overrides_win_over_the_markers_and_the_restart() {
        let on = decide_dmabuf(&args(&[ARG_DMABUF]), None, None, false, true, true, true);
        assert_eq!(on, dm(true, DmabufWhy::Argument));
        assert!(!on.guarded(), "a DMA-BUF the user asked for is not switched off by the launcher");
        let off = decide_dmabuf(&args(&[ARG_NO_DMABUF]), None, None, false, false, false, false);
        assert_eq!(off, dm(false, DmabufWhy::Argument));
        assert!(off.sets_var());
        // the last argument wins; the arguments win over the variable
        assert_eq!(dmabuf_of(&[ARG_NO_DMABUF, "--deck", ARG_DMABUF], None, None), dm(true, DmabufWhy::Argument));
        assert_eq!(dmabuf_of(&[ARG_NO_DMABUF], None, Some("on")), dm(false, DmabufWhy::Argument));
        assert_eq!(decide_dmabuf(&args(&[]), None, Some("on"), false, true, true, true), dm(true, DmabufWhy::Environment));
        assert_eq!(dmabuf_of(&[], None, Some(" OFF ")), dm(false, DmabufWhy::Environment));
        // an empty value is no value; an unknown one is said and ignored
        assert_eq!(dmabuf_of(&[], None, Some("")), dm(true, DmabufWhy::Default));
        let odd = dmabuf_of(&[], None, Some("maybe"));
        assert_eq!((odd.on, odd.why), (true, DmabufWhy::Default));
        assert!(odd.ignored.as_deref().is_some_and(|i| i.contains("KILN_LAUNCHER_DMABUF=maybe")), "{odd:?}");
    }

    #[test]
    fn software_mode_keeps_dmabuf_off_as_before() {
        let s = decide_dmabuf(&args(&[ARG_DMABUF]), None, Some("on"), true, false, false, false);
        assert_eq!((s.on, s.why), (false, DmabufWhy::Software));
        assert!(s.sets_var() && !s.guarded());
        // the user's own value still wins
        assert_eq!(decide_dmabuf(&args(&[]), Some("0"), None, true, false, false, false).why, DmabufWhy::Yours);
    }

    /// The watchdog's steps: DMA-BUF on, then the GPU without DMA-BUF, then software; one restart per step, and the
    /// restart's variable alone (a marker that could not be written) keeps each step from coming back.
    #[test]
    fn the_watchdog_steps_dmabuf_then_gpu_then_software_and_never_loops() {
        let gpu = c(Mode::Gpu, Why::Default);
        let first = dmabuf_of(&[], None, None);
        assert_eq!(when_late(&gpu, &first), Late::NoDmabuf);
        let r = Relaunch::dmabuf_fallback();
        assert!(r.set.contains(&(DMABUF_RESTARTED, "1".to_string())), "{r:?}");
        assert!(!r.set.iter().any(|(k, _)| *k == RESTARTED), "the first step does not ask for software mode");
        // the restart: the GPU, without DMA-BUF (no marker read: it may not have been written)
        let second = decide_dmabuf(&args(&[]), None, None, false, true, false, false);
        assert_eq!(second, dm(false, DmabufWhy::Restart));
        assert!(second.sets_var() && !second.guarded());
        assert_eq!(when_late(&gpu, &second), Late::Restart, "late again: software mode");
        // software mode: only reports
        let third_render = decide(&args(&[]), None, true, false);
        let third = decide_dmabuf(&args(&[]), None, None, true, true, false, false);
        assert_eq!(when_late(&third_render, &third), Late::Report);
        // a start already without DMA-BUF goes straight to software
        for d in [dm(false, DmabufWhy::Marker), dm(false, DmabufWhy::Crashed), dm(false, DmabufWhy::Argument), dm(false, DmabufWhy::Yours)] {
            assert_eq!(when_late(&gpu, &d), Late::Restart, "{d:?}");
        }
        // DMA-BUF the user asked for: no DMA-BUF step (theirs), software as before
        for d in [dm(true, DmabufWhy::Argument), dm(true, DmabufWhy::Environment), dm(true, DmabufWhy::Yours)] {
            assert_eq!(when_late(&gpu, &d), Late::Restart, "{d:?}");
        }
        // a render mode the user forced: only reports, whatever DMA-BUF does
        assert_eq!(when_late(&c(Mode::Gpu, Why::Argument), &first), Late::Report);
        assert_eq!(when_late(&c(Mode::Gpu, Why::Environment), &first), Late::Report);
    }

    #[test]
    fn the_dmabuf_restart_keeps_the_arguments_and_says_its_step() {
        let os = |s: &str| OsString::from(s);
        let a = [os("--deck"), os(ARG_GPU)];
        let cmd = restart_command(Path::new("/usr/bin/kiln-launcher"), false, &a, Vec::new(), &Relaunch::dmabuf_fallback());
        assert_eq!(cmd.get_args().collect::<Vec<_>>(), ["--deck", ARG_GPU]);
        let envs: Vec<_> = cmd.get_envs().map(|(k, v)| (k.to_os_string(), v.map(|v| v.to_os_string()))).collect();
        assert_eq!(envs, vec![(os(DMABUF_RESTARTED), Some(os("1")))]);
    }

    #[test]
    fn the_dmabuf_markers_live_next_to_the_render_marker() {
        let t = std::env::temp_dir().join(format!("kl-dmabuf-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let d = DataDirs::at(&t);
        assert_eq!(dmabuf_pending(&d), t.join("state").join("dmabuf-pending.json"));
        assert_eq!(dmabuf_marker(&d), t.join("state").join("dmabuf-off.json"));
        // the start guard's file, then what the next start makes of it: a sentence with the dead start's time
        write_marker_file(&dmabuf_pending(&d), "test start").unwrap();
        let at = kiln_manage::util::read_json::<Marker>(&dmabuf_pending(&d)).unwrap().unwrap().at;
        let why = dmabuf_crash_why(&dmabuf_pending(&d));
        assert!(why.contains(&at) && why.contains("DMA-BUF") && why.starts_with("The launcher turned this on by itself"), "{why}");
        // a pending file that is not readable as one still counts (the start died); the sentence says less
        std::fs::write(dmabuf_pending(&d), b"{").unwrap();
        assert!(dmabuf_crash_why(&dmabuf_pending(&d)).contains("DMA-BUF"));
        write_marker_file(&dmabuf_marker(&d), &why).unwrap();
        assert!(marker_file_why(&dmabuf_marker(&d)).is_some_and(|w| w.starts_with(&why)));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn the_dmabuf_variable_the_launcher_set_is_taken_back() {
        assert_eq!(set_by_us(Some("WEBKIT_DISABLE_DMABUF_RENDERER,PATH")), vec![DMABUF_VAR]);
        assert_eq!(set_by_us(Some("LIBGL_ALWAYS_SOFTWARE, WEBKIT_DISABLE_DMABUF_RENDERER")), vec!["LIBGL_ALWAYS_SOFTWARE", DMABUF_VAR]);
    }

    #[test]
    fn the_nvidia_line_keeps_the_version_and_not_the_build_host() {
        let open = "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  615.78.08  Release Build  (root@players-pc)  Sat Oct 4 2026\nGCC version: gcc 15\n";
        assert_eq!(nvidia_line(open).as_deref(), Some("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64 615.78.08 Release Build Sat Oct 4 2026"));
        assert_eq!(nvidia_line("").as_deref(), None);
    }

    #[test]
    fn the_watchdog_waits_from_the_window_and_sees_a_ready_call() {
        let w = Watch::new();
        let begun = Instant::now();
        // no window yet: the longer wait runs from the start
        assert_eq!(w.wait(begun, Duration::from_millis(10), Duration::from_millis(30)), Seen::Late { window: false });
        assert!(begun.elapsed() >= Duration::from_millis(30));
        w.window_created();
        let t = Instant::now();
        assert_eq!(w.wait(begun, Duration::from_millis(40), Duration::from_millis(0)), Seen::Late { window: true });
        assert!(t.elapsed() >= Duration::from_millis(30), "the timeout counts from the window");
        // a ready call from another thread ends the wait at once
        let w = std::sync::Arc::new(Watch::new());
        w.window_created();
        let w2 = w.clone();
        let h = std::thread::spawn(move || { std::thread::sleep(Duration::from_millis(20)); assert!(w2.page_started()); assert!(!w2.page_started()); });
        let t = Instant::now();
        assert!(matches!(w.wait(Instant::now(), Duration::from_secs(20), Duration::from_secs(20)), Seen::Started(_)));
        assert!(t.elapsed() < Duration::from_secs(5));
        h.join().unwrap();
    }

    // ------------------------------------------------ forcing past Ubuntu WebKit's NVIDIA block (0.2.0-dev.30.7)

    #[test]
    fn forcing_past_ubuntus_nvidia_block_is_opt_in_on_shared_memory() {
        use ForceWhy as W;
        let f = decide_force;
        assert_eq!(f(false, false, true, false), ForceDmabuf { step: Force::Off, why: W::Off });
        assert_eq!(f(true, false, true, false), ForceDmabuf { step: Force::Shm, why: W::On });
        // the start before with it failed (its guard left, or its page late): nothing forced now
        assert_eq!(f(true, false, true, true), ForceDmabuf { step: Force::Off, why: W::Failed });
        // the user's own variables win; without WebKit's DMA-BUF renderer (software mode, its off switch) nothing to force
        assert_eq!(f(true, true, true, false), ForceDmabuf { step: Force::Off, why: W::Yours });
        assert_eq!(f(true, false, false, false), ForceDmabuf { step: Force::Off, why: W::Unused });
        assert_eq!(f(false, false, false, true).why, W::Off);
    }

    /// A setting that ends the launcher at its start must never lock the player out of it: the forced start's guard, left
    /// behind, makes the next start force nothing and turn the switch off with a note Settings shows.
    #[test]
    fn a_forced_start_that_dies_turns_the_switch_off() {
        let t = std::env::temp_dir().join(format!("kl-force-guard-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let d = DataDirs::at(&t);
        assert_eq!(force_pending(&d), t.join("state").join("force-dmabuf-pending.json"));
        // never set: the default; Settings turns it on (and takes away what would stand in its way)
        assert_eq!(force_chosen(&force_choice(&d)), FORCE_DEFAULT);
        write_marker_file(&dmabuf_marker(&d), "an earlier crash").unwrap();
        write_marker_file(&x11_marker(&d), "X11 helped").unwrap();
        force_switch_files(&d, true).unwrap();
        assert!(force_chosen(&force_choice(&d)) && !dmabuf_marker(&d).exists() && !x11_marker(&d).exists());
        let start = |d: &DataDirs| decide_force(force_chosen(&force_choice(d)), false, true, force_pending(d).is_file());
        assert_eq!(start(&d).step, Force::Shm);
        // the forced start writes its guard and dies before its page started (Gdk Error 71)
        write_marker_file(&force_pending(&d), "test forced start").unwrap();
        let at = kiln_manage::util::read_json::<Marker>(&force_pending(&d)).unwrap().unwrap().at;
        // the next start forces nothing; once the data folder opens the switch goes off with the dead start's time
        assert_eq!(start(&d), ForceDmabuf { step: Force::Off, why: ForceWhy::Failed });
        let why = force_crash_why(&force_pending(&d));
        assert!(why.starts_with("The launcher turned this off by itself") && why.contains(&at), "{why}");
        force_gave_up(&d, &why).unwrap();
        clear_marker_file(&force_pending(&d)).unwrap();
        assert!(!force_chosen(&force_choice(&d)));
        assert!(marker_file_why(&force_failed(&d)).is_some_and(|w| w.starts_with(&why)));
        assert_eq!(start(&d), ForceDmabuf { step: Force::Off, why: ForceWhy::Off });
        // the player turns it on again: the note goes, the next start forces again
        force_switch_files(&d, true).unwrap();
        assert!(!force_failed(&d).exists());
        assert_eq!(start(&d).step, Force::Shm);
        // a guard that is not readable still counts (the start died); the sentence says less
        std::fs::write(force_pending(&d), b"{").unwrap();
        assert_eq!(start(&d).why, ForceWhy::Failed);
        assert!(force_crash_why(&force_pending(&d)).contains("ended before its page had started"));
        std::fs::remove_dir_all(&t).unwrap();
    }

    /// Run prepare before any renderer or sandbox initialization, with no other tests sharing its environment or
    /// OnceLocks. folder::process_env gives each child a fresh synthetic data folder, never the player's settings.
    #[cfg(target_os = "linux")]
    #[test]
    fn forced_startup_is_guarded_before_any_webkit_use() {
        const CASE: &str = "KILN_TEST_FORCED_STARTUP";
        if let Ok(case) = std::env::var(CASE) {
            let dirs = crate::folder::found_dirs().unwrap();
            let manual_on = ["success", "guard-denied", "software", "failed-off-write"].contains(&case.as_str());
            if manual_on { force_switch_files(&dirs, true).unwrap(); }
            if ["manual-off", "recovered-off"].contains(&case.as_str()) { force_switch_files(&dirs, false).unwrap(); }
            let context = startup_context(false);
            let fixture_context = if case == "other-backend" { "another installation|x11" } else { &context };
            if !manual_on && case != "fresh" {
                write_startup_guard(&normal_pending(&dirs), fixture_context, case != "hidden", "synthetic normal start never painted").unwrap();
            }
            if ["shm-failed", "failed-recovered"].contains(&case.as_str()) {
                write_startup_guard(&force_pending(&dirs), &context, true, "synthetic SHM start never painted").unwrap();
            }
            if ["recovered", "recovered-off", "failed-recovered", "other-backend"].contains(&case.as_str()) {
                write_startup_guard(&force_recovered(&dirs), fixture_context, true, "synthetic successful recovery").unwrap();
            }
            if case == "sticky-dmabuf" { write_marker_file(&dmabuf_marker(&dirs), "an internal fallback").unwrap(); }
            if ["guard-denied", "auto-guard-denied"].contains(&case.as_str()) { std::fs::create_dir(force_pending(&dirs)).unwrap(); }
            if case == "unwritable-escape" { std::fs::create_dir(force_choice(&dirs)).unwrap(); }
            let args = if case == "software" { args(&[ARG_SOFTWARE]) } else if ["escape", "unwritable-escape"].contains(&case.as_str()) { args(&[ARG_NO_DMABUF]) } else { args(&[]) };
            prepare_for_driver(&args, case != "non-nvidia");
            match case.as_str() {
                "failed-off-write" => {
                    assert_eq!(force().unwrap().step, Force::Shm);
                    std::fs::create_dir(force_failed(&dirs)).unwrap();
                    assert!(force_gave_up(&dirs, "synthetic late SHM startup").is_err());
                    before_restart(&Relaunch::force_fallback());
                    assert!(force_pending(&dirs).is_file(), "keep the failed SHM guard until the off choice is durable");
                }
                "success" | "automatic" | "recovered" | "sticky-dmabuf" => {
                    assert_eq!(force().unwrap().step, Force::Shm);
                    assert!(force_pending(&dirs).is_file(), "the guard must exist before even sandbox::prepare can use WebKit");
                    assert!(dmabuf().unwrap().on);
                    for (k, v) in force_env(Force::Shm) {
                        assert_eq!(std::env::var(k).unwrap(), v);
                        assert!(set_by_us(std::env::var(SET_BY_US).ok().as_deref()).contains(&k));
                    }
                    page_started();
                    assert!(force_pending(&dirs).is_file(), "page navigation alone does not prove painting");
                    frame_painted(false, false);
                    assert!(!expected_frame(&force_pending(&dirs), &context), "an intentional hidden window is no failed visible attempt");
                    frame_painted(false, true);
                    assert!(expected_frame(&force_pending(&dirs), &context));
                    let note = frame_painted(true, true);
                    assert_eq!(note.is_some(), ["automatic", "sticky-dmabuf"].contains(&case.as_str()));
                    assert!(frame_painted(true, true).is_none(), "the explanation is shown once");
                    leaving();
                    if !manual_on {
                        assert!(expected_frame(&force_recovered(&dirs), &context));
                        assert_eq!(force_setting(&force_choice(&dirs)), None, "automatic recovery is not a manual opt-in");
                    }
                    assert!(!force_pending(&dirs).exists(), "a successful clean exit clears its guard");
                    assert!(!normal_pending(&dirs).exists());
                }
                _ => {
                    assert_eq!(force().unwrap().step, Force::Off, "no guarded GPU start is possible: {case}");
                    assert!(std::env::var_os(FORCE_VAR).is_none());
                    assert!(std::env::var_os(SHM_VAR).is_none());
                    if ["shm-failed", "failed-recovered"].contains(&case.as_str()) {
                        after_gate();
                        assert_eq!(force_setting(&force_choice(&dirs)), Some(false));
                        assert!(!force_pending(&dirs).exists());
                        assert!(!force_recovered(&dirs).exists());
                        assert!(!normal_pending(&dirs).exists());
                    }
                    if case == "fresh" {
                        assert!(!FORCE_DEFAULT);
                        assert!(normal_pending(&dirs).is_file());
                        page_started();
                        assert!(normal_pending(&dirs).is_file());
                        frame_painted(false, false);
                        assert!(!expected_frame(&normal_pending(&dirs), &context));
                        frame_painted(true, true);
                        leaving();
                        assert!(!normal_pending(&dirs).exists());
                        assert!(!force_recovered(&dirs).exists(), "normal painting must not enable SHM");
                    }
                }
            }
            std::fs::remove_dir_all(dirs.root.parent().unwrap()).unwrap();
            return;
        }
        let mut failures = Vec::new();
        for case in ["success", "guard-denied", "software", "automatic", "non-nvidia", "manual-off", "escape", "shm-failed", "recovered", "fresh", "hidden", "other-backend", "recovered-off", "failed-recovered", "sticky-dmabuf", "restart-escape", "failed-off-write", "auto-guard-denied", "unwritable-escape"] {
            let mut cmd = Command::new(std::env::current_exe().unwrap());
            cmd.args(["--exact", "render::tests::forced_startup_is_guarded_before_any_webkit_use", "--nocapture"])
                .env(CASE, case);
            for name in kiln_manage::hostenv::RENDER_VARS.into_iter().chain([
                ENV, DMABUF_ENV, SET_BY_US, RESTARTED, DMABUF_RESTARTED, X11_RESTARTED, FORCE_RESTARTED, TEST_WITHHOLD,
            ]) {
                cmd.env_remove(name);
            }
            // A user's DMA-BUF=0 must not make the opt-in apply in software mode.
            if case == "software" { cmd.env(DMABUF_VAR, "0"); }
            if case == "restart-escape" { cmd.env(FORCE_RESTARTED, "off"); }
            let out = cmd.output().unwrap();
            if !out.status.success() {
                failures.push(format!("{case}: {}\n{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr)));
            }
        }
        assert!(failures.is_empty(), "{}", failures.join("\n"));
    }

    #[test]
    fn the_forced_variables_are_the_launchers_to_take_back() {
        assert_eq!(set_by_us(Some("WEBKIT_FORCE_DMABUF_RENDERER,WEBKIT_DMABUF_RENDERER_FORCE_SHM,HOME")), vec![FORCE_VAR, SHM_VAR]);
        assert_eq!(force_env(Force::Shm), vec![(FORCE_VAR, "1"), (SHM_VAR, "1")], "never the plain forced renderer");
        assert!(force_env(Force::Off).is_empty());
        // every variable the launcher sets for its own webview is one a host program never gets
        for k in SOFTWARE_ENV.iter().map(|(k, _)| *k).chain([DMABUF_VAR, X11_VAR, FORCE_VAR, SHM_VAR]) {
            assert!(kiln_manage::hostenv::RENDER_VARS.contains(&k), "{k}");
        }
    }

    #[test]
    fn a_late_forced_start_steps_down_first() {
        let c = decide(&[], None, false, false);
        let d = decide_dmabuf(&[], None, None, false, false, false, false);
        let x = X11 { on: true, why: X11Why::Restart };
        let forced = ForceDmabuf { step: Force::Shm, why: ForceWhy::On };
        let none = ForceDmabuf { step: Force::Off, why: ForceWhy::Off };
        assert_eq!(when_late_forced(&c, &d, &x, &forced), Late::NoForce);
        assert_eq!(when_late_forced(&c, &d, &x, &none), Late::NoX11);
        assert_eq!(when_late_forced(&c, &d, &X11 { on: false, why: X11Why::Default }, &none), Late::NoDmabuf);
        // a forced start has its own guard: it turns the switch off, never WebKit's DMA-BUF renderer
        assert!(!guarded(&d, &forced) && guarded(&d, &none));
        assert_eq!(Relaunch::force_fallback().set, vec![(FORCE_RESTARTED, "off".to_string())]);
    }

    #[test]
    fn ubuntus_nvidia_block_is_named_in_the_why() {
        let r = nvidia_block_reason(Some(true), Some("NVIDIA Corporation"), true, false).unwrap();
        assert!(r.contains("disable-nvidia-dmabuf.patch") && r.contains("GL vendor NVIDIA Corporation"), "{r}");
        assert!(r.contains("Settings > Advanced > Drawing the launcher > \"Force GPU drawing on NVIDIA\""), "{r}");
        // it never sends the player the other way, or to root
        for no in ["WEBKIT_DISABLE_DMABUF_RENDERER", "chmod", "sudo", "root"] {
            assert!(!r.contains(no), "{no}: {r}");
        }
        // the driver alone when GL could not be asked
        assert!(nvidia_block_reason(Some(true), None, true, false).unwrap().contains("NVIDIA's driver is loaded"));
        // not NVIDIA's GL, no driver, not this build, unknown, or forced already: not this cause
        assert_eq!(nvidia_block_reason(Some(true), Some("Mesa"), true, false), None);
        assert_eq!(nvidia_block_reason(Some(true), None, false, false), None);
        assert_eq!(nvidia_block_reason(Some(false), Some("NVIDIA Corporation"), true, false), None);
        assert_eq!(nvidia_block_reason(None, Some("NVIDIA Corporation"), true, false), None);
        assert_eq!(nvidia_block_reason(Some(true), Some("NVIDIA Corporation"), true, true), None);
    }

    #[test]
    fn the_patch_is_found_in_the_library_whatever_the_chunks() {
        let marker = FORCE_VAR.as_bytes();
        let mut lib = vec![0u8; 3 * MARKER_CHUNK + 10];
        let at = MARKER_CHUNK - 5; // across a chunk boundary
        lib[at..at + marker.len()].copy_from_slice(marker);
        assert!(has_marker(&lib[..], marker).unwrap());
        assert!(!has_marker(&vec![b'W'; 3 * MARKER_CHUNK][..], marker).unwrap());
        assert!(!has_marker(&b"WEBKIT_FORCE_DMABUF_RENDERE"[..], marker).unwrap());
        assert!(has_marker(&b"WEBKIT_FORCE_DMABUF_RENDERER"[..], marker).unwrap());
    }

    #[test]
    fn the_switch_lives_in_its_file() {
        let t = std::env::temp_dir().join(format!("kl-force-{}", std::process::id()));
        std::fs::create_dir_all(&t).unwrap();
        let choice = t.join("force-dmabuf.json");
        assert_eq!(force_chosen(&choice), FORCE_DEFAULT, "never set: the default");
        write_force_choice(&choice, true).unwrap();
        assert!(force_chosen(&choice));
        write_force_choice(&choice, false).unwrap();
        assert!(!force_chosen(&choice));
        std::fs::write(&choice, b"not json").unwrap();
        assert_eq!(force_chosen(&choice), FORCE_DEFAULT, "a file that is not one: the default");
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn glib_lines_go_into_the_log_short_and_only_the_loud_ones() {
        use gtk::glib::LogLevel;
        assert_eq!(glib_line(Some("Gdk"), LogLevel::Warning, "Error 71 (Protocol error) dispatching to Wayland display.").as_deref(),
            Some("Gdk WARNING: Error 71 (Protocol error) dispatching to Wayland display."));
        assert_eq!(glib_line(None, LogLevel::Critical, "x").as_deref(), Some("CRITICAL: x"));
        assert_eq!(glib_line(Some("WebKit"), LogLevel::Debug, "noise"), None);
        assert_eq!(glib_line(Some("WebKit"), LogLevel::Info, "noise"), None);
        let long = glib_line(Some("Gtk"), LogLevel::Message, &"y\n".repeat(400)).unwrap();
        assert!(long.starts_with("Gtk MESSAGE: y y") && long.ends_with("...") && !long.contains('\n'), "{long}");
        assert!(long.len() <= "Gtk MESSAGE: ".len() + GLIB_LINE_MAX + "...".len(), "{}", long.len());
    }
}
