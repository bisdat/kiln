//! Read-only registry snapshot. No caller-provided file name or game mapping crosses this command.
use crate::state::blocking;
use kiln_manage::{Error, ErrorCode};
#[path = "registry_report_file.rs"]
mod file;

#[tauri::command]
pub async fn registry_report_read(game_id: String) -> Result<Option<String>, Error> {
    blocking(move || {
        let game = crate::state::game(&game_id)?;
        file::read_report(&game.path)
            .map_err(|e| Error::new(ErrorCode::Io, format!("Registry report could not be read: {e}")))
    }).await
}
