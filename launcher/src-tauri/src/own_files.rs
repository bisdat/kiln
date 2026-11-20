//! The launcher's own files in its install folder, looked at once per start (dev.29.18, inbox 92).
//!
//! A player's antivirus blocked Kiln's install and then took the launcher's exe, its uninstaller and the setup the
//! player had downloaded. A running launcher can see only what is gone next to it: a Windows install (Tauri's NSIS
//! installer) puts `uninstall.exe` next to the launcher's exe, and the exe itself can be gone when it was taken while it
//! ran. When one of them is gone, launcher.log says it, the window says it once (`AppInfo.own_files_missing`,
//! launcher/src/shell/OwnFilesNotice.tsx) and the support report has a finding (`launcher-files-missing`). The setup is
//! in the player's downloads, which the launcher does not know: not looked at. Other installs (the AppImage, a .deb, a
//! development build, an MSI, a build's own exe without the runtime the setup puts beside it) are not looked at either.

use kiln_manage::report::{Finding, Level};
use std::path::Path;
use std::sync::OnceLock;

/// What Tauri's NSIS installer names the uninstaller it writes next to the launcher (`$INSTDIR\uninstall.exe`).
pub const UNINSTALLER: &str = "uninstall.exe";

/// The file names of a Windows install's own files that are not in the folder of `exe` (the launcher's exe): the exe
/// itself, then the uninstaller. Nothing when `nsis` is false (not that installer's install), and nothing in a folder
/// without the Kiln runtime the setup puts next to the exe (`runtime/kiln-runtime.zip`): the exe a Windows build leaves
/// beside its setup reports the NSIS bundle too, and a tester runs it on its own.
pub fn missing_next_to(exe: &Path, nsis: bool) -> Vec<String> {
    let Some(dir) = exe.parent().filter(|d| nsis && d.join(crate::commands::runtime::SHIPPED_RUNTIME).is_file()) else { return Vec::new() };
    let mut out = Vec::new();
    if !exe.exists() {
        out.extend(exe.file_name().map(|n| n.to_string_lossy().into_owned()));
    }
    if !dir.join(UNINSTALLER).exists() {
        out.push(UNINSTALLER.into());
    }
    out
}

/// This launcher was installed by the NSIS installer on Windows.
fn nsis() -> bool {
    cfg!(windows) && matches!(tauri::utils::platform::bundle_type(), Some(tauri::utils::config::BundleType::Nsis))
}

/// What [`missing_next_to`] found at this start (looked at once; later calls answer the same).
pub fn missing() -> &'static [String] {
    static FOUND: OnceLock<Vec<String>> = OnceLock::new();
    FOUND.get_or_init(|| std::env::current_exe().map(|exe| missing_next_to(&exe, nsis())).unwrap_or_default())
}

/// The start's look: one launcher.log line when something is gone.
pub fn start() {
    let gone = missing();
    if !gone.is_empty() {
        let dir = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.display().to_string())).unwrap_or_default();
        crate::commands::app::launcher_log("launcher", &format!(
            "the launcher's own files are missing from {dir}: {} (an antivirus may have quarantined them)", gone.join(", ")));
    }
}

/// The support report's finding for `gone` (None: nothing is gone).
pub fn finding_for(gone: &[String]) -> Option<Finding> {
    (!gone.is_empty()).then(|| Finding {
        id: "launcher-files-missing".into(),
        level: Level::Warning,
        text: format!(
            "The launcher's own {} {} missing from its install folder: {}. An antivirus may have quarantined {}. Restore {} from \
             the antivirus's quarantine and add an exclusion, or install the launcher again.",
            if gone.len() == 1 { "file" } else { "files" }, if gone.len() == 1 { "is" } else { "are" }, gone.join(", "),
            if gone.len() == 1 { "it" } else { "them" }, if gone.len() == 1 { "it" } else { "them" }),
    })
}

/// This start's finding.
pub fn finding() -> Option<Finding> {
    finding_for(missing())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_windows_installs_missing_uninstaller_and_exe_are_named() {
        let dir = std::env::temp_dir().join(format!("kiln-own-files-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let exe = dir.join("Kiln Launcher.exe");
        std::fs::write(&exe, b"MZ").unwrap();
        // the exe of a build folder (no runtime beside it, as the setup puts there): not an install, nothing is looked at
        assert!(missing_next_to(&exe, true).is_empty());
        std::fs::create_dir_all(dir.join("runtime")).unwrap();
        std::fs::write(dir.join(crate::commands::runtime::SHIPPED_RUNTIME), b"PK").unwrap();
        std::fs::write(dir.join(UNINSTALLER), b"MZ").unwrap();
        assert!(missing_next_to(&exe, true).is_empty());
        std::fs::remove_file(dir.join(UNINSTALLER)).unwrap();
        assert_eq!(missing_next_to(&exe, true), ["uninstall.exe"]);
        std::fs::remove_file(&exe).unwrap();
        assert_eq!(missing_next_to(&exe, true), ["Kiln Launcher.exe", "uninstall.exe"]);
        // not the NSIS installer's install (an AppImage, a development build): nothing is looked at
        assert!(missing_next_to(&exe, false).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_finding_names_the_files_and_what_to_do() {
        assert_eq!(finding_for(&[]), None);
        let f = finding_for(&["uninstall.exe".into()]).unwrap();
        assert_eq!((f.id.as_str(), f.level), ("launcher-files-missing", Level::Warning));
        assert_eq!(f.text, "The launcher's own file is missing from its install folder: uninstall.exe. An antivirus may have quarantined it. \
            Restore it from the antivirus's quarantine and add an exclusion, or install the launcher again.");
        assert!(finding_for(&["Kiln Launcher.exe".into(), "uninstall.exe".into()]).unwrap().text
            .starts_with("The launcher's own files are missing from its install folder: Kiln Launcher.exe, uninstall.exe. An antivirus may have quarantined them."));
    }

    #[test]
    fn this_test_binary_is_no_nsis_install() {
        assert!(missing().is_empty());
        assert_eq!(finding(), None);
    }
}
