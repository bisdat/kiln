//! Tauri commands: Pack Studio, the creator's tool for Crucible packs (kiln_manage::modpack::studio and
//! kiln_manage::crucible::publish; docs/kiln/launcher/pack-studio.md). A pack source lives in <data>/studio/<id>/;
//! its mods, order and settings are its profile's (edited with the launcher's usual mod commands). Long steps are tasks.

use super::TaskId;
use super::app::{launcher_log, spawn_task, to_value};
use crate::state::{blocking, dirs, game};
use kiln_manage::catalog::auto::{AutoCatalog, AutoState, AutoStatus};
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
pub async fn studio_pick(pack_id: String, section: String, query: String, limit: Option<usize>, hidden: Option<bool>) -> Result<Vec<PickHit>, Error> {
    blocking(move || studio::pick(&dirs()?, &dir(&pack_id)?, &section, &query, limit.unwrap_or(50), hidden.unwrap_or(false))).await
}

/// The game's recipe input categories with their items and the recipes that take them (empty without a catalog).
#[tauri::command]
pub async fn studio_categories() -> Result<Vec<kiln_manage::catalog::ItemCategory>, Error> {
    blocking(move || Ok(studio::categories())).await
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

// ================================================================================================= the game-data catalog

/// The client catalog's upkeep for the pickers, fields and lint (kiln_manage::catalog::auto): one per process.
static CATALOG: AutoCatalog = AutoCatalog::new();

/// Where the catalog's upkeep is (the Data tab polls it while it builds).
#[tauri::command]
pub async fn studio_catalog_status() -> Result<AutoStatus, Error> {
    Ok(CATALOG.status())
}

/// Pack Studio opening, and its "Build the game catalog" button: checks the client catalog on a thread of its own and
/// makes it when it is missing or not current (about a second). Answers at once: building, or the status of the check
/// that runs already. Refusals (Steam updating, exe and data of two builds, the game folder) come back as `failed`.
#[tauri::command]
pub async fn studio_catalog_ensure() -> Result<AutoStatus, Error> {
    Ok(start_catalog(&CATALOG, catalog_inputs, launcher_log))
}

/// The client to read (the one Prepare game art reads) and the folder the catalog's readers look in.
fn catalog_inputs() -> CatalogFound {
    dirs()?; // the data folder is not held (a move, the beta copy's gate)
    Ok((super::app::art_game(), kiln_manage::catalog::catalog_dir()?))
}

type CatalogFound = Result<(Option<kiln_manage::game::GameInstall>, PathBuf), Error>;

/// Starts one check in the background unless one runs; `log` gets its outcome (launcher.log).
fn start_catalog(auto: &'static AutoCatalog, inputs: impl FnOnce() -> CatalogFound + Send + 'static, log: fn(&str, &str)) -> AutoStatus {
    if !auto.begin() {
        return auto.status();
    }
    let started = auto.status();
    std::thread::spawn(move || {
        let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| catalog_round(auto, inputs)));
        let s = r.unwrap_or_else(|_| auto.fail(Error::new(ErrorCode::Internal, "reading the game's items panicked (a bug; see launcher.log)"), None));
        if let Some(line) = catalog_log_line(&s) {
            log("catalog", &line);
        }
    });
    started
}

/// One begun check, on this thread: the launcher's own Steam check (any library layout), then kiln-manage's.
fn catalog_round(auto: &AutoCatalog, inputs: impl FnOnce() -> CatalogFound) -> AutoStatus {
    // a data folder move waits for this round (it writes <data>/catalog); taken before dirs(), as state::writing says
    let _writing = crate::state::writing();
    let (game, dest) = match inputs() {
        Ok(v) => v,
        Err(e) => return auto.fail(e, None),
    };
    if let Some(g) = &game
        && let Err(e) = super::app::refuse_steam_updating(g)
    {
        return auto.fail(e, Some(&dest));
    }
    auto.run(game.as_ref().map(|g| g.path.as_path()), &dest, &mut kiln_manage::task::NoProgress)
}

/// What launcher.log says about a check: a catalog made, or why not (the same refusal only once in a row). A current
/// catalog says nothing (every Pack Studio opening checks).
fn catalog_log_line(s: &AutoStatus) -> Option<String> {
    static LAST: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());
    let mut last = LAST.lock().unwrap_or_else(|e| e.into_inner());
    let path = s.path.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "-".into());
    let line = match (s.state, &s.error) {
        (AutoState::Ready, _) if s.built == Some(true) => format!("game-data catalog made for Pack Studio: {path} ({:.1} s)", s.seconds.unwrap_or(0.0)),
        (AutoState::Ready, _) => {
            last.clear();
            return None;
        }
        (_, Some(e)) => format!("game-data catalog not made: [{}] {}{}", e.code.as_str(), e.message, if s.exists { " (the older one stays in use)" } else { "" }),
        _ => return None,
    };
    if s.state != AutoState::Ready && *last == line {
        return None;
    }
    *last = line.clone();
    Some(line)
}

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn client(path: PathBuf, library: Option<PathBuf>) -> GameInstall {
        GameInstall {
            id: "g".into(), kind: GameKind::Client, path, exe: "enshrouded.exe".into(), app_id: kiln_manage::CLIENT_APP_ID,
            library, platform: Platform::Proton, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy,
            source: "steam".into(),
        }
    }
    fn tmp(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("kiln-studio-catalog-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        p
    }
    fn wait_done(auto: &AutoCatalog) -> AutoStatus {
        for _ in 0..500 {
            let s = auto.status();
            if s.state != AutoState::Building {
                return s;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("the catalog check never ended");
    }
    fn no_log(_: &str, _: &str) {}

    #[test]
    fn catalog_check_runs_in_the_background_once_at_a_time() {
        static AUTO: AutoCatalog = AutoCatalog::new();
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        let slow_no_game = || -> CatalogFound {
            CALLS.fetch_add(1, Ordering::SeqCst);
            std::thread::sleep(std::time::Duration::from_millis(200));
            Ok((None, tmp("none")))
        };
        // answers at once with "building"; a second opening while it runs starts nothing
        let s = start_catalog(&AUTO, slow_no_game, no_log);
        assert_eq!(s.state, AutoState::Building);
        assert_eq!(start_catalog(&AUTO, slow_no_game, no_log).state, AutoState::Building);
        let s = wait_done(&AUTO);
        assert_eq!(s.state, AutoState::NoGame);
        assert_eq!(s.error.as_ref().map(|e| e.code), Some(ErrorCode::NotFound));
        assert_eq!(CALLS.load(Ordering::SeqCst), 1);
        // the button: a new check
        start_catalog(&AUTO, slow_no_game, no_log);
        wait_done(&AUTO);
        assert_eq!(CALLS.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn catalog_refusals_are_surfaced_and_nothing_is_written() {
        // Steam updating, seen in the game's own library (the launcher's check)
        let lib = tmp("lib");
        let game = lib.join("steamapps/common/Enshrouded");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join("enshrouded.kfc"), b"not read").unwrap();
        std::fs::write(lib.join(format!("steamapps/appmanifest_{}.acf", kiln_manage::CLIENT_APP_ID)),
            format!("\"AppState\"\n{{\n\t\"appid\"\t\t\"{}\"\n\t\"StateFlags\"\t\t\"6\"\n\t\"installdir\"\t\t\"Enshrouded\"\n}}\n", kiln_manage::CLIENT_APP_ID)).unwrap();
        let dest = tmp("dest");
        let auto = AutoCatalog::new();
        assert!(auto.begin());
        let s = catalog_round(&auto, || Ok((Some(client(game.clone(), Some(lib.clone()))), dest.clone())));
        assert_eq!(s.state, AutoState::Failed);
        assert_eq!(s.error.as_ref().map(|e| e.code), Some(ErrorCode::SteamUpdating));
        assert!(!s.exists && !dest.join("client.json").exists());
        let line = catalog_log_line(&s).expect("a refusal is logged");
        assert!(line.contains("[steam_updating]"), "{line}");
        assert_eq!(catalog_log_line(&s), None, "the same refusal is logged once in a row");
        // the data folder held (or anything before the run): failed with that error
        assert!(auto.begin());
        let s = catalog_round(&auto, || Err(Error::new(ErrorCode::Refused, "The data folder is being moved.")));
        assert_eq!((s.state, s.error.as_ref().map(|e| e.code)), (AutoState::Failed, Some(ErrorCode::Refused)));
        let _ = std::fs::remove_dir_all(&lib);
        let _ = std::fs::remove_dir_all(&dest);
    }

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
