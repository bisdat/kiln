//! Tauri commands: runtime. Names and arguments: docs/kiln/launcher/contracts.md section 9.
//! Bundles on kiln_manage::bundle, install/uninstall/rollback on kiln_manage::install, run as tasks (spawn_task).

use super::TaskId;
use super::app::{spawn_task, to_value};
use crate::state::{blocking, dirs};
use kiln_manage::bundle::{self, RuntimeBundle};
use kiln_manage::game::{GameInstall, GameKind};
use kiln_manage::install::{self, InstallOptions, InstallReceipt, InstallState};
use kiln_manage::manifest::RunSide;
use kiln_manage::pe::{self, ProxyClass};
use kiln_manage::task::Progress;
use kiln_manage::{Error, ErrorCode};
use tauri::path::BaseDirectory;
use tauri::{AppHandle, Manager};

/// The Kiln runtime this launcher ships (tools/launcher/shipped-runtime.sh), as a resource of the app.
pub(crate) const SHIPPED_RUNTIME: &str = "runtime/kiln-runtime.zip";
static SHIPPED_DONE: std::sync::Mutex<bool> = std::sync::Mutex::new(false);
/// The version of the runtime this launcher ships, once [`import_shipped`] read it (None: none shipped, not read yet).
static SHIPPED_VERSION: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);

/// (dev.29) The runtime this launcher ships: the game's Kiln follows it after a switch back from a test build
/// (state::install_state, InstallState.switch_back).
pub fn shipped_version() -> Option<String> {
    SHIPPED_VERSION.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

/// Copies the shipped runtime into `<data>/runtime/`, once per run. A build without one has nothing to copy, and
/// Runtime still takes a dropped zip. Callers wait on the lock, so the list never shows before the copy is done.
pub fn import_shipped(app: &AppHandle) {
    let mut done = SHIPPED_DONE.lock().unwrap_or_else(|e| e.into_inner());
    if std::mem::replace(&mut *done, true) {
        return;
    }
    let Ok(zip) = app.path().resolve(SHIPPED_RUNTIME, BaseDirectory::Resource) else { return };
    if !zip.is_file() {
        return;
    }
    if let Ok(m) = bundle::read(&zip) {
        *SHIPPED_VERSION.lock().unwrap_or_else(|e| e.into_inner()) = Some(m.version);
    }
    // it writes `runtime/` (also from setup's own thread): a writer a move waits for
    let _writing = crate::state::writing();
    match dirs().and_then(|d| bundle::import_shipped(&d, &zip)) {
        Ok(Some(b)) => {
            eprintln!("shipped Kiln runtime {} added", b.version);
            // a game status made while the copy ran said "nothing to update" (the bundle was not there yet): after a
            // launcher update the "Updated to" notice and Play's note waited for the next poll, which an unfocused
            // window never sends. Fresh statuses go out now.
            crate::state::forget_install_states();
            let app = app.clone();
            std::thread::spawn(move || {
                let games = dirs().and_then(|d| kiln_manage::game::known(&d)).unwrap_or_default();
                for g in games {
                    crate::watch::emit_status(&app, &g.id);
                }
            });
        }
        Ok(None) => {}
        Err(e) => eprintln!("shipped Kiln runtime {}: {e}", zip.display()),
    }
}

#[tauri::command]
pub async fn runtime_list(app: AppHandle) -> Result<Vec<RuntimeBundle>, Error> {
    blocking(move || {
        import_shipped(&app);
        bundle::list(&dirs()?)
    })
    .await
}

#[tauri::command]
pub async fn runtime_import(path: String) -> Result<RuntimeBundle, Error> {
    blocking(move || bundle::import(&dirs()?, std::path::Path::new(&path))).await
}

/// Always fresh (hashes the installed files to list changed ones).
#[tauri::command]
pub async fn runtime_status(game_id: String) -> Result<InstallState, Error> {
    blocking(move || crate::state::install_state(&crate::state::game(&game_id)?, true)).await
}

/// The receipt's `installer`: `kiln-launcher <version>`, and (dev.29) ` beta <n>` from a test build, so a release
/// launcher knows the game's Kiln came with one (kiln_manage::install::installed_by_test_build).
/// A manually selected different runtime gets the ordinary installer name.
fn installer(version: &str) -> Option<String> {
    Some(installer_for_runtime(env!("CARGO_PKG_VERSION"), crate::distribution::beta(), shipped_version().as_deref(), version))
}

fn installer_for_runtime(version: &str, beta: Option<u32>, shipped: Option<&str>, runtime: &str) -> String {
    installer_name(version, beta.filter(|_| shipped == Some(runtime)))
}

fn installer_name(version: &str, beta: Option<u32>) -> String {
    match beta {
        Some(n) => format!("kiln-launcher {version} beta {n}"),
        None => format!("kiln-launcher {version}"),
    }
}

/// The fresh status goes out from its own thread: computing it (hashes, the bundle list, the process list) must not
/// hold the task's end back.
fn after_change(app: &AppHandle, game_id: &str) {
    crate::state::forget_install_state(game_id);
    let (app, id) = (app.clone(), game_id.to_string());
    std::thread::spawn(move || crate::watch::emit_status(&app, &id));
}

fn side_of(g: &GameInstall) -> RunSide {
    match g.kind {
        GameKind::Server => RunSide::Server,
        GameKind::Client => RunSide::Client,
    }
}

/// The task result of an install: the receipt, with `changedBackedUp` (the Kiln files changed by hand that the install
/// replaced; their copies are in the backup) and `backup` (the launcher's copy of the backup folder, or null) added.
pub(crate) fn install_result(receipt: &InstallReceipt, report: &install::InstallReport) -> Result<serde_json::Value, Error> {
    let mut v = to_value(receipt)?;
    if let Some(o) = v.as_object_mut() {
        o.insert("changedBackedUp".into(), serde_json::json!(report.changed_backed_up));
        o.insert("backup".into(), serde_json::json!(report.backup));
        o.insert("restoredShroudforge".into(), serde_json::json!(report.restored_shroudforge));
        o.insert("warnings".into(), serde_json::json!(report.warnings));
    }
    Ok(v)
}

/// After an install: the profile the launcher remembers for the game becomes the active one again
/// ([`kiln_manage::quick::resume_active`]), so a reinstall after an uninstall loads the player's mods without a step.
/// Fails soft: the install stands whatever happens here. The active profile is written again for the runtime the game
/// has now (any install kind). Returns `resumedProfile` (a remembered profile made active), the activation (resumed or
/// written again: what it kept in the game folder) and a note when that could not be done, for the task's result.
pub(crate) fn resume_profile(d: &kiln_manage::paths::DataDirs, g: &GameInstall, p: &mut dyn Progress)
                             -> (Option<String>, Option<kiln_manage::profile::Activation>, Option<String>) {
    let active_before = kiln_manage::profile::read_game_profile(&g.path).ok().flatten().is_some();
    match kiln_manage::quick::resume_active(d, g) {
        Ok(Some(a)) if !active_before => {
            p.report(Some(0.98), &format!("profile {} is active again", a.profile_id));
            (Some(a.profile_id.clone()), Some(a), None)
        }
        Ok(a) => (None, a, None),
        // the game started during the install: the next Play writes the profile again
        Err(e) if e.code == ErrorCode::GameRunning => (None, None, Some(format!("The game is running: the profile is written again for this Kiln at the next Play ({}).", e.message))),
        Err(e) => {
            let note = format!("The active or remembered profile was not written again for this Kiln: {}", e.message);
            p.report(None, &note);
            (None, None, Some(note))
        }
    }
}

/// Puts what [`resume_profile`] found into an install task's result: `resumedProfile`, `activation`, and its note
/// among `warnings`.
pub(crate) fn add_resumed(v: &mut serde_json::Value, (resumed, activation, note): (Option<String>, Option<kiln_manage::profile::Activation>, Option<String>)) {
    if let Some(o) = v.as_object_mut() {
        o.insert("resumedProfile".into(), serde_json::json!(resumed));
        o.insert("activation".into(), serde_json::json!(activation));
        if let Some(note) = note {
            match o.get_mut("warnings").and_then(|w| w.as_array_mut()) {
                Some(w) => w.push(note.into()),
                None => { o.insert("warnings".into(), serde_json::json!([note])); }
            }
        }
    }
}

/// An install only counts when the game folder says so afterwards: the receipt names the bundle's version and parts,
/// and every file it lists has its bytes. Anything else fails the task with what is wrong.
pub(crate) fn check_applied(g: &GameInstall, version: &str, receipt: &InstallReceipt, p: &mut dyn Progress) -> Result<(), Error> {
    p.report(Some(0.95), "checking the game folder");
    let on_disk = install::read_receipt(&g.path)?
        .ok_or_else(|| Error::new(ErrorCode::Internal, format!("the install ended, but {} has no kiln/.install.json", g.path.display())))?;
    if on_disk.runtime.version != version || on_disk.runtime.components != receipt.runtime.components {
        return Err(Error::new(ErrorCode::Internal, format!(
            "the install ended, but kiln/.install.json says Kiln {} ({}), not {version} ({})",
            on_disk.runtime.version,
            on_disk.runtime.components.join(", "),
            receipt.runtime.components.join(", ")
        )));
    }
    let wrong = install::modified_files(&g.path, &on_disk);
    if !wrong.is_empty() {
        return Err(Error::new(ErrorCode::HashMismatch, format!(
            "Kiln {version} is not fully in the game folder: {} file(s) differ from the install, e.g. {}",
            wrong.len(),
            wrong.iter().take(3).cloned().collect::<Vec<_>>().join(", ")
        ))
        .with_detail(serde_json::json!({ "files": wrong })));
    }
    p.report(Some(1.0), &format!("Kiln {version} is in the game folder ({} files, {})", on_disk.files.len(), on_disk.runtime.components.join(", ")));
    Ok(())
}

#[tauri::command]
pub async fn runtime_install(app: AppHandle, game_id: String, version: String, options: InstallOptions) -> Result<TaskId, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let b = blocking({ let v = version.clone(); move || bundle::find(&dirs()?, &v) }).await?;
    // parts this side cannot take fail now, before the bundle's verify (a minute's work on a slow disk). None: the
    // installer keeps the player's parts and adds new or newly default ones not turned off (resolve_components).
    if let Some(parts) = options.components.clone() {
        let (file, side) = (b.file.clone(), side_of(&g));
        blocking(move || install::select_components(&bundle::read(&file)?, side, Some(&parts)).map(|_| ())).await?;
    }
    let title = format!("Install Kiln {}", b.kiln);
    let a = app.clone();
    Ok(spawn_task(&app, "runtime_install", title, move |p| {
        crate::state::refuse_running(&g)?;
        let d = dirs()?;
        let mut options = options;
        options.installer = options.installer.or_else(|| installer(&b.version));
        let r = install::install_report(&d, &g, &b.file, &options, p);
        after_change(&a, &game_id);
        let (r, report) = r?;
        check_applied(&g, &b.version, &r, p)?;
        let resumed = resume_profile(&d, &g, p);
        let mut v = install_result(&r, &report)?;
        add_resumed(&mut v, resumed);
        Ok(v)
    }))
}

#[tauri::command]
pub async fn runtime_uninstall(app: AppHandle, game_id: String) -> Result<TaskId, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let a = app.clone();
    Ok(spawn_task(&app, "runtime_uninstall", "Uninstall Kiln".into(), move |p| {
        crate::state::refuse_running(&g)?;
        let r = install::uninstall_report(&dirs()?, &g, p);
        after_change(&a, &game_id);
        let r = r?;
        // Keep structured retained paths/reasons in the task result, not only a transient progress line.
        let mut msg = format!("removed {} files", r.removed);
        if !r.restored.is_empty() {
            msg.push_str(&format!("; restored {}", r.restored.join(", ")));
        }
        if !r.kept.is_empty() {
            msg.push_str(&format!("; kept {} changed files", r.kept.len()));
        }
        if let Some(warning) = r.placements.warning() { msg.push_str(&format!("; {warning}")); }
        for kept in &r.placements.kept_files { msg.push_str(&format!("; {kept}")); }
        for warning in &r.warnings { msg.push_str(&format!("; {warning}")); }
        p.report(Some(1.0), &msg);
        to_value(&r)
    }))
}

#[tauri::command]
pub async fn runtime_rollback(app: AppHandle, game_id: String, version: String) -> Result<TaskId, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let a = app.clone();
    Ok(spawn_task(&app, "runtime_rollback", format!("Roll back to {version}"), move |p| {
        crate::state::refuse_running(&g)?;
        // install::rollback, with the launcher as the receipt's installer
        let d = dirs()?;
        let b = bundle::find(&d, &version)?;
        // the player's parts carry over by install::resolve_components (an older bundle without some of them leaves
        // them out instead of failing; their opt-outs stay recorded)
        let options = InstallOptions { components: None, switch_from_shroudforge: false, installer: installer(&version) };
        let r = install::install_report(&d, &g, &b.file, &options, p);
        after_change(&a, &game_id);
        let (r, report) = r?;
        check_applied(&g, &b.version, &r, p)?;
        let resumed = resume_profile(&d, &g, p);
        let mut v = install_result(&r, &report)?;
        add_resumed(&mut v, resumed);
        Ok(v)
    }))
}

#[tauri::command]
pub async fn runtime_classify(path: String) -> Result<ProxyClass, Error> {
    blocking(move || pe::classify_proxy(std::path::Path::new(&path))).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manually_selected_runtime_is_not_marked_as_the_betas_runtime() {
        let tag = |shipped, runtime| installer_for_runtime("0.2.0-dev.29", Some(1), shipped, runtime);
        assert_eq!(tag(Some("beta-runtime"), "beta-runtime"), "kiln-launcher 0.2.0-dev.29 beta 1");
        assert_eq!(tag(Some("beta-runtime"), "newer-release"), "kiln-launcher 0.2.0-dev.29");
        assert_eq!(tag(None, "manual-runtime"), "kiln-launcher 0.2.0-dev.29");
    }

    #[test]
    fn a_test_build_names_itself_in_the_receipt() {
        let r = |installer: String| InstallReceipt {
            format: 1, install_id: "x".into(), installed: "2026-11-15T00:00:00Z".into(), updated: None, installer: Some(installer),
            runtime: kiln_manage::install::ReceiptRuntime { version: "0.2.0-dev+1".into(), sha256: None, components: vec![], deselected: None },
            side: RunSide::Client, platform: None, game: None, files: vec![], replaced: vec![],
        };
        assert_eq!(installer_name("0.2.0-dev", Some(3)), "kiln-launcher 0.2.0-dev beta 3");
        assert_eq!(installer_name("0.2.0-dev", None), "kiln-launcher 0.2.0-dev");
        assert!(install::installed_by_test_build(&r(installer_name("0.2.0-dev", Some(3)))));
        assert!(!install::installed_by_test_build(&r(installer_name("0.2.0-dev", None))));
        // this test binary is no test build
        assert_eq!(installer("any-runtime").as_deref(), Some(concat!("kiln-launcher ", env!("CARGO_PKG_VERSION"))));
    }
}
