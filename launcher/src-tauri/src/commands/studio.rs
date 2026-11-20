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

/// The pack's draft goes to backups; a test instance the game has loaded goes back to the pack's own profile first.
#[tauri::command]
pub async fn studio_delete(pack_id: String, game_id: Option<String>) -> Result<(), Error> {
    blocking(move || {
        let g = game_id.as_deref().map(game).transpose()?;
        studio::delete(&dirs()?, &dir(&pack_id)?, g.as_ref())
    })
    .await
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
pub async fn studio_test_end(pack_id: String, game_id: Option<String>) -> Result<(), Error> {
    blocking(move || {
        let g = game_id.as_deref().map(game).transpose()?;
        studio::test_end(&dirs()?, &dir(&pack_id)?, g.as_ref())
    })
    .await
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

/// "Make public" / "Link only" for a published pack (no new version); Crucible's refusal comes back as needs.
#[tauri::command]
pub async fn studio_set_visibility(pack_id: String, visibility: studio::Visibility, attest: bool) -> Result<publish::VisibilityResult, Error> {
    blocking(move || {
        let key = super::community::session_key().ok_or_else(|| Error::new(ErrorCode::AuthFailed, "Sign in with Discord to publish."))?;
        let (ep, t) = super::crucible::service()?;
        if !ep.enabled() {
            return Err(Error::new(ErrorCode::Unsupported, "Crucible is not available in this version of Kiln yet."));
        }
        let c = Crucible::new(&ep, t.as_ref(), &super::crucible::launcher_version()).with_session(Some(key));
        publish::set_visibility(&dirs()?, &c, &dir(&pack_id)?, visibility, attest)
    })
    .await
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

#[cfg(test)]
mod tests {
    use super::*;

    /// Pack Studio's publish end to end against a Crucible running on this machine (launcher/scripts/crucible-e2e.mjs
    /// starts it and runs this with KILN_CRUCIBLE_DEV and KILN_CRUCIBLE_E2E_SESSION; plain cargo test skips it): a pack
    /// source from a profile with a layer file, a captured settings key and credits; published link-only, then the next
    /// version public; a friend opens it by its code without a sign-in and installs it, layer and override included.
    #[test]
    #[ignore = "needs a local Crucible: launcher/scripts/crucible-e2e.mjs"]
    fn studio_publish_end_to_end_against_a_local_crucible() {
        use kiln_manage::crucible::{self, publish::PublishRequest};
        use kiln_manage::modpack::studio::{CaptureChoice, Visibility};
        use kiln_manage::paths::DataDirs;
        use kiln_manage::task::NoProgress;
        let session = std::env::var("KILN_CRUCIBLE_E2E_SESSION").expect("KILN_CRUCIBLE_E2E_SESSION");
        let (ep, t) = super::super::crucible::service().expect("KILN_CRUCIBLE_DEV");
        assert!(ep.dev && ep.enabled(), "KILN_CRUCIBLE_DEV must name the local service and its key");
        let root = std::env::temp_dir().join(format!("kiln-studio-e2e-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let step = |s: &str| eprintln!("ok   {s}");
        let a = DataDirs::at(root.join("creator"));
        let src = root.join("src");
        let mk = |id: &str, v: &str| {
            let dir = src.join(format!("{id}-{v}"));
            std::fs::create_dir_all(dir.join("content")).unwrap();
            std::fs::write(dir.join("kiln.mod.json"), format!(r#"{{ "id": "{id}", "name": "{id}", "version": "{v}", "apiVersion": "0.2", "side": "both", "authors": ["frostdev"] }}"#)).unwrap();
            std::fs::write(dir.join("content/data.json"), format!(r#"{{ "format": 1, "note": "{id} {v}" }}"#)).unwrap();
            dir
        };
        let p = kiln_manage::profile::create(&a, "Studio run", None).unwrap();
        let r = kiln_manage::library::import(&a, &[mk("studio-alpha", "1.0.0"), mk("studio-beta", "0.2.0")], &mut NoProgress).unwrap();
        let mut prof = kiln_manage::profile::get(&a, &p.id).unwrap();
        for lr in r.imported.iter().chain(&r.existing) {
            prof.mods.kiln.push(kiln_manage::profile::ProfileMod { id: lr.id.clone(), version: lr.version.clone(), hash: lr.hash.clone(), enabled: true });
        }
        kiln_manage::profile::save(&a, &prof).unwrap();
        let cfg = a.profile(&p.id).join("config");
        std::fs::create_dir_all(&cfg).unwrap();
        std::fs::write(cfg.join("studio-alpha.json"), r#"{ "range": 20, "hotkey": "F9" }"#).unwrap();
        let st = studio::create(&a, &NewPack { name: "Studio run".into(), from: Some(p.id.clone()), id: None }).unwrap();
        studio::layer_write(&st.dir, "content/items.json", r#"{ "format": 1, "items": { "Material_T1_Wood_Logs": { "maxStackSize": 500 } } }"#).unwrap();
        studio::set_capture(&a, &st.dir, &[CaptureChoice { path: "studio-alpha.json".into(), side: Default::default(), keys: vec!["range".into()], whole: false }]).unwrap();
        step("pack source: 2 mods, a layer file, one captured key");
        let c = Crucible::new(&ep, t.as_ref(), "0.2.0-dev").with_session(Some(session));
        let r1 = publish::publish(&a, &c, &st.dir, &PublishRequest { version: "1.0.0".into(), changes: Some("first".into()), ..Default::default() }, None, &mut NoProgress).expect("publish 1");
        assert!(!r1.public);
        assert_eq!(r1.version.status, "ready", "{r1:?}");
        step(&format!("published link-only: {} code {} ({} files, {} bytes up)", r1.pack.url, r1.pack.code, r1.files, r1.uploaded));
        // credits, a summary, the attestation: version 1.0.1 public
        let mut credits = BTreeMap::new();
        for id in ["studio-alpha", "studio-beta"] {
            credits.insert(id.to_string(), pack::Credit { author: "frostdev".into(), source: format!("https://github.com/frostdev-ops/{id}"), match_kind: Some("manifest".into()) });
        }
        let st = studio::set_credits(&a, &st.dir, &credits).unwrap();
        let mut s = st.source.clone();
        s.summary = Some("Two mods and a layer, end to end".into());
        studio::save(&a, &st.dir, s).unwrap();
        let r2 = publish::publish(&a, &c, &st.dir, &PublishRequest { version: "1.0.1".into(), visibility: Visibility::Public, attest: true, ..Default::default() }, None, &mut NoProgress).expect("publish 2");
        assert_eq!((r2.pack.id.as_str(), r2.version.number), (r1.pack.id.as_str(), 2));
        assert!(r2.public, "public: {:?}", r2.needs);
        assert!(r2.skipped > 0, "the mods were on Crucible already: {r2:?}");
        step(&format!("version 1.0.1 public ({} bytes up, {} kept)", r2.uploaded, r2.skipped));
        // a friend, no sign-in
        let b = DataDirs::at(root.join("friend"));
        let cb = Crucible::new(&ep, t.as_ref(), "0.2.0-dev");
        let (pv, _) = crucible::preview(&b, &cb, &r2.pack.code, None).expect("preview");
        assert_eq!(pv.version.version, "1.0.1");
        assert!(pv.index.mods.iter().all(|m| m.credit.as_ref().is_some_and(|c| c.author == "frostdev")), "credits travel in the index");
        let ir = crucible::install(&b, &cb, None, &crucible::InstallRequest { target: pv.version.id.clone(), consent: true, ..Default::default() }, pv.pack.owner.as_ref(), false, &mut NoProgress).expect("install");
        assert!(ir.profile.mods.kiln.iter().any(|m| m.id == "pack.studio-run"), "the layer is installed");
        let ov = std::fs::read_to_string(b.profile(&ir.profile.id).join("config/studio-alpha.json")).unwrap();
        assert!(ov.contains("range") && !ov.contains("hotkey"), "{ov}");
        step(&format!("installed by code as {} with the layer and the captured key", ir.profile.id));
        let _ = std::fs::remove_dir_all(&root);
    }
}
