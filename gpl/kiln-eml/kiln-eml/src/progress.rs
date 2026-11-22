// SPDX-License-Identifier: GPL-3.0-or-later
//! Where a run's progress goes, so that a start never looks dead (EML 0.2.0 is silent for minutes on a big install):
//!
//! - the state file `<game>/kiln/state/eml.json` gets `phase: "running"` and a `progress` object (stage, bytes done and to
//!   do) a few times a second, replaced whole each time, so the launcher can show it;
//! - Kiln's progress window (the `kiln.progress` service of kiln-core: a small native window that appears when the work
//!   lasts more than a couple of seconds), when the core has one on this side and desktop.
//!
//! The run's counters are `mod_loader::lua::Progress` (mod-loader-lua/src/progress.rs); `Live` is its `Sink` and is called
//! from that thread only. Nothing here can fail a run: every error is swallowed, a missing window or an unwritable state
//! file only costs the display.

use std::ffi::c_void;
use std::sync::Mutex;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use camino::Utf8PathBuf;
use mod_loader::lua::{
    Event, Sink, Stage,
    archive_work::{self, Note},
};
use serde::Serialize;

use crate::runner::{Outcome, Phase};
use crate::startup::Point;

/// The state file is rewritten at most this often (but at every change of stage).
const WRITE_EVERY: std::time::Duration = std::time::Duration::from_millis(250);

/// The `progress` object of a running state file (`schema/state-eml.schema.json`).
#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Live {
    /// `starting` before the run reports, then `checking`, `backup`, `mods`, `writing`, `saving`.
    pub stage: &'static str,
    /// For a person: "Checking the game data".
    pub label: &'static str,
    /// Bytes read so far in this stage and the bytes it has announced (0: not known).
    pub done_bytes: u64,
    pub total_bytes: u64,
    pub files_done: usize,
    pub files_total: usize,
    /// The file being read now (its name).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub file: Option<String>,
    /// Milliseconds the stage has lasted.
    pub stage_ms: u64,
    /// While a file is being read: milliseconds since a byte came (0 otherwise). A big number is a stuck read.
    pub quiet_ms: u64,
    /// When the run began and when this was written, in ms since 1970.
    pub started_ms: u64,
    pub updated_ms: u64,
    /// A kiln-mode start's startup pass (stage `startup`): the step it is at (`PassStep::word`), its number and how many
    /// there are. Absent in every other stage.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub step: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub step_number: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub steps: Option<usize>,
}

fn unix_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_millis() as u64)
}

// ---- the window: the kiln.progress service (native/core/abi/services/kiln_progress.h) ----

#[repr(C)]
#[derive(Clone, Copy)]
struct Str {
    ptr: *const u8,
    len: usize,
}

impl Str {
    fn of(s: &str) -> Self {
        Self { ptr: s.as_ptr(), len: s.len() }
    }
}

/// kiln_progress.h KilnProgressApi v1.
#[repr(C)]
struct Table {
    size: u32,
    version: u32,
    begin: unsafe extern "C" fn(Str) -> u64,
    update: unsafe extern "C" fn(u64, Str, u64, u64),
    end: unsafe extern "C" fn(u64),
}

/// One open display task of the service. Ended when dropped.
pub struct Window {
    table: *const Table,
    task: u64,
}

// The table is Kiln's immutable function table and the task is a plain number; the service is callable from any thread.
unsafe impl Send for Window {}

impl Window {
    /// `table`: what `api->service("kiln.progress", 1)` returned (null: the core has none). The header is checked before
    /// any slot is read. None when the service says there is no display for this process (a server, `KILN_UI_DISABLE=1`).
    ///
    /// # Safety
    /// `table` is null or points to a Kiln service table that lives for the process.
    pub unsafe fn open(table: *const c_void, title: &str) -> Option<Window> {
        if table.is_null() {
            return None;
        }
        let (size, version) = unsafe { (std::ptr::read(table.cast::<u32>()), std::ptr::read(table.cast::<u32>().add(1))) };
        if version < 1 || (size as usize) < std::mem::size_of::<Table>() {
            return None;
        }
        let table = table.cast::<Table>();
        for offset in [std::mem::offset_of!(Table, begin), std::mem::offset_of!(Table, update), std::mem::offset_of!(Table, end)] {
            if unsafe { std::ptr::read(table.cast::<u8>().add(offset).cast::<usize>()) } == 0 {
                return None;
            }
        }
        let task = unsafe { ((*table).begin)(Str::of(title)) };
        (task != 0).then_some(Window { table, task })
    }

    pub fn update(&self, text: &str, done: u64, total: u64) {
        unsafe { ((*self.table).update)(self.task, Str::of(text), done, total) };
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        unsafe { ((*self.table).end)(self.task) };
    }
}

/// The line the window shows: `Checking the game data: 12.3 of 48.6 GiB (25%)`, or the stage alone.
pub fn window_text(e: &Event) -> String {
    if e.total > 0 && e.stage == Stage::Checking {
        let percent = e.percent().unwrap_or(0);
        format!("{}: {} of {} ({percent}%)", e.stage.label(), size(e.done.min(e.total)), size(e.total))
    } else {
        e.stage.label().to_owned()
    }
}

/// `12.3 GiB`, `312 B`.
fn size(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 { format!("{bytes} B") } else { format!("{value:.1} {}", UNITS[unit]) }
}

// ---- the sink ----

/// What a run reports into: the state file and the window. Built by the plugin (it knows the paths and the service);
/// the runner hands it the run's mods (`set_base`) and gives it to `Progress` as its sink.
pub struct Display {
    state_dir: Option<Utf8PathBuf>,
    window: Mutex<Option<Window>>,
    inner: Mutex<Inner>,
}

struct Inner {
    /// The state as it stands (the mods), published with `phase: running` and the progress.
    base: Option<Outcome>,
    started_ms: u64,
    last_write: Option<Instant>,
    last_stage: Option<(Stage, u64)>,
}

impl std::fmt::Debug for Display {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Display").field("state_dir", &self.state_dir).finish_non_exhaustive()
    }
}

impl Display {
    /// `table`: the `kiln.progress` service table, or null. The window opens now (the service shows it only when the
    /// work lasts, and `end`s with the run).
    ///
    /// # Safety
    /// See `Window::open`.
    pub unsafe fn new(state_dir: Option<Utf8PathBuf>, table: *const c_void) -> Self {
        let window = unsafe { Window::open(table, "Preparing the game data for EML mods") };
        Self::with_window(state_dir, window)
    }

    pub fn with_window(state_dir: Option<Utf8PathBuf>, window: Option<Window>) -> Self {
        Self {
            state_dir,
            window: Mutex::new(window),
            inner: Mutex::new(Inner { base: None, started_ms: unix_ms(), last_write: None, last_stage: None }),
        }
    }

    /// The state the live file carries besides the progress (the mods found so far). Writes the file at once with the
    /// run `starting`, so that the last run's `done` is not mistaken for this one's.
    pub fn set_base(&self, out: &Outcome) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.base = Some(out.clone());
        let live = Live {
            stage: "starting", label: "Starting", done_bytes: 0, total_bytes: 0, files_done: 0, files_total: 0, file: None,
            stage_ms: 0, quiet_ms: 0, started_ms: inner.started_ms, updated_ms: unix_ms(), step: None, step_number: None,
            steps: None,
        };
        let json = Self::document(&inner, live);
        inner.last_write = Some(Instant::now());
        drop(inner);
        self.write(&json);
    }

    fn document(inner: &Inner, live: Live) -> Option<String> {
        let mut out = inner.base.clone()?;
        out.phase = Phase::Running;
        out.progress = Some(live);
        serde_json::to_string_pretty(&out).ok()
    }

    /// A kiln-mode start's startup pass (`PassSteps`): stage `startup` at `step`, `done` bytes of archive work moved in
    /// it, for `stage_ms`. Written at most every `WRITE_EVERY`, at once when `now` (a new step).
    pub fn pass(&self, step: PassStep, done: u64, stage_ms: u64, now: bool) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if !now && inner.last_write.is_some_and(|t| t.elapsed() < WRITE_EVERY) {
            return;
        }
        inner.last_write = Some(Instant::now());
        let live = Live {
            stage: "startup",
            label: PASS_LABEL,
            done_bytes: done,
            total_bytes: 0,
            files_done: 0,
            files_total: 0,
            file: None,
            stage_ms,
            quiet_ms: 0,
            started_ms: inner.started_ms,
            updated_ms: unix_ms(),
            step: Some(step.word()),
            step_number: Some(step.number()),
            steps: Some(PassStep::ALL.len()),
        };
        let json = Self::document(&inner, live);
        drop(inner);
        self.write(&json);
    }

    fn write(&self, json: &Option<String>) {
        let (Some(dir), Some(json)) = (&self.state_dir, json) else { return };
        let _ = std::fs::create_dir_all(dir);
        let tmp = dir.join("eml.json.live.tmp");
        if std::fs::write(&tmp, json).is_ok() && std::fs::rename(&tmp, dir.join("eml.json")).is_err() {
            let _ = std::fs::remove_file(&tmp);
        }
    }
}

impl Sink for Display {
    fn update(&self, e: &Event) {
        if let Some(w) = self.window.lock().unwrap_or_else(|p| p.into_inner()).as_ref() {
            w.update(&window_text(e), if e.stage == Stage::Checking { e.done.min(e.total) } else { 0 }, if e.stage == Stage::Checking { e.total } else { 0 });
        }
        let mut inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
        let new_stage = inner.last_stage != Some((e.stage, e.serial));
        if !new_stage && inner.last_write.is_some_and(|t| t.elapsed() < WRITE_EVERY) {
            return;
        }
        inner.last_stage = Some((e.stage, e.serial));
        inner.last_write = Some(Instant::now());
        let live = Live {
            stage: e.stage.as_str(),
            label: e.stage.label(),
            done_bytes: e.done,
            total_bytes: e.total,
            files_done: e.files_done,
            files_total: e.files_total,
            file: e.file.clone(),
            stage_ms: e.elapsed.as_millis() as u64,
            quiet_ms: e.quiet.as_millis() as u64,
            started_ms: inner.started_ms,
            updated_ms: unix_ms(),
            step: None,
            step_number: None,
            steps: None,
        };
        let json = Self::document(&inner, live);
        drop(inner);
        self.write(&json);
    }

    fn finished(&self) {
        // the window closes now, before the plugin goes on to the game's entry point; the state file is the plugin's to
        // publish (`publish` in plugin.rs), with the result
        *self.window.lock().unwrap_or_else(|p| p.into_inner()) = None;
    }
}

// ---- kiln mode: the startup pass's steps ----

/// The state file's `label` while a kiln-mode start's startup pass runs.
pub const PASS_LABEL: &str = "Preparing ShroudForge mods for Kiln's interpreter";

/// While a step of the startup pass moves bytes, a kiln.log line at least this often says it is still going.
const PASS_LOG_EVERY: std::time::Duration = std::time::Duration::from_secs(10);

/// The steps of a kiln-mode start's startup pass, as the launcher's start bar shows them ("Preparing ShroudForge mods for
/// Kiln's interpreter: checking the prepared game data (3 of 5)") and kiln.log times them. A start the startup cache
/// serves ends after the first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PassStep {
    /// The input witness checked again, the packages admitted, the startup cache looked up (and, first, the wait for
    /// Kiln's interpreter's DLL step).
    Checking,
    /// The packages' startup code, writing into the private stage.
    Mods,
    /// The stage closed, flushed to the drive and checked.
    Verifying,
    /// The one publication.
    Writing,
    /// The change journal and the startup cache.
    Saving,
}

impl PassStep {
    pub const ALL: [PassStep; 5] = [PassStep::Checking, PassStep::Mods, PassStep::Verifying, PassStep::Writing, PassStep::Saving];

    /// The state file's `step` (`schema/state-eml.schema.json`).
    pub fn word(self) -> &'static str {
        match self {
            PassStep::Checking => "checking",
            PassStep::Mods => "mods",
            PassStep::Verifying => "verify",
            PassStep::Writing => "write",
            PassStep::Saving => "save",
        }
    }

    /// For kiln.log: "checking the prepared game data".
    pub fn label(self) -> &'static str {
        match self {
            PassStep::Checking => "checking the game files",
            PassStep::Mods => "running the mods",
            PassStep::Verifying => "checking the prepared game data",
            PassStep::Writing => "writing the game data",
            PassStep::Saving => "saving the result",
        }
    }

    /// 1 to 5.
    pub fn number(self) -> usize {
        PassStep::ALL.iter().position(|s| *s == self).unwrap_or(0) + 1
    }
}

struct PassState {
    display: Option<std::sync::Arc<Display>>,
    began: Instant,
    step: PassStep,
    since: Instant,
    /// Bytes of archive work moved since the pass began, and when the step began.
    moved: u64,
    moved_at_step: u64,
    last_line: Instant,
    took: Vec<(PassStep, std::time::Duration)>,
}

impl PassState {
    fn enter(&mut self, step: PassStep) {
        let now = Instant::now();
        self.took.push((self.step, now - self.since));
        self.step = step;
        self.since = now;
        self.moved_at_step = self.moved;
        self.last_line = now;
        tracing::debug!("Kiln's ShroudForge startup pass: {} ({} of {})", step.label(), step.number(), PassStep::ALL.len());
        self.write(true);
    }

    fn write(&self, now: bool) {
        if let Some(display) = &self.display {
            display.pass(self.step, self.moved - self.moved_at_step, self.since.elapsed().as_millis() as u64, now);
        }
    }

    /// The bytes moved so far: the state file (at its own rate), and a kiln.log line when the step has been quiet in the
    /// log for `PASS_LOG_EVERY`.
    fn moved(&mut self, moved: u64) {
        self.moved = moved;
        self.write(false);
        if self.last_line.elapsed() >= PASS_LOG_EVERY {
            self.last_line = Instant::now();
            tracing::info!(
                "Kiln's ShroudForge startup pass is still {} ({} of {}): {} of the game's files read or written in {} s",
                self.step.label(), self.step.number(), PassStep::ALL.len(), size(self.moved - self.moved_at_step),
                self.since.elapsed().as_secs()
            );
        }
    }
}

/// The one pass going on in this process (kiln-eml runs one per start). The archive work reports through a plain
/// function (`archive_work`), which finds it here.
static PASS: Mutex<Option<PassState>> = Mutex::new(None);

fn pass_state() -> std::sync::MutexGuard<'static, Option<PassState>> {
    PASS.lock().unwrap_or_else(|e| e.into_inner())
}

/// What the archive work of the pass's thread says (`archive_work::install`).
fn on_archive_work(note: Note) {
    let mut guard = pass_state();
    let Some(state) = guard.as_mut() else { return };
    match note {
        Note::Moved(moved) => state.moved(moved),
        Note::Checking if state.step != PassStep::Verifying => state.enter(PassStep::Verifying),
        Note::Publishing if state.step != PassStep::Writing => state.enter(PassStep::Writing),
        Note::Checking | Note::Publishing => {}
    }
}

/// The steps of one kiln-mode start's startup pass (runner.rs `run_kiln`), on the thread that runs it: the state file says
/// `phase: running`, stage `startup` and the step as the pass goes (the launcher's start bar shows it), kiln.log gets a
/// line every `PASS_LOG_EVERY` while a step moves bytes, and `finish` logs what each step took. No thread of its own:
/// the pass may run while another thread's DLL initializer holds the loader lock.
pub struct PassSteps {
    _work: archive_work::Installed,
}

impl PassSteps {
    /// The pass begins (step 1): `base` is the state the file carries meanwhile (the mods found).
    pub fn begin(display: Option<std::sync::Arc<Display>>, base: &Outcome) -> Self {
        if let Some(display) = &display {
            display.set_base(base);
        }
        let now = Instant::now();
        let state = PassState {
            display,
            began: now,
            step: PassStep::Checking,
            since: now,
            moved: 0,
            moved_at_step: 0,
            last_line: now,
            took: Vec::new(),
        };
        state.write(true);
        *pass_state() = Some(state);
        PassSteps { _work: archive_work::install(on_archive_work) }
    }

    /// A point the startup controller reached (startup.rs `Point`): the admission ends step 1, a retry runs the mods
    /// again, the publication's return begins the last step. The archive work marks the two in between.
    pub fn observe(&self, point: &Point<'_>) {
        let next = match point {
            Point::Admitted => PassStep::Mods,
            Point::Attempt(n) if *n > 1 => PassStep::Mods,
            Point::Published => PassStep::Saving,
            _ => return,
        };
        if let Some(state) = pass_state().as_mut() {
            state.enter(next);
        }
    }

    /// The pass is over: the line for kiln.log with what it and each step took (None when no pass began).
    pub fn finish(self) -> Option<String> {
        let mut state = pass_state().take()?;
        let now = Instant::now();
        state.took.push((state.step, now - state.since));
        let steps: Vec<String> = state.took.iter().map(|(s, d)| format!("{} {}", s.label(), seconds(*d))).collect();
        Some(format!("Kiln's ShroudForge startup pass took {}: {}", seconds(now - state.began), steps.join(", ")))
    }
}

impl Drop for PassSteps {
    fn drop(&mut self) {
        pass_state().take();
    }
}

/// `0.4 s`, `12 s`.
fn seconds(d: std::time::Duration) -> String {
    let s = d.as_secs_f64();
    if s < 10.0 { format!("{s:.1} s") } else { format!("{s:.0} s") }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::Duration;

    static CALLS: Mutex<Vec<String>> = Mutex::new(Vec::new());
    static NEXT: AtomicU64 = AtomicU64::new(7);

    unsafe extern "C" fn begin(title: Str) -> u64 {
        let t = unsafe { std::str::from_utf8(std::slice::from_raw_parts(title.ptr, title.len)).unwrap().to_owned() };
        CALLS.lock().unwrap().push(format!("begin {t}"));
        NEXT.load(Ordering::Relaxed)
    }
    unsafe extern "C" fn update(task: u64, text: Str, done: u64, total: u64) {
        let t = unsafe { std::str::from_utf8(std::slice::from_raw_parts(text.ptr, text.len)).unwrap().to_owned() };
        CALLS.lock().unwrap().push(format!("update {task} {t} {done}/{total}"));
    }
    unsafe extern "C" fn end(task: u64) {
        CALLS.lock().unwrap().push(format!("end {task}"));
    }
    unsafe extern "C" fn begin_none(_: Str) -> u64 {
        0
    }

    fn table(begin: unsafe extern "C" fn(Str) -> u64) -> Table {
        Table { size: std::mem::size_of::<Table>() as u32, version: 1, begin, update, end }
    }

    fn event(stage: Stage, done: u64, total: u64) -> Event {
        Event { stage, serial: 1, done, total, files_done: 1, files_total: 3, file: Some("enshrouded_001.dat".into()), elapsed: Duration::from_secs(4), quiet: Duration::from_secs(2) }
    }

    fn dir(name: &str) -> Utf8PathBuf {
        let d = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap().join(format!("kiln-eml-live-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        d
    }

    #[test]
    fn the_window_gets_the_stage_the_bytes_and_an_end() {
        // one test owns the recorder
        CALLS.lock().unwrap().clear();
        let t = table(begin);
        let display = unsafe { Display::new(None, (&t as *const Table).cast()) };
        display.update(&event(Stage::Checking, 3 << 30, 12 << 30));
        display.update(&event(Stage::Mods, 0, 0));
        display.finished();
        let calls = CALLS.lock().unwrap().clone();
        assert_eq!(calls, [
            "begin Preparing the game data for EML mods",
            "update 7 Checking the game data: 3.0 GiB of 12.0 GiB (25%) 3221225472/12884901888",
            "update 7 Running the mods 0/0",
            "end 7",
        ]);
        // a service that has no display here (a server, KILN_UI_DISABLE): no window, no error
        let none = table(begin_none);
        assert!(unsafe { Window::open((&none as *const Table).cast(), "t") }.is_none());
        assert!(unsafe { Window::open(std::ptr::null(), "t") }.is_none());
        // a table too short to hold the slots, or an unknown version, is not touched
        let short = [4u32, 1];
        assert!(unsafe { Window::open(short.as_ptr().cast(), "t") }.is_none());
        let future = Table { version: 0, ..table(begin) };
        assert!(unsafe { Window::open((&future as *const Table).cast(), "t") }.is_none());
    }

    #[test]
    fn the_state_file_says_running_with_the_progress_and_is_replaced_whole() {
        let d = dir("state");
        let display = Display::with_window(Some(d.clone()), None);
        let mut base = Outcome::new(Phase::Done, Vec::new());
        base.file = Some("enshrouded".into());
        display.set_base(&base);
        let read = || -> serde_json::Value { serde_json::from_str(&std::fs::read_to_string(d.join("eml.json")).unwrap()).unwrap() };
        let first = read();
        assert_eq!((first["phase"].as_str(), first["progress"]["stage"].as_str(), first["format"].as_u64()), (Some("running"), Some("starting"), Some(1)));
        assert!(first["mods"].is_array(), "a running state is still a format 1 document");

        display.update(&event(Stage::Checking, 1000, 4000));
        let v = read();
        assert_eq!(v["phase"], "running");
        assert_eq!((v["progress"]["doneBytes"].as_u64(), v["progress"]["totalBytes"].as_u64(), v["progress"]["filesTotal"].as_u64()), (Some(1000), Some(4000), Some(3)));
        assert_eq!(v["progress"]["file"], "enshrouded_001.dat");
        assert_eq!(v["progress"]["quietMs"].as_u64(), Some(2000));
        assert!(v["progress"]["startedMs"].as_u64().unwrap() > 0 && v["pid"].as_u64() == Some(std::process::id() as u64));

        // a second look within the rate limit changes nothing, a new stage is written at once
        display.update(&event(Stage::Checking, 2000, 4000));
        assert_eq!(read()["progress"]["doneBytes"].as_u64(), Some(1000));
        display.update(&event(Stage::Mods, 0, 0));
        assert_eq!(read()["progress"]["stage"], "mods");
        assert_eq!(std::fs::read_dir(&d).unwrap().count(), 1, "no temp file is left behind");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_kiln_mode_pass_says_its_step_in_the_state_file_and_times_each_step() {
        // one pass per process: the runs of this crate's other tests hold the run lock while theirs goes
        let _serial = crate::runner::RUN_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let d = dir("pass");
        let display = std::sync::Arc::new(Display::with_window(Some(d.clone()), None));
        let mut base = Outcome::new(Phase::Done, Vec::new());
        base.file = Some("enshrouded".into());
        let read = || -> serde_json::Value { serde_json::from_str(&std::fs::read_to_string(d.join("eml.json")).unwrap()).unwrap() };
        let step = || {
            let v = read();
            let p = &v["progress"];
            assert_eq!((v["phase"].as_str(), p["stage"].as_str(), p["label"].as_str(), p["steps"].as_u64()),
                (Some("running"), Some("startup"), Some(PASS_LABEL), Some(5)), "{v}");
            (p["step"].as_str().unwrap().to_owned(), p["stepNumber"].as_u64().unwrap())
        };
        let steps = PassSteps::begin(Some(display), &base);
        assert_eq!(step(), ("checking".to_owned(), 1));
        steps.observe(&Point::Admitted);
        assert_eq!(step(), ("mods".to_owned(), 2));
        // the archive work marks the next two (mod-loader-lua's PatchWriter::prepare and publish_as), and counts bytes
        archive_work::mark(Note::Checking);
        assert_eq!(step(), ("verify".to_owned(), 3));
        archive_work::add(4096);
        archive_work::mark(Note::Publishing);
        assert_eq!(step(), ("write".to_owned(), 4));
        steps.observe(&Point::Published);
        assert_eq!(step(), ("save".to_owned(), 5));
        let line = steps.finish().unwrap();
        assert!(line.starts_with("Kiln's ShroudForge startup pass took "), "{line}");
        for s in PassStep::ALL {
            assert!(line.contains(s.label()), "{line}");
        }
        // over: the archive work reaches nothing, the file keeps its last step until the run publishes its result
        archive_work::mark(Note::Checking);
        assert_eq!(step(), ("save".to_owned(), 5));
        assert!(pass_state().is_none());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn what_the_window_says() {
        assert_eq!(window_text(&event(Stage::Checking, 0, 0)), "Checking the game data");
        assert_eq!(window_text(&event(Stage::Checking, 5 << 20, 10 << 20)), "Checking the game data: 5.0 MiB of 10.0 MiB (50%)");
        assert_eq!(window_text(&event(Stage::Writing, 5, 10)), "Writing the patched game data");
    }
}
