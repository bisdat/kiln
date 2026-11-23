//! Tauri commands: logs. Names and arguments: docs/kiln/launcher/contracts.md section 9.
//! Log files on kiln_manage::logs (names relative to the game/server folder, checked there); follows run in
//! crate::watch and arrive as `kiln://log`.

use crate::state::blocking;
use kiln_manage::{Error, ErrorCode};
use kiln_manage::logs::{self, CrashReport, LogChunk, LogFile};
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// `{"game": id}` | `{"server": id}`
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum LogTarget {
    Game(String),
    Server(String),
}

/// The folder log names are relative to.
pub fn target_dir(target: &LogTarget) -> Result<PathBuf, Error> {
    match target {
        LogTarget::Game(id) => Ok(crate::state::game(id)?.path),
        LogTarget::Server(id) => kiln_manage::server::instance::get(id)?
            .dir
            .ok_or_else(|| Error::new(ErrorCode::Unsupported, "Remote server logs come through the admin pages.")),
    }
}

#[tauri::command]
pub async fn logs_list(target: LogTarget) -> Result<Vec<LogFile>, Error> {
    blocking(move || logs::list(&target_dir(&target)?)).await
}

#[tauri::command]
pub async fn logs_read(target: LogTarget, file: String, offset: Option<u64>, max_bytes: Option<u64>) -> Result<LogChunk, Error> {
    blocking(move || logs::read(&target_dir(&target)?, &file, offset, max_bytes)).await
}

#[tauri::command]
pub async fn logs_follow(target: LogTarget, file: String) -> Result<String, Error> {
    blocking(move || {
        let dir = target_dir(&target)?;
        // the same name check as logs_read (a zero-byte read)
        logs::read(&dir, &file, None, Some(0))?;
        let path = dir.join(&file);
        crate::watch::follow(target, file, path)
    })
    .await
}

#[tauri::command]
pub async fn logs_unfollow(subscription: String) -> Result<(), Error> {
    if crate::watch::unfollow(&subscription) { Ok(()) } else { Err(Error::not_found(format!("no follow {subscription}"))) }
}

#[tauri::command]
pub async fn logs_crash_reports(target: LogTarget) -> Result<Vec<CrashReport>, Error> {
    blocking(move || logs::crash_reports(&target_dir(&target)?)).await
}

// ---------------------------------------------------------------- support report (kiln_manage::report)

/// `report_save`'s answer (launcher type).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportSaved {
    /// The zip written.
    pub path: String,
    pub size: u64,
    pub findings: Vec<kiln_manage::report::Finding>,
    /// Sections that could not be read (the rest is in the zip).
    pub errors: Vec<String>,
}

/// What a report covers: a game, a local server, or (None) the system and the launcher only. Reads every time, keeps
/// nothing between calls.
pub(crate) fn report_input(app: &tauri::AppHandle, target: Option<LogTarget>) -> Result<kiln_manage::report::ReportInput, Error> {
    let version = app.package_info().version.to_string();
    let mut input = kiln_manage::report::ReportInput {
        // a test build says so in the tool's name and in the channel line (dev.29)
        tool: match crate::distribution::beta() {
            Some(n) => format!("kiln-launcher {version} beta {n}"),
            None => format!("kiln-launcher {version}"),
        },
        channel: Some(report_channel(&version)),
        dirs: crate::state::dirs().ok(),
        deck: Some(crate::deck::detect::current().detected),
        roots: Some(crate::state::steam_roots()),
        ..Default::default()
    };
    // Linux: how the launcher draws its window (crate::render), one line in summary.txt
    if let Some(c) = crate::render::choice() {
        input.facts.push(("Launcher".into(), crate::render::says(c)));
    }
    // Linux: WebKit's DMA-BUF renderer in this start, why, and the sticky marker's sentence (crate::render)
    if let Some(d) = crate::render::dmabuf_report() {
        input.facts.push(("Launcher DMA-BUF".into(), d));
    }
    // Linux: whether WebKit composites (its hardware acceleration as applied; what the page measured: inbox 94)
    input.facts.extend(crate::render::compositing_report());
    // Linux: WebKit's sandbox in this start and the probe of bwrap; a finding when bwrap cannot start (crate::sandbox)
    if let Some(s) = crate::sandbox::state() {
        input.facts.push(("WebKit sandbox".into(), s.says()));
        input.findings.extend(crate::sandbox::finding(s));
    }
    // Windows: the launcher's own exe or uninstaller gone from its folder (an antivirus; crate::own_files)
    input.findings.extend(crate::own_files::finding());
    match target {
        Some(LogTarget::Game(id)) => input.install = Some(crate::state::game(&id)?),
        Some(t @ LogTarget::Server(_)) => input.game_dir = Some(target_dir(&t)?),
        None => {}
    }
    Ok(input)
}

/// The launcher's channel for the report: its build's (baked in for a test build) and the one it follows.
fn report_channel(version: &str) -> kiln_manage::report::ReportChannel {
    let baked = env!("KILN_LAUNCHER_BUILD_CHANNEL");
    let build = if !baked.is_empty() { baked } else if version.contains('-') { "dev" } else { "stable" };
    // the Nexus build follows nothing (no self-update): its own channel
    let setting = if crate::distribution::CURRENT.self_updates() { crate::state::read_settings().ok().and_then(|s| s.update_channel) } else { None };
    let follows = setting.filter(|s| matches!(s.as_str(), "stable" | "dev" | "beta")).unwrap_or_else(|| build.to_string());
    kiln_manage::report::ReportChannel { build: build.into(), beta: crate::distribution::beta(), follows }
}

/// "What's in this report" before Save or Send: the report built as `report_save` would write it (nothing is written),
/// each file with its group, size and how often each replacement stands in it, the files `exclude` leaves out, and what
/// a report never holds (kiln_manage::report::contents).
#[tauri::command]
pub async fn report_preview(app: tauri::AppHandle, target: Option<LogTarget>, exclude: Option<Vec<String>>) -> Result<kiln_manage::report::Contents, Error> {
    let log = |line: String| crate::commands::app::launcher_log("report", &line);
    log("building the support report for \"What's in this report\"".into());
    let mut input = report_input(&app, target).inspect_err(|e| log(format!("the support report could not be prepared: {}", e.message)))?;
    input.exclude = exclude.unwrap_or_default();
    let started = std::time::Instant::now();
    let r = blocking(move || Ok(kiln_manage::report::contents(&kiln_manage::report::build(&input)))).await;
    match &r {
        Ok(c) => log(format!("support report preview built in {} ms ({} files)", started.elapsed().as_millis(), c.files.len())),
        Err(e) => log(format!("the support report preview failed: {}", e.message)),
    }
    r
}

/// What the window says about a support report that never reaches `report_save` (the Save dialog closed without a
/// path, the dialog failing, an error shown to the player), so launcher.log tells a missing report's story.
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ReportNote {
    /// "Save report..." opened the contents dialog.
    Opened,
    /// The Save dialog answered without a path (the player cancelled it).
    DialogCancelled,
    /// The Save dialog answered with a path; report_save follows.
    DialogChosen,
    /// The native Save dialog failed; an automatic save follows.
    DialogFailed,
    /// The player chose the dialog-free Downloads action.
    SaveToDownloads,
    /// The save itself failed; `detail` is the error the window showed.
    Failed,
}

#[tauri::command]
pub async fn report_note(note: ReportNote, detail: Option<String>) -> Result<(), Error> {
    // the window's own words only, one line, bounded
    let detail: String = detail.unwrap_or_default().chars().filter(|c| !c.is_control()).take(400).collect();
    let line = match note {
        ReportNote::Opened => "support report: \"What's in this report\" opened".to_string(),
        ReportNote::DialogCancelled => "support report: the Save dialog was closed without a file name; nothing written".to_string(),
        ReportNote::DialogChosen => format!("support report: the Save dialog chose {detail}"),
        ReportNote::DialogFailed => format!("support report: the Save dialog failed: {detail}; trying Kiln's state folder, then Downloads, then home"),
        ReportNote::SaveToDownloads => "support report: Save to Downloads chosen; no Save dialog".to_string(),
        ReportNote::Failed => format!("support report: the window showed an error: {detail}"),
    };
    crate::commands::app::launcher_log("report", &line);
    Ok(())
}

/// Writes the support report zip to `path` (".zip" added when missing). `exclude`: other mods' files the player left
/// out ("What's in this report"; Kiln's own files cannot be left out). `destination` absent: the chosen path;
/// automatic: dialog failure (Kiln's state folder first); downloads: no dialog (Downloads, then home).
#[tauri::command]
pub async fn report_save(app: tauri::AppHandle, target: Option<LogTarget>, path: String, exclude: Option<Vec<String>>, destination: Option<kiln_manage::report::SaveDestination>) -> Result<ReportSaved, Error> {
    if path.trim().is_empty() {
        return Err(Error::invalid("Choose where to save the report."));
    }
    let log = |line: String| crate::commands::app::launcher_log("report", &line);
    let mut out = PathBuf::from(&path);
    if !out.extension().is_some_and(|e| e.eq_ignore_ascii_case("zip")) {
        out.as_mut_os_string().push(".zip");
    }
    log(format!("building the support report for {}", out.display()));
    let mut input = report_input(&app, target).inspect_err(|e| log(format!("the support report for {} could not be prepared: {}", out.display(), e.message)))?;
    input.exclude = exclude.unwrap_or_default();
    let places = crate::dialog_folder::Places::of_this_system(&app);
    let folders = kiln_manage::report::SaveFolders { owned: kiln_manage::util::state_dir(), downloads: places.downloads, home: places.home };
    let destination = destination.unwrap_or_default();
    log(format!("support report destination: {destination:?}"));
    // the blocking pool runs the write to its end even when the window stops waiting; every end is logged
    blocking(move || {
        let log = |line: String| crate::commands::app::launcher_log("report", &line);
        let started = std::time::Instant::now();
        let r = kiln_manage::report::build(&input);
        log(format!("support report built in {} ms ({} files); writing {}", started.elapsed().as_millis(), r.files.len(), out.display()));
        let (out, size) = kiln_manage::report::save_zip(&r, &out, destination, &folders, log)?;
        log(format!("support report written to {} ({size} bytes, {} findings, {} files left out)", out.display(), r.findings.len(), r.left_out.len()));
        Ok(ReportSaved { path: out.display().to_string(), size, findings: r.findings, errors: r.errors })
    })
    .await
}

/// The short summary for a forum post ("Copy summary"), redacted like the zip.
#[tauri::command]
pub async fn report_summary(app: tauri::AppHandle, target: Option<LogTarget>) -> Result<String, Error> {
    let input = report_input(&app, target)?;
    blocking(move || Ok(kiln_manage::report::build(&input).short)).await
}
