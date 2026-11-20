//! Tauri commands: Pack Studio, the creator's tool for Crucible packs (kiln_manage::modpack::studio and
//! kiln_manage::crucible::publish; docs/kiln/launcher/pack-studio.md). A pack source lives in <data>/studio/<id>/;
//! its mods, order and settings are its profile's (edited with the launcher's usual mod commands). Long steps are tasks.

use super::TaskId;
use super::app::{launcher_log, spawn_task, to_value};
use crate::state::{blocking, dirs, game};
use kiln_manage::crucible::{Crucible, publish};
use kiln_manage::lint::LintReport;
use kiln_manage::modpack::pack::{self, Credit};
use kiln_manage::modpack::studio::{self, CaptureChoice, CaptureFile, LayerFile, NewPack, PackSource, PickHit, Studio, StudioOrder, StudioSummary, TestReport};
use kiln_manage::{Error, ErrorCode};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::PathBuf;
use tauri::AppHandle;

fn dir(pack_id: &str) -> Result<PathBuf, Error> {
    studio::dir_of(&dirs()?, pack_id)
}

#[tauri::command]
pub async fn studio_list() -> Result<Vec<StudioSummary>, Error> {
    blocking(|| studio::list(&dirs()?)).await
}

#[tauri::command]
pub async fn studio_create(request: NewPack) -> Result<Studio, Error> {
    blocking(move || studio::create(&dirs()?, &request)).await
}

#[tauri::command]
pub async fn studio_get(pack_id: String) -> Result<Studio, Error> {
    blocking(move || studio::open(&dirs()?, &dir(&pack_id)?)).await
}

#[tauri::command]
pub async fn studio_save(pack_id: String, source: PackSource) -> Result<Studio, Error> {
    blocking(move || studio::save(&dirs()?, &dir(&pack_id)?, source)).await
}

#[tauri::command]
pub async fn studio_delete(pack_id: String) -> Result<(), Error> {
    blocking(move || studio::delete(&dirs()?, &dir(&pack_id)?)).await
}

#[tauri::command]
pub async fn studio_credits_set(pack_id: String, credits: BTreeMap<String, Credit>) -> Result<Studio, Error> {
    blocking(move || studio::set_credits(&dirs()?, &dir(&pack_id)?, &credits)).await
}

/// "Capture from my current settings": with a game whose active profile is the pack's, the game's config folder is
/// copied back into the profile first (what the creator changed while playing).
#[tauri::command]
pub async fn studio_capture(pack_id: String, game_id: Option<String>) -> Result<Vec<CaptureFile>, Error> {
    blocking(move || {
        let d = dirs()?;
        let at = dir(&pack_id)?;
        if let Some(g) = game_id.as_deref().map(game).transpose().ok().flatten() {
            let s = studio::read_source(&at)?;
            if kiln_manage::profile::active(&d, &g).ok().flatten().is_some_and(|a| a.profile_id == s.profile) {
                let _ = kiln_manage::profile::copy_back(&d, &s.profile, &g);
            }
        }
        studio::capture(&d, &at)
    })
    .await
}

#[tauri::command]
pub async fn studio_capture_set(pack_id: String, choices: Vec<CaptureChoice>) -> Result<Studio, Error> {
    blocking(move || studio::set_capture(&dirs()?, &dir(&pack_id)?, &choices)).await
}

#[tauri::command]
pub async fn studio_order(pack_id: String) -> Result<StudioOrder, Error> {
    blocking(move || studio::order(&dirs()?, &dir(&pack_id)?)).await
}

#[tauri::command]
pub async fn studio_layer_files(pack_id: String) -> Result<Vec<LayerFile>, Error> {
    blocking(move || studio::layer_files(&dir(&pack_id)?)).await
}

#[tauri::command]
pub async fn studio_layer_read(pack_id: String, path: String) -> Result<String, Error> {
    blocking(move || studio::layer_read(&dir(&pack_id)?, &path)).await
}

/// Writes a layer file and answers the layer's lint (the live lint).
#[tauri::command]
pub async fn studio_layer_write(pack_id: String, path: String, text: String) -> Result<LintReport, Error> {
    blocking(move || {
        let d = dirs()?;
        let at = dir(&pack_id)?;
        studio::layer_write(&at, &path, &text)?;
        studio::layer_lint(&d, &at, None)
    })
    .await
}

#[tauri::command]
pub async fn studio_layer_delete(pack_id: String, path: String) -> Result<LintReport, Error> {
    blocking(move || {
        let d = dirs()?;
        let at = dir(&pack_id)?;
        studio::layer_delete(&at, &path)?;
        studio::layer_lint(&d, &at, None)
    })
    .await
}

/// "Set in the pack" (Load order, conflicts): one field in content/pack.json; null removes it.
#[tauri::command]
pub async fn studio_layer_set(pack_id: String, section: String, selector: String, field: String, value: Value) -> Result<LintReport, Error> {
    blocking(move || {
        let d = dirs()?;
        let at = dir(&pack_id)?;
        studio::layer_set(&at, &section, &selector, &field, &value)?;
        studio::layer_lint(&d, &at, None)
    })
    .await
}

#[tauri::command]
pub async fn studio_layer_lint(pack_id: String) -> Result<LintReport, Error> {
    blocking(move || studio::layer_lint(&dirs()?, &dir(&pack_id)?, None)).await
}

#[tauri::command]
pub async fn studio_pick(pack_id: String, section: String, query: String, limit: Option<usize>) -> Result<Vec<PickHit>, Error> {
    blocking(move || studio::pick(&dirs()?, &dir(&pack_id)?, &section, &query, limit.unwrap_or(50))).await
}

#[tauri::command]
pub async fn studio_fields(section: String) -> Result<Vec<kiln_manage::catalog::FieldInfo>, Error> {
    blocking(move || Ok(studio::fields(&section))).await
}

/// The pack analysis (a task; result check::CheckReport): checks, code scan, data conflicts, prefilled credits.
#[tauri::command]
pub async fn studio_analyze(app: AppHandle, pack_id: String) -> Result<TaskId, Error> {
    let (d, at) = blocking(move || Ok((dirs()?, dir(&pack_id)?))).await?;
    Ok(spawn_task(&app, "studio_analyze", "Checking the pack".into(), move |p| to_value(studio::analyze(&d, &at, p)?)))
}

/// "Test this pack" (a task; result pack::InstallResult): the pack built and installed as "<name> (test)", activated
/// for the game (then Play starts it); the last test instance is replaced.
#[tauri::command]
pub async fn studio_test(app: AppHandle, pack_id: String, game_id: Option<String>) -> Result<TaskId, Error> {
    let (d, at, g) = blocking(move || Ok((dirs()?, dir(&pack_id)?, game_id.as_deref().map(game).transpose()?))).await?;
    Ok(spawn_task(&app, "studio_test", "Making the test instance".into(), move |p| {
        let r = studio::test(&d, &at, g.as_ref(), p)?;
        launcher_log("studio", &format!("test instance {} made", r.profile.id));
        to_value(r)
    }))
}

#[tauri::command]
pub async fn studio_test_report(pack_id: String, game_id: Option<String>) -> Result<TestReport, Error> {
    blocking(move || {
        let g = game_id.as_deref().map(game).transpose()?;
        studio::test_report(&dir(&pack_id)?, g.as_ref().map(|g| g.path.as_path()))
    })
    .await
}

#[tauri::command]
pub async fn studio_test_end(pack_id: String) -> Result<(), Error> {
    blocking(move || studio::test_end(&dirs()?, &dir(&pack_id)?)).await
}

/// "Save as file" (a task; result pack::BuiltPack).
#[tauri::command]
pub async fn studio_build(app: AppHandle, pack_id: String, out_dir: PathBuf) -> Result<TaskId, Error> {
    if !out_dir.is_absolute() {
        return Err(Error::invalid("choose a folder for the pack"));
    }
    let (d, at) = blocking(move || Ok((dirs()?, dir(&pack_id)?))).await?;
    Ok(spawn_task(&app, "studio_build", "Saving the pack as a file".into(), move |p| to_value(studio::build(&d, &at, Some(&out_dir), p)?)))
}

/// What publishing would still need (empty: ready), without sending anything.
#[tauri::command]
pub async fn studio_publish_check(pack_id: String, request: publish::PublishRequest) -> Result<Vec<String>, Error> {
    blocking(move || Ok(publish::precheck(&studio::open(&dirs()?, &dir(&pack_id)?)?, &request))).await
}

/// Publish the next version to Crucible (a task; result publish::PublishResult). Needs the Discord sign-in.
#[tauri::command]
pub async fn studio_publish(app: AppHandle, pack_id: String, request: publish::PublishRequest, game_id: Option<String>) -> Result<TaskId, Error> {
    let (d, at, g, key) = blocking(move || {
        super::crucible::service().and_then(|(ep, _)| if ep.enabled() { Ok(()) } else { Err(Error::new(ErrorCode::Unsupported, "Crucible is not available in this version of Kiln yet.")) })?;
        let key = super::community::session_key().ok_or_else(|| Error::new(ErrorCode::AuthFailed, "Sign in with Discord to publish."))?;
        let g = game_id.as_deref().map(game).transpose().ok().flatten();
        Ok((dirs()?, dir(&pack_id)?, g, key))
    })
    .await?;
    Ok(spawn_task(&app, "studio_publish", "Publishing to Crucible".into(), move |p| {
        let (ep, t) = super::crucible::service()?;
        let c = Crucible::new(&ep, t.as_ref(), &super::crucible::launcher_version()).with_session(Some(key));
        let r = publish::publish(&d, &c, &at, &request, g.as_ref(), p)?;
        launcher_log("studio", &format!("published {} version {} ({})", r.pack.id, r.version.version, if r.public { "public" } else { "link" }));
        to_value(r)
    }))
}

/// A server pack from a published version (DESIGN 3.6): the version's server part put on one of the launcher's servers
/// and the player modpack (format 1) built from it, as kilnctl pack server --modpack-out does (a task; result
/// pack::ServerInstallResult). Refused while the server runs (its settings file).
#[tauri::command]
pub async fn studio_server_pack(app: AppHandle, pack_id: String, version: String, server_id: String) -> Result<TaskId, Error> {
    let (d, file) = blocking(move || Ok((dirs()?, studio::version_file(&dir(&pack_id)?, &version)?))).await?;
    Ok(spawn_task(&app, "studio_server_pack", "Building the server pack".into(), move |p| {
        let inst = kiln_manage::server::instance::get(&server_id)?;
        let out = d.root.join("modpacks").join("built").join(&inst.id);
        let req = pack::ServerInstallRequest {
            source: kiln_manage::modpack::ModpackSource::Path { path: file.clone(), sha256: None },
            consent: true,
            allow_http: false,
            optional: BTreeMap::new(),
            settings: true,
            modpack_out: Some(out),
        };
        to_value(pack::install_server(&d, &inst, &req, p)?)
    }))
}
