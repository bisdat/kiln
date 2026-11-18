//! Tauri commands: vendor upscaler files (Settings, Graphics: the upscaler behind the game's FSR 3 option and its files,
//! components/UpscalerSettings.tsx; the Deck's Settings). Names and arguments: docs/kiln/launcher/contracts.md section
//! 9; the logic is kiln_manage::vendor.
//!
//! (0.2.0-dev.29.18) Out of the preview "upscalers": phase B runs the files behind the game's FSR 3 option. A dedicated
//! server is refused: it draws no frames.

use super::app::{spawn_task, to_value};
use super::TaskId;
use crate::state::{blocking, dirs};
use kiln_manage::vendor::{self, agreements, Agreements, ComponentStatus, RemoveReport, Store};
use kiln_manage::{net, Error, ErrorCode};
use serde::{Deserialize, Serialize};
use tauri::AppHandle;

/// The previews on for this start: settings.json's `preview` list, and `KILN_LAUNCHER_PREVIEW` (names separated by
/// commas or spaces) for one start. Sorted, each once. (dev.29.18) No feature is behind one now: the upscaler files'
/// preview ended with phase B; the list stays for the next preview (`app_info.preview`).
pub fn previews() -> Vec<String> {
    let mut out: Vec<String> = crate::state::read_settings().map(|s| s.preview).unwrap_or_default();
    out.extend(previews_in(std::env::var("KILN_LAUNCHER_PREVIEW").ok().as_deref()));
    out.retain(|p| !p.is_empty());
    out.sort();
    out.dedup();
    out
}

fn previews_in(env: Option<&str>) -> Vec<String> {
    env.unwrap_or("").split([',', ' ']).map(|s| s.trim().to_ascii_lowercase()).filter(|s| !s.is_empty()).collect()
}

/// `vendor_status`: what the Upscalers section shows for a game.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VendorStatus {
    /// `kiln/vendor`, relative to the game folder.
    pub folder: String,
    pub components: Vec<ComponentStatus>,
}

/// `vendor_license`: a component's licence, the text in full.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LicenseText {
    pub component: String,
    pub vendor: String,
    pub name: String,
    pub text: String,
    pub sha256: String,
    pub from: String,
    pub agreed: bool,
}

fn component(id: &str) -> Result<vendor::Component, Error> {
    vendor::builtin()?.component(id).cloned().ok_or_else(|| Error::not_found(format!("Kiln knows no vendor component {id}.")))
}

/// The status of every component Kiln knows, for one client.
#[tauri::command]
pub async fn vendor_status(game_id: String) -> Result<VendorStatus, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        let store = Store::for_game(&g)?;
        let m = vendor::builtin()?;
        let agreed = Agreements::load(&dirs()?);
        Ok(VendorStatus { folder: vendor::store::DIR.into(), components: store.status(&m, &agreed) })
    })
    .await
}

/// A component's licence text (bundled with Kiln; nothing is fetched).
#[tauri::command]
pub async fn vendor_license(component_id: String) -> Result<LicenseText, Error> {
    blocking(move || {
        let c = component(&component_id)?;
        let text = vendor::manifest::license_text(&c.license.file).ok_or_else(|| Error::new(ErrorCode::Internal, "the licence text is not bundled (a bug)"))?;
        let agreed = Agreements::load(&dirs()?).agreed(&c);
        Ok(LicenseText { component: c.id, vendor: c.vendor, name: c.license.name, text: text.to_string(), sha256: c.license.sha256, from: c.license.from, agreed })
    })
    .await
}

/// The checks before a download starts, in the order the player meets them.
fn admit(g: &kiln_manage::game::GameInstall, c: &vendor::Component, license_sha256: Option<&str>) -> Result<(), Error> {
    Store::for_game(g)?;
    if !c.pinned() {
        return Err(Error::new(ErrorCode::Refused, format!("{} is not available in this build of Kiln. Nothing was downloaded.", c.name)));
    }
    let d = dirs()?;
    if !Agreements::load(&d).agreed(c) {
        match license_sha256 {
            Some(shown) => {
                agreements::record(&d, c, shown)?;
            }
            None => return Err(Error::new(ErrorCode::ConsentRequired, format!("Read {}'s {} and agree to it first.", c.vendor_name, c.license.name))),
        }
    }
    crate::state::refuse_running(g)
}

/// Downloads a component into the game's `kiln/vendor/<vendor>/<version>/` as a task (progress, cancel). The first
/// download of a vendor's licence text passes `licenseSha256`, the hash of the text the dialog showed and the player
/// agreed to: it is recorded (`<data>/state/vendor-licenses.json`) before anything is fetched.
#[tauri::command]
pub async fn vendor_download(app: AppHandle, game_id: String, component_id: String, license_sha256: Option<String>) -> Result<TaskId, Error> {
    let (g, c) = blocking(move || {
        let g = crate::state::game(&game_id)?;
        let c = component(&component_id)?;
        admit(&g, &c, license_sha256.as_deref())?;
        Ok((g, c))
    })
    .await?;
    let title = format!("Download {}", c.name);
    Ok(spawn_task(&app, "vendor_download", title, move |p| {
        let store = Store::for_game(&g)?;
        let transport = net::UreqTransport::new();
        let client = net::Client::new(vendor::download::policy(), &transport, &vendor::download::user_agent());
        let installer = format!("kiln-launcher {}", env!("CARGO_PKG_VERSION"));
        let report = vendor::download(&store, &c, &client, &installer, p)?;
        to_value(&report)
    }))
}

/// Removes a component's files from the game (every version Kiln put there; files it did not put there stay, named).
#[tauri::command]
pub async fn vendor_remove(game_id: String, component_id: String) -> Result<RemoveReport, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        let store = Store::for_game(&g)?;
        let c = component(&component_id)?;
        crate::state::refuse_running(&g)?;
        store.remove(&c)
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};

    fn install(kind: GameKind) -> GameInstall {
        GameInstall {
            id: "g".into(), kind, path: std::env::temp_dir().join("kiln-vendor-cmd-none"), exe: "enshrouded.exe".into(), app_id: kiln_manage::CLIENT_APP_ID,
            library: None, platform: Platform::Proton, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy,
            source: "manual".into(),
        }
    }

    #[test]
    fn previews_come_from_settings_and_the_environment() {
        assert_eq!(previews_in(Some("upscalers")), ["upscalers"]);
        assert_eq!(previews_in(Some(" Upscalers, later  other")), ["upscalers", "later", "other"]);
        assert!(previews_in(Some("")).is_empty() && previews_in(None).is_empty());
        let s: crate::commands::app::LauncherSettings = serde_json::from_str(r#"{"preview": ["upscalers"]}"#).unwrap();
        assert_eq!(s.preview, ["upscalers"]);
        // absent in every settings.json before it, and not written while empty
        let s: crate::commands::app::LauncherSettings = serde_json::from_str("{}").unwrap();
        assert!(s.preview.is_empty());
        assert!(serde_json::to_value(&s).unwrap().get("preview").is_none());
    }

    #[test]
    fn a_download_is_refused_on_a_server_and_when_not_pinned_and_needs_no_preview() {
        let m = vendor::builtin().unwrap();
        let shipped = m.component("amd-fsr").unwrap();
        assert!(shipped.pinned());
        assert_eq!(admit(&install(GameKind::Server), shipped, None).unwrap_err().code, ErrorCode::Unsupported);
        // a component with a file not pinned (a fake): refused before any licence or data folder is looked at
        let mut c = shipped.clone();
        c.files[2].sha256 = vendor::manifest::NOT_PINNED.into();
        let e = admit(&install(GameKind::Client), &c, Some(&c.license.sha256)).unwrap_err();
        assert_eq!(e.code, ErrorCode::Refused);
        assert!(e.message.contains("not available in this build"), "{}", e.message);
    }
}
