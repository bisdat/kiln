//! The folder a file dialog opens in (dev.30). Without one, the dialog library (rfd, under tauri-plugin-dialog) leaves
//! GTK's file chooser at the process's working directory, and an AppImage's AppRun runs the launcher inside its own
//! read-only mount (`/tmp/.mount_kiln-XXXX/usr`): "Save report..." offered a folder nothing can be written to, or one
//! that is gone once the AppImage exits. So every Save and Open dialog of the page asks [`default_for`] first
//! (`app_dialog_default`, launcher/src/ui/dialogFolder.ts): the folder that kind of dialog was last used in, while it
//! still exists; else Downloads; else home. Never the working directory, `$APPDIR` or an AppImage mount. The folder a
//! dialog ended in is kept per kind in settings.json (`dialogFolders`, [`remember`] through `app_dialog_remember`).

use kiln_manage::Error;
use std::path::{Component, Path, PathBuf};

/// At most this many dialog kinds are kept (the page has about ten).
pub const MAX_KINDS: usize = 32;

/// A kind is the page's own short name ("supportReport", "addMods"...).
pub fn valid_kind(kind: &str) -> bool {
    !kind.is_empty() && kind.len() <= 40 && kind.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

/// The folders a dialog falls back to, in order after the last-used one.
#[derive(Debug, Clone, Default)]
pub struct Places {
    /// The system's Downloads folder, then `<home>/Downloads`.
    pub downloads: Vec<PathBuf>,
    pub home: Option<PathBuf>,
    /// `$APPDIR` (Linux, an AppImage's mount): never offered.
    pub appdir: Option<PathBuf>,
}

impl Places {
    pub fn of_this_system(app: &tauri::AppHandle) -> Places {
        use tauri::Manager;
        let home = app.path().home_dir().ok();
        let mut downloads: Vec<PathBuf> = app.path().download_dir().ok().into_iter().collect();
        if let Some(h) = &home {
            downloads.push(h.join("Downloads"));
        }
        Places { downloads, home, appdir: appdir() }
    }
}

#[cfg(target_os = "linux")]
fn appdir() -> Option<PathBuf> {
    std::env::var_os("APPDIR").filter(|v| !v.is_empty()).map(PathBuf::from)
}
#[cfg(not(target_os = "linux"))]
fn appdir() -> Option<PathBuf> {
    None
}

/// A path inside the AppImage: under `$APPDIR`, or under any `.mount_*` folder (the AppImage runtime's mount point,
/// `/tmp/.mount_<first six letters of its name><random>`), whatever `$APPDIR` says.
pub fn in_appimage(p: &Path, appdir: Option<&Path>) -> bool {
    appdir.is_some_and(|a| a.is_absolute() && p.starts_with(a))
        || p.components().any(|c| matches!(c, Component::Normal(n) if n.to_string_lossy().starts_with(".mount_")))
}

/// A folder a dialog may open in: absolute (a relative one is the working directory's) and outside the AppImage.
pub fn acceptable(p: &Path, appdir: Option<&Path>) -> bool {
    p.is_absolute() && !in_appimage(p, appdir)
}

/// The folder a dialog opens in: `preferred` (a path the page has for it, such as a path box's value) when it exists;
/// else `last` (the folder that kind of dialog was last used in) while it is a folder; else Downloads; else home.
/// Each only when [`acceptable`]. None: the page passes no folder (the dialog's own choice).
pub fn choose(
    preferred: Option<&Path>,
    last: Option<&Path>,
    places: &Places,
    is_dir: impl Fn(&Path) -> bool,
    exists: impl Fn(&Path) -> bool,
) -> Option<PathBuf> {
    let ok = |p: &Path| acceptable(p, places.appdir.as_deref());
    if let Some(p) = preferred.filter(|p| ok(p) && exists(p)) {
        return Some(p.to_path_buf());
    }
    last.into_iter()
        .chain(places.downloads.iter().map(PathBuf::as_path))
        .chain(places.home.as_deref())
        .find(|p| ok(p) && is_dir(p))
        .map(Path::to_path_buf)
}

/// The suggested file's bare name (anything before a separator dropped).
fn bare_name(name: &str) -> Option<&std::ffi::OsStr> {
    Path::new(name.trim()).file_name()
}

/// [`choose`] for this system and the settings' last-used folder of `kind`; with `file_name`, `<folder>/<file_name>`
/// (a Save dialog's suggestion).
pub fn default_for(app: &tauri::AppHandle, kind: &str, file_name: Option<&str>, preferred: Option<&str>) -> Option<PathBuf> {
    let last = valid_kind(kind)
        .then(|| crate::state::read_settings().ok().and_then(|s| s.dialog_folders.get(kind).cloned()))
        .flatten();
    let places = Places::of_this_system(app);
    let dir = choose(preferred.map(Path::new), last.as_deref().map(Path::new), &places, |p| p.is_dir(), |p| p.exists())?;
    Some(match file_name.and_then(bare_name) {
        Some(n) if dir.is_dir() => dir.join(n),
        _ => dir,
    })
}

/// The folder to keep after a dialog chose `path`: the path itself for a folder dialog, else its parent; only an
/// [`acceptable`] folder that exists.
pub fn folder_to_keep(path: &Path, folder: bool, appdir: Option<&Path>, is_dir: impl Fn(&Path) -> bool) -> Option<PathBuf> {
    let dir = if folder { path } else { path.parent()? };
    (acceptable(dir, appdir) && is_dir(dir)).then(|| dir.to_path_buf())
}

/// Keeps the folder a dialog of `kind` ended in (settings.json, `dialogFolders`). A kind not seen before is not kept
/// once [`MAX_KINDS`] are; nothing is written when the folder is the one already kept.
pub fn remember(kind: &str, path: &str, folder: bool) -> Result<(), Error> {
    if !valid_kind(kind) {
        return Err(Error::invalid(format!("{kind:?} is not a dialog kind.")));
    }
    let Some(dir) = folder_to_keep(Path::new(path), folder, appdir().as_deref(), |p| p.is_dir()) else {
        return Ok(());
    };
    let dir = dir.display().to_string();
    let _write = crate::state::settings_write();
    let mut s = crate::state::read_settings()?;
    if s.dialog_folders.get(kind) == Some(&dir) || (!s.dialog_folders.contains_key(kind) && s.dialog_folders.len() >= MAX_KINDS) {
        return Ok(());
    }
    s.dialog_folders.insert(kind.to_string(), dir);
    kiln_manage::util::write_json_atomic(&crate::state::dirs()?.settings(), &s)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn places() -> Places {
        Places {
            downloads: vec![PathBuf::from("/home/p/Downloads")],
            home: Some(PathBuf::from("/home/p")),
            appdir: Some(PathBuf::from("/tmp/.mount_kiln-AbCd12")),
        }
    }
    fn dirs(list: &[&str]) -> impl Fn(&Path) -> bool + use<> {
        let set: HashSet<PathBuf> = list.iter().map(PathBuf::from).collect();
        move |p: &Path| set.contains(p)
    }

    #[test]
    fn the_last_used_folder_is_kept_while_it_exists() {
        let is_dir = dirs(&["/home/p/Mods", "/home/p/Downloads", "/home/p"]);
        let got = choose(None, Some(Path::new("/home/p/Mods")), &places(), &is_dir, &is_dir);
        assert_eq!(got, Some(PathBuf::from("/home/p/Mods")));
    }

    #[test]
    fn a_missing_last_used_folder_falls_back_to_downloads_then_home() {
        let is_dir = dirs(&["/home/p/Downloads", "/home/p"]);
        assert_eq!(choose(None, Some(Path::new("/home/p/Gone")), &places(), &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")));
        assert_eq!(choose(None, None, &places(), &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")));
        // no Downloads folder: home
        let only_home = dirs(&["/home/p"]);
        assert_eq!(choose(None, Some(Path::new("/home/p/Gone")), &places(), &only_home, &only_home), Some(PathBuf::from("/home/p")));
        // the second Downloads candidate (<home>/Downloads) when the system's is missing
        let p = Places { downloads: vec![PathBuf::from("/data/dl"), PathBuf::from("/home/p/Downloads")], ..places() };
        let is_dir = dirs(&["/home/p/Downloads", "/home/p"]);
        assert_eq!(choose(None, None, &p, &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")));
        assert_eq!(choose(None, None, &places(), dirs(&[]), dirs(&[])), None);
    }

    #[test]
    fn an_appimage_mount_is_never_chosen() {
        // the AppImage's working directory, its $APPDIR and any other mount: all exist, none is offered
        let mount = "/tmp/.mount_kiln-AbCd12/usr";
        let other = "/tmp/.mount_Kiln LXyZ99/usr/share";
        let is_dir = dirs(&[mount, other, "/tmp/.mount_kiln-AbCd12", "/home/p/Downloads", "/home/p"]);
        for last in [mount, other, "/tmp/.mount_kiln-AbCd12"] {
            assert_eq!(choose(None, Some(Path::new(last)), &places(), &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")), "{last}");
            assert_eq!(choose(Some(Path::new(last)), None, &places(), &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")), "{last}");
        }
        // $APPDIR somewhere else than /tmp (an extracted AppImage, --appimage-extract-and-run)
        let p = Places { appdir: Some(PathBuf::from("/opt/kiln/squashfs-root")), ..places() };
        let is_dir = dirs(&["/opt/kiln/squashfs-root/usr", "/home/p/Downloads"]);
        assert_eq!(choose(None, Some(Path::new("/opt/kiln/squashfs-root/usr")), &p, &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")));
        // a home or Downloads inside a mount is skipped too: no folder at all rather than the mount
        let p = Places { downloads: vec![PathBuf::from("/tmp/.mount_x/dl")], home: Some(PathBuf::from("/tmp/.mount_x")), appdir: None };
        let is_dir = dirs(&["/tmp/.mount_x/dl", "/tmp/.mount_x"]);
        assert_eq!(choose(None, None, &p, &is_dir, &is_dir), None);
        assert!(in_appimage(Path::new(mount), None));
        assert!(!in_appimage(Path::new("/tmp/mount_notes"), None));
    }

    #[test]
    fn a_relative_path_is_never_chosen() {
        // a relative path is the working directory's
        let is_dir = dirs(&["usr", ".", "/home/p/Downloads"]);
        assert_eq!(choose(Some(Path::new(".")), Some(Path::new("usr")), &places(), &is_dir, &is_dir), Some(PathBuf::from("/home/p/Downloads")));
    }

    #[test]
    fn a_preferred_path_wins_when_it_exists() {
        let is_dir = dirs(&["/home/p/Mods", "/home/p/Downloads"]);
        let exists = dirs(&["/home/p/Mods", "/home/p/Downloads", "/home/p/Mods/a.dll"]);
        assert_eq!(choose(Some(Path::new("/home/p/Mods/a.dll")), Some(Path::new("/home/p/Mods")), &places(), &is_dir, &exists), Some(PathBuf::from("/home/p/Mods/a.dll")));
        // a preferred path that is gone: the kind's last folder
        assert_eq!(choose(Some(Path::new("/gone/x")), Some(Path::new("/home/p/Mods")), &places(), &is_dir, &exists), Some(PathBuf::from("/home/p/Mods")));
    }

    #[test]
    fn the_kept_folder_is_the_files_parent_or_the_picked_folder() {
        let is_dir = dirs(&["/home/p/Reports", "/home/p/Packs", "/tmp/.mount_kiln-AbCd12/usr"]);
        assert_eq!(folder_to_keep(Path::new("/home/p/Reports/kiln-report.zip"), false, None, &is_dir), Some(PathBuf::from("/home/p/Reports")));
        assert_eq!(folder_to_keep(Path::new("/home/p/Packs"), true, None, &is_dir), Some(PathBuf::from("/home/p/Packs")));
        // a save into the mount (it failed) is not kept; nor a folder that does not exist, nor a relative path
        assert_eq!(folder_to_keep(Path::new("/tmp/.mount_kiln-AbCd12/usr/kiln-report.zip"), false, None, &is_dir), None);
        assert_eq!(folder_to_keep(Path::new("/home/p/Gone/x.zip"), false, None, &is_dir), None);
        assert_eq!(folder_to_keep(Path::new("x.zip"), false, None, &is_dir), None);
    }

    #[test]
    fn kinds_are_short_names() {
        assert!(valid_kind("supportReport") && valid_kind("add-mods_2"));
        assert!(!valid_kind("") && !valid_kind("../x") && !valid_kind(&"a".repeat(41)));
        assert_eq!(bare_name("kiln-report-2026-11-20.zip").unwrap(), "kiln-report-2026-11-20.zip");
        assert_eq!(bare_name("/etc/../x.zip").unwrap(), "x.zip");
    }
}
