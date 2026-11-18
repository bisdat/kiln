//! Tauri commands: game. Names and arguments: docs/kiln/launcher/contracts.md section 9.
//! Game installs on kiln_manage::game; `game_status` reuses a cached install state (crate::state::install_state).

use crate::state::{blocking, dirs};
use kiln_manage::Error;
use kiln_manage::game::{self, GameInstall, GameStatus};
use std::path::PathBuf;

#[tauri::command]
pub async fn game_list() -> Result<Vec<GameInstall>, Error> {
    blocking(|| {
        let d = dirs()?;
        let known = game::known(&d)?;
        // (dev.29.17) a folder that starts directly runs in its own prefix: a record from an older launcher (Steam's
        // prefix guessed for a custom folder) is brought in line once
        let roots = crate::state::steam_roots();
        Ok(known.into_iter().map(|g| {
            if g.kind == game::GameKind::Client && kiln_manage::direct::mode_of(&d, &g) == kiln_manage::direct::StartMode::Direct {
                game::sync_start(&d, &g, &roots).unwrap_or(g)
            } else {
                g
            }
        }).collect())
    })
    .await
}

/// Scans every Steam library (plus the settings' extra folders) and records the installs not known yet.
#[tauri::command]
pub async fn game_detect() -> Result<Vec<GameInstall>, Error> {
    blocking(|| {
        let d = dirs()?;
        let roots = crate::state::steam_roots();
        let found = game::detect_in(&roots, &d.root)?;
        let known = game::known(&d)?;
        for g in found {
            if !known.iter().any(|k| k.path == g.path) {
                // the same roots (the settings' extra Steam folders too): an install found there is Steam's copy
                game::add_with(&d, &g.path, &roots)?;
            }
        }
        game::known(&d)
    })
    .await
}

#[tauri::command]
pub async fn game_add(path: String) -> Result<GameInstall, Error> {
    if path.trim().is_empty() {
        return Err(Error::invalid("Give a game folder."));
    }
    blocking(move || {
        let d = dirs()?;
        let p = PathBuf::from(kiln_manage::util::expand(path.trim(), &kiln_manage::util::home()));
        game::add_with(&d, &p, &crate::state::steam_roots())
    })
    .await
}

#[tauri::command]
pub async fn game_remove(game_id: String) -> Result<(), Error> {
    blocking(move || {
        crate::state::forget_install_state(&game_id);
        game::remove(&dirs()?, &game_id)
    })
    .await
}

#[tauri::command]
pub async fn game_status(game_id: String) -> Result<GameStatus, Error> {
    blocking(move || crate::state::game_status(&crate::state::game(&game_id)?)).await
}

/// `game_start_get`: how a client starts (through Steam or directly), what the player chose, whether a Steam install was
/// found, and on Linux the umu-run, Proton and Wine builds found (in the order automatic takes them), the one in use,
/// Kiln's own prefix and the command a direct start runs (dev.29.17).
#[tauri::command]
pub async fn game_start_get(game_id: String) -> Result<kiln_manage::direct::StartSetup, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        Ok(kiln_manage::direct::setup(&dirs()?, &g, &crate::state::steam_roots()))
    })
    .await
}

/// `game_start_set`: keeps the player's start choice in the install's record and returns the install as it starts now
/// (its platform and prefix follow a direct start under umu-run, Proton or Wine). Refused while the game runs.
#[tauri::command]
pub async fn game_start_set(game_id: String, settings: kiln_manage::direct::StartSettings) -> Result<GameInstall, Error> {
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        crate::state::refuse_running(&g)?;
        let r = game::set_start(&dirs()?, &game_id, &settings, &crate::state::steam_roots())?;
        crate::commands::app::launcher_log("start", &format!("{}: {:?}", game_id, kiln_manage::direct::effective_mode(&r, Some(&settings))));
        Ok(r)
    })
    .await
}
