use kiln_manage::Error;
use tauri::AppHandle;

#[tauri::command]
pub fn snapshot_status() -> crate::snapshot::startup::Status { crate::snapshot::startup::status() }

#[tauri::command]
pub fn snapshot_continue(app: AppHandle) -> Result<(), Error> {
    let f = crate::folder::current();
    let root = &f.located()?.dirs.root;
    let version = app.package_info().version.to_string();
    let channel = crate::snapshot_channel(&version);
    crate::snapshot::startup::resume(root, &version, channel, crate::distribution::beta())?;
    crate::after_snapshot(&app);
    Ok(())
}
