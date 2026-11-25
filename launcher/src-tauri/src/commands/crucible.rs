//! Tauri commands: Crucible, the modpack platform (kiln_manage::crucible; docs/kiln/launcher/crucible.md, "The launcher's
//! Crucible client"). Receiving and new unlisted shares need no sign-in; account-owned shares use the Community relay
//! session as the Bearer key, sent to Crucible's API only). kiln:// links (`kiln://crucible/<CODE>`) arrive with the
//! launcher's own start (held until the window is up) or, from a later start while this launcher runs, through
//! `<data>/crucible/inbox/`; the event `kiln://crucible-link` says they wait.
//!
//! Crucible is on only when a signing key is known: compiled in (`kiln_manage::crucible::KEYS`, empty until the owner
//! approves the service's key) or, in debug builds and builds with the `crucible-dev` feature, a local service named by
//! `KILN_CRUCIBLE_DEV` (`api=http://127.0.0.1:<port>/kiln/crucible/api/v1 cdn=http://127.0.0.1:<port>/<bucket> key=<base64>`).

use super::TaskId;
use super::app::{launcher_log, spawn_task, to_value};
use crate::state::{blocking, dirs, game};
use kiln_manage::crucible::{self, Crucible, CruciblePack, Endpoints, links};
use kiln_manage::net::Transport;
use kiln_manage::{Error, ErrorCode};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::sync::{LazyLock, Mutex, OnceLock};
use tauri::{AppHandle, Emitter, Manager};

/// `{count}` kiln:// links wait (`crucible_links`).
pub const LINK_EVENT: &str = "kiln://crucible-link";

static VERSION: OnceLock<String> = OnceLock::new();
static INSTANCE: Mutex<Option<links::Instance>> = Mutex::new(None);
static LINKS: LazyLock<Mutex<Vec<String>>> = LazyLock::new(|| Mutex::new(Vec::new()));
/// Packs as the last preview showed them (version id -> pack): the creator for "Always trust".
static SEEN: LazyLock<Mutex<BTreeMap<String, CruciblePack>>> = LazyLock::new(|| Mutex::new(BTreeMap::new()));
/// The last update check (game id -> when, what).
static UPDATES: LazyLock<Mutex<BTreeMap<String, (std::time::Instant, Vec<crucible::UpdateAvailable>)>>> = LazyLock::new(|| Mutex::new(BTreeMap::new()));

/// The launcher's version as Crucible's User-Agent names it (Pack Studio's publish too).
pub(crate) fn launcher_version() -> String {
    version()
}

fn version() -> String {
    VERSION.get().cloned().unwrap_or_else(|| env!("CARGO_PKG_VERSION").to_string())
}

/// Where Crucible is, and the transport that reaches it.
pub(crate) fn service() -> Result<(Endpoints, Box<dyn Transport>), Error> {
    #[cfg(any(debug_assertions, feature = "crucible-dev"))]
    if let Ok(spec) = std::env::var("KILN_CRUCIBLE_DEV")
        && !spec.trim().is_empty()
    {
        let (ep, t) = crucible::dev::from_spec(&spec)?;
        return Ok((ep, Box::new(t)));
    }
    Ok((Endpoints::production(), Box::new(kiln_manage::net::UreqTransport::new())))
}

fn enabled() -> Result<(Endpoints, Box<dyn Transport>), Error> {
    let (ep, t) = service()?;
    if !ep.enabled() {
        return Err(Error::new(ErrorCode::Unsupported, "Crucible is not available in this version of Kiln yet."));
    }
    Ok((ep, t))
}

/// What the UI needs to know before it shows any Crucible surface.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CrucibleStatus {
    pub enabled: bool,
    pub dev: bool,
    pub api: String,
    pub signed_in: bool,
    pub account: Option<StatusAccount>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusAccount {
    pub id: String,
    pub name: String,
}

#[tauri::command]
pub async fn crucible_status() -> Result<CrucibleStatus, Error> {
    blocking(|| {
        let (ep, _) = service()?;
        let account = super::community::session_account();
        Ok(CrucibleStatus {
            enabled: ep.enabled(),
            dev: ep.dev,
            api: ep.api.clone(),
            signed_in: super::community::session_key().is_some(),
            account: account.map(|a| StatusAccount { id: a.id, name: a.name }),
        })
    })
    .await
}

/// A code or link -> the pack preview. A pack still uploading rejects with `busy` and `{uploading, pack}` in detail.
#[tauri::command]
pub async fn crucible_open(game_id: Option<String>, target: String) -> Result<crucible::Preview, Error> {
    blocking(move || {
        let (ep, t) = enabled()?;
        let d = dirs()?;
        let g = game_id.as_deref().map(game).transpose().ok().flatten();
        let c = Crucible::new(&ep, t.as_ref(), &version());
        let (p, _) = crucible::preview(&d, &c, &target, g.as_ref())?;
        SEEN.lock().unwrap().insert(p.version.id.clone(), p.pack.clone());
        Ok(p)
    })
    .await
}

/// Install and play (a task; result `crucible::InstallResult`). The UI starts the game once it ends.
#[tauri::command]
pub async fn crucible_install(app: AppHandle, game_id: Option<String>, request: crucible::InstallRequest) -> Result<TaskId, Error> {
    if !request.consent {
        return Err(Error::new(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    let (g, d, count) = blocking(move || {
        let g = game_id.as_deref().map(game).transpose()?;
        let count = crate::state::read_settings().map(|s| s.crucible_stats).unwrap_or(false);
        Ok((g, dirs()?, count))
    })
    .await?;
    let owner = SEEN.lock().unwrap().get(&request.target).and_then(|p| p.owner.clone());
    let title = format!("Installing a Crucible pack ({})", request.target);
    Ok(spawn_task(&app, "crucible_install", title, move |p| {
        let (ep, t) = enabled()?;
        let c = Crucible::new(&ep, t.as_ref(), &version());
        let r = crucible::install(&d, &c, g.as_ref(), &request, owner.as_ref(), count, p)?;
        launcher_log("crucible", &format!("installed {} {} as profile {}", r.instance.name, r.instance.version, r.profile.id));
        UPDATES.lock().unwrap().clear();
        to_value(r)
    }))
}

/// Instances with a newer version on Crucible (cached 5 minutes; `force` asks again).
#[tauri::command]
pub async fn crucible_updates(game_id: Option<String>, force: Option<bool>) -> Result<Vec<crucible::UpdateAvailable>, Error> {
    blocking(move || {
        let key = game_id.unwrap_or_default();
        if force != Some(true)
            && let Some((at, v)) = UPDATES.lock().unwrap().get(&key)
            && at.elapsed() < std::time::Duration::from_secs(300)
        {
            return Ok(v.clone());
        }
        let (ep, t) = service()?;
        if !ep.enabled() {
            return Ok(Vec::new());
        }
        let c = Crucible::new(&ep, t.as_ref(), &version());
        let v = crucible::updates(&dirs()?, &c)?;
        UPDATES.lock().unwrap().insert(key, (std::time::Instant::now(), v.clone()));
        Ok(v)
    })
    .await
}

/// The newest version of an instance's pack, downloaded and planned (nothing written yet).
#[tauri::command]
pub async fn crucible_update_plan(profile_id: String) -> Result<crucible::CrucibleUpdatePlan, Error> {
    blocking(move || {
        let (ep, t) = enabled()?;
        let c = Crucible::new(&ep, t.as_ref(), &version());
        crucible::update_plan(&dirs()?, &c, &profile_id, &mut kiln_manage::task::NoProgress)
    })
    .await
}

/// Update and play (a task; result `pack::UpdateResult`; error `conflict` with the plan in detail).
#[tauri::command]
pub async fn crucible_update(app: AppHandle, game_id: Option<String>, request: crucible::UpdateRequest) -> Result<TaskId, Error> {
    if !request.consent {
        return Err(Error::new(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    let _ = game_id;
    let d = blocking(dirs).await?;
    let title = format!("Updating a Crucible pack ({})", request.version_id);
    Ok(spawn_task(&app, "crucible_update", title, move |p| {
        let (ep, t) = enabled()?;
        let c = Crucible::new(&ep, t.as_ref(), &version());
        let r = crucible::update(&d, &c, &request, p)?;
        UPDATES.lock().unwrap().clear();
        to_value(r)
    }))
}

/// Share to Crucible (a task; result `crucible::ShareResult`). New signed-out shares use native owner capabilities;
/// existing account-owned shares still use the Community session. Owner keys never cross IPC.
#[tauri::command]
pub async fn crucible_share(app: AppHandle, game_id: Option<String>, request: crucible::ShareRequest) -> Result<TaskId, Error> {
    let (d, g, key) = blocking(move || {
        enabled()?;
        let key = super::community::session_key();
        let g = game_id.as_deref().map(game).transpose().ok().flatten();
        Ok((dirs()?, g, key))
    })
    .await?;
    let title = "Sharing to Crucible".to_string();
    Ok(spawn_task(&app, "crucible_share", title, move |p| {
        let (ep, t) = enabled()?;
        let c = Crucible::new(&ep, t.as_ref(), &version()).with_session(key);
        let r = crucible::share(&d, &c, &request, g.as_ref(), p)?;
        launcher_log("crucible", &format!("shared profile {} as {} version {}", request.profile, r.pack.id, r.version.number));
        to_value(r)
    }))
}

/// What a profile was shared as, and whether it changed since (the "Pack changed" pill).
#[tauri::command]
pub async fn crucible_shared(profile_id: String) -> Result<Option<crucible::SharedState>, Error> {
    blocking(move || crucible::shared(&dirs()?, &profile_id)).await
}

#[tauri::command]
pub async fn crucible_trust() -> Result<Vec<crucible::Trust>, Error> {
    blocking(|| Ok(crucible::trusted(&dirs()?))).await
}

#[tauri::command]
pub async fn crucible_trust_set(id: String, name: String, trusted: bool) -> Result<(), Error> {
    blocking(move || crucible::set_trust(&dirs()?, &id, &name, trusted)).await
}

/// kiln:// links that arrived since the last call (drained).
#[tauri::command]
pub async fn crucible_links() -> Result<Vec<String>, Error> {
    Ok(std::mem::take(&mut *LINKS.lock().unwrap()))
}

/// "Come back after installing Kiln" (DESIGN 1.3): the first start with this data folder, and a pack code the download
/// carried (the Windows installer's note beside the launcher, or the AppImage's name). Asked once by the UI at start.
#[tauri::command]
pub async fn crucible_resume() -> Result<links::Resume, Error> {
    blocking(|| Ok(links::resume(&dirs()?, launcher_exe().ok().as_deref()))).await
}

/// "Got a code?" and its Paste button, and the start's pack link when `crucible_resume` says a pack waits: the
/// clipboard's text, read here (the OS clipboard through arboard) and not by the webview, so WebView2 never asks the
/// player for clipboard access. Only text that may be a Crucible code or link crosses into the UI (the field's own
/// check, `crucibleTarget`, decides); anything else is None. Never logged, never kept.
#[tauri::command]
pub async fn crucible_clipboard() -> Result<Option<String>, Error> {
    blocking(|| Ok(clipboard_target(read_clipboard_text()))).await
}

fn read_clipboard_text() -> Option<String> {
    arboard::Clipboard::new().ok()?.get_text().ok()
}

/// The clipboard's text when it may be a Crucible code or link (a superset of the UI's `crucibleTarget`: a code with
/// any separators, a share, pack or kiln:// link, with or without https://, a version id), trimmed; None otherwise.
fn clipboard_target(text: Option<String>) -> Option<String> {
    let t = text?.trim().to_string();
    if t.is_empty() || t.chars().count() > 200 {
        return None;
    }
    let code: String = t.chars().filter(|c| !c.is_whitespace() && !matches!(c, '-' | '_' | '.')).collect();
    let ok = crucible::parse_code(&code).is_some()
        || crucible::parse_target(&t).is_some()
        || (!t.contains("://") && crucible::parse_target(&format!("https://{t}")).is_some());
    ok.then_some(t)
}

// ---------------------------------------------------------------- links arriving

/// The link this process was started with while no launcher ran (`link_entry`): held in memory until the window can
/// take it (`start`), then given to it once.
static HELD: Mutex<Option<String>> = Mutex::new(None);

/// `main` calls this for `--link <url>` (or a bare kiln:// argument), before Tauri and before the data folder's gates
/// open: `state::dirs()` refuses until the beta copy is safe (crate::snapshot::startup; up to 0.2.0-dev.31.3 the link
/// was lost here). True: start the window, the link waits for it (in memory, or in the inbox when the launcher that ran
/// ended meanwhile); false: a launcher already runs and takes the link from its inbox, this process ends. The data
/// folder is written only to hand the link to that launcher, and only while it is free for it
/// (crate::folder::handoff_dirs); otherwise this start shows why, as any start would. A refused link starts the window
/// without it, as before.
pub fn link_entry(url: &str) -> bool {
    if !links::accepted(url) {
        launcher_log("crucible", "a kiln:// link that is not a Crucible link was refused");
        return true;
    }
    if let Some(d) = crate::folder::handoff_dirs() {
        match links::find_instance(&d) {
            Ok(links::Found::Running) => match links::queue(&d, url) {
                Ok(_) => match links::find_instance(&d) {
                    // the launcher that runs takes it from its inbox
                    Ok(links::Found::Running) => return false,
                    // it ended meanwhile: this start reads the inbox once its window is up
                    Ok(links::Found::Free(i)) => {
                        *INSTANCE.lock().unwrap() = i;
                        return true;
                    }
                    Err(_) => return true,
                },
                Err(e) => launcher_log("crucible", &format!("could not hand a link to the running launcher: {}", e.message)),
            },
            // no launcher runs: this one holds the lock from now on (read-only; none is made before the gate)
            Ok(links::Found::Free(i)) => *INSTANCE.lock().unwrap() = i,
            Err(_) => {}
        }
    }
    *HELD.lock().unwrap() = Some(url.to_string());
    true
}

/// What the gate's opening (`start`) found.
struct GateOpen {
    /// The link this process was started with (at most once in a process).
    held: Option<String>,
    /// The data folder is usable (else nothing more runs, as before).
    folder: bool,
}

/// The gate is open (lib.rs `after_snapshot`): the start's own link, and the instance lock unless another launcher has it.
fn gate_open() -> GateOpen {
    let held = HELD.lock().unwrap().take();
    let Ok(d) = dirs() else { return GateOpen { held, folder: false } };
    let mut inst = INSTANCE.lock().unwrap();
    if inst.is_none() {
        *inst = links::acquire_instance(&d).ok().flatten();
    }
    GateOpen { held, folder: true }
}

/// One round of the links thread: the inbox, read only by the launcher with the instance lock ([`reader`]).
fn links_round() -> Vec<String> {
    if !reader() { return Vec::new(); }
    inbox_round()
}

/// This process holds the instance lock, or takes it now: the launcher that had it when this window's gate opened has
/// ended since. Without that a second window stayed without a reader for good: a link handed to the launcher that
/// ended, in its last moments before its next round, waited in the inbox for the next start, and the next start for a
/// link opened another window. Read-only while the lock file is there; never while the folder is held or the beta
/// copy's gate is closed.
fn reader() -> bool {
    let mut inst = INSTANCE.lock().unwrap();
    if inst.is_some() {
        return true;
    }
    let taken = {
        let _writing = crate::state::writing();
        let Ok(d) = dirs() else { return false };
        match links::find_instance(&d) {
            Ok(links::Found::Free(Some(i))) => Some(i),
            Ok(links::Found::Free(None)) => links::acquire_instance(&d).ok().flatten(),
            Ok(links::Found::Running) | Err(_) => None,
        }
    };
    let got = taken.is_some();
    *inst = taken;
    drop(inst);
    if got {
        launcher_log("crucible", "this window takes kiln:// links from now on: the launcher that had them ended");
    }
    got
}

/// One look into the inbox (the reader's round). Its files are removed once read: a writer of the data folder that a
/// move or a restore of the beta copy waits for, refused while the folder is held.
fn inbox_round() -> Vec<String> {
    let _writing = crate::state::writing();
    dirs().map(|d| links::take(&d)).unwrap_or_default()
}

/// Links for `crucible_links` (the newest 20 wait); how many wait now.
fn keep(got: Vec<String>) -> usize {
    let mut l = LINKS.lock().unwrap();
    l.extend(got);
    if l.len() > 20 {
        let n = l.len() - 20;
        l.drain(..n);
    }
    l.len()
}

/// The window gets the links: `kiln://crucible-link` says how many wait, and it comes to the front.
fn to_window(app: &AppHandle, got: Vec<String>) {
    let count = keep(got);
    let _ = app.emit(LINK_EVENT, serde_json::json!({ "count": count }));
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.unminimize();
        let _ = w.show();
        let _ = w.set_focus();
    }
}

/// Starts (once, when the gate opens): the link this process was started with goes to the window, the instance lock,
/// the kiln:// registration while Crucible is on, and the links thread: the inbox is read only by the launcher that
/// holds the lock (a second window never takes a link the other one was given), and a second window takes the lock
/// over once that launcher ended ([`reader`]).
pub fn start(app: &AppHandle) {
    let _ = VERSION.set(app.package_info().version.to_string());
    let open = gate_open();
    if let Some(url) = open.held {
        launcher_log("crucible", "the Crucible link this launcher was started with goes to the window");
        to_window(app, vec![url]);
    }
    if !open.folder {
        return;
    }
    if service().is_ok_and(|(ep, _)| ep.enabled() && !ep.dev)
        && let Ok(exe) = launcher_exe()
    {
        match links::register(&exe) {
            Ok(true) => launcher_log("crucible", "registered for kiln:// links"),
            Ok(false) => {}
            Err(e) => launcher_log("crucible", &format!("kiln:// links not registered: {}", e.message)),
        }
    }
    let app = app.clone();
    let _ = std::thread::Builder::new().name("crucible links".into()).spawn(move || loop {
        let got = links_round();
        if !got.is_empty() {
            launcher_log("crucible", &format!("{} Crucible link(s) arrived from another start", got.len()));
            to_window(&app, got);
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    });
}

/// The program the system should start for a link: the AppImage itself when run as one.
fn launcher_exe() -> Result<std::path::PathBuf, Error> {
    if let Some(a) = std::env::var_os("APPIMAGE").filter(|a| !a.is_empty()) {
        return Ok(a.into());
    }
    std::env::current_exe().map_err(|e| Error::new(ErrorCode::Io, format!("where is the launcher? {e}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_clipboard_reaches_the_ui_only_when_it_may_be_a_pack() {
        let some = |s: &str| clipboard_target(Some(s.to_string()));
        // codes, as players copy them
        assert_eq!(some(" H7QK-3MZP\n").as_deref(), Some("H7QK-3MZP"));
        assert_eq!(some("h7qk3mzp").as_deref(), Some("h7qk3mzp"));
        assert_eq!(some("h7qk 3mzp").as_deref(), Some("h7qk 3mzp"));
        assert_eq!(some("H7QK.3MZP").as_deref(), Some("H7QK.3MZP"));
        // links: share, public pack, kiln://, without the scheme; a version id
        for t in [
            "https://frostdev.io/kiln/crucible/p/H7QK-3MZP",
            "frostdev.io/kiln/crucible/p/H7QK-3MZP",
            "https://frostdev.io/kiln/crucible/ember-run",
            "kiln://crucible/H7QK-3MZP",
            "kiln://crucible/pack/ember-run",
            "pv_abc123",
        ] {
            assert_eq!(some(t).as_deref(), Some(t), "{t}");
        }
        // anything else stays out of the webview
        for t in ["", "   ", "a shopping list", "hunter2", "https://example.com/p/H7QK-3MZP", "H7QK-3MZ0", "https://frostdev.io/kiln"] {
            assert_eq!(some(t), None, "{t:?}");
        }
        assert_eq!(some(&format!("H7QK-3MZP{}", " ".repeat(200))).as_deref(), Some("H7QK-3MZP"), "trimmed first");
        assert_eq!(some(&"x".repeat(201)), None);
        assert_eq!(clipboard_target(None), None, "an empty or unreadable clipboard");
    }

    const LINK: &str = "kiln://crucible/H7QK-3MZP";

    /// A start for a link as `main` runs it in every production build: the process's data folder is opened (its lock
    /// taken shared), the beta copy's gate is still "preparing" (a test run's would be "ready": that hid the bug), no
    /// window yet. The link state of the process goes with it.
    struct LinkStart {
        root: std::path::PathBuf,
        dirs: kiln_manage::paths::DataDirs,
        gate: crate::snapshot::startup::TestGate,
        _serial: crate::folder::TestGuard,
    }

    impl Drop for LinkStart {
        fn drop(&mut self) {
            HELD.lock().unwrap().take();
            INSTANCE.lock().unwrap().take();
            LINKS.lock().unwrap().clear();
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    fn link_start(tag: &str, before: impl FnOnce(&std::path::Path)) -> LinkStart {
        let serial = crate::folder::test_serial();
        let root = std::env::temp_dir().join(format!("kl-crucible-link-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        before(&root);
        let folder = std::sync::Arc::new(crate::folder::Folder::open(kiln_manage::paths::DataEnv {
            kiln_data_dir: Some(root.clone().into()), ..Default::default()
        }));
        crate::folder::replace(folder);
        let gate = crate::snapshot::startup::TestGate::preparing();
        LinkStart { dirs: kiln_manage::paths::DataDirs::at(root.clone()), root, gate, _serial: serial }
    }

    fn waiting() -> Vec<String> {
        tauri::async_runtime::block_on(crucible_links()).unwrap()
    }

    /// 0.2.0-dev.31.3 (a player's report, Somamint): a pack link that started the launcher showed no preview. `main`
    /// called `link_entry` before the gate opened, `state::dirs()` refused, and the link was dropped.
    #[test]
    fn a_link_that_starts_the_launcher_reaches_its_window_once_the_gate_opens() {
        let s = link_start("cold", |_| {});
        assert!(crate::state::dirs().is_err(), "the production start: the gate is not open yet");
        let before = crate::commands::data_folder::tests::tree(&s.root);
        assert!(link_entry(LINK), "no launcher runs: this one starts its window");
        assert_eq!(crate::commands::data_folder::tests::tree(&s.root), before, "nothing is written before the gate opens");
        assert!(waiting().is_empty(), "nothing for the window before the gate opens");
        // lib.rs after_snapshot: the gate is open, `start` gives the window what waits
        s.gate.open();
        let open = gate_open();
        assert!(open.folder && INSTANCE.lock().unwrap().is_some(), "this launcher holds the instance lock: later links come to it");
        keep(open.held.into_iter().collect());
        assert_eq!(waiting(), [LINK], "the preview opens with the start's link");
        // once: neither a second look nor the inbox has it again
        let again = gate_open();
        assert!(again.held.is_none());
        assert!(inbox_round().is_empty());
        assert!(waiting().is_empty());
        // a second start for a link finds this launcher running
        assert!(matches!(links::find_instance(&s.dirs).unwrap(), links::Found::Running));
    }

    #[test]
    fn a_link_for_the_running_launcher_goes_to_its_inbox_once_and_this_process_ends() {
        let s = link_start("warm", |_| {});
        // the launcher that runs (another process: its own lock on the file)
        let running = links::acquire_instance(&s.dirs).unwrap().expect("the running launcher's lock");
        assert!(!link_entry(LINK), "the process started for the link ends");
        assert!(HELD.lock().unwrap().is_none(), "and keeps nothing for a window of its own");
        assert!(INSTANCE.lock().unwrap().is_none());
        // what the running launcher's reader takes: the link, once
        assert_eq!(links::take(&s.dirs), [LINK]);
        assert!(links::take(&s.dirs).is_empty());
        drop(running);
    }

    #[test]
    fn a_refused_link_starts_the_window_without_it() {
        let s = link_start("refused", |_| {});
        let before = crate::commands::data_folder::tests::tree(&s.root);
        for bad in ["kiln://crucible/../../x", "kiln://other/H7QK-3MZP", "kiln://crucible/", "nxm://enshrouded/mods/1/files/2"] {
            assert!(link_entry(bad), "{bad}");
        }
        assert!(HELD.lock().unwrap().is_none());
        assert_eq!(crate::commands::data_folder::tests::tree(&s.root), before);
        s.gate.open();
        assert!(gate_open().held.is_none());
        assert!(waiting().is_empty());
    }

    /// The handoff writes only while nothing else may change the folder. Another launcher holds the data-folder lock
    /// exclusively (a move, the beta copy's capture or restore), a restore stopped half way, or this start will remove an
    /// unfinished move's partial copy: nothing is written, the start shows why, as any start would.
    #[test]
    fn a_link_start_writes_nothing_while_the_folder_is_held() {
        // another launcher moves the folder or protects the beta copy (the data-folder lock, exclusive)
        let mut exclusive = None;
        let s = link_start("elsewhere", |root| {
            exclusive = kiln_manage::data_folder::DataLock::exclusive(root).unwrap();
        });
        assert!(exclusive.is_some());
        assert_eq!(crate::folder::current().hold(), crate::folder::Hold::Elsewhere);
        let running = links::acquire_instance(&s.dirs).unwrap().unwrap();
        let before = crate::commands::data_folder::tests::tree(&s.root);
        assert!(link_entry(LINK), "the window starts: it shows that the folder is held");
        assert_eq!(crate::commands::data_folder::tests::tree(&s.root), before);
        drop((running, exclusive, s));

        // a restore of the beta copy stopped half way
        let s = link_start("restore", |root| {
            std::fs::create_dir_all(root.join("snapshots/restore-beta")).unwrap();
            std::fs::write(root.join("snapshots/restore-beta/journal.json"), r#"{"format":1,"files":{}}"#).unwrap();
        });
        let running = links::acquire_instance(&s.dirs).unwrap().unwrap();
        let before = crate::commands::data_folder::tests::tree(&s.root);
        assert!(link_entry(LINK));
        assert_eq!(crate::commands::data_folder::tests::tree(&s.root), before);
        drop((running, s));

        // this start holds the folder to remove an unfinished move's partial copy
        let serial = crate::folder::test_serial();
        let (t, f) = crate::commands::data_folder::tests::after_an_unfinished_move("crucible-link");
        let dirs = f.located().unwrap().dirs.clone();
        let running = links::acquire_instance(&dirs).unwrap().unwrap();
        let gate = crate::snapshot::startup::TestGate::preparing();
        let before = crate::commands::data_folder::tests::tree(&dirs.root);
        assert!(link_entry(LINK));
        assert_eq!(crate::commands::data_folder::tests::tree(&dirs.root), before, "the removal's proof stays as it is");
        drop((running, gate));
        HELD.lock().unwrap().take();
        crate::commands::data_folder::forget_start();
        drop(serial);
        let _ = std::fs::remove_dir_all(t);
    }

    /// A second window (its gate opened while another launcher held the instance lock) takes the lock over once that
    /// launcher ended: a link handed to the one that ended, in its last moments before its next round, reaches this
    /// window (it waited in the inbox for the next start before, and Crucible drops links older than an hour), and the
    /// next start for a link finds this window running instead of opening another.
    #[test]
    fn a_second_window_takes_the_links_over_once_the_launcher_that_had_them_ended() {
        let s = link_start("take-over", |_| {});
        s.gate.open();
        let first = links::acquire_instance(&s.dirs).unwrap().unwrap();
        let open = gate_open();
        assert!(open.folder && INSTANCE.lock().unwrap().is_none(), "the first launcher has the lock");
        links::queue(&s.dirs, LINK).unwrap();
        assert!(links_round().is_empty(), "while the first launcher runs its links stay for it");
        assert!(INSTANCE.lock().unwrap().is_none());
        // the first launcher ended before its next round
        drop(first);
        assert_eq!(links_round(), [LINK], "this window took the lock over and reads the inbox");
        assert!(links_round().is_empty(), "once");
        assert!(matches!(links::find_instance(&s.dirs).unwrap(), links::Found::Running), "a later link start hands over to this window");
        // with the folder held (here: the beta copy's gate closed again) no lock is taken over
        INSTANCE.lock().unwrap().take();
        let gate = crate::snapshot::startup::TestGate::preparing();
        assert!(links_round().is_empty());
        assert!(INSTANCE.lock().unwrap().is_none());
        drop(gate);
    }

    #[test]
    fn only_the_launcher_with_the_instance_lock_reads_the_inbox() {
        let s = link_start("second-window", |_| {});
        s.gate.open();
        let running = links::acquire_instance(&s.dirs).unwrap().unwrap();
        links::queue(&s.dirs, LINK).unwrap();
        // a second window (started without a link) finds the lock taken: no reader, the link stays for the first
        let open = gate_open();
        assert!(open.folder && INSTANCE.lock().unwrap().is_none());
        assert!(links_round().is_empty());
        assert_eq!(links::take(&s.dirs), [LINK]);
        drop(running);
    }

    /// End to end against a Crucible running on this machine (MinIO for R2 and the CDN, the fake relay for the
    /// sign-in), the way the launcher's commands drive it: share a profile, open its code without a sign-in, install
    /// it in a second data folder, share the next version and update. launcher/scripts/crucible-e2e.mjs starts the
    /// service and runs this with KILN_CRUCIBLE_DEV and KILN_CRUCIBLE_E2E_SESSION; plain `cargo test` skips it.
    #[test]
    #[ignore = "needs a local Crucible: launcher/scripts/crucible-e2e.mjs"]
    fn end_to_end_against_a_local_crucible() {
        use kiln_manage::paths::DataDirs;
        use kiln_manage::task::NoProgress;
        let session = std::env::var("KILN_CRUCIBLE_E2E_SESSION").expect("KILN_CRUCIBLE_E2E_SESSION");
        let (ep, t) = service().expect("KILN_CRUCIBLE_DEV");
        assert!(ep.dev && ep.enabled(), "KILN_CRUCIBLE_DEV must name the local service and its key");
        let root = std::env::temp_dir().join(format!("kiln-crucible-e2e-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let step = |s: &str| eprintln!("ok   {s}");
        // the creator: two Kiln mods in a profile
        let a = DataDirs::at(root.join("creator"));
        let src = root.join("src");
        let mk = |id: &str, v: &str| {
            let dir = src.join(format!("{id}-{v}"));
            std::fs::create_dir_all(dir.join("content")).unwrap();
            std::fs::write(dir.join("kiln.mod.json"), format!(r#"{{ "id": "{id}", "version": "{v}", "apiVersion": "0.2", "side": "both" }}"#)).unwrap();
            std::fs::write(dir.join("content/data.json"), format!(r#"{{ "format": 1, "note": "{id} {v} {}" }}"#, "x".repeat(40_000))).unwrap();
            dir
        };
        let add = |d: &DataDirs, pid: &str, dirs: Vec<std::path::PathBuf>| {
            let r = kiln_manage::library::import(d, &dirs, &mut NoProgress).unwrap();
            let mut p = kiln_manage::profile::get(d, pid).unwrap();
            for lr in r.imported.iter().chain(&r.existing) {
                p.mods.kiln.push(kiln_manage::profile::ProfileMod { id: lr.id.clone(), version: lr.version.clone(), hash: lr.hash.clone(), enabled: true });
            }
            kiln_manage::profile::save(d, &p).unwrap();
        };
        let p = kiln_manage::profile::create(&a, "Ember run", None).unwrap();
        // one mod with 6 MB that does not compress: over the script's multipart threshold, two 5 MiB parts
        let big = mk("kiln-auto-loot", "0.1.8");
        let mut x: u64 = 0x9e37_79b9_7f4a_7c15;
        let noise: Vec<u8> = (0..6_000_000).map(|_| { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x as u8 }).collect();
        std::fs::create_dir_all(big.join("assets")).unwrap();
        std::fs::write(big.join("assets/noise.bin"), noise).unwrap();
        add(&a, &p.id, vec![big, mk("busy-hands", "0.2.8")]);
        let ca = Crucible::new(&ep, t.as_ref(), "0.2.0-dev").with_session(Some(session));
        let r1 = crucible::share(&a, &ca, &crucible::ShareRequest { profile: p.id.clone(), ..Default::default() }, None, &mut NoProgress).expect("share");
        assert_eq!(r1.version.status, "ready", "{r1:?}");
        step(&format!("shared: {} code {} ({} files, {} bytes up)", r1.pack.url, r1.pack.code, r1.files, r1.uploaded));
        // the friend: no sign-in
        let b = DataDirs::at(root.join("friend"));
        let cb = Crucible::new(&ep, t.as_ref(), "0.2.0-dev");
        let (pv, _) = crucible::preview(&b, &cb, &r1.pack.code.to_lowercase(), None).expect("preview");
        assert_eq!(pv.missing.len(), 2);
        step(&format!("preview by code: signed index verified, {} to download", pv.download));
        let ir = crucible::install(&b, &cb, None, &crucible::InstallRequest { target: pv.version.id.clone(), consent: true, ..Default::default() }, pv.pack.owner.as_ref(), true, &mut NoProgress).expect("install");
        assert_eq!(ir.profile.mods.kiln.len(), 2);
        step(&format!("installed as profile {} ({} bytes downloaded)", ir.profile.id, ir.downloaded));
        // the friend's own change: a data edit in the player layer (local.<pid>), which must outlive the update
        let mine = r#"{ "format": 1, "items": { "Material_T1_Wood_Logs": { "maxStackSize": 999 } } }"#;
        kiln_manage::modpack::layers::write(&b, &ir.profile.id, "content/mine.json", Some(mine)).expect("player layer");
        let lp = kiln_manage::modpack::layers::save(&b, &ir.profile.id).expect("save player layer");
        let local_id = kiln_manage::modpack::layers::local_id(&ir.profile.id);
        assert_eq!(lp.mods.kiln.last().map(|m| m.id.as_str()), Some(local_id.as_str()));
        step("the player's data change saved as the player layer");
        // the creator adds a mod and updates the link: only the new archive goes up
        add(&a, &p.id, vec![mk("kiln-equipment", "0.3.0")]);
        let r2 = crucible::share(&a, &ca, &crucible::ShareRequest { profile: p.id.clone(), ..Default::default() }, None, &mut NoProgress).expect("share 2");
        assert_eq!((r2.pack.id.as_str(), r2.version.number), (r1.pack.id.as_str(), 2));
        assert!(r2.skipped > 0 && r2.uploaded < r1.uploaded + r2.skipped, "{r2:?}");
        step(&format!("version 2 shared on the same link ({} bytes up, {} kept)", r2.uploaded, r2.skipped));
        let ups = crucible::updates(&b, &cb).expect("updates");
        assert_eq!(ups.len(), 1);
        let plan = crucible::update_plan(&b, &cb, &ir.profile.id, &mut NoProgress).expect("plan");
        assert_eq!(plan.plan.mods.added.len(), 1);
        assert!(plan.plan.mods.yours.is_empty(), "the player layer is no mod of yours: {:?}", plan.plan.mods.yours);
        assert_eq!(plan.plan.local.as_ref().map(|l| l.fields), Some(1), "the plan shows the player's data edit");
        let done = crucible::update(&b, &cb, &crucible::UpdateRequest { profile: ir.profile.id.clone(), version_id: plan.version_id, consent: true, ..Default::default() }, &mut NoProgress).expect("update");
        assert_eq!(done.profile.mods.kiln.len(), 4);
        assert_eq!(done.profile.mods.kiln.last().map(|m| m.id.as_str()), Some(local_id.as_str()), "the player layer still loads last");
        let kept = kiln_manage::modpack::layers::files(&b, &ir.profile.id).unwrap();
        assert_eq!(kept.iter().map(|f| f.text.as_str()).collect::<Vec<_>>(), [mine], "the player's change survived the update");
        step("update found, planned and applied; the player's change kept, still last");
        let back = kiln_manage::modpack::pack::undo(&b, &ir.profile.id).expect("undo");
        assert_eq!(back.mods.kiln.len(), 3);
        assert_eq!(back.mods.kiln.last().map(|m| m.id.as_str()), Some(local_id.as_str()));
        step("undo (the player layer stays)");
        let _ = std::fs::remove_dir_all(&root);
    }
}
