//! Tauri commands: Crucible packs on this PC (kiln_manage::modpack::pack; docs/kiln/launcher/crucible.md, "API for the
//! launcher UI"): a dropped .kilnpack (inspect, install), instances (list, undo, uninstall, update from a file), "Save as
//! file" (build), verify, diff and the secret scan Pack Studio shows live; the player layer (local.<pid>) for the editors. Crucible's network side is commands/crucible.rs.

use super::TaskId;
use super::app::{spawn_task, to_value};
use crate::state::{blocking, dirs, game};
use kiln_manage::modpack::ModpackSource;
use kiln_manage::modpack::layers;
use kiln_manage::modpack::pack::{self, BuildRequest, BuiltPack, Instance, PackDiff, PackIndex, PackPreview, UninstallResult, UpdatePlan};
use kiln_manage::profile::Profile;
use kiln_manage::{Error, ErrorCode};
use std::collections::BTreeMap;
use std::path::PathBuf;
use tauri::AppHandle;

/// A pack on this PC (a dropped file). Downloads go through Crucible (commands/crucible.rs), never a bare URL here.
fn local(source: &ModpackSource) -> Result<(PathBuf, Option<String>), Error> {
    match source {
        ModpackSource::Path { path, sha256 } => Ok((path.clone(), sha256.clone())),
        ModpackSource::Url { .. } => Err(Error::new(ErrorCode::Refused, "Crucible packs are opened by their code or link, not by a download address.")),
    }
}

#[tauri::command]
pub async fn pack_inspect(source: ModpackSource) -> Result<PackPreview, Error> {
    blocking(move || {
        let (p, sha) = local(&source)?;
        pack::inspect(&dirs()?, &p, sha.as_deref())
    })
    .await
}

#[tauri::command]
pub async fn pack_install(app: AppHandle, game_id: Option<String>, request: pack::InstallRequest) -> Result<TaskId, Error> {
    if !request.consent {
        return Err(Error::new(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    local(&request.source)?;
    let (d, g) = blocking(move || Ok((dirs()?, game_id.as_deref().map(game).transpose()?))).await?;
    Ok(spawn_task(&app, "pack_install", "Installing a pack".into(), move |p| to_value(pack::install(&d, g.as_ref(), &request, p)?)))
}

#[tauri::command]
pub async fn pack_instance(profile_id: String) -> Result<Option<Instance>, Error> {
    blocking(move || pack::instance(&dirs()?, &profile_id)).await
}

#[tauri::command]
pub async fn pack_instances() -> Result<Vec<(String, Instance)>, Error> {
    blocking(|| pack::instances(&dirs()?)).await
}

#[tauri::command]
pub async fn pack_update_plan(profile_id: String, source: ModpackSource, optional: Option<BTreeMap<String, bool>>) -> Result<UpdatePlan, Error> {
    blocking(move || {
        let (p, sha) = local(&source)?;
        pack::plan_update(&dirs()?, &profile_id, &p, sha.as_deref(), &optional.unwrap_or_default())
    })
    .await
}

#[tauri::command]
pub async fn pack_update(app: AppHandle, request: pack::UpdateRequest) -> Result<TaskId, Error> {
    if !request.consent {
        return Err(Error::new(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    local(&request.source)?;
    let d = blocking(dirs).await?;
    Ok(spawn_task(&app, "pack_update", "Updating a pack".into(), move |p| to_value(pack::update(&d, &request, p)?)))
}

#[tauri::command]
pub async fn pack_undo(profile_id: String) -> Result<Profile, Error> {
    blocking(move || pack::undo(&dirs()?, &profile_id)).await
}

#[tauri::command]
pub async fn pack_uninstall(profile_id: String) -> Result<UninstallResult, Error> {
    blocking(move || pack::uninstall(&dirs()?, &profile_id)).await
}

/// "Save as file": the pack of a profile into `out_dir` (a task; result `BuiltPack`).
#[tauri::command]
pub async fn pack_build(app: AppHandle, request: BuildRequest, out_dir: PathBuf) -> Result<TaskId, Error> {
    if !out_dir.is_absolute() {
        return Err(Error::invalid("choose a folder for the pack"));
    }
    let d = blocking(dirs).await?;
    Ok(spawn_task(&app, "pack_build", "Saving the pack as a file".into(), move |p| {
        let r: BuiltPack = pack::build(&d, &request, &out_dir, p)?;
        to_value(r)
    }))
}

#[tauri::command]
pub async fn pack_verify(path: PathBuf, sha256: Option<String>) -> Result<PackIndex, Error> {
    blocking(move || pack::verify(&path, sha256.as_deref())).await
}

#[tauri::command]
pub async fn pack_diff(old_path: PathBuf, new_path: PathBuf) -> Result<PackDiff, Error> {
    blocking(move || Ok(pack::diff(&pack::verify(&old_path, None)?, &pack::verify(&new_path, None)?))).await
}

#[tauri::command]
pub async fn pack_secret_scan(file: String, text: String) -> Result<Vec<String>, Error> {
    Ok(pack::secret_findings(&file, &text))
}

/// The pack analysis (kiln_manage::modpack::check, as Crucible runs it on uploads) of a .kilnpack on this PC: the
/// preview of a dropped or opened file shows it (errors block the install). Never runs anything the pack holds.
#[tauri::command]
pub async fn pack_check(path: PathBuf) -> Result<kiln_manage::modpack::check::CheckReport, Error> {
    blocking(move || {
        let d = dirs()?;
        let opts = kiln_manage::modpack::check::CheckOptions { work: Some(d.root.join("modpacks").join("check")), ..Default::default() };
        kiln_manage::modpack::check::check(kiln_manage::modpack::check::Input::Pack(&path), &opts)
    })
    .await
}

/// The player layer of an instance (local.<pid>): its data files.
#[tauri::command]
pub async fn pack_local_files(profile_id: String) -> Result<Vec<layers::LocalFile>, Error> {
    blocking(move || layers::files(&dirs()?, &profile_id)).await
}

/// Writes (or with `text` null removes) one data file of the player layer's working copy (`content/<name>.json`);
/// `pack_local_save` makes it count.
#[tauri::command]
pub async fn pack_local_write(profile_id: String, path: String, text: Option<String>) -> Result<(), Error> {
    blocking(move || layers::write(&dirs()?, &profile_id, &path, text.as_deref())).await
}

/// Puts the player layer into the Library as the profile's last Kiln mod (or out of the profile when it is empty).
#[tauri::command]
pub async fn pack_local_save(profile_id: String) -> Result<Profile, Error> {
    blocking(move || layers::save(&dirs()?, &profile_id)).await
}

/// The Crucible pack's format (1: a server modpack, 2: a Crucible pack) of a dropped zip.
#[tauri::command]
pub async fn pack_peek_format(path: PathBuf) -> Result<u32, Error> {
    blocking(move || pack::peek_format(&path)).await
}
