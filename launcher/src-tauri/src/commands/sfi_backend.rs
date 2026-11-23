// SPDX-License-Identifier: MIT
//! Tauri commands: Settings' ShroudForge card (`kiln_manage::sfi_backend`). kiln.json
//! `shroudforge.backend` of one game: what is set (the default, a choice, the older useInstalled switch, a value no
//! reader takes), what the next start gets, why Kiln's interpreter cannot be chosen now, and the last start as the core
//! froze it. A choice is previewed first (nothing written), then written only over the kiln.json bytes the card read.

use crate::state::blocking;
use kiln_manage::Error;
use kiln_manage::game::{GameInstall, GameKind};
use kiln_manage::manifest::RunSide;
use kiln_manage::sfi_backend::{self, Backend, Card, Env, Preview};
use serde::Serialize;
use std::path::PathBuf;

/// The card's answer, with the game it is for (the window drops an answer for a game it no longer shows).
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BackendCard {
    pub game_id: String,
    #[serde(flatten)]
    pub card: Card,
}

/// A preview's answer, with the game it is for.
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BackendPreview {
    pub game_id: String,
    #[serde(flatten)]
    pub preview: Preview,
}

/// The game folder and side of `game_id`; Kiln must be installed (the kiln.json card's rule).
fn game_of(game_id: &str) -> Result<(GameInstall, PathBuf, RunSide), Error> {
    let g = crate::state::game(game_id)?;
    if !g.path.join(kiln_manage::paths::game::KILN).is_dir() {
        return Err(Error::not_found("Kiln is not installed in this game."));
    }
    let side = if g.kind == GameKind::Server { RunSide::Server } else { RunSide::Client };
    let path = g.path.clone();
    Ok((g, path, side))
}

/// `sfi_backend_read`: the card of one game.
#[tauri::command]
pub async fn sfi_backend_read(game_id: String) -> Result<BackendCard, Error> {
    blocking(move || {
        let (_, path, side) = game_of(&game_id)?;
        Ok(BackendCard { card: sfi_backend::card(&path, side, &Env::from_process())?, game_id })
    })
    .await
}

/// `sfi_backend_preview`: what choosing `backend` (null: back to the default) would do; writes nothing.
#[tauri::command]
pub async fn sfi_backend_preview(game_id: String, backend: Option<Backend>) -> Result<BackendPreview, Error> {
    blocking(move || {
        let (_, path, side) = game_of(&game_id)?;
        Ok(BackendPreview { preview: sfi_backend::preview(&path, backend, side, &Env::from_process())?, game_id })
    })
    .await
}

/// `sfi_backend_set`: writes the choice, only over the kiln.json bytes `revision` names and never while the game runs;
/// one line in launcher.log. The card of the file as written.
#[tauri::command]
pub async fn sfi_backend_set(game_id: String, backend: Option<Backend>, revision: String) -> Result<BackendCard, Error> {
    blocking(move || {
        let (g, path, side) = game_of(&game_id)?;
        crate::state::refuse_running(&g)?;
        let card = sfi_backend::write_game_at(&path, backend, side, &revision, &Env::from_process())?;
        let value = backend.map(|b| serde_json::Value::String(b.as_str().into()));
        crate::commands::app::launcher_log("settings",
            &crate::commands::config::kiln_setting_line(&game_id, "shroudforge.backend", value.as_ref()));
        Ok(BackendCard { game_id, card })
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::sfi_backend::SettingView;

    #[test]
    fn the_answers_name_their_game_and_the_card_keeps_its_fields_at_the_top() {
        let g = std::env::temp_dir().join(format!("kiln-launcher-sfi-card-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(g.join("kiln")).unwrap();
        std::fs::write(g.join("kiln/kiln.json"), r#"{"shroudforge": {"useInstalled": true}}"#).unwrap();
        let card = sfi_backend::card(&g, RunSide::Client, &Env::default()).unwrap();
        assert_eq!(card.setting, SettingView::Inherited { backend: Backend::Installed });
        let v = serde_json::to_value(BackendCard { game_id: "client-a".into(), card }).unwrap();
        assert_eq!(v["gameId"], "client-a");
        assert_eq!(v["setting"], serde_json::json!({"kind": "inherited", "backend": "installed"}));
        assert_eq!(v["next"]["selected"], "installed");
        assert_eq!(v["next"]["reason"], "kiln.json shroudforge.useInstalled: true (the older setting): installed");
        // Kiln's interpreter is a normal choice in this build (its gate list has no kiln-sfi.interpreter row)
        assert_eq!(v.get("interpreterHeld"), None, "{v}");
        assert!(v["revision"].as_str().is_some_and(|r| r.len() == 64), "{v}");
        let p = sfi_backend::preview(&g, Some(Backend::Kiln), RunSide::Client, &Env::default()).unwrap();
        let v = serde_json::to_value(BackendPreview { game_id: "client-a".into(), preview: p }).unwrap();
        assert_eq!((v["gameId"].as_str(), v["backend"].as_str(), v.get("refused")), (Some("client-a"), Some("kiln"), None), "{v}");
        assert_eq!((v["next"]["selected"].as_str(), v["next"]["reason"].as_str()),
                   (Some("kiln"), Some("kiln.json shroudforge.backend: \"kiln\"")), "{v}");
        assert_eq!(v["unchanged"], false);
        // a preview writes nothing
        assert_eq!(std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap(), r#"{"shroudforge": {"useInstalled": true}}"#);
        let _ = std::fs::remove_dir_all(&g);
    }
}
