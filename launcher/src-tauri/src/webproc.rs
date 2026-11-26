//! The page's web process and the window's close (0.2.0-dev.31.13).
//!
//! On Linux the page runs in WebKitGTK's own process (WebKitWebProcess). When that process ends (WebKit's
//! `web-process-terminated`: it crashed, went over WebKit's memory limit, or was ended through WebKit's API), the window
//! stayed black and could not be closed (31.10, a box at load 54): the page's close guard (launcher/src/shell/
//! closeGuard.ts) listens to `tauri://close-requested`, so Tauri holds every close for the page, and a dead page never
//! answers. So:
//! - **recover**: the first end reloads the page once ([`Recovery`]); a second end within [`RETRY_WINDOW`] of that reload
//!   puts a native GTK screen in the window instead ("The launcher's page crashed.", Reload and Quit), never a loop;
//! - **always close**: a close while the page is dead quits at once; else the page has [`CLOSE_ANSWER_WITHIN`] to say it
//!   took the close (`app_close_seen`), a second close before it answered quits at once, and a page that said it closes
//!   the window but did not quits too ([`Life`]). A page asking about unsaved work keeps the window (the player's
//!   choice). The launcher has no tray: closing the window quits it;
//! - **diagnostics**: one line in launcher.log per end ([`says_end`]): the reason, the web process's last memory use
//!   (sampled from /proc every [`SAMPLE_EVERY`]), uptimes, the system's load and free memory, what happens next;
//! - **running state**: only the page is reloaded; the backend (its tasks, downloads, installs, a dedicated server it
//!   started, watchers) never stops, and the new page reads its state back through the same commands as at a start.
//!
//! docs/kiln/launcher/app.md, "When the page's web process ends (Linux)".

use std::sync::Mutex;
use std::time::{Duration, Instant};

/// A second end this soon after the automatic reload shows the fallback screen instead of reloading again.
pub const RETRY_WINDOW: Duration = Duration::from_secs(60);
/// How long the page has to say it took a close request (`app_close_seen`), or to close the window after saying so.
/// Under heavy load the page's answer can take a moment; past this the window closes without it.
pub const CLOSE_ANSWER_WITHIN: Duration = Duration::from_secs(5);
/// How long a page that said it has unsaved work (`app_close_unsaved`) has to answer a close: a live page on a box at
/// load 54 may need more than [`CLOSE_ANSWER_WITHIN`], and quitting then would throw the work away. A hung page still
/// lets the window go after this.
pub const CLOSE_UNSAVED_WITHIN: Duration = Duration::from_secs(30);
/// Whether a close is taken away from a dead or silent page: Linux only, where the page's web process is watched
/// (`linux::watch`). On Windows and macOS Tauri and the page's close guard close the window exactly as before 31.13.
pub const FORCES_CLOSE: bool = cfg!(target_os = "linux");
/// Commands of the close path, open behind every gate of the data folder (lib.rs `gated`): closing never waits for it.
pub const CLOSE_COMMANDS: &[&str] = &["app_close_seen", "app_close_unsaved"];
/// How often the web process's memory is sampled (Linux).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub const SAMPLE_EVERY: Duration = Duration::from_secs(10);

/// Why WebKit says the page's web process ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ended {
    Crashed,
    ExceededMemoryLimit,
    TerminatedByApi,
    Other(i32),
}

impl Ended {
    /// What launcher.log says after "the page's web process ended (…)", as before 31.13 (WebKit's own names).
    pub fn name(self) -> String {
        match self {
            Ended::Crashed => "Crashed".into(),
            Ended::ExceededMemoryLimit => "ExceededMemoryLimit".into(),
            Ended::TerminatedByApi => "TerminatedByApi".into(),
            Ended::Other(n) => format!("unknown reason {n}"),
        }
    }
}

/// What to do about an end.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// Load the page again (a new web process).
    Reload,
    /// The native fallback screen: the player reloads or quits.
    Fallback,
}

/// The page's ends this start and the one automatic reload.
#[derive(Debug, Default, Clone)]
pub struct Recovery {
    /// Ends this start.
    pub ends: u32,
    /// When the page was last loaded again (automatically or with the fallback's Reload).
    reloaded: Option<Instant>,
    /// The fallback screen is up.
    pub fallback: bool,
}

impl Recovery {
    pub const fn new() -> Recovery {
        Recovery { ends: 0, reloaded: None, fallback: false }
    }

    /// The web process ended at `now`.
    pub fn ended(&mut self, now: Instant) -> Step {
        self.ends += 1;
        if self.fallback {
            return Step::Fallback;
        }
        let soon = self.reloaded.is_some_and(|t| now.saturating_duration_since(t) < RETRY_WINDOW);
        if soon {
            self.fallback = true;
            Step::Fallback
        } else {
            self.reloaded = Some(now);
            Step::Reload
        }
    }

    /// The fallback's Reload: the page loads again; another end within [`RETRY_WINDOW`] shows the fallback again.
    pub fn reload_clicked(&mut self, now: Instant) {
        self.fallback = false;
        self.reloaded = Some(now);
    }
}

/// What a close request does now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Close {
    /// Nothing here: Tauri and the page's close guard close the window as before 31.13 (Windows and macOS, where the
    /// page's web process is never watched, or the launcher already quits).
    Tauri,
    /// Quit without the page (why, for launcher.log).
    Now(&'static str),
    /// The page gets the request (its close guard); this request's number, for [`Life::after_wait`].
    Page(u64),
    /// The page has unsaved work and has not answered the last request yet: that request's wait goes on and decides.
    Waiting,
}

/// [`Life::after_wait`]: what the wait for a close request ends with.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AfterWait {
    /// Nothing (the page answered and asks the player, a newer request decides, or the launcher quits already).
    Done,
    /// Quit without the page (why, for launcher.log).
    Quit(&'static str),
    /// Wait this much longer: the page said it has unsaved work and is not dead, so it gets [`CLOSE_UNSAVED_WITHIN`].
    Again(Duration),
}

/// The page as the close path sees it.
#[derive(Debug, Default, Clone)]
pub struct Life {
    /// The web process ended and no new page has said it started since.
    pub dead: bool,
    /// The page's last word on its work (`app_close_unsaved`): closing now would lose some. A page that ended lost it.
    unsaved: bool,
    /// The last close request handed to the page, and when.
    asked: u64,
    asked_at: Option<Instant>,
    /// The last request the page answered, and whether it asks the player (unsaved work) or closes the window.
    answered: u64,
    asking: bool,
    /// The launcher quits (a forced close, the fallback's Quit, or any exit): never a second quit.
    quitting: bool,
}

impl Life {
    pub const fn new() -> Life {
        Life { dead: false, unsaved: false, asked: 0, asked_at: None, answered: 0, asking: false, quitting: false }
    }

    /// The web process ended (its unsaved work went with it).
    pub fn page_ended(&mut self) {
        self.dead = true;
        self.unsaved = false;
    }

    /// A page said it started (`app_started`).
    pub fn page_started(&mut self) {
        self.dead = false;
    }

    /// The page says whether closing now would lose work (`app_close_unsaved`, sent whenever that changes).
    pub fn page_unsaved(&mut self, unsaved: bool) {
        if !self.dead {
            self.unsaved = unsaved;
        }
    }

    /// The window's close button (or the desktop's close). `forces`: this platform takes the close away from a dead or
    /// silent page ([`FORCES_CLOSE`]: Linux only).
    pub fn close_requested(&mut self, now: Instant, fallback: bool, forces: bool) -> Close {
        if !forces || self.quitting {
            return Close::Tauri;
        }
        if fallback {
            return Close::Now("the fallback screen is up");
        }
        if self.dead {
            return Close::Now("the page's web process has ended");
        }
        let open = self.answered < self.asked;
        if open && self.unsaved {
            // a slow page with unsaved work: more clicks neither force the close nor make the wait longer
            return Close::Waiting;
        }
        if open && self.asked_at.is_some_and(|t| now.saturating_duration_since(t) < CLOSE_ANSWER_WITHIN) {
            return Close::Now("closed again before the page answered");
        }
        self.asked += 1;
        self.asked_at = Some(now);
        Close::Page(self.asked)
    }

    /// The page took the close request (`app_close_seen`): `asking` the player about unsaved work, or closing.
    pub fn page_answered(&mut self, asking: bool) {
        self.answered = self.asked;
        self.asking = asking;
    }

    /// The wait for request `id` is over at `now`, the window still there: quit without the page, wait more, or nothing.
    /// A newer request's own wait decides, a page asking the player keeps the window, and a page that said it has
    /// unsaved work (and has not ended) gets [`CLOSE_UNSAVED_WITHIN`] to answer before the launcher quits without it.
    pub fn after_wait(&self, id: u64, now: Instant) -> AfterWait {
        if id != self.asked || self.quitting {
            return AfterWait::Done;
        }
        if self.answered < id {
            let waited = self.asked_at.map_or(CLOSE_UNSAVED_WITHIN, |t| now.saturating_duration_since(t));
            if self.unsaved && !self.dead && waited < CLOSE_UNSAVED_WITHIN {
                return AfterWait::Again(CLOSE_UNSAVED_WITHIN - waited);
            }
            return AfterWait::Quit("the page did not answer the close");
        }
        if self.asking { AfterWait::Done } else { AfterWait::Quit("the page said it closes the window, and the window is still there") }
    }

    /// The launcher begins to quit: true the first time only.
    pub fn begin_quit(&mut self) -> bool {
        !std::mem::replace(&mut self.quitting, true)
    }
}

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
struct State {
    recovery: Recovery,
    life: Life,
    /// When the window was made and when the page was last loaded again.
    window: Option<Instant>,
    loaded: Option<Instant>,
    /// The web process's last memory sample.
    sample: Option<Sample>,
}

static STATE: Mutex<State> = Mutex::new(State {
    recovery: Recovery::new(),
    life: Life::new(),
    window: None,
    loaded: None,
    sample: None,
});

fn state() -> std::sync::MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
fn log(msg: &str) {
    crate::commands::app::launcher_log("render", msg);
}

/// A page said it started (`app_started`, after each load).
pub fn page_started() {
    state().life.page_started();
    #[cfg(all(target_os = "linux", any(debug_assertions, feature = "dev-eval")))]
    linux::test_kill_after_start();
}

/// `RunEvent::ExitRequested` and `RunEvent::Exit`: the launcher quits, the close path quits nothing more.
pub fn leaving() {
    state().life.begin_quit();
}

/// The page says whether closing now would lose work (`app_close_unsaved`).
pub fn page_unsaved(unsaved: bool) {
    state().life.page_unsaved(unsaved);
}

/// The page answered a close request (`app_close_seen`).
pub fn page_answered(asking: bool) {
    state().life.page_answered(asking);
}

/// The main window's `CloseRequested` (after Tauri's own handler, which holds the close for the page's listener).
/// Nothing on Windows and macOS ([`FORCES_CLOSE`]).
pub fn close_requested<R: tauri::Runtime>(app: &tauri::AppHandle<R>) {
    let step = {
        let mut s = state();
        let fallback = s.recovery.fallback;
        s.life.close_requested(Instant::now(), fallback, FORCES_CLOSE)
    };
    match step {
        Close::Tauri | Close::Waiting => {}
        Close::Now(why) => quit(app, why),
        Close::Page(id) => {
            let app = app.clone();
            let _ = std::thread::Builder::new().name("kiln-close".into()).spawn(move || {
                let mut wait = CLOSE_ANSWER_WITHIN;
                loop {
                    std::thread::sleep(wait);
                    let next = state().life.after_wait(id, Instant::now());
                    match next {
                        AfterWait::Done => return,
                        AfterWait::Again(more) => wait = more,
                        AfterWait::Quit(why) => {
                            use tauri::Manager;
                            if app.get_webview_window("main").is_some() {
                                quit(&app, why);
                            }
                            return;
                        }
                    }
                }
            });
        }
    }
}

/// Quits the launcher without the page, once (a second forced close, or one after another exit began, does nothing):
/// as Deck's Quit, `RunEvent::Exit` runs as at every exit.
fn quit<R: tauri::Runtime>(app: &tauri::AppHandle<R>, why: &str) {
    if !state().life.begin_quit() {
        return;
    }
    crate::commands::app::launcher_log("close", &format!("the window closes without the page ({why}): the launcher quits"));
    app.exit(0);
}

// ---------------------------------------------------------------- what launcher.log says

/// The web process's memory, as last sampled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Sample {
    pub at: Instant,
    pub pid: u32,
    /// Resident memory of the largest web process, and the largest seen this start (kB).
    pub rss_kb: u64,
    pub peak_kb: u64,
    /// How many web processes there were.
    pub count: usize,
}

/// The system at the end: /proc/loadavg's three averages, the CPUs, MemAvailable and MemTotal (kB).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct System {
    pub load: Option<[f64; 3]>,
    pub cpus: Option<usize>,
    pub available_kb: Option<u64>,
    pub total_kb: Option<u64>,
}

/// What launcher.log says about an end, in one line: the reason, the last memory sample, uptimes, the system's load
/// and memory, and the next step. `up`: since the window was made and since the page last loaded.
pub fn says_end(ended: Ended, sample: Option<&Sample>, now: Instant, up: (Option<Duration>, Option<Duration>), sys: &System,
    ends: u32, step: Step) -> String {
    let memory = match sample {
        Some(s) => format!("the web process (pid {}) used {} {} (largest this start {}; {} web process{})",
            s.pid, mb(s.rss_kb), ago(now.saturating_duration_since(s.at)), mb(s.peak_kb), s.count, if s.count == 1 { "" } else { "es" }),
        None => "its memory was never sampled".into(),
    };
    let (launcher_up, page_up) = up;
    let up = |d: Option<Duration>| d.map_or("?".to_string(), span);
    let load = match (sys.load, sys.cpus) {
        (Some([a, b, c]), cpus) => format!("load {a:.2} {b:.2} {c:.2}{}", cpus.map(|n| format!(" on {n} CPUs")).unwrap_or_default()),
        (None, _) => "load not known".into(),
    };
    let memory_free = match (sys.available_kb, sys.total_kb) {
        (Some(a), Some(t)) => format!("memory available {} of {}", gb(a), gb(t)),
        _ => "memory available not known".into(),
    };
    let next = match step {
        Step::Reload => "loading the page again (the backend keeps running)",
        Step::Fallback => "again soon after a reload: showing the fallback screen (Reload, Quit)",
    };
    let limit = if ended == Ended::ExceededMemoryLimit { " (over WebKit's memory limit)" } else { "" };
    format!("PROBLEM: the page's web process ended ({}){limit}; {memory}; the launcher up {}, this page {}; {load}; {memory_free}; \
        end {ends} this start: {next}", ended.name(), up(launcher_up), up(page_up))
}

fn mb(kb: u64) -> String {
    format!("{} MB", kb.div_ceil(1024))
}

fn gb(kb: u64) -> String {
    format!("{:.1} GB", kb as f64 / (1024.0 * 1024.0))
}

/// "7 s", "3 min 4 s", "2 h 13 min".
pub fn span(d: Duration) -> String {
    let s = d.as_secs();
    match s {
        0..60 => format!("{s} s"),
        60..3600 => format!("{} min {} s", s / 60, s % 60),
        _ => format!("{} h {} min", s / 3600, (s % 3600) / 60),
    }
}

fn ago(d: Duration) -> String {
    format!("{} ago", span(d))
}

// ---------------------------------------------------------------- /proc

/// `/proc/<pid>/stat`: the command's name and the parent's pid (the name is in parentheses and may hold anything).
pub fn parse_stat(text: &str) -> Option<(String, u32)> {
    let open = text.find('(')?;
    let close = text.rfind(')')?;
    let comm = text.get(open + 1..close)?.to_string();
    let mut rest = text.get(close + 1..)?.split_whitespace();
    let _state = rest.next()?;
    let ppid = rest.next()?.parse().ok()?;
    Some((comm, ppid))
}

/// A `Key:   1234 kB` line of /proc/<pid>/status or /proc/meminfo, in kB.
pub fn kb_of(text: &str, key: &str) -> Option<u64> {
    text.lines().find_map(|l| l.strip_prefix(key)?.strip_prefix(':')?.split_whitespace().next()?.parse().ok())
}

/// /proc/loadavg's three averages.
pub fn parse_loadavg(text: &str) -> Option<[f64; 3]> {
    let mut it = text.split_whitespace().map(|v| v.parse::<f64>().ok());
    Some([it.next()??, it.next()??, it.next()??])
}

/// Every process below `root` in a (pid, parent) list: WebKit's processes are the launcher's children, or bwrap's
/// children when WebKit's sandbox runs.
pub fn descendants(procs: &[(u32, u32)], root: u32) -> Vec<u32> {
    let mut out = Vec::new();
    let mut todo = vec![root];
    while let Some(p) = todo.pop() {
        for &(pid, parent) in procs {
            if parent == p && pid != root && !out.contains(&pid) {
                out.push(pid);
                todo.push(pid);
            }
        }
    }
    out
}

/// WebKitGTK's web process as /proc names it (the kernel keeps 15 bytes of the name).
pub fn is_web_process(comm: &str) -> bool {
    comm.starts_with("WebKitWebProces")
}

#[cfg(target_os = "linux")]
mod linux {
    use super::*;
    use std::path::Path;

    /// The web processes below this one, with their resident memory (kB); also crate::fonthang's probe.
    pub(crate) fn web_processes() -> Vec<(u32, u64)> {
        let procs: Vec<(u32, String, u32)> = std::fs::read_dir("/proc").into_iter().flatten().flatten()
            .filter_map(|e| e.file_name().to_str()?.parse::<u32>().ok())
            .filter_map(|pid| {
                let (comm, ppid) = parse_stat(&std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?)?;
                Some((pid, comm, ppid))
            })
            .collect();
        let tree: Vec<(u32, u32)> = procs.iter().map(|(p, _, pp)| (*p, *pp)).collect();
        let below = descendants(&tree, std::process::id());
        procs.iter()
            .filter(|(pid, comm, _)| below.contains(pid) && is_web_process(comm))
            .map(|(pid, _, _)| (*pid, std::fs::read_to_string(format!("/proc/{pid}/status")).ok()
                .and_then(|s| kb_of(&s, "VmRSS")).unwrap_or(0)))
            .collect()
    }

    fn sample_now() {
        let found = web_processes();
        let Some(&(pid, rss)) = found.iter().max_by_key(|(_, rss)| *rss) else { return };
        let mut s = state();
        let peak = s.sample.map_or(0, |p| p.peak_kb).max(rss);
        s.sample = Some(Sample { at: Instant::now(), pid, rss_kb: rss, peak_kb: peak, count: found.len() });
    }

    fn system() -> System {
        let read = |p: &str| std::fs::read_to_string(Path::new(p)).ok();
        let mem = read("/proc/meminfo");
        System {
            load: read("/proc/loadavg").as_deref().and_then(parse_loadavg),
            cpus: std::thread::available_parallelism().ok().map(|n| n.get()),
            available_kb: mem.as_deref().and_then(|m| kb_of(m, "MemAvailable")),
            total_kb: mem.as_deref().and_then(|m| kb_of(m, "MemTotal")),
        }
    }

    /// The window's webview exists (crate::render::window_created, on the main thread): the sampler and the handler.
    pub fn watch(app: tauri::AppHandle, wv: &webkit2gtk::WebView) {
        use webkit2gtk::WebViewExt;
        {
            let mut s = state();
            let now = Instant::now();
            s.window.get_or_insert(now);
            s.loaded.get_or_insert(now);
        }
        let _ = std::thread::Builder::new().name("kiln-webproc".into()).spawn(|| loop {
            sample_now();
            std::thread::sleep(SAMPLE_EVERY);
        });
        wv.connect_web_process_terminated(move |wv, reason| ended(&app, wv, reason));
    }

    fn ended(app: &tauri::AppHandle, wv: &webkit2gtk::WebView, reason: webkit2gtk::WebProcessTerminationReason) {
        use webkit2gtk::WebProcessTerminationReason as R;
        let why = match reason {
            R::Crashed => Ended::Crashed,
            R::ExceededMemoryLimit => Ended::ExceededMemoryLimit,
            R::TerminatedByApi => Ended::TerminatedByApi,
            other => Ended::Other(gtk::glib::translate::IntoGlib::into_glib(other)),
        };
        let now = Instant::now();
        let sys = system();
        let released = release_page_resources();
        let line = {
            let mut s = state();
            s.life.page_ended();
            let step = s.recovery.ended(now);
            let since = |t: Option<Instant>| t.map(|t| now.saturating_duration_since(t));
            let line = says_end(why, s.sample.as_ref(), now, (since(s.window), since(s.loaded)), &sys, s.recovery.ends, step);
            if step == Step::Reload {
                s.loaded = Some(now);
            }
            (line, step)
        };
        log(&line.0);
        if !released.is_empty() {
            log(&format!("the ended page's {released} stopped (the backend's work goes on)"));
        }
        eprintln!("Kiln Launcher: the page's web process ended ({})", why.name());
        let (wv, app) = (wv.clone(), app.clone());
        // not inside WebKit's own signal: a moment later on the main loop
        gtk::glib::idle_add_local_once(move || match line.1 {
            Step::Reload => {
                use webkit2gtk::WebViewExt;
                wv.reload();
            }
            Step::Fallback => fallback(&app, &wv),
        });
    }

    /// What only the page used goes with it (it never says it is done with them); the backend's own work stays.
    fn release_page_resources() -> String {
        let follows = crate::watch::unfollow_all();
        let consoles = crate::commands::server::close_all_consoles();
        let mut said = Vec::new();
        if follows > 0 {
            said.push(format!("{follows} log follow{}", if follows == 1 { "" } else { "s" }));
        }
        if consoles > 0 {
            said.push(format!("{consoles} server console{}", if consoles == 1 { "" } else { "s" }));
        }
        said.join(" and ")
    }

    const FALLBACK_NAME: &str = "kiln-page-crashed";
    const FALLBACK_CSS: &str = "#kiln-page-crashed { background-color: #080504; } \
        #kiln-page-crashed label { color: #f2e6d8; } #kiln-page-crashed .kiln-dim { color: #b9a898; }";
    pub(super) const TITLE: &str = "The launcher's page crashed.";

    fn body() -> String {
        let logs = crate::state::dirs().ok().or_else(crate::folder::found_dirs)
            .map(|d| d.logs().join("launcher.log").display().to_string()).unwrap_or_else(|| "launcher.log".into());
        format!("Its web process ended again soon after the launcher loaded it once more. The launcher itself is still \
            running: a server it started, downloads and installs go on.\n\nlauncher.log says more: {logs}")
    }

    /// The fallback screen in the window, in place of the webview (GTK: the window's box holds the webview); a message
    /// over the window when the webview is not in a box.
    fn fallback(app: &tauri::AppHandle, wv: &webkit2gtk::WebView) {
        use gtk::prelude::*;
        log("the fallback screen is up: Reload or Quit");
        let Some(parent) = wv.parent().and_then(|p| p.downcast::<gtk::Box>().ok()) else {
            return fallback_message(app, wv);
        };
        if let Some(screen) = gtk::gdk::Screen::default() {
            let css = gtk::CssProvider::new();
            if css.load_from_data(FALLBACK_CSS.as_bytes()).is_ok() {
                gtk::StyleContext::add_provider_for_screen(&screen, &css, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);
            }
        }
        let outer = gtk::Box::new(gtk::Orientation::Vertical, 0);
        outer.set_widget_name(FALLBACK_NAME);
        let inner = gtk::Box::new(gtk::Orientation::Vertical, 14);
        inner.set_valign(gtk::Align::Center);
        inner.set_halign(gtk::Align::Center);
        let title = gtk::Label::new(None);
        title.set_markup(&format!("<span size='x-large' weight='bold'>{}</span>", gtk::glib::markup_escape_text(TITLE)));
        let text = gtk::Label::new(Some(&body()));
        text.set_line_wrap(true);
        text.set_max_width_chars(70);
        text.set_justify(gtk::Justification::Center);
        text.set_selectable(true);
        text.style_context().add_class("kiln-dim");
        let buttons = gtk::Box::new(gtk::Orientation::Horizontal, 10);
        buttons.set_halign(gtk::Align::Center);
        let reload = gtk::Button::with_label("Reload");
        let quit_b = gtk::Button::with_label("Quit");
        buttons.pack_start(&reload, false, false, 0);
        buttons.pack_start(&quit_b, false, false, 0);
        inner.pack_start(&title, false, false, 0);
        inner.pack_start(&text, false, false, 0);
        inner.pack_start(&buttons, false, false, 0);
        outer.pack_start(&inner, true, true, 0);
        parent.pack_start(&outer, true, true, 0);
        wv.hide();
        outer.show_all();
        text.select_region(0, 0);
        reload.grab_focus();
        {
            let (wv, outer, parent) = (wv.clone(), outer.clone(), parent.clone());
            reload.connect_clicked(move |_| {
                log("the player chose \"Reload\" on the fallback screen");
                parent.remove(&outer);
                wv.show();
                reload_page(&wv);
            });
        }
        let app = app.clone();
        quit_b.connect_clicked(move |_| {
            if state().life.begin_quit() {
                crate::commands::app::launcher_log("close", "the player chose \"Quit\" on the fallback screen");
                app.exit(0);
            }
        });
    }

    /// The fallback as a message over the window (the webview not in a box: never seen with wry 0.57).
    fn fallback_message(app: &tauri::AppHandle, wv: &webkit2gtk::WebView) {
        use gtk::prelude::*;
        use tauri::Manager;
        let parent = app.get_webview_window("main").and_then(|w| w.gtk_window().ok());
        let d = gtk::MessageDialog::new(parent.as_ref(), gtk::DialogFlags::empty(), gtk::MessageType::Warning,
            gtk::ButtonsType::None, TITLE);
        d.set_title("Kiln Launcher");
        d.set_secondary_text(Some(&body()));
        d.add_button("Reload", gtk::ResponseType::Other(0));
        d.add_button("Quit", gtk::ResponseType::Other(1));
        d.set_default_response(gtk::ResponseType::Other(0));
        let (wv, app) = (wv.clone(), app.clone());
        d.connect_response(move |d, r| match r {
            gtk::ResponseType::Other(1) => {
                if state().life.begin_quit() {
                    crate::commands::app::launcher_log("close", "the player chose \"Quit\" on the fallback message");
                    app.exit(0);
                }
            }
            gtk::ResponseType::Other(0) => {
                log("the player chose \"Reload\" on the fallback message");
                d.hide();
                reload_page(&wv);
            }
            // closed without a button: the window stays as it is, its close quits (the page is dead)
            _ => d.hide(),
        });
        d.show_all();
    }

    fn reload_page(wv: &webkit2gtk::WebView) {
        use webkit2gtk::WebViewExt;
        {
            let mut s = state();
            let now = Instant::now();
            s.recovery.reload_clicked(now);
            s.loaded = Some(now);
        }
        wv.reload();
    }

    /// Debug and QA builds (`dev-eval`): `KILN_LAUNCHER_TEST_KILL_PAGE=<seconds>` ends the page's web process (SIGKILL:
    /// WebKit says Crashed) that long after each page start: the first end reloads, the second shows the fallback.
    #[cfg(any(debug_assertions, feature = "dev-eval"))]
    pub(super) fn test_kill_after_start() {
        let Some(secs) = std::env::var("KILN_LAUNCHER_TEST_KILL_PAGE").ok().and_then(|v| v.parse::<u64>().ok()) else { return };
        log(&format!("test: the page's web process is ended in {secs} s (KILN_LAUNCHER_TEST_KILL_PAGE)"));
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(secs));
            sample_now();
            if let Some((pid, _)) = web_processes().into_iter().max_by_key(|(_, rss)| *rss) {
                // SAFETY: kill only sends a signal to the launcher's own child process
                unsafe { libc::kill(pid as i32, libc::SIGKILL) };
            }
        });
    }
}

#[cfg(target_os = "linux")]
pub use linux::watch;
#[cfg(target_os = "linux")]
pub(crate) use linux::web_processes;

#[cfg(test)]
mod tests {
    use super::*;

    fn at(base: Instant, secs: u64) -> Instant {
        base + Duration::from_secs(secs)
    }

    #[test]
    fn the_first_end_reloads_a_second_soon_after_shows_the_fallback() {
        let t = Instant::now();
        let mut r = Recovery::new();
        assert_eq!(r.ended(at(t, 100)), Step::Reload);
        assert!(!r.fallback);
        assert_eq!(r.ended(at(t, 130)), Step::Fallback, "30 s after the reload");
        assert!(r.fallback);
        // while the fallback is up, any end stays there (never a reload loop)
        assert_eq!(r.ended(at(t, 1000)), Step::Fallback);
        assert_eq!(r.ends, 3);
    }

    #[test]
    fn an_end_long_after_the_reload_reloads_again() {
        let t = Instant::now();
        let mut r = Recovery::new();
        assert_eq!(r.ended(at(t, 0)), Step::Reload);
        assert_eq!(r.ended(at(t, RETRY_WINDOW.as_secs())), Step::Reload, "at the window's end: a fresh reload");
        assert_eq!(r.ended(at(t, RETRY_WINDOW.as_secs() + 1)), Step::Fallback);
    }

    #[test]
    fn the_fallbacks_reload_gets_one_more_try_then_the_fallback_again() {
        let t = Instant::now();
        let mut r = Recovery::new();
        r.ended(at(t, 0));
        assert_eq!(r.ended(at(t, 5)), Step::Fallback);
        r.reload_clicked(at(t, 50));
        assert!(!r.fallback);
        assert_eq!(r.ended(at(t, 60)), Step::Fallback, "10 s after the player's reload");
        r.reload_clicked(at(t, 100));
        assert_eq!(r.ended(at(t, 100 + RETRY_WINDOW.as_secs() + 5)), Step::Reload, "long after it: automatic again");
    }

    #[test]
    fn a_dead_page_or_the_fallback_closes_at_once() {
        let t = Instant::now();
        let mut l = Life::new();
        l.page_ended();
        assert!(matches!(l.close_requested(t, false, true), Close::Now(_)));
        l.page_started();
        assert!(matches!(l.close_requested(t, true, true), Close::Now(_)), "the fallback screen is up");
        let mut l = Life::new();
        assert_eq!(l.close_requested(t, false, true), Close::Page(1), "a live page gets the request");
    }

    #[test]
    fn windows_and_macos_close_as_before() {
        // (review) the close path is Linux only: elsewhere Tauri and the page's guard close the window, never a forced
        // quit and never a wait that quits
        let t = Instant::now();
        let mut l = Life::new();
        assert_eq!(l.close_requested(t, false, false), Close::Tauri);
        assert_eq!(l.close_requested(at(t, 1), false, false), Close::Tauri, "a second close too");
        l.page_ended();
        assert_eq!(l.close_requested(at(t, 2), true, false), Close::Tauri);
        assert!(FORCES_CLOSE == cfg!(target_os = "linux"));
    }

    #[test]
    fn a_page_that_does_not_answer_is_not_waited_for() {
        let t = Instant::now();
        let mut l = Life::new();
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Quit("the page did not answer the close"));
        // a second close before the answer quits at once
        assert!(matches!(l.close_requested(at(t, 1), false, true), Close::Now(_)));
        // past the wait, a new request is the page's again
        let Close::Page(id2) = l.close_requested(at(t, 10), false, true) else { panic!() };
        assert_eq!(id2, 2);
        assert_eq!(l.after_wait(id, at(t, 10)), AfterWait::Done, "the older request's wait no longer decides");
    }

    #[test]
    fn a_page_asking_about_unsaved_work_keeps_the_window() {
        let t = Instant::now();
        let mut l = Life::new();
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        l.page_answered(true);
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Done, "the player decides");
        // the player chose Stay, then closes again: the page gets it again (not a forced close)
        assert_eq!(l.close_requested(at(t, 6), false, true), Close::Page(2));
        l.page_answered(false);
        assert_eq!(l.after_wait(2, at(t, 11)), AfterWait::Quit("the page said it closes the window, and the window is still there"));
    }

    #[test]
    fn a_slow_page_with_unsaved_work_is_not_quit_after_5_s() {
        // (review) a live page on a loaded box can take longer than 5 s to answer: with unsaved work it gets 30 s, and
        // impatient clicks neither force the close nor make the wait longer
        let t = Instant::now();
        let mut l = Life::new();
        l.page_unsaved(true);
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Again(CLOSE_UNSAVED_WITHIN - Duration::from_secs(5)));
        assert_eq!(l.close_requested(at(t, 6), false, true), Close::Waiting, "a second close while it is slow");
        assert_eq!(l.after_wait(id, at(t, 12)), AfterWait::Again(CLOSE_UNSAVED_WITHIN - Duration::from_secs(12)));
        // its answer comes: the player decides
        l.page_answered(true);
        assert_eq!(l.after_wait(id, at(t, 30)), AfterWait::Done);
        // a page that never answers still lets the window go
        let mut l = Life::new();
        l.page_unsaved(true);
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        assert_eq!(l.after_wait(id, at(t, 30)), AfterWait::Quit("the page did not answer the close"));
        // the work saved again: 5 s as before
        let mut l = Life::new();
        l.page_unsaved(true);
        l.page_unsaved(false);
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Quit("the page did not answer the close"));
    }

    #[test]
    fn a_page_that_ended_has_no_unsaved_work_left() {
        let t = Instant::now();
        let mut l = Life::new();
        l.page_unsaved(true);
        l.page_ended();
        assert!(matches!(l.close_requested(t, false, true), Close::Now(_)), "a dead page closes at once");
        // a word of the dead page arriving late counts for nothing
        l.page_unsaved(true);
        l.page_started();
        let Close::Page(id) = l.close_requested(at(t, 1), false, true) else { panic!() };
        assert_eq!(l.after_wait(id, at(t, 6)), AfterWait::Quit("the page did not answer the close"));
        // the page ending during the wait: no longer waited for
        let mut l = Life::new();
        l.page_unsaved(true);
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        l.page_ended();
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Quit("the page did not answer the close"));
    }

    #[test]
    fn the_launcher_quits_once() {
        // (review) a forced close, the fallback's Quit and any other exit quit once: no second app.exit, which after
        // the event loop ended would be Tauri's process::exit in the middle of RunEvent::Exit
        let t = Instant::now();
        let mut l = Life::new();
        let Close::Page(id) = l.close_requested(t, false, true) else { panic!() };
        assert!(l.begin_quit());
        assert!(!l.begin_quit(), "the second quit does nothing");
        assert_eq!(l.after_wait(id, at(t, 5)), AfterWait::Done, "the wait quits nothing once the launcher quits");
        l.page_ended();
        assert_eq!(l.close_requested(at(t, 6), false, true), Close::Tauri, "a close while quitting");
    }

    #[test]
    fn a_reloaded_page_takes_closes_again() {
        let t = Instant::now();
        let mut l = Life::new();
        l.page_ended();
        assert!(matches!(l.close_requested(t, false, true), Close::Now(_)));
        l.page_started();
        assert_eq!(l.close_requested(t, false, true), Close::Page(1));
    }

    #[test]
    fn the_end_line_says_everything_in_one_line() {
        let t = Instant::now();
        let s = Sample { at: t, pid: 4242, rss_kb: 812 * 1024, peak_kb: 905 * 1024, count: 1 };
        let sys = System { load: Some([54.1, 48.22, 30.0]), cpus: Some(16), available_kb: Some(2 * 1024 * 1024), total_kb: Some(32 * 1024 * 1024) };
        let line = says_end(Ended::Crashed, Some(&s), at(t, 7), (Some(Duration::from_secs(7980)), Some(Duration::from_secs(65))), &sys, 1, Step::Reload);
        assert!(line.starts_with("PROBLEM: the page's web process ended (Crashed); "), "the line before 31.13 still matches: {line}");
        for part in ["(pid 4242) used 812 MB 7 s ago", "largest this start 905 MB; 1 web process)", "the launcher up 2 h 13 min",
            "this page 1 min 5 s", "load 54.10 48.22 30.00 on 16 CPUs", "memory available 2.0 GB of 32.0 GB", "end 1 this start: loading the page again"] {
            assert!(line.contains(part), "{part:?} in {line}");
        }
        assert!(!line.contains('\n'));
        let line = says_end(Ended::ExceededMemoryLimit, None, t, (None, None), &System::default(), 2, Step::Fallback);
        assert!(line.starts_with("PROBLEM: the page's web process ended (ExceededMemoryLimit) (over WebKit's memory limit); its memory was never sampled"), "{line}");
        assert!(line.contains("load not known") && line.contains("memory available not known") && line.contains("fallback screen"), "{line}");
        assert_eq!(Ended::Other(9).name(), "unknown reason 9");
    }

    #[test]
    fn proc_files_are_read_as_the_kernel_writes_them() {
        assert_eq!(parse_stat("4242 (WebKitWebProces) S 4100 4242 4100 0 -1"), Some(("WebKitWebProces".into(), 4100)));
        // a name with spaces and parentheses
        assert_eq!(parse_stat("7 (a (b) c) R 1 7 7"), Some(("a (b) c".into(), 1)));
        assert_eq!(parse_stat("garbage"), None);
        let status = "Name:\tWebKitWebProces\nVmPeak:\t 999 kB\nVmRSS:\t  831488 kB\n";
        assert_eq!(kb_of(status, "VmRSS"), Some(831488));
        assert_eq!(kb_of(status, "VmSwap"), None);
        assert_eq!(kb_of("MemTotal:       32617856 kB\nMemAvailable:    2154320 kB\n", "MemAvailable"), Some(2154320));
        assert_eq!(parse_loadavg("54.12 48.20 30.01 12/3456 98765\n"), Some([54.12, 48.20, 30.01]));
        assert_eq!(parse_loadavg("1.0"), None);
        assert!(is_web_process("WebKitWebProces") && !is_web_process("WebKitNetworkPr"));
    }

    #[test]
    fn web_processes_behind_bwrap_are_found() {
        // launcher 100 -> WebKitNetworkProcess 101; bwrap 102 -> bwrap 103 -> WebKitWebProcess 104; another program 200 -> 201
        let tree = [(101, 100), (102, 100), (103, 102), (104, 103), (201, 200), (100, 1)];
        let mut d = descendants(&tree, 100);
        d.sort();
        assert_eq!(d, vec![101, 102, 103, 104]);
        assert!(descendants(&tree, 999).is_empty());
    }

    #[test]
    fn spans_read_well() {
        assert_eq!(span(Duration::from_secs(7)), "7 s");
        assert_eq!(span(Duration::from_secs(184)), "3 min 4 s");
        assert_eq!(span(Duration::from_secs(7980)), "2 h 13 min");
    }
}
