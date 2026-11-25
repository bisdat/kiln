//! Tauri commands: launch. Names and arguments: docs/kiln/launcher/contracts.md section 9.
//! The launch flow is kiln_manage::launch::play (play-once/once files, save backup, steam:// URI); the watcher
//! (crate::watch) reports started/boot/mods/native/stopped as `kiln://game`.

use crate::state::{blocking, dirs};
use kiln_manage::Error;
use kiln_manage::launch::{LaunchRequest, LaunchResult};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use tauri::AppHandle;

/// `launch_state`: the game's state files, read together (launcher-only type).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BootState {
    pub boot: Option<kiln_manage::state::BootJson>,
    pub stopped_start: Option<kiln_manage::stopped_start::StoppedStart>,
    pub mods: Option<kiln_manage::state::ModsJson>,
    pub native: Option<kiln_manage::state::NativeJson>,
    /// boot.json is not from the game process that runs now (`launch::boot_is_current`).
    pub stale: bool,
    /// The error lines kiln.log has for the run that wrote boot.json (errors after the boot: service ticks, mods,
    /// the menu). None when kiln.log is from another run or missing.
    #[serde(default)]
    pub log: Option<LogErrors>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LogErrors {
    /// `[E ...]` lines since the start (`counted_error`: an older runtime's slow-writer line is not one).
    pub errors: u32,
    /// The first few, as written (trimmed to 300 characters).
    pub first: Vec<String>,
}

/// What has been counted of one kiln.log: its first line (a new run truncates the file and writes a new one), how far,
/// and the errors so far. launch_state runs every few seconds; only new bytes are read.
#[derive(Default)]
struct LogScan {
    head: String,
    offset: u64,
    errors: u32,
    first: Vec<String>,
}

static SCANS: std::sync::LazyLock<std::sync::Mutex<std::collections::HashMap<PathBuf, LogScan>>> = std::sync::LazyLock::new(Default::default);

/// Whether a kiln.log line counts as an error. Kiln 0.2.0-dev.31 to 31.4 logged a world write that went ahead of a slow
/// Kiln.Saves writer as `[E] ... player-storage: persistence degraded: world generation N completed before storage became
/// durable (write failed or bounded barrier wait expired)` at every save (one player's launcher showed "373 errors since
/// start"); later runtimes log it as a warning (docs/kiln/services/saves.md, "A slow writer"). A staging that really failed
/// also has its own error lines in those runtimes ("pending generation N could not be written", "writing mod data
/// generation N failed"), which still count.
fn counted_error(line: &[u8]) -> bool {
    const SLOW_WRITER: &[u8] = b"completed before storage became durable (write failed or bounded barrier wait expired)";
    line.starts_with(b"[E ") && !line.windows(SLOW_WRITER.len()).any(|w| w == SLOW_WRITER)
}

fn count_errors(path: &Path) -> Option<(String, LogErrors)> {
    use std::io::{BufRead, Read, Seek, SeekFrom};
    let f = std::fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let mut r = std::io::BufReader::new(f);
    let mut head = Vec::new();
    r.by_ref().take(512).read_until(b'\n', &mut head).ok()?;
    let head = String::from_utf8_lossy(&head).trim_end().to_string();
    let mut scans = SCANS.lock().ok()?;
    let s = scans.entry(path.to_path_buf()).or_default();
    if s.head != head || len < s.offset {
        *s = LogScan { head: head.clone(), ..Default::default() };
    }
    r.seek(SeekFrom::Start(s.offset)).ok()?;
    let mut buf = Vec::new();
    r.read_to_end(&mut buf).ok()?;
    // whole lines only: a line being written is counted next time
    let end = buf.iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
    for line in buf[..end].split(|&b| b == b'\n') {
        if counted_error(line) {
            s.errors += 1;
            if s.first.len() < 5 {
                s.first.push(String::from_utf8_lossy(line).trim_end().chars().take(300).collect());
            }
        }
    }
    s.offset += end as u64;
    Some((head, LogErrors { errors: s.errors, first: s.first.clone() }))
}

/// kiln.log's errors when it is the log of the run that wrote `boot`: its start line names boot.json's pid, and
/// boot.json was written after the previous run's log (kiln.prev.log) ended. A start that died before writing boot.json
/// leaves an older boot.json behind, which kiln.log's errors do not belong to.
fn log_errors(game: &Path, boot: &kiln_manage::state::BootJson) -> Option<LogErrors> {
    let logs = game.join(kiln_manage::paths::game::LOGS);
    let boot_written = std::fs::metadata(game.join(kiln_manage::paths::game::BOOT_JSON)).and_then(|m| m.modified()).ok()?;
    if let Ok(prev) = std::fs::metadata(logs.join("kiln.prev.log")).and_then(|m| m.modified())
        && boot_written <= prev
    {
        return None;
    }
    let (head, errors) = count_errors(&logs.join("kiln.log"))?;
    let pid = head.split("(pid ").nth(1).and_then(|r| r.split(')').next()).and_then(|p| p.trim().parse::<u32>().ok());
    (pid == Some(boot.pid)).then_some(errors)
}

/// `KILN_LAUNCH_DRY_RUN=1`: launch_play does everything except opening the steam:// URI or starting the exe.
pub fn dry_run() -> bool {
    std::env::var("KILN_LAUNCH_DRY_RUN").is_ok_and(|v| v == "1")
}

#[tauri::command]
pub async fn launch_play(app: AppHandle, window: tauri::Window, game_id: String, request: LaunchRequest) -> Result<LaunchResult, Error> {
    // (the launcher's Play starts nothing whose warnings nobody acknowledged: it answers them as a
    // preflight, which launch_confirm (commands/sfi.rs) then starts through the same steps)
    let caller = window.label().to_owned();
    start_game(app, game_id, move |dirs, g, starter| {
        kiln_manage::sfi_launch::play_interactive(kiln_manage::sfi_launch::Book::global(), dirs, g, &request, Some(&caller), None, starter)
    })
    .await
}

/// A start of this launcher (launch_play, launch_confirm): the install as it starts, its steps timed, the game started
/// by `run` with Steam's opener or the launcher's direct start (neither under KILN_LAUNCH_DRY_RUN).
pub(crate) async fn start_game(
    app: AppHandle,
    game_id: String,
    run: impl FnOnce(&kiln_manage::paths::DataDirs, &kiln_manage::game::GameInstall, kiln_manage::launch::Starter<'_>) -> Result<LaunchResult, Error>
        + Send
        + 'static,
) -> Result<LaunchResult, Error> {
    let id = game_id.clone();
    let r = blocking(move || {
        let g = crate::state::game(&id)?;
        crate::state::refuse_running(&g)?;
        // (0.2.0-dev.31.13) no second start while one is pending (Steam asked, no game yet; or another launch_play in
        // here now): held until this start's own steps are done
        let _claim = claim(&id, now_ms())?;
        // the start's own steps: the profile is checked now, Steam is asked when the opener runs
        track(&id, |t| t.begin("profile", now_ms()));
        let dirs = dirs()?;
        // (dev.29.17) a folder that starts directly: the launcher starts its exe (umu-run, Proton or Wine on Linux)
        // instead of asking Steam, which only ever starts the copy in its own library; nothing of Steam is needed. The
        // command is built before anything is written, so a missing runner refuses the start as it is.
        let mut g = g;
        let direct = if kiln_manage::direct::mode_of(&dirs, &g) == kiln_manage::direct::StartMode::Direct {
            // the install as it starts directly (its prefix: the saves backed up, the path mode), then the command
            let roots = crate::state::steam_roots();
            g = kiln_manage::game::sync_start(&dirs, &g, &roots).unwrap_or(g);
            Some(kiln_manage::direct::plan(&g, &kiln_manage::direct::settings(&dirs, &g), &roots)?)
        } else {
            None
        };
        // (the starter built apart: a test drives it with its own opener)
        let log = kiln_manage::launch::direct_log(&dirs, &g);
        let spawn = |c: &kiln_manage::direct::DirectCommand, log: &Path| kiln_manage::direct::spawn(c, Some(log)).map(|_| ());
        let r = with_starter(&id, direct.as_ref().map(|c| (c, log.as_path())), dry_run(), &kiln_manage::launch::desktop_open,
            &spawn, &crate::commands::app::launcher_log, |starter| run(&dirs, &g, starter));
        match &r {
            // the start's mod set, as the launch record has it
            Ok(res) => {
                // the active profile written again before the start (profile::refresh_active): what it kept and noted
                if let Some(a) = &res.activation {
                    crate::commands::app::launcher_log("profile", &format!("{} written again before the start ({})", a.profile_id, g.path.display()));
                    crate::commands::profile::log_notes(a);
                }
                let fp = kiln_manage::launch::last_launch(&dirs, &g).ok().flatten().map(|l| l.fingerprint);
                track(&id, |t| t.fingerprint = fp.or(t.fingerprint.take()));
            }
            Err(e) => failed(&id, e),
        }
        r
    })
    .await?;
    crate::watch::emit_status(&app, &game_id);
    Ok(r)
}

/// The starter of a start of this launcher, `f` run with it: Steam's opener (`open`, the desktop's) or the direct start
/// (`spawn` with its log file), each timed as the start's "steam" step and the direct one written to the launcher's log
/// (`log`). Under KILN_LAUNCH_DRY_RUN (`dry`) neither is asked: tests against game copies write the play-once files and
/// back up, but never open steam:// (that would start the real game through Steam) or start the exe.
pub(crate) fn with_starter<T>(
    id: &str,
    direct: Option<(&kiln_manage::direct::DirectCommand, &Path)>,
    dry: bool,
    open: &dyn Fn(&str) -> Result<(), Error>,
    spawn: &dyn Fn(&kiln_manage::direct::DirectCommand, &Path) -> Result<(), Error>,
    log: &dyn Fn(&str, &str),
    f: impl FnOnce(kiln_manage::launch::Starter<'_>) -> T,
) -> T {
    match direct {
        Some((cmd, file)) => {
            let go = |c: &kiln_manage::direct::DirectCommand| {
                track(id, |t| {
                    // the step keeps its id (the timeline's contract) and says what it is: no Steam in it
                    t.direct = true;
                    t.begin("steam", now_ms());
                    if let Some(s) = t.steps.iter_mut().rev().find(|s| s.id == "steam") {
                        s.label = kiln_manage::start_progress::DIRECT_STEP_LABEL.into();
                    }
                    t.asked = true;
                });
                log("start", &format!("{}: {} {:?} in {}", id, c.program.display(), c.args, c.cwd.display()));
                if dry {
                    eprintln!("KILN_LAUNCH_DRY_RUN: not starting {}", c.program.display());
                    return Ok(());
                }
                spawn(c, file)
            };
            f(kiln_manage::launch::Starter::Direct(cmd, &go))
        }
        None => {
            let steam = |uri: &str| {
                track(id, |t| {
                    t.begin("steam", now_ms());
                    t.asked = true;
                });
                if dry {
                    eprintln!("KILN_LAUNCH_DRY_RUN: not opening {uri}");
                    return Ok(());
                }
                open(uri)
            };
            f(kiln_manage::launch::Starter::Steam(&steam))
        }
    }
}

// ---------------------------------------------------------------- what a start is doing (kiln_manage::start_progress)

/// The starts this launcher makes, per game: its own steps, timed here (Play, the cook, the profile, Steam).
static STARTS: std::sync::LazyLock<std::sync::Mutex<std::collections::HashMap<String, kiln_manage::start_progress::LauncherStart>>> =
    std::sync::LazyLock::new(Default::default);


pub(crate) fn now_ms() -> i64 {
    kiln_manage::start_progress::unix_ms(std::time::SystemTime::now())
}

/// The watcher's round for one game: a start this launcher tracks is kept or given up by its deadlines
/// (LauncherStart::keep_tracking) whatever the window does; `running`: the game's process runs now.
pub(crate) fn prune(game_id: &str, running: bool) {
    let mut starts = STARTS.lock().unwrap_or_else(|e| e.into_inner());
    if starts.get_mut(game_id).is_some_and(|t| !t.keep_tracking(running, now_ms()) || (t.vanilla() && t.seen)) {
        starts.remove(game_id);
    }
}

/// Changes the record of `game_id`'s start, when there is one.
pub(crate) fn track(game_id: &str, f: impl FnOnce(&mut kiln_manage::start_progress::LauncherStart)) {
    if let Some(t) = STARTS.lock().unwrap_or_else(|e| e.into_inner()).get_mut(game_id) {
        f(t);
    }
}

/// A start of this launcher is under way for some game and its process is not there yet (Play pressed, the cook, the
/// warning before Play, Steam asked): the update's restart waits (crate::selfupdate::restart; a running game does not hold
/// it). Such a start is given up by its deadlines (LauncherStart::keep_tracking), so this never holds for long.
#[cfg_attr(not(feature = "self-update"), allow(dead_code))]
pub(crate) fn start_tracked() -> bool {
    STARTS.lock().unwrap_or_else(|e| e.into_inner()).values().any(|t| !t.seen)
}

pub(crate) fn forget(game_id: &str) {
    STARTS.lock().unwrap_or_else(|e| e.into_inner()).remove(game_id);
}

/// A start of this launcher ended with `e` before the game existed: its record goes (the window shows what it showed
/// before Play, and the next start is not held by it). Warnings to acknowledge keep it (the dialog follows, and the start
/// the launcher times with it).
fn failed(game_id: &str, e: &Error) {
    if kiln_manage::sfi_launch::preflight_of(e).is_none() {
        forget(game_id);
    }
}

// ---------------------------------------------------------------- one start at a time (0.2.0-dev.31.13)

/// The `detail.kind` of the refusal of a second start ([`already_starting`]); the window tells it by this
/// (src/shell/play.ts `isAlreadyStarting`).
pub(crate) const ALREADY_STARTING: &str = "already_starting";

/// The refusal of a start while another start of the game is pending: `busy`, said as the window shows it.
pub(crate) fn already_starting() -> Error {
    Error::new(kiln_manage::ErrorCode::Busy, "The game is already starting.").with_detail(serde_json::json!({ "kind": ALREADY_STARTING }))
}

/// The games a launch_play (or launch_confirm) is inside its start for now: from its checks to Steam asked (or the
/// direct start made). A second one meanwhile is refused.
static PLAYING: std::sync::LazyLock<std::sync::Mutex<std::collections::HashSet<String>>> = std::sync::LazyLock::new(Default::default);

/// Refuses a start of `game_id` at `now` (unix ms) while another one is pending: a start this launcher tracks holds it
/// (Steam asked, no game process seen yet, within the window's wait: [`LauncherStart::holds_next_start`]; Kiln and
/// Vanilla starts alike), or a launch_play is inside its start for the game ([`PLAYING`]). A start the window ended
/// (launch_end: Stop waiting, a failed Play), one that failed, one whose game was seen, and one past the wait hold nothing.
///
/// [`LauncherStart::holds_next_start`]: kiln_manage::start_progress::LauncherStart::holds_next_start
pub(crate) fn refuse_pending(game_id: &str, now: i64) -> Result<(), Error> {
    let held = STARTS.lock().unwrap_or_else(|e| e.into_inner()).get(game_id).is_some_and(|t| t.holds_next_start(now));
    if held || PLAYING.lock().unwrap_or_else(|e| e.into_inner()).contains(game_id) {
        return Err(already_starting());
    }
    Ok(())
}

/// A launch_play's hold on its game ([`claim`]): let go when it is dropped (the start's own steps done, whatever came).
pub(crate) struct Claim(String);

impl Drop for Claim {
    fn drop(&mut self) {
        PLAYING.lock().unwrap_or_else(|e| e.into_inner()).remove(&self.0);
    }
}

/// [`refuse_pending`], then holds `game_id` for this start until the returned claim is dropped.
pub(crate) fn claim(game_id: &str, now: i64) -> Result<Claim, Error> {
    // (one lock for the check and the hold: two launch_plays at once cannot both pass)
    let mut playing = PLAYING.lock().unwrap_or_else(|e| e.into_inner());
    let held = STARTS.lock().unwrap_or_else(|e| e.into_inner()).get(game_id).is_some_and(|t| t.holds_next_start(now));
    if held || !playing.insert(game_id.to_owned()) {
        return Err(already_starting());
    }
    Ok(Claim(game_id.to_owned()))
}

/// The Kiln runtime in the game (its install receipt's version).
fn runtime_version(g: &kiln_manage::game::GameInstall) -> Option<String> {
    kiln_manage::install::read_receipt(&g.path).ok().flatten().map(|r| r.runtime.version)
}

/// `launch_begin`: Play was pressed (before the cook): the start's own steps are timed from now. `mode` as
/// launch_play's; a start with the active profile cooks first (its `cook` step, with the mods due as Play's plan says).
/// (0.2.0-dev.31.13) A Vanilla start is tracked too (its profile and Steam steps, the wait for its process), so it holds
/// the next start as a Kiln start does; the window shows it without a bar, as before. Refused (`busy`,
/// [`already_starting`]) while another start of the game is pending ([`refuse_pending`]): that start's record stays.
#[tauri::command]
pub async fn launch_begin(game_id: String, mode: kiln_manage::launch::LaunchMode) -> Result<(), Error> {
    blocking(move || {
        refuse_pending(&game_id, now_ms())?;
        let g = crate::state::game(&game_id)?;
        let t = new_start(&dirs()?, &g, mode, now_ms());
        STOPS.lock().unwrap_or_else(|e| e.into_inner()).remove(&game_id);
        STARTS.lock().unwrap_or_else(|e| e.into_inner()).insert(game_id, t);
        Ok(())
    })
    .await
}

/// The record of a start Play began at `at` with `mode`: a Vanilla start has no Kiln, no cook and no mods to time.
fn new_start(dirs: &kiln_manage::paths::DataDirs, g: &kiln_manage::game::GameInstall, mode: kiln_manage::launch::LaunchMode, at: i64)
    -> kiln_manage::start_progress::LauncherStart {
    use kiln_manage::launch::LaunchMode;
    let mut t = if mode == LaunchMode::Vanilla {
        kiln_manage::start_progress::LauncherStart::new(at, None, None, None)
    } else {
        let cook = (mode == LaunchMode::Profile).then(|| {
            kiln_manage::cook::plan(g).unwrap_or_default().into_iter().filter(|j| j.reason.is_some())
                .map(|j| kiln_manage::start_progress::cook_key(&j.id, &j.identity)).collect()
        });
        let request = LaunchRequest { mode, profile_id: None, skip_backup: true };
        let fp = if mode == LaunchMode::Once { None } else { kiln_manage::launch::launch_fingerprint(dirs, g, &request).ok() };
        kiln_manage::start_progress::LauncherStart::new(at, cook, runtime_version(g), fp)
    };
    // a direct start's last launcher step is the launcher starting the game, not Steam (its label before it begins)
    t.direct = kiln_manage::direct::mode_of(dirs, g) == kiln_manage::direct::StartMode::Direct;
    t.mode = Some(mode);
    t
}

/// One look at a game's start: the timeline (None: nothing to show, no start of this launcher and no progress.json of
/// the running game), and the start kept in the history once it reached `ready`. The watcher calls it when
/// progress.json changes, `launch_progress` when the window asks.
pub(crate) fn progress_round(dirs: &kiln_manage::paths::DataDirs, g: &kiln_manage::game::GameInstall) -> Option<kiln_manage::start_progress::Timeline> {
    use kiln_manage::start_progress as sp;
    let side = sp::game_side(g);
    let now = now_ms();
    let mut starts = STARTS.lock().unwrap_or_else(|e| e.into_inner());
    // the deadlines first, then the process is this start's (LauncherStart::keep_tracking); a Vanilla start is over once
    // its game is there (nothing of Kiln to follow: the window shows the running game as before)
    if starts.get_mut(&g.id).is_some_and(|t| !t.keep_tracking(side.running, now) || (t.vanilla() && t.seen)) {
        starts.remove(&g.id);
    }
    let launcher = starts.get(&g.id).cloned();
    drop(starts);
    // a progress.json of another start than this launcher's (Steam's Play while this one waited): its own timeline
    let launcher = launcher.filter(|l| side.progress.as_ref().and_then(|p| sp::parse_ms(&p.started)).is_none_or(|s| s >= l.began));
    // the version a start is stored and looked up by is progress.json's `kiln`; the receipt's only finds it before then
    let receipt = launcher.as_ref().and_then(|l| l.kiln.clone()).or_else(|| runtime_version(g));
    let fingerprint = launcher.as_ref().and_then(|l| l.fingerprint.clone()).or_else(|| {
        let req = LaunchRequest { mode: kiln_manage::launch::LaunchMode::Profile, profile_id: None, skip_backup: true };
        side.progress.as_ref().and_then(|_| kiln_manage::launch::launch_fingerprint(dirs, g, &req).ok())
    });
    let r = {
        let mut steady = STEADY.lock().unwrap_or_else(|e| e.into_inner());
        sp::round(dirs, &g.id, launcher.as_ref(), &side, receipt.as_deref(), fingerprint.as_deref(), now, &mut steady)
    };
    match &r.kept {
        Some(sp::Kept::Kept | sp::Kept::Already) => track(&g.id, |t| t.kept = true),
        Some(sp::Kept::LeftAlone(why)) => {
            // said once per launcher run: every finished start would say it again
            static SAID: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
            if !SAID.swap(true, std::sync::atomic::Ordering::SeqCst) {
                crate::commands::app::launcher_log("start times", why);
            }
            track(&g.id, |t| t.kept = true);
        }
        None => {}
    }
    let tl = r.timeline?;
    if tl.phase == sp::Phase::Ended || (!side.running && launcher.as_ref().is_some_and(|l| l.kept)) {
        forget(&g.id);
    }
    Some(tl)
}

/// The bar's highest point per start: it never moves back, whoever looks.
static STEADY: std::sync::LazyLock<std::sync::Mutex<kiln_manage::start_progress::Steady>> = std::sync::LazyLock::new(Default::default);

/// `launch_end`: Play ended before the game's process existed (the start was refused before Steam was asked, or the
/// window gave it up): the start this launcher was timing is over, and the window shows what it showed before Play.
#[tauri::command]
pub async fn launch_end(game_id: String) -> Result<(), Error> {
    forget(&game_id);
    Ok(())
}

/// `launch_progress`: what the start of the game is doing now, as one timeline (`kiln_manage::start_progress::Timeline`:
/// the launcher's own steps, then progress.json's, the running step, the time left from this PC's earlier starts), or
/// null when there is none to show (the window shows the game's state as before). A runtime without progress.json
/// (0.2.0-dev.26) says `legacy` once its boot.json is there. Polled by the window every second while a start runs.
#[tauri::command]
pub async fn launch_progress(game_id: String) -> Result<Option<kiln_manage::start_progress::Timeline>, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        Ok(progress_round(&dirs()?, &g))
    })
    .await
}

/// `launch_prestart`: what kiln-eml is doing before the game's window exists (a first start that fingerprints the game's data
/// files, a patch run): `kiln/state/eml.json` with phase `running`, while the game runs and the file is fresh. None otherwise.
/// The Play page polls it faster than `launch_state` while it waits for Kiln to boot.
#[tauri::command]
pub async fn launch_prestart(game_id: String) -> Result<Option<kiln_manage::prestart::PreStart>, Error> {
    blocking(move || kiln_manage::prestart::read(&crate::state::game(&game_id)?)).await
}

#[tauri::command]
pub async fn launch_state(game_id: String) -> Result<BootState, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        let boot = kiln_manage::stopped_start::boot_state(&g.path);
        let mods = kiln_manage::state::read_mods(&g.path)?;
        let native = kiln_manage::state::read_native(&g.path)?;
        // under Proton the pid in boot.json is the game's Windows pid: boot_is_current compares write and start times
        let stale = boot.as_ref().is_some_and(|b| !kiln_manage::launch::boot_is_current(&g, b));
        let log = boot.as_ref().and_then(|b| log_errors(&g.path, b));
        observe_stop(&g, kiln_manage::proc::find_game(&g).ok().flatten().map(|p| p.pid));
        let stopped_start = STOPS.lock().unwrap_or_else(|e| e.into_inner()).get(&game_id).and_then(|s| s.result.clone());
        Ok(BootState { boot, mods, native, stale, log, stopped_start })
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, SystemTime};

    fn put(p: &Path, text: &str, at: SystemTime) {
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, text).unwrap();
        std::fs::File::options().write(true).open(p).unwrap().set_modified(at).unwrap();
    }

    #[test]
    fn last_boot_counts_the_error_lines_of_its_own_run() {
        let game = std::env::temp_dir().join(format!("kl-logerr-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&game);
        let t0 = SystemTime::now() - Duration::from_secs(600);
        let boot: kiln_manage::state::BootJson = serde_json::from_str(
            r#"{"format":1,"kiln":"0.2.0-dev","pid":384,"written":"2026-11-06T07:00:02Z","side":"client","hooks":true,"profile":{"mode":"active"}}"#,
        )
        .unwrap();
        let logs = game.join("kiln/logs");
        put(&logs.join("kiln.prev.log"), "[I 01:50:00.000] [kiln] Kiln 0.2.0-dev starting (pid 384)\n", t0);
        put(&game.join("kiln/state/boot.json"), "{}", t0 + Duration::from_secs(60));
        let start = "[I 02:00:00.000] [kiln] Kiln 0.2.0-dev+20261106.8d20598 starting (pid 384)\n";
        let clean = format!("{start}[I 02:00:01.000] [kiln] boot complete; handing control to the game\n");
        put(&logs.join("kiln.log"), &clean, t0 + Duration::from_secs(61));
        assert_eq!(log_errors(&game, &boot).unwrap().errors, 0, "a clean boot");
        // errors after the boot, a stack trace under one of them, and a line still being written
        let later = format!(
            "{clean}[E 02:00:20.749] [kiln] kiln service tick 'kiln creature brains' threw (1x): boom\n   at Kiln.World.Find()\n[W 02:00:21.000] [kiln] slow\n[E 02:00:22.073] [kiln] kiln service tick 'kiln creature brains' threw (2x): boom\n[E 02:00:23"
        );
        put(&logs.join("kiln.log"), &later, t0 + Duration::from_secs(90));
        let e = log_errors(&game, &boot).unwrap();
        assert_eq!(e.errors, 2, "{e:?}");
        assert!(e.first[0].starts_with("[E 02:00:20.749] [kiln] kiln service tick"));
        // the half line completes: counted once, from where the last read stopped
        put(&logs.join("kiln.log"), &format!("{later}.407] [kiln] third\n"), t0 + Duration::from_secs(95));
        assert_eq!(log_errors(&game, &boot).unwrap().errors, 3);
        // another process's log (the pid differs) is not this boot's
        let other: kiln_manage::state::BootJson = serde_json::from_value(serde_json::json!({
            "format": 1, "kiln": "0.2.0-dev", "pid": 512, "written": "2026-11-06T07:00:02Z", "side": "client", "hooks": true, "profile": { "mode": "active" }
        }))
        .unwrap();
        assert!(log_errors(&game, &other).is_none());
        // a new start died before writing boot.json: kiln.log is the new run's, boot.json the old one's
        put(&logs.join("kiln.prev.log"), &later, t0 + Duration::from_secs(120));
        put(&logs.join("kiln.log"), &format!("{start}[E 02:10:00.000] [kiln] cannot parse the game image\n"), t0 + Duration::from_secs(130));
        assert!(log_errors(&game, &boot).is_none());
        let _ = std::fs::remove_dir_all(&game);
    }

    /// Kiln 0.2.0-dev.31 to 31.4 logged a world write that went ahead of a slow Kiln.Saves writer as an error at every
    /// autosave (one player's launcher said "373 errors since start"); later runtimes log it as a warning. That old line
    /// is not counted; a staging that really failed still is, by its own error lines.
    #[test]
    fn a_slow_writer_line_of_an_older_runtime_is_not_an_error() {
        let game = std::env::temp_dir().join(format!("kl-slowwriter-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&game);
        let t0 = SystemTime::now() - Duration::from_secs(600);
        let boot: kiln_manage::state::BootJson = serde_json::from_str(
            r#"{"format":1,"kiln":"0.2.0-dev","pid":384,"written":"2026-11-06T07:00:02Z","side":"client","hooks":true,"profile":{"mode":"active"}}"#,
        )
        .unwrap();
        let logs = game.join("kiln/logs");
        put(&game.join("kiln/state/boot.json"), "{}", t0);
        let log = [
            "[I 02:00:00.000] [kiln] Kiln 0.2.0-dev.31.2 starting (pid 384)",
            "[W 02:00:30.100] [service:kiln-saves.dll] saves: the world write of revision 120 waited 6001 ms for Kiln.Saves' writer and went ahead (staging 40 of 41 on disk); a crash before the writer finishes loads an older generation of mod data",
            "[E 02:00:30.100] [kiln] player-storage: persistence degraded: world generation 120 completed before storage became durable (write failed or bounded barrier wait expired); pairs kept in memory, next save retries",
            "[E 02:01:00.100] [kiln] player-storage: persistence degraded: world generation 121 completed before storage became durable (write failed or bounded barrier wait expired); pairs kept in memory, next save retries",
            "[W 02:01:30.100] [kiln] player-storage: world generation 122 was written before its storage reached the disk: Kiln.Saves' writer is slow here (a delay, not an error)",
            "[E 02:02:00.000] [kiln] Kiln.Saves: writing mod data generation 123 failed 3 times: injected (the world write goes ahead without it)",
            "[E 02:02:00.100] [kiln] player-storage: persistence degraded: pending generation 123 could not be written; pairs kept in memory, next save retries",
            "[E 02:02:00.200] [kiln] player-storage: persistence degraded: world generation 123 completed before storage became durable (its staging could not be written); pairs kept in memory, next save retries",
            "",
        ]
        .join("\n");
        put(&logs.join("kiln.log"), &log, t0 + Duration::from_secs(60));
        let e = log_errors(&game, &boot).unwrap();
        assert_eq!(e.errors, 3, "{e:?}");
        assert!(e.first.iter().all(|l| !l.contains("bounded barrier wait expired")), "{e:?}");
        assert!(e.first[0].contains("writing mod data generation 123 failed"), "{e:?}");
        let _ = std::fs::remove_dir_all(&game);
    }
}

// Retain the process identity through exit: state_file_is_current requires a live process and cannot be used then.
#[derive(Default)]
struct StopRun {
    identity: Option<kiln_manage::stopped_start::StartIdentity>,
    result: Option<kiln_manage::stopped_start::StoppedStart>,
}
static STOPS: std::sync::LazyLock<std::sync::Mutex<std::collections::HashMap<String, StopRun>>> = std::sync::LazyLock::new(Default::default);

fn adopt_stop_process(run: &mut StopRun, pid: u32, at: i64, began: Option<i64>) {
    if began.is_none_or(|b| at >= b - 1000)
        && run.identity.as_ref().is_none_or(|i| i.began_ms != at)
    {
        *run = StopRun { identity: Some(kiln_manage::stopped_start::StartIdentity {
            pid: cfg!(windows).then_some(pid), began_ms: at, ended_ms: None,
        }), result: None };
    }
}

pub(crate) fn observe_stop(g: &kiln_manage::game::GameInstall, pid: Option<u32>) {
    use kiln_manage::{stopped_start as ss, start_progress as sp};
    let began = STARTS.lock().unwrap_or_else(|e| e.into_inner()).get(&g.id).filter(|s| s.asked && !s.vanilla()).map(|s| s.began + s.steps.iter().find(|step| step.id == "steam").map_or(0, |step| step.ms as i64));
    let mut all = STOPS.lock().unwrap_or_else(|e| e.into_inner());
    let run = all.entry(g.id.clone()).or_default();
    if let Some(pid) = pid {
        if let Some(at) = kiln_manage::proc::start_time(pid).map(sp::unix_ms) {
            adopt_stop_process(run, pid, at, began);
        }
    } else if let Some(i) = &mut run.identity {
        i.ended_ms.get_or_insert_with(now_ms);
    }
    if run.identity.is_none() && let Some(asked) = began {
        run.identity = ss::failed_identity(&g.path, asked, now_ms());
    }
    let Some(identity) = &mut run.identity else { return };
    if run.result.is_some() { return; }
    if pid.is_some() && let Some((progress, _)) = sp::read_progress(g) {
        identity.pid = Some(progress.pid);
    }
    // A stop is final even while the client's message box is still open. Only deliberate stop evidence qualifies.
    if let Some(mut stop) = ss::read(&g.path, identity, &Default::default()) {
        if let Ok(d) = dirs() && let Ok(entries) = kiln_manage::library::list(&d, Some(kiln_manage::manifest::ModKind::Kiln)) {
            for (id, name) in stop.mods.iter().zip(stop.names.iter_mut()) {
                if let Some(e) = entries.iter().find(|e| &e.id == id) { *name = e.name.clone(); }
            }
        }
        identity.pid = Some(stop.pid);
        crate::commands::app::launcher_log("stopped start", &format!("{}: {}", g.id, stop.reason));
        run.result = Some(stop);
    }
}

#[cfg(test)]
mod stop_process_tests {
    use super::*;
    #[test]
    fn a_later_external_process_replaces_the_refused_run_without_a_launcher_request() {
        let stop = kiln_manage::stopped_start::StoppedStart {
            kind: kiln_manage::stopped_start::StopKind::Roster, reason: "full".into(), mods: vec![], names: vec![], advice: "switch one off".into(),
            utc: "2026-11-15T12:00:00Z".into(), exit_code: 19202, pid: 42,
        };
        let mut run = StopRun { identity: Some(kiln_manage::stopped_start::StartIdentity {
            pid: Some(42), began_ms: 1000, ended_ms: Some(1500),
        }), result: Some(stop.clone()) };
        adopt_stop_process(&mut run, 9000, 2000, None);
        assert_eq!(run.identity.as_ref().unwrap().began_ms, 2000);
        assert!(run.result.is_none(), "old refusal must not be returned for the later Steam process");
        run.result = Some(stop.clone());
        adopt_stop_process(&mut run, 42, 2000, None);
        assert_eq!(run.result, Some(stop), "the refused process with its message box still open keeps its notice");
        let mut fresh = StopRun::default();
        adopt_stop_process(&mut fresh, 9000, 3000, None);
        assert_eq!(fresh.identity.unwrap().began_ms, 3000, "reopening during a Steam start adopts its identity");
    }
}

/// (0.2.0-dev.31.13) One start at a time: a start Steam was asked for (or a direct start's) holds the next one, Kiln and
/// Vanilla alike, until its game is seen, the window's wait is over, the window ends it (Stop waiting) or it fails.
#[cfg(test)]
mod guard_tests {
    use super::*;
    use kiln_manage::launch::{LaunchMode, Starter};
    use kiln_manage::start_progress::{LauncherStart, STEAM_WAIT_MS};

    /// Play pressed for `id` at `at` with `mode` (what launch_begin records, without a game folder).
    fn begin(id: &str, mode: LaunchMode, at: i64) {
        let cook = (mode == LaunchMode::Profile).then(Vec::new);
        let mut t = LauncherStart::new(at, cook, None, None);
        t.mode = Some(mode);
        STARTS.lock().unwrap().insert(id.to_owned(), t);
    }

    /// launch_play's steps for `id`: the profile, then Steam asked through `open` (dry: nothing is opened), with the
    /// claim held meanwhile as start_game holds it. The record is dropped on a failure, as start_game drops it.
    fn play(id: &str, open: &dyn Fn(&str) -> Result<(), Error>) -> Result<(), Error> {
        let _claim = claim(id, now_ms())?;
        track(id, |t| t.begin("profile", now_ms()));
        let spawn = |_: &kiln_manage::direct::DirectCommand, _: &Path| -> Result<(), Error> { unreachable!("no direct start here") };
        let r = with_starter(id, None, false, open, &spawn, &|_, _| {}, |s| match s {
            Starter::Steam(f) => f("steam://rungameid/1203620"),
            Starter::Direct(..) => unreachable!(),
        });
        if let Err(e) = &r {
            failed(id, e);
        }
        r
    }

    fn steam_ok(_: &str) -> Result<(), Error> {
        Ok(())
    }

    fn refused(r: Result<(), Error>) -> bool {
        match r {
            Err(e) => {
                assert_eq!(e.code, kiln_manage::ErrorCode::Busy);
                assert_eq!(e.message, "The game is already starting.");
                assert_eq!(e.detail.as_ref().and_then(|d| d["kind"].as_str()), Some(ALREADY_STARTING));
                true
            }
            Ok(()) => false,
        }
    }

    #[test]
    fn a_vanilla_start_steam_was_asked_for_refuses_a_second_vanilla_and_a_kiln_start() {
        let id = "guard-vanilla";
        begin(id, LaunchMode::Vanilla, now_ms());
        play(id, &steam_ok).unwrap();
        let t = STARTS.lock().unwrap().get(id).cloned().unwrap();
        assert!(t.asked && t.vanilla(), "the Vanilla start is tracked and Steam was asked: {t:?}");
        // a second Play of either kind: launch_begin and launch_play (and launch_confirm) refuse it
        assert!(refused(refuse_pending(id, now_ms())), "a second launch_begin");
        assert!(refused(play(id, &|_| panic!("Steam asked a second time"))), "a second Vanilla launch_play");
        assert!(refused(claim(id, now_ms()).map(|_| ())), "a Kiln start's launch_play");
        // the refusal leaves the pending start as it was (no launch_end, no forget)
        assert_eq!(STARTS.lock().unwrap().get(id), Some(&t));
        forget(id);
    }

    #[test]
    fn a_kiln_start_steam_was_asked_for_refuses_a_vanilla_start() {
        let id = "guard-kiln";
        begin(id, LaunchMode::Profile, now_ms());
        // the cook ran, then launch_play
        track(id, |t| t.cook_done(now_ms()));
        play(id, &steam_ok).unwrap();
        assert!(refused(refuse_pending(id, now_ms())), "Vanilla's launch_begin");
        assert!(refused(play(id, &|_| panic!("Steam asked a second time"))), "Vanilla's launch_play");
        forget(id);
    }

    #[test]
    fn a_launch_play_inside_its_start_refuses_another_until_it_is_done() {
        let id = "guard-inflight";
        begin(id, LaunchMode::Vanilla, now_ms());
        let held = claim(id, now_ms()).unwrap();
        // the first launch_play has not asked Steam yet (it writes the play-once file): a second one waits for nothing
        assert!(refused(claim(id, now_ms()).map(|_| ())));
        assert!(refused(refuse_pending(id, now_ms())));
        drop(held);
        // done (here without Steam asked: a refusal before it): nothing holds the next start
        assert!(refuse_pending(id, now_ms()).is_ok());
        forget(id);
    }

    #[test]
    fn nothing_holds_a_start_before_steam_is_asked() {
        // Play pressed, the cook or the warning before Play under way: the next Play replaces that start, as before
        let id = "guard-unasked";
        begin(id, LaunchMode::Profile, now_ms());
        assert!(refuse_pending(id, now_ms()).is_ok());
        begin(id, LaunchMode::Vanilla, now_ms());
        track(id, |t| t.begin("profile", now_ms()));
        assert!(refuse_pending(id, now_ms()).is_ok());
        forget(id);
    }

    #[test]
    fn the_hold_ends_when_the_game_is_seen() {
        for (id, mode) in [("guard-seen-vanilla", LaunchMode::Vanilla), ("guard-seen-kiln", LaunchMode::NoMods)] {
            begin(id, mode, now_ms());
            play(id, &steam_ok).unwrap();
            assert!(refused(refuse_pending(id, now_ms())));
            // the watcher's round sees the game's process (the game-running checks refuse a second start from now on)
            prune(id, true);
            assert!(refuse_pending(id, now_ms()).is_ok(), "{mode:?}");
            // a Vanilla start is over then (nothing of Kiln to follow); a Kiln start goes on being followed
            assert_eq!(STARTS.lock().unwrap().contains_key(id), mode != LaunchMode::Vanilla, "{mode:?}");
            forget(id);
        }
    }

    #[test]
    fn the_hold_ends_with_the_windows_wait() {
        let id = "guard-timeout";
        begin(id, LaunchMode::Vanilla, now_ms());
        play(id, &steam_ok).unwrap();
        let now = now_ms();
        assert!(refused(refuse_pending(id, now + STEAM_WAIT_MS as i64 - 1_000)));
        // the window offers Play again after STEAM_WAIT_MS: that Play is not refused, and replaces the start as before
        assert!(refuse_pending(id, now + STEAM_WAIT_MS as i64 + 1_000).is_ok());
        assert!(claim(id, now + STEAM_WAIT_MS as i64 + 1_000).is_ok());
        assert!(STARTS.lock().unwrap().get(id).is_some_and(|t| t.holds_next_start(now) && !t.holds_next_start(now + STEAM_WAIT_MS as i64 + 1_000)));
        forget(id);
    }

    #[test]
    fn the_hold_ends_when_the_window_stops_waiting() {
        let id = "guard-cancel";
        begin(id, LaunchMode::Vanilla, now_ms());
        play(id, &steam_ok).unwrap();
        assert!(refused(refuse_pending(id, now_ms())));
        // Stop waiting (and a Play that failed in the window): launch_end
        tauri::async_runtime::block_on(launch_end(id.to_owned())).unwrap();
        assert!(refuse_pending(id, now_ms()).is_ok());
        assert!(!STARTS.lock().unwrap().contains_key(id));
    }

    #[test]
    fn the_hold_ends_when_the_start_fails() {
        let id = "guard-failed";
        begin(id, LaunchMode::Vanilla, now_ms());
        // Steam's opener failed: the start is over, and the claim went with it
        let e = play(id, &|_| Err(Error::new(kiln_manage::ErrorCode::Io, "xdg-open failed"))).unwrap_err();
        assert_eq!(e.code, kiln_manage::ErrorCode::Io);
        assert!(!STARTS.lock().unwrap().contains_key(id));
        assert!(refuse_pending(id, now_ms()).is_ok());
        // and Play again goes through
        begin(id, LaunchMode::Vanilla, now_ms());
        play(id, &steam_ok).unwrap();
        forget(id);
    }

    #[test]
    fn a_vanilla_starts_timeline_says_it_is_vanilla() {
        let id = "guard-timeline";
        let at = now_ms();
        begin(id, LaunchMode::Vanilla, at);
        play(id, &steam_ok).unwrap();
        let t = STARTS.lock().unwrap().get(id).cloned().unwrap();
        let tl = kiln_manage::start_progress::timeline(&kiln_manage::start_progress::Inputs { launcher: Some(&t), now: now_ms(), ..Default::default() }).unwrap();
        assert_eq!(tl.mode, Some(LaunchMode::Vanilla));
        assert_eq!(tl.phase, kiln_manage::start_progress::Phase::Launcher);
        assert!(tl.steps.iter().any(|s| s.id == "steam" && s.state == kiln_manage::start_progress::StepState::Running), "{tl:?}");
        assert!(tl.steps.iter().all(|s| s.id != "cook"), "a Vanilla start cooks nothing");
        let json = serde_json::to_value(&tl).unwrap();
        assert_eq!(json["mode"], "vanilla");
        forget(id);
        // a start without a mode says none (the field is left out)
        let plain = LauncherStart::new(at, None, None, None);
        let tl = kiln_manage::start_progress::timeline(&kiln_manage::start_progress::Inputs { launcher: Some(&plain), now: at, ..Default::default() }).unwrap();
        assert!(serde_json::to_value(&tl).unwrap().get("mode").is_none());
    }
}
