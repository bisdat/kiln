//! Tauri commands: library. Names and arguments: docs/kiln/launcher/contracts.md section 9.
//! The content-addressed mod library on kiln_manage::library; imports run as tasks.

use super::TaskId;
use super::app::{launcher_log, spawn_task, to_value};
use crate::state::{blocking, dirs};
use kiln_manage::Error;
use kiln_manage::library::{self, Detected, LibraryEntry, LibraryRef, RemoveReport, TakenOver};
use kiln_manage::lint::{self, LintOptions, LintReport};
use kiln_manage::manifest::ModKind;
use kiln_manage::mod_settings::SettingsRemoval;
use kiln_manage::quick::AddReport;
use std::path::PathBuf;
use tauri::AppHandle;

#[tauri::command]
pub async fn library_list(kind: Option<ModKind>) -> Result<Vec<LibraryEntry>, Error> {
    blocking(move || library::list(&dirs()?, kind)).await
}

#[tauri::command]
pub async fn library_detect(path: String) -> Result<Detected, Error> {
    blocking(move || {
        let path = std::path::Path::new(&path);
        kiln_manage::quick::validate_import_path(path)?;
        library::detect(path)
    }).await
}

#[tauri::command]
pub async fn library_import(app: AppHandle, paths: Vec<String>) -> Result<TaskId, Error> {
    if paths.is_empty() {
        return Err(Error::invalid("Nothing to import."));
    }
    let title = match paths.as_slice() {
        [one] => format!("Import {}", std::path::Path::new(one).file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_else(|| one.clone())),
        many => format!("Import {} items", many.len()),
    };
    let paths: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
    Ok(spawn_task(&app, "library_import", title, move |p| {
        // a file still downloading or being scanned: waited for a few seconds, then said so (as library_add does)
        kiln_manage::quick::wait_until_ready(&paths, p)?;
        for path in &paths { kiln_manage::quick::validate_import_path(path)?; }
        let r = library::import(&dirs()?, &paths, p)?;
        log_import("library_import", &r);
        to_value(r)
    }))
}

/// What an import took, left out (with the reason) and did not need, one line each in launcher.log: the toast goes, a
/// support report keeps these (inbox 42). The task's own line says only that it ended.
fn log_import(task: &str, r: &library::ImportReport) {
    let name = |x: &LibraryRef| r.names.get(&format!("{}/{}", x.kind.as_str(), x.id)).cloned().unwrap_or_else(|| x.id.clone());
    for x in &r.imported {
        launcher_log("import", &format!("{task}: added {} ({}/{} {})", name(x), x.kind.as_str(), x.id, x.version));
    }
    for x in &r.existing {
        launcher_log("import", &format!("{task}: already there {} ({}/{} {})", name(x), x.kind.as_str(), x.id, x.version));
    }
    for s in &r.skipped {
        launcher_log("import", &format!("{task}: left out {}: {}", s.path, s.reason));
    }
    for n in &r.notes {
        launcher_log("import", &format!("{task}: not needed {}: {}", n.path, n.text));
    }
}

/// The simple flow (kiln_manage::quick::add): imports `paths`, adds what they hold to the game's active profile
/// (making "My mods" for a first-time player) and writes the game's profile.json again, as one task. Modpack zips are
/// not imported: the result lists them for the consent flow. Refused at once (no task) when Kiln is not in the game.
#[tauri::command]
pub async fn library_add(app: AppHandle, game_id: String, paths: Vec<String>) -> Result<TaskId, Error> {
    if paths.is_empty() {
        return Err(Error::invalid("Nothing to add."));
    }
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    if !g.path.join(kiln_manage::paths::game::KILN).is_dir() {
        return Err(Error::new(kiln_manage::ErrorCode::Unsupported, "Kiln is not in this game yet: install it first."));
    }
    let title = match paths.as_slice() {
        [one] => format!("Add {}", std::path::Path::new(one).file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_else(|| one.clone())),
        many => format!("Add {} items", many.len()),
    };
    let paths: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
    let a = app.clone();
    Ok(spawn_task(&app, "library_add", title, move |p| {
        let r = kiln_manage::quick::add(&dirs()?, &g, &paths, p)?;
        log_import("library_add", &r.import);
        if r.applied {
            super::profile::emit_status_later(a, game_id);
        }
        to_value(r)
    }))
}

/// Puts library versions into the profile the player picked: what a drop offers when it only reached the Library
/// (`AddReport.libraryOnly`: no profile is active in the game and none is remembered). The result is `library_add`'s,
/// with an empty `import`, so the toast and its Undo (`profile_revert`) are the same.
#[tauri::command]
pub async fn library_add_to_profile(app: AppHandle, game_id: String, profile_id: String, entries: Vec<LibraryRef>) -> Result<AddReport, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let r = blocking(move || kiln_manage::quick::add_to_profile(&dirs()?, &g, &profile_id, &entries, &mut kiln_manage::task::NoProgress)).await?;
    if r.applied {
        super::profile::emit_status_later(app, game_id);
    }
    Ok(r)
}

/// Refuses (game_running) while a game runs, or a server whose profile file points into the library, whatever `force`
/// says (it may be reading the mod). With `shown` (the `library_users` answer a confirmation named), it goes past
/// exactly those users: anything else that lists the version now is a conflict, whatever `force` says.
/// `settings` (the remove dialog's "Remove its settings too"): once this was the mod's last version, its settings go too
/// (kiln_manage::mod_settings, after a copy into the launcher's backups folder); the answer says what went. Null
/// without it, or while other versions of the mod stay.
#[tauri::command]
pub async fn library_remove(entry: LibraryRef, force: bool, shown: Option<Vec<String>>, settings: Option<bool>)
    -> Result<Option<SettingsRemoval>, Error> {
    let force = match shown {
        Some(shown) => library::Force::Past(shown),
        None => library::Force::from(force),
    };
    let r = blocking(move || library::remove_and_settings(&dirs()?, &entry, force, settings.unwrap_or(false))).await?;
    if let Some(s) = &r {
        log_settings_removal(s);
    }
    Ok(r)
}

/// "Remove its settings too" for a mod whose files are gone from the Library already (the remove dialog of a row
/// without a Library entry): kiln_manage::mod_settings::remove. Refused (conflict) while the Library holds a version of
/// it, (game_running) while a game runs.
#[tauri::command]
pub async fn library_remove_settings(kind: ModKind, id: String) -> Result<SettingsRemoval, Error> {
    let r = blocking(move || kiln_manage::mod_settings::remove(&dirs()?, kind, &id, &[])).await?;
    log_settings_removal(&r);
    Ok(r)
}

fn log_settings_removal(s: &SettingsRemoval) {
    for p in &s.removed {
        launcher_log("library", &format!("removed settings: {p}"));
    }
    if let Some(b) = &s.backup {
        launcher_log("library", &format!("the removed settings are copied in {b}"));
    }
    for n in &s.notes {
        launcher_log("library", n);
    }
}

/// "Use the changed files": a Library entry whose files the player changed in its folder (`LibraryVersion.changed`, or
/// the `detail.libraryChanged` of a refused Play or activation) becomes the mod's new entry, and every profile that
/// listed it lists that one; then the games where such a profile is active get their profile.json written again.
/// Refuses (game_running) while a game, or a server that may read the entry, runs.
#[tauri::command]
pub async fn library_take_over(app: AppHandle, entry: LibraryRef) -> Result<TakenOver, Error> {
    let (done, applied) = blocking(move || {
        let d = dirs()?;
        let done = library::take_over(&d, &entry)?;
        let mut applied = Vec::new();
        if done.to != done.from {
            for p in kiln_manage::profile::list(&d)? {
                let p = kiln_manage::profile::get(&d, &p.id)?;
                if p.mods.list(done.to.kind).iter().any(|m| m.id == done.to.id && m.hash == done.to.hash) {
                    applied.extend(super::profile::reapply_where_active(&d, &p.id));
                }
            }
        }
        Ok((done, applied))
    }).await?;
    super::profile::announce_applied(&app, applied);
    Ok(done)
}

/// "Roll back to <version>" (kiln_manage::kept::rollback): in the profile `profile_id` (the game's active one), the mod
/// `kind`/`id` uses its kept version `to` (null: the newest kept one) instead of its current entry. An ordinary profile
/// change: like profile_save, the game's profile.json is written again where the profile is active and the game is not
/// running (that placement puts the kept settings back into the mod's game-folder copy), and its Undo is profile_revert
/// with the result's `changes`. The version rolled away from becomes the kept one.
#[tauri::command]
pub async fn library_rollback(app: AppHandle, game_id: Option<String>, profile_id: String, kind: ModKind, id: String,
                              to: Option<kiln_manage::kept::VersionRef>) -> Result<kiln_manage::kept::RollbackReport, Error> {
    let (r, applied) = blocking(move || {
        let d = dirs()?;
        let g = game_id.as_deref().and_then(|g| crate::state::game(g).ok());
        let r = kiln_manage::kept::rollback(&d, g.as_ref(), &profile_id, kind, &id, to.as_ref())?;
        let applied = super::profile::reapply_where_active(&d, &r.profile_id);
        Ok((r, applied))
    }).await?;
    super::profile::announce_applied(&app, applied);
    Ok(r)
}

/// "Clean up": removes the versions with one look at the running processes and what lists them. Refuses (game_running)
/// the whole batch while a game runs, or a server may read one of them; otherwise reports each version not removed.
#[tauri::command]
pub async fn library_remove_many(entries: Vec<LibraryRef>, force: bool) -> Result<RemoveReport, Error> {
    blocking(move || library::remove_many(&dirs()?, &entries, force)).await
}

/// The versions "Clean up" may remove: listed by no profile and by no known game's active profile file or play-once
/// file a start can still read.
#[tauri::command]
pub async fn library_unused() -> Result<Vec<LibraryRef>, Error> {
    blocking(move || library::unused(&dirs()?)).await
}

/// What lists one version: profile ids, and `<profile> (active in <game>)` / `(play-once in <game>)` for each game
/// whose profile file points at it (what `library_remove` refuses with, Conflict, unless `force`).
#[tauri::command]
pub async fn library_users(entry: LibraryRef) -> Result<Vec<String>, Error> {
    blocking(move || Ok(library::users(&dirs()?, &entry))).await
}

/// `kiln lint` on one library entry (with `profile_id`, the profile's other mods join the cross-mod checks), or on a
/// whole profile (no entry). The report's shape: schema/lint-report.schema.json.
#[tauri::command]
pub async fn library_lint(entry: Option<LibraryRef>, profile_id: Option<String>) -> Result<LintReport, Error> {
    blocking(move || {
        let dirs = dirs()?;
        let options = LintOptions::default();
        match entry {
            Some(e) => lint::lint_library_entry(&dirs, &e, profile_id.as_deref(), &options),
            None => {
                let profile = profile_id.as_deref().ok_or_else(|| Error::invalid("Pick a mod or a profile to check."))?;
                lint::lint_profile(&dirs, profile, &options)
            }
        }
    })
    .await
}

/// Local native DLL replacement: ordinary profile retention, then the update placement path.
#[tauri::command]
pub async fn library_replace_dll(app: AppHandle, profile_id: String, entry: LibraryRef, path: String, confirmed: bool) -> Result<library::ReplacedDll, Error> {
    let (r, applied) = blocking(move || {
        let d = dirs()?;
        let r = library::replace_dll(&d, &profile_id, &entry, std::path::Path::new(&path), confirmed)?;
        Ok((r, super::profile::reapply_where_active(&d, &profile_id)))
    }).await?;
    super::profile::announce_applied(&app, applied);
    Ok(r)
}
