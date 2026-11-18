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
fn report_input(app: &tauri::AppHandle, target: Option<LogTarget>) -> Result<kiln_manage::report::ReportInput, Error> {
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
    // Linux: WebKit's sandbox in this start and the probe of bwrap; a finding when bwrap cannot start (crate::sandbox)
    if let Some(s) = crate::sandbox::state() {
        input.facts.push(("WebKit sandbox".into(), s.says()));
        input.findings.extend(crate::sandbox::finding(s));
    }
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

/// Writes the support report zip to `path` (".zip" added when missing).
#[tauri::command]
pub async fn report_save(app: tauri::AppHandle, target: Option<LogTarget>, path: String) -> Result<ReportSaved, Error> {
    if path.trim().is_empty() {
        return Err(Error::invalid("Choose where to save the report."));
    }
    let input = report_input(&app, target)?;
    blocking(move || {
        let mut out = PathBuf::from(&path);
        if !out.extension().is_some_and(|e| e.eq_ignore_ascii_case("zip")) {
            out.as_mut_os_string().push(".zip");
        }
        let r = kiln_manage::report::build(&input);
        let size = kiln_manage::report::write_zip(&r, &out)?;
        crate::commands::app::launcher_log("report", &format!("support report written ({size} bytes, {} findings)", r.findings.len()));
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
