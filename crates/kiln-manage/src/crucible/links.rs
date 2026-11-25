//! kiln:// links: `kiln://crucible/<CODE>` and `kiln://crucible/pack/<slug>` (the pack page's "Open in Kiln", the
//! share card). The system starts the launcher with `--link <url>`. When a launcher already runs (it holds
//! `<data>/crucible/instance.lock`, [`find_instance`]) that process puts the link into `<data>/crucible/inbox/` and ends;
//! else it starts with the link held in memory until its window is up (the launcher's commands/crucible.rs). The running
//! launcher takes the inbox every half second.
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

/// The Windows installer's note beside the launcher: the setup file's own name, which carries the pack's code when the
/// download came from a pack page's "Get Kiln" (`...-setup.crucible-H7QK3MZP.exe`). The launcher reads it once and
/// removes it.
pub const RESUME_FILE: &str = "crucible-resume.txt";

/// The share code a downloaded file's name carries after "crucible" (`Kiln Launcher_0.2.0_x64-setup.crucible-H7QK3MZP.exe`,
/// `Kiln_Launcher crucible-H7QK-3MZP (1).AppImage`, `?crucible=H7QK3MZP`).
pub fn code_in_name(name: &str) -> Option<String> {
    let lower = name.to_ascii_lowercase();
    let mut from = 0;
    while let Some(i) = lower[from..].find("crucible") {
        let rest = name[from + i + 8..].trim_start_matches(['-', '_', '=', ' ', '.', '[', '(']);
        let mut s = String::new();
        for c in rest.chars() {
            if c.is_ascii_alphanumeric() {
                s.push(c);
                if s.len() == 8 {
                    break;
                }
            } else if !(c == '-' && s.len() == 4) {
                break;
            }
        }
        if let Some(code) = super::parse_code(&s) {
            return Some(code);
        }
        from += i + 8;
    }
    None
}

/// What the launcher's start learns for "come back after installing Kiln" (DESIGN 1.3).
#[derive(Debug, Clone, Default, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Resume {
    /// The first start of a Crucible launcher with this data folder (`<data>/crucible/started` was missing). Also true
    /// at the first start after an update from a launcher without Crucible: it is not "a fresh install" and never a
    /// reason to look at the clipboard on its own.
    pub first_start: bool,
    /// A pack code the download carried (the installer's note, or the AppImage's own name at the first start).
    pub code: Option<String>,
    /// The download came from a pack page's Get Kiln (its name says `crucible`) but carries no readable code, at the
    /// first start: the only case the UI reads the clipboard at start (the pack page copied the pack's link there). An
    /// update, an ordinary download, a later start: false.
    #[serde(default)]
    pub pack_waiting: bool,
}

/// The download page's mark in a launcher file's name (`...setup.crucible-<CODE>.exe`): a pack page's Get Kiln.
fn names_crucible(name: &str) -> bool {
    name.to_ascii_lowercase().contains("crucible")
}

/// Once per start, asked by the UI: the first start (`<data>/crucible/started` is made), and the code the installer's
/// note or the downloaded AppImage's name carries. The note is removed once read.
pub fn resume(dirs: &DataDirs, exe: Option<&Path>) -> Resume {
    let dir = dirs.root.join("crucible");
    let marker = dir.join("started");
    let first_start = !marker.exists();
    if first_start {
        let _ = std::fs::create_dir_all(&dir).and_then(|_| std::fs::write(&marker, crate::util::now_rfc3339()));
    }
    let mut code = None;
    let mut named = false;
    if let Some(exe) = exe {
        if let Some(note) = exe.parent().map(|p| p.join(RESUME_FILE)) {
            if let Ok(text) = std::fs::read_to_string(&note) {
                let name = text.trim();
                let file = name.rsplit(['/', '\\']).next().unwrap_or(name);
                code = code_in_name(file);
                named |= names_crucible(file);
                let _ = std::fs::remove_file(&note);
            }
        }
        if code.is_none() && first_start {
            let own = exe.file_name().and_then(|n| n.to_str()).unwrap_or("");
            code = code_in_name(own);
            named |= names_crucible(own);
        }
    }
    let pack_waiting = first_start && code.is_none() && named;
    Resume { first_start, code, pack_waiting }
}

/// Held by the launcher that shows the window: a second process started for a link hands it over and ends.
pub struct Instance {
    _file: std::fs::File,
}

fn instance_lock(dirs: &DataDirs) -> PathBuf {
    dirs.root.join("crucible").join("instance.lock")
}

/// Some: this process is the launcher (keep it while the window lives). None: another one runs.
pub fn acquire_instance(dirs: &DataDirs) -> Result<Option<Instance>> {
    let dir = dirs.root.join("crucible");
    std::fs::create_dir_all(&dir).map_err(crate::util::io_at(&dir, "creating"))?;
    let p = instance_lock(dirs);
    let f = std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(&p).map_err(crate::util::io_at(&p, "opening"))?;
    match f.try_lock() {
        Ok(()) => Ok(Some(Instance { _file: f })),
        Err(std::fs::TryLockError::WouldBlock) => Ok(None),
        Err(std::fs::TryLockError::Error(e)) => Err(Error::from(e)),
    }
}

/// What a process started for a link finds ([`find_instance`]).
pub enum Found {
    /// No launcher holds the instance lock. Some: this process holds it now (keep it while the window lives); None:
    /// there is no lock file yet (no launcher with Crucible took it in this data folder), nothing was created.
    Free(Option<Instance>),
    /// A launcher holds it: it takes links from the inbox.
    Running,
}

/// Whether a launcher runs with this data folder, without writing anything into it (a process started for a link,
/// before the data folder's gates open): the lock file is opened read-only and never created.
pub fn find_instance(dirs: &DataDirs) -> Result<Found> {
    let p = instance_lock(dirs);
    let f = match std::fs::symlink_metadata(&p) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Found::Free(None)),
        Err(e) => return Err(crate::util::io_at(&p, "checking")(e)),
        Ok(m) if !m.is_file() => return Err(Error::invalid(format!("{} is not a file", p.display()))),
        Ok(_) => std::fs::File::open(&p).map_err(crate::util::io_at(&p, "opening"))?,
    };
    match f.try_lock() {
        Ok(()) => Ok(Found::Free(Some(Instance { _file: f }))),
        Err(std::fs::TryLockError::WouldBlock) => Ok(Found::Running),
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
    fn a_process_for_a_link_finds_the_running_launcher_without_writing() {
        let root = std::env::temp_dir().join(format!("km-find-instance-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let dirs = DataDirs::at(root.clone());
        // no launcher with Crucible took the lock here yet: nothing to find, nothing created
        assert!(matches!(find_instance(&dirs).unwrap(), Found::Free(None)));
        assert!(!root.join("crucible").exists());
        // a launcher runs
        let running = acquire_instance(&dirs).unwrap().expect("the first launcher");
        assert!(matches!(find_instance(&dirs).unwrap(), Found::Running));
        drop(running);
        // it ended: the lock is this process's now (read-only), and a later launcher sees it taken
        let Found::Free(Some(mine)) = find_instance(&dirs).unwrap() else { panic!("the lock was free") };
        assert!(acquire_instance(&dirs).unwrap().is_none());
        assert!(matches!(find_instance(&dirs).unwrap(), Found::Running));
        drop(mine);
        assert_eq!(std::fs::metadata(instance_lock(&dirs)).unwrap().len(), 0, "the lock file was never written");
        // a lock that is not a plain file is no answer
        let _ = std::fs::remove_file(instance_lock(&dirs));
        std::fs::create_dir_all(instance_lock(&dirs)).unwrap();
        assert!(find_instance(&dirs).is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_download_name_carries_the_code() {
        for (name, want) in [
            ("Kiln Launcher_0.2.0_x64-setup.crucible-H7QK3MZP.exe", Some("H7QK3MZP")),
            ("Kiln_Launcher crucible-h7qk-3mzp (1).AppImage", Some("H7QK3MZP")),
            ("kiln-launcher?crucible=H7QK3MZP", Some("H7QK3MZP")),
            ("Kiln Launcher_0.2.0_x64-setup [crucible H7QK-3MZP].exe", Some("H7QK3MZP")),
            ("Kiln Launcher_0.2.0_x64-setup.exe", None),
            ("crucible-H0QK3MZP.exe", None),
            ("crucible-H7QK3MZ.exe", None),
            ("crucible.exe crucible-H7QK3MZP", Some("H7QK3MZP")),
        ] {
            assert_eq!(code_in_name(name).as_deref(), want, "{name}");
        }
    }

    #[test]
    fn resume_reads_the_installer_note_once_and_the_first_start() {
        let root = std::env::temp_dir().join(format!("km-resume-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dirs = DataDirs::at(root.join("data"));
        let app = root.join("app");
        std::fs::create_dir_all(&app).unwrap();
        let exe = app.join("kiln-launcher.exe");
        std::fs::write(app.join(RESUME_FILE), "C:\\Users\\a\\Downloads\\Kiln Launcher_0.2.0_x64-setup.crucible-H7QK3MZP.exe\r\n").unwrap();
        assert_eq!(resume(&dirs, Some(&exe)), Resume { first_start: true, code: Some("H7QK3MZP".into()), pack_waiting: false });
        assert!(!app.join(RESUME_FILE).exists(), "read once");
        assert_eq!(resume(&dirs, Some(&exe)), Resume { first_start: false, code: None, pack_waiting: false });
        // an AppImage named by the download: only at the first start
        let dirs2 = DataDirs::at(root.join("data2"));
        let img = app.join("Kiln_Launcher crucible-H7QK-3MZP.AppImage");
        assert_eq!(resume(&dirs2, Some(&img)).code.as_deref(), Some("H7QK3MZP"));
        assert_eq!(resume(&dirs2, Some(&img)).code, None);
        // an installer note without a code (an ordinary download, the updater) is removed and gives none
        std::fs::write(app.join(RESUME_FILE), "Kiln Launcher_0.2.0_x64-setup.exe").unwrap();
        assert_eq!(resume(&dirs, Some(&exe)).code, None);
        assert!(!app.join(RESUME_FILE).exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn only_a_get_kiln_download_without_its_code_leaves_a_pack_waiting() {
        let root = std::env::temp_dir().join(format!("km-waiting-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let app = root.join("app");
        std::fs::create_dir_all(&app).unwrap();
        let exe = app.join("kiln-launcher.exe");
        // the first start after an update (from a launcher without Crucible): the updater's note names no pack
        let updated = DataDirs::at(root.join("updated"));
        std::fs::write(app.join(RESUME_FILE), "Kiln Launcher_0.2.0-dev.30_x64-setup.exe").unwrap();
        assert_eq!(resume(&updated, Some(&exe)), Resume { first_start: true, code: None, pack_waiting: false });
        // a fresh install from a pack page whose code the name lost: the pack page's clipboard is the only handoff left
        let fresh = DataDirs::at(root.join("fresh"));
        std::fs::write(app.join(RESUME_FILE), "C:\\Downloads\\Kiln Launcher_0.2.0_x64-setup.crucible-H7QK.exe").unwrap();
        assert_eq!(resume(&fresh, Some(&exe)), Resume { first_start: true, code: None, pack_waiting: true });
        // its next start: nothing waits any more
        assert_eq!(resume(&fresh, Some(&exe)), Resume { first_start: false, code: None, pack_waiting: false });
        // a Get Kiln name at a later start (a reinstall over a used data folder) without its code: not the first start
        std::fs::write(app.join(RESUME_FILE), "Kiln Launcher_0.2.0_x64-setup.crucible.exe").unwrap();
        assert!(!resume(&fresh, Some(&exe)).pack_waiting);
        // the AppImage's own name, at the first start only
        let img = app.join("Kiln_Launcher crucible-H7Q.AppImage");
        let linux = DataDirs::at(root.join("linux"));
        assert!(resume(&linux, Some(&img)).pack_waiting);
        assert!(!resume(&linux, Some(&img)).pack_waiting);
        // a plain AppImage at the first start: nothing waits
        assert!(!resume(&DataDirs::at(root.join("plain")), Some(&app.join("Kiln_Launcher.AppImage"))).pack_waiting);
        let _ = std::fs::remove_dir_all(&root);
    }

    // The desktop entry is registered only on Linux; Windows registers its own registry command.
    #[cfg(unix)]
    #[test]
    fn the_desktop_entry_passes_the_link_as_one_argument() {
        let e = desktop_entry(Path::new("/opt/Kiln Launcher.AppImage")).unwrap();
        assert!(e.contains("MimeType=x-scheme-handler/kiln;"));
        assert!(e.contains("--link %u"));
        assert!(e.contains("NoDisplay=true"));
        assert!(desktop_entry(Path::new("relative")).is_err());
    }
}
