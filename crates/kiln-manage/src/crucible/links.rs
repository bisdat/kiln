//! kiln:// links: `kiln://crucible/<CODE>` and `kiln://crucible/pack/<slug>` (the pack page's "Open in Kiln", the
//! share card). The system starts the launcher with `--link <url>`; that process puts the link into
//! `<data>/crucible/inbox/` and ends when a launcher already runs (it holds `<data>/crucible/instance.lock`), else it
//! starts with the link waiting. The running launcher takes the inbox every half second.
//!
//! Kiln is the only program for kiln:// links, so registering takes nothing from anyone: Linux
//! `$XDG_DATA_HOME/applications/kiln-launcher-links.desktop` (`MimeType=x-scheme-handler/kiln;`, `NoDisplay=true`) made the
//! default with xdg-mime; Windows `HKCU\Software\Classes\kiln` (`URL Protocol`, `"<launcher>" --link "%1"`).

use crate::paths::DataDirs;
use crate::{Error, Result};
use std::path::{Path, PathBuf};

pub const SCHEME: &str = "kiln";
pub const MIME: &str = "x-scheme-handler/kiln";
pub const DESKTOP_ID: &str = "kiln-launcher-links.desktop";
/// The argument the registration passes before the link.
pub const ARG: &str = "--link";

/// A link this launcher takes: kiln:// naming something [`super::parse_target`] reads, at most 300 characters.
pub fn accepted(url: &str) -> bool {
    url.len() <= 300
        && url.get(..7).is_some_and(|p| p.eq_ignore_ascii_case("kiln://"))
        && url.bytes().all(|b| (0x21..=0x7e).contains(&b))
        && super::parse_target(&format!("kiln://{}", &url[7..])).is_some()
}

fn inbox(dirs: &DataDirs) -> PathBuf {
    dirs.root.join("crucible").join("inbox")
}

/// Queues a link for the running launcher (or this one, once its window is up).
pub fn queue(dirs: &DataDirs, url: &str) -> Result<PathBuf> {
    if !accepted(url) {
        return Err(Error::invalid("not a Crucible link"));
    }
    let dir = inbox(dirs);
    std::fs::create_dir_all(&dir).map_err(crate::util::io_at(&dir, "creating"))?;
    let name = format!("{:013}-{}.link", crate::util::now_unix() * 1000, crate::hash::hex(&crate::util::random_bytes::<4>()?));
    let p = dir.join(name);
    crate::util::write_atomic(&p, url.as_bytes())?;
    Ok(p)
}

/// Takes every queued link, oldest first; anything that is not a fresh, small, valid link is dropped unread.
pub fn take(dirs: &DataDirs) -> Vec<String> {
    let dir = inbox(dirs);
    let mut files: Vec<PathBuf> = std::fs::read_dir(&dir).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "link")).collect();
    files.sort();
    let mut out = Vec::new();
    for p in files {
        let ok = std::fs::symlink_metadata(&p).is_ok_and(|m| {
            m.is_file() && m.len() <= 300 && m.modified().ok().and_then(|t| t.elapsed().ok()).is_some_and(|a| a.as_secs() < 3600)
        });
        let text = if ok { std::fs::read_to_string(&p).ok() } else { None };
        let _ = std::fs::remove_file(&p);
        if let Some(t) = text.map(|t| t.trim().to_string()).filter(|t| accepted(t)) {
            out.push(t);
        }
    }
    out
}

/// Held by the launcher that shows the window: a second process started for a link hands it over and ends.
pub struct Instance {
    _file: std::fs::File,
}

/// Some: this process is the launcher (keep it while the window lives). None: another one runs.
pub fn acquire_instance(dirs: &DataDirs) -> Result<Option<Instance>> {
    let dir = dirs.root.join("crucible");
    std::fs::create_dir_all(&dir).map_err(crate::util::io_at(&dir, "creating"))?;
    let p = dir.join("instance.lock");
    let f = std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(&p).map_err(crate::util::io_at(&p, "opening"))?;
    match f.try_lock() {
        Ok(()) => Ok(Some(Instance { _file: f })),
        Err(std::fs::TryLockError::WouldBlock) => Ok(None),
        Err(std::fs::TryLockError::Error(e)) => Err(Error::from(e)),
    }
}

/// The .desktop entry for kiln:// links (Linux).
pub fn desktop_entry(exe: &Path) -> Result<String> {
    if !exe.is_absolute() || exe.to_string_lossy().chars().any(char::is_control) {
        return Err(Error::invalid("launcher path must be an absolute path without control characters"));
    }
    Ok(format!(
        "[Desktop Entry]\nType=Application\nName=Kiln Launcher (Crucible links)\nComment=Open Crucible modpacks in the Kiln Launcher\n\
         Exec={} {ARG} %u\nIcon=kiln-launcher\nTerminal=false\nNoDisplay=true\nMimeType={MIME};\n",
        crate::nexus::handler::exec_quote(&exe.display().to_string())
    ))
}

/// Registers the launcher for kiln:// links (idempotent; called at start while Crucible is on).
#[cfg(target_os = "linux")]
pub fn register(exe: &Path) -> Result<bool> {
    let data = std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| crate::util::home().join(".local/share"));
    let file = data.join("applications").join(DESKTOP_ID);
    let entry = desktop_entry(exe)?;
    if std::fs::read_to_string(&file).is_ok_and(|t| t == entry) {
        return Ok(false);
    }
    if let Some(d) = file.parent() {
        std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
    }
    crate::util::write_atomic(&file, entry.as_bytes())?;
    if let Some(db) = crate::util::which("update-desktop-database") {
        let _ = std::process::Command::new(db).arg(file.parent().unwrap_or(&data)).output();
    }
    if let Some(x) = crate::util::which("xdg-mime") {
        let _ = std::process::Command::new(x).args(["default", DESKTOP_ID, MIME]).output();
    }
    Ok(true)
}

#[cfg(windows)]
pub fn register(exe: &Path) -> Result<bool> {
    use windows_sys::Win32::System::Registry::{HKEY, HKEY_CURRENT_USER, KEY_WRITE, REG_OPTION_NON_VOLATILE, REG_SZ, RegCloseKey, RegCreateKeyExW, RegSetValueExW};
    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }
    fn set(key: &str, name: Option<&str>, value: &str) -> Result<()> {
        let k = wide(key);
        let mut h: HKEY = std::ptr::null_mut();
        // SAFETY: NUL-terminated strings; h is closed below.
        unsafe {
            let rc = RegCreateKeyExW(HKEY_CURRENT_USER, k.as_ptr(), 0, std::ptr::null(), REG_OPTION_NON_VOLATILE, KEY_WRITE, std::ptr::null(), &mut h, std::ptr::null_mut());
            if rc != 0 {
                return Err(Error::new(crate::ErrorCode::Permission, format!("registry {key}: error {rc}")));
            }
            let n = name.map(wide);
            let v = wide(value);
            let rc = RegSetValueExW(h, n.as_ref().map_or(std::ptr::null(), |n| n.as_ptr()), 0, REG_SZ, v.as_ptr().cast(), (v.len() * 2) as u32);
            RegCloseKey(h);
            if rc != 0 {
                return Err(Error::new(crate::ErrorCode::Permission, format!("registry {key}: error {rc}")));
            }
        }
        Ok(())
    }
    let e = exe.display().to_string();
    if e.contains('"') || e.chars().any(char::is_control) {
        return Err(Error::invalid("launcher path must not contain quotes or control characters"));
    }
    let base = r"Software\Classes\kiln";
    set(base, None, "URL:Kiln Launcher")?;
    set(base, Some("URL Protocol"), "")?;
    set(&format!(r"{base}\DefaultIcon"), None, &format!("\"{e}\",0"))?;
    set(&format!(r"{base}\shell\open\command"), None, &format!("\"{e}\" {ARG} \"%1\""))?;
    Ok(true)
}

#[cfg(not(any(target_os = "linux", windows)))]
pub fn register(exe: &Path) -> Result<bool> {
    let _ = exe;
    Err(Error::new(crate::ErrorCode::Unsupported, "kiln:// links are registered on Linux and Windows"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_crucible_links_are_taken() {
        assert!(accepted("kiln://crucible/H7QK-3MZP"));
        assert!(accepted("KILN://crucible/pack/ashen-frontier"));
        for bad in ["nxm://enshrouded/mods/1", "kiln://crucible/", "kiln://crucible/H7QK-3MZP --selftest", "kiln://other/x", "https://frostdev.io/kiln/crucible/p/H7QK-3MZP"] {
            assert!(!accepted(bad), "{bad}");
        }
    }

    #[test]
    fn the_inbox_hands_links_over_once() {
        let root = std::env::temp_dir().join(format!("km-links-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.clone());
        queue(&dirs, "kiln://crucible/H7QK-3MZP").unwrap();
        assert!(queue(&dirs, "kiln://crucible/../../x").is_err());
        std::fs::write(inbox(&dirs).join("0000000000000-bad.link"), "nxm://enshrouded/mods/1/files/2").unwrap();
        assert_eq!(take(&dirs), ["kiln://crucible/H7QK-3MZP"]);
        assert!(take(&dirs).is_empty());
        let one = acquire_instance(&dirs).unwrap();
        assert!(one.is_some());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_desktop_entry_passes_the_link_as_one_argument() {
        let e = desktop_entry(Path::new("/opt/Kiln Launcher.AppImage")).unwrap();
        assert!(e.contains("MimeType=x-scheme-handler/kiln;"));
        assert!(e.contains("--link %u"));
        assert!(e.contains("NoDisplay=true"));
        assert!(desktop_entry(Path::new("relative")).is_err());
    }
}
