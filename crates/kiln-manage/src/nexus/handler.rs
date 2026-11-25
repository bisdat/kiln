//! The launcher as the `nxm://` handler ("Mod Manager Download" on Nexus Mods), for Enshrouded links only. Opt-in from
//! Settings, never at install or start:
//! - **Linux:** `$XDG_DATA_HOME/applications/kiln-launcher-nxm.desktop` (`MimeType=x-scheme-handler/nxm;`,
//!   `NoDisplay=true`, `Exec="<launcher>" --nxm %u`; the AppImage's own path when run as one), made the default by editing
//!   only the nxm default in the highest-priority user mimeapps.list (queried with xdg-mime).
//! - **Windows:** `HKCU\Software\Classes\nxm` (`URL Protocol`, `shell\open\command` = `"<launcher>" --nxm "%1"`).
//!
//! The handler before Kiln (Vortex, Mod Organizer 2, another manager) is remembered in `<data>/nexus/handler.json`:
//! links for other games go to it unchanged, and turning the option off gives it back. Linux restores the exact
//! prior nxm default list (legacy state knows only the handler id); Windows restores every overwritten registry value,
//! type and absence, removing only empty keys Kiln created. Kiln touches only its own `.desktop` file, the user's
//! default for `x-scheme-handler/nxm`, and its own per-user registry key.
//!
//! A second launcher process started for a link hands it to the running one through `<data>/nexus/inbox/` and exits
//! ([`acquire_instance`], [`queue_link`], [`take_links`]).
//!
//! A build with Nexus off takes no links and registers nothing, but it gives back a registration an older build made
//! ([`retire_leftover`], [`forward_to_previous`]): the platform calls that undo Kiln's own registration (`is_ours`,
//! `restore`, `restore_snapshot`, `forward`) are not behind the Nexus switch; every call that takes the links over
//! (`install`, `current`, `snapshot`, [`register`]) is.

use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const DESKTOP_ID: &str = "kiln-launcher-nxm.desktop";
pub const MIME: &str = "x-scheme-handler/nxm";

/// The handler Kiln replaced.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", tag = "type")]
pub enum Previous {
    /// Linux: a desktop file id ("com.nexusmods.app.desktop", "modorganizer2-nxm-handler.desktop").
    Desktop { id: String },
    /// Windows: the `shell\open\command` it had, its `DefaultIcon`, and where it lived ("hkcu" or "hklm").
    Command { command: String, icon: Option<String>, scope: String },
}

impl Previous {
    /// A name for Settings: "Vortex", "modorganizer2-nxm-handler".
    pub fn label(&self) -> String {
        let raw = match self {
            Previous::Desktop { id } => id.trim_end_matches(".desktop").rsplit('.').next().unwrap_or(id).to_string(),
            Previous::Command { command, .. } => {
                let prog = command.trim().trim_start_matches('"');
                let prog = prog.split('"').next().unwrap_or(prog);
                let base = prog.rsplit(['\\', '/']).next().unwrap_or(prog);
                base.split(' ').next().unwrap_or(base).trim_end_matches(".exe").trim_end_matches(".EXE").to_string()
            }
        };
        if raw.to_lowercase().contains("vortex") { "Vortex".into() } else if raw.to_lowercase().contains("modorganizer") || raw.eq_ignore_ascii_case("nxmhandler") { "Mod Organizer 2".into() } else { raw }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HandlerState {
    pub format: u32,
    pub registered: bool,
    #[serde(default)]
    pub previous: Option<Previous>,
    #[serde(default)]
    pub exe: Option<String>,
    #[serde(default)]
    pub registered_at: Option<String>,
    #[serde(default)]
    pub snapshot: Option<serde_json::Value>,
}

/// What Settings shows.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HandlerStatus {
    /// This platform can register a handler.
    pub supported: bool,
    /// The option is on (Kiln registered itself).
    pub registered: bool,
    /// The system's nxm handler is Kiln right now (another manager may have taken it back since).
    pub active: bool,
    /// The handler links for other games go to.
    pub previous: Option<String>,
}

/// The system side (tests use a fake).
pub trait HandlerPlatform {
    fn supported(&self) -> bool;
    /// The current nxm handler, unless it is Kiln's own.
    fn current(&self) -> Result<Option<Previous>>;
    /// Not behind the Nexus switch: a build with Nexus off still gives back the registration an older build made
    /// ([`retire_leftover`]). Everything that takes the links over (`install`, `current`, `snapshot`) stays behind it.
    fn is_ours(&self) -> bool;
    fn matches_registered(&self, _: Option<&str>) -> bool { self.is_ours() }
    /// The system's nxm handler is a Kiln registration, wherever its launcher has moved to since (the leftover check).
    fn is_kiln_registration(&self) -> bool { self.is_ours() }
    fn install(&self, exe: &Path) -> Result<()>;
    /// Gives the handler back (None: there was none before Kiln).
    fn restore(&self, previous: Option<&Previous>) -> Result<()>;
    fn forward(&self, previous: &Previous, url: &str) -> Result<()>;
    /// Snapshot only the nxm association before changing it; persisted before install for crash recovery.
    fn snapshot(&self) -> Result<Option<serde_json::Value>> {
        crate::nexus::require_enabled()?; Ok(None) }
    fn restore_snapshot(&self, previous: Option<&Previous>, _: Option<&serde_json::Value>) -> Result<()> {
        self.restore(previous) }

}

fn state_path(root: &Path) -> PathBuf {
    root.join("handler.json")
}

pub fn state(root: &Path) -> HandlerState {
    if !crate::nexus::enabled() { return HandlerState::default() }
    read_state(root).unwrap_or_default()
}

fn read_state(root: &Path) -> Result<HandlerState> {
    let state = crate::util::read_json::<HandlerState>(&state_path(root))?.unwrap_or_default();
    if state.format > 2 { return Err(Error::new(ErrorCode::Unsupported, "This nxm registration belongs to a newer launcher.")); }
    Ok(state)
}

pub fn status(root: &Path, p: &dyn HandlerPlatform) -> HandlerStatus {
    if !crate::nexus::enabled() { return HandlerStatus { supported: false, registered: false, active: false, previous: None } }
    let s = state(root);
    HandlerStatus { supported: p.supported(), registered: s.registered, active: s.registered && p.matches_registered(s.exe.as_deref()), previous: s.previous.as_ref().map(Previous::label) }
}

/// Turns the option on: remembers the current handler (unless it is Kiln's), then makes Kiln the handler. Again while
/// on: refreshes the launcher's path (an AppImage that moved) and keeps the remembered handler.
pub fn register(root: &Path, p: &dyn HandlerPlatform, exe: &Path) -> Result<HandlerStatus> {
        crate::nexus::require_enabled()?;
    if !p.supported() {
        return Err(Error::new(ErrorCode::Unsupported, "Kiln can't take Nexus Mods links on this system."));
    }
    let _lock = lock_state(root, "handler.lock")?;
    let mut s = read_state(root)?;
    // A manager selected since our last registration becomes the new handler to restore.
    if !s.registered || !p.matches_registered(s.exe.as_deref()) {
        s.previous = p.current()?;
        s.snapshot = p.snapshot()?;
    }
    s.format = 2;
    s.registered = true;
    s.exe = Some(exe.display().to_string());
    s.registered_at = Some(crate::util::now_rfc3339());
    super::keystore::write_private(&state_path(root), &s)?;
    if let Err(e) = p.install(exe) {
        let _ = p.restore_snapshot(s.previous.as_ref(), s.snapshot.as_ref());
        return Err(e);
    }
    Ok(status(root, p))
}

/// Turns the option off: the handler before Kiln comes back.
pub fn unregister(root: &Path, p: &dyn HandlerPlatform) -> Result<HandlerStatus> {
        crate::nexus::require_enabled()?;
    let _lock = lock_state(root, "handler.lock")?;
    let s = read_state(root)?;
    if p.matches_registered(s.exe.as_deref()) {
        p.restore_snapshot(s.previous.as_ref(), s.snapshot.as_ref())?;
    }
    super::keystore::write_private(&state_path(root), &HandlerState { format: 2, ..Default::default() })?;
    Ok(status(root, p))
}

/// A link for another game: to the handler before Kiln.
pub fn forward(root: &Path, p: &dyn HandlerPlatform, url: &str) -> Result<String> {
        crate::nexus::require_enabled()?;
    if super::nxm::game_of(url).is_none() { return Err(Error::invalid("invalid nxm link")); }
    let s = read_state(root)?;
    let prev = s.previous.ok_or_else(|| Error::not_found("No other mod manager was set up for Nexus Mods links before Kiln."))?;
    p.forward(&prev, url)?;
    Ok(prev.label())
}

/// The platform's handler side.
pub fn platform() -> Box<dyn HandlerPlatform> {
    #[cfg(windows)]
    return Box::new(win::WinRegistry::default());
    #[cfg(all(unix, not(target_os = "macos")))]
    return Box::new(LinuxMime::from_env());
    #[cfg(target_os = "macos")]
    return Box::new(Unsupported);
}

#[allow(dead_code)]
struct Unsupported;
impl HandlerPlatform for Unsupported {
    fn supported(&self) -> bool {
        if !crate::nexus::enabled() { return false }
        false
    }
    fn current(&self) -> Result<Option<Previous>> {
        crate::nexus::require_enabled()?;
        Ok(None)
    }
    fn is_ours(&self) -> bool {
        if !crate::nexus::enabled() { return false }
        false
    }
    fn install(&self, _: &Path) -> Result<()> {
        crate::nexus::require_enabled()?;
        Err(Error::new(ErrorCode::Unsupported, "not on this system"))
    }
    fn restore(&self, _: Option<&Previous>) -> Result<()> {
        crate::nexus::require_enabled()?;
        Ok(())
    }
    fn forward(&self, _: &Previous, _: &str) -> Result<()> {
        crate::nexus::require_enabled()?;
        Err(Error::new(ErrorCode::Unsupported, "not on this system"))
    }
}

// ---------------------------------------------------------------- Linux

/// `xdg-mime` and the user's applications folder. `env` goes to every xdg-mime call (tests: temporary XDG dirs).
#[derive(Debug, Clone)]
pub struct LinuxMime {
    pub data_home: PathBuf,
    pub config_home: PathBuf,
    pub data_dirs: Vec<PathBuf>,
    pub env: Vec<(String, String)>,
}

impl LinuxMime {
    pub fn from_env() -> LinuxMime {
        let home = crate::util::home();
        let var = |k: &str| std::env::var_os(k).filter(|v| !v.is_empty()).map(PathBuf::from);
        let data_home = var("XDG_DATA_HOME").unwrap_or_else(|| home.join(".local/share"));
        let config_home = var("XDG_CONFIG_HOME").unwrap_or_else(|| home.join(".config"));
        let mut data_dirs: Vec<PathBuf> = std::env::var("XDG_DATA_DIRS").ok().filter(|v| !v.is_empty()).unwrap_or_else(|| "/usr/local/share:/usr/share".into())
            .split(':').filter(|s| !s.is_empty()).map(PathBuf::from).collect();
        data_dirs.push(data_home.join("flatpak/exports/share"));
        data_dirs.push(PathBuf::from("/var/lib/flatpak/exports/share"));
        LinuxMime { data_home, config_home, data_dirs, env: vec![] }
    }

    pub fn desktop_file(&self) -> PathBuf {
        self.data_home.join("applications").join(DESKTOP_ID)
    }

    fn xdg_mime(&self, args: &[&str]) -> Result<String> {
        let exe = crate::util::which("xdg-mime").ok_or_else(|| Error::new(ErrorCode::Unsupported, "xdg-mime is not installed (package xdg-utils)."))?;
        let mut c = crate::hostenv::command(exe);
        c.args(args).envs(self.env.iter().map(|(k, v)| (k.as_str(), v.as_str()))).stdin(std::process::Stdio::null()).stderr(std::process::Stdio::null());
        let out = c.output().map_err(|e| Error::new(ErrorCode::Unsupported, format!("xdg-mime: {e}")))?;
        if !out.status.success() {
            return Err(Error::new(ErrorCode::Io, format!("xdg-mime {} failed", args.first().unwrap_or(&""))));
        }
        Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
    }

    fn refresh_db(&self) {
        if let Some(u) = crate::util::which("update-desktop-database") {
            let _ = crate::hostenv::command(u).arg(self.data_home.join("applications")).envs(self.env.iter().map(|(k, v)| (k.as_str(), v.as_str())))
                .stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).status();
        }
    }

    /// Where a desktop id's file is (the user's folder first).
    pub fn find_desktop(&self, id: &str) -> Option<PathBuf> {
        if id.contains('/') || id.contains("..") {
            return None;
        }
        std::iter::once(&self.data_home).chain(self.data_dirs.iter()).map(|d| d.join("applications").join(id)).find(|p| p.is_file())
    }

    fn mime_path(&self) -> PathBuf {
        let desktops = self.env.iter().find(|(k, _)| k == "XDG_CURRENT_DESKTOP").map(|(_, v)| v.clone()).unwrap_or_else(|| std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default());
        for desktop in desktops.split(':').filter(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_'))) {
            let path = self.config_home.join(format!("{}-mimeapps.list", desktop.to_ascii_lowercase()));
            if path.is_file() { return path; }
        }
        self.config_home.join("mimeapps.list")
    }

    fn read_mime(&self) -> Result<Option<String>> {
        match std::fs::read_to_string(self.mime_path()) {
            Ok(s) => Ok(Some(s)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(crate::util::io_at(&self.mime_path(), "reading")(e)),
        }
    }

    /// Edits only the nxm default, leaving every other MIME key byte-for-byte unchanged.
    fn set_default(&self, id: Option<&str>) -> Result<()> {
        let text = self.read_mime()?.unwrap_or_default();
        crate::util::write_atomic(&self.mime_path(), mime_default(&text, id).as_bytes())
    }

}

/// Replace/remove only nxm in [Default Applications]; never alter associations for other schemes.
fn mime_default(text: &str, id: Option<&str>) -> String {
    let mut out = String::new();
    let mut in_defaults = false;
    let mut wrote = false;
    let mut saw_defaults = false;
    for line in text.split_inclusive('\n') {
        let trimmed = line.trim();
        if trimmed.starts_with('[') {
            if in_defaults && !wrote && let Some(id) = id {
                if !out.is_empty() && !out.ends_with('\n') { out.push('\n'); }
                out.push_str(&format!("{MIME}={id}\n")); wrote = true;
            }
            in_defaults = trimmed == "[Default Applications]";
            saw_defaults |= in_defaults;
        }
        if in_defaults && trimmed.split_once('=').is_some_and(|(key, _)| key.trim() == MIME) {
            if !wrote && let Some(id) = id { out.push_str(&format!("{MIME}={id}\n")); }
            wrote = true;
        } else { out.push_str(line); }
    }
    if !wrote && let Some(id) = id {
        if !out.is_empty() && !out.ends_with('\n') { out.push('\n'); }
        if !saw_defaults { out.push_str("[Default Applications]\n"); }
        out.push_str(&format!("{MIME}={id}\n"));
    }
    out
}

/// The nxm default in a mimeapps.list's `[Default Applications]` (its first id).
fn mime_default_id(text: &str) -> Option<String> {
    let mut defaults = false;
    text.lines().find_map(|line| {
        let t = line.trim();
        if t.starts_with('[') { defaults = t == "[Default Applications]"; return None; }
        if !defaults { return None; }
        let (key, value) = t.split_once('=')?;
        (key.trim() == MIME).then(|| value.split(';').next().unwrap_or("").trim().to_string())
    })
}

/// A desktop entry's `Exec` value for a path (quoted per the Desktop Entry spec).
pub fn exec_quote(path: &str) -> String {
    let mut s = String::from("\"");
    for c in path.chars() {
        match c {
            '%' => s.push_str("%%"),
            '\\' => s.push_str("\\\\\\\\"),
            '"' | '`' | '$' => { s.push_str("\\\\"); s.push(c); }
            '\n' => s.push_str("\\n"),
            '\r' => s.push_str("\\r"),
            _ => s.push(c),
        }
    }
    s.push('"');
    s
}

/// Splits a desktop entry's `Exec` and fills its field codes with `url` (`%u %U %f %F`; `%i %c %k` dropped).
pub fn exec_argv(exec: &str, url: &str) -> Vec<String> {
    let mut args: Vec<String> = Vec::new();
    let mut cur = String::new();
    let mut quoted = false;
    let mut any = false;
    let mut chars = exec.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                quoted = !quoted;
                any = true;
            }
            '\\' if quoted => {
                if let Some(n) = chars.next() {
                    cur.push(n);
                }
            }
            ' ' | '\t' if !quoted => {
                if any || !cur.is_empty() {
                    args.push(std::mem::take(&mut cur));
                    any = false;
                }
            }
            _ => cur.push(c),
        }
    }
    if any || !cur.is_empty() {
        args.push(cur);
    }
    let mut out = Vec::new();
    let mut used = false;
    for a in args {
        match a.as_str() {
            "%u" | "%U" | "%f" | "%F" => {
                out.push(url.to_string());
                used = true;
            }
            "%i" | "%c" | "%k" => {}
            _ => out.push(a.replace("%%", "%")),
        }
    }
    if !used {
        out.push(url.to_string());
    }
    out
}

fn desktop_exec(file: &Path) -> Option<String> {
    let text = std::fs::read_to_string(file).ok()?;
    let mut in_entry = false;
    for line in text.lines() {
        let l = line.trim();
        if l.starts_with('[') {
            in_entry = l == "[Desktop Entry]";
            continue;
        }
        if in_entry && let Some(v) = l.strip_prefix("Exec=") {
            return Some(v.to_string());
        }
    }
    None
}

impl HandlerPlatform for LinuxMime {
    fn supported(&self) -> bool {
        if !crate::nexus::enabled() { return false }
        crate::util::which("xdg-mime").is_some()
    }

    fn current(&self) -> Result<Option<Previous>> {
        crate::nexus::require_enabled()?;
        let id = self.xdg_mime(&["query", "default", MIME])?;
        let id = id.lines().next().unwrap_or("").trim().to_string();
        Ok((!id.is_empty() && id != DESKTOP_ID).then_some(Previous::Desktop { id }))
    }

    fn is_ours(&self) -> bool {
        self.xdg_mime(&["query", "default", MIME]).is_ok_and(|id| id.trim() == DESKTOP_ID) && self.desktop_file().is_file()
    }

    fn is_kiln_registration(&self) -> bool {
        // Kiln's id is the default even if its .desktop file was deleted by hand since. Without a working xdg-mime the
        // user's mimeapps.list decides (never "not ours" just because the query failed: the record would be dropped).
        match self.xdg_mime(&["query", "default", MIME]) {
            Ok(id) => id.trim() == DESKTOP_ID,
            Err(_) => self.read_mime().ok().flatten().is_some_and(|text| mime_default_id(&text).as_deref() == Some(DESKTOP_ID)),
        }
    }

    fn install(&self, exe: &Path) -> Result<()> {
        crate::nexus::require_enabled()?;
        if !exe.is_absolute() || exe.to_string_lossy().chars().any(char::is_control) { return Err(Error::invalid("launcher path must be an absolute path without control characters")); }
        let entry = format!(
            "[Desktop Entry]\nType=Application\nName=Kiln Launcher (Nexus Mods links)\nComment=Mod Manager Download links for Enshrouded\n\
             Exec={} --nxm %u\nIcon=kiln-launcher\nTerminal=false\nNoDisplay=true\nMimeType={MIME};\n",
            exec_quote(&exe.display().to_string())
        );
        crate::util::write_atomic(&self.desktop_file(), entry.as_bytes())?;
        self.refresh_db();
        self.set_default(Some(DESKTOP_ID))?;
        Ok(())
    }

    fn restore(&self, previous: Option<&Previous>) -> Result<()> {
        match previous {
            Some(Previous::Desktop { id }) => {
                self.set_default(Some(id))?;
            }
            _ => self.set_default(None)?,
        }
        let _ = std::fs::remove_file(self.desktop_file());
        self.refresh_db();
        Ok(())
    }

    fn snapshot(&self) -> Result<Option<serde_json::Value>> {
        crate::nexus::require_enabled()?;
        let mime = self.read_mime()?;
        let desktop = match std::fs::read(self.desktop_file()) {
            Ok(bytes) => Some(bytes),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(crate::util::io_at(&self.desktop_file(), "reading")(e)),
        };
        Ok(Some(serde_json::json!({"mime": mime, "desktop": desktop, "path": self.mime_path()})))
    }

    fn restore_snapshot(&self, previous: Option<&Previous>, snapshot: Option<&serde_json::Value>) -> Result<()> {
        let Some(snapshot) = snapshot else { return self.restore(previous) };
        let old: Option<String> = serde_json::from_value(snapshot["mime"].clone())?;
        let path: PathBuf = snapshot.get("path").cloned().map(serde_json::from_value).transpose()?.unwrap_or_else(|| self.mime_path());
        let file = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if path.parent() != Some(self.config_home.as_path()) || !(file == "mimeapps.list" || file.ends_with("-mimeapps.list")) {
            return Err(Error::invalid("The saved nxm association is outside the current user configuration folder."));
        }
        let current = match std::fs::read_to_string(&path) { Ok(s) => s, Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(), Err(e) => return Err(crate::util::io_at(&path, "reading")(e)) };
        if mime_default(&current, None) == mime_default(&mime_default(old.as_deref().unwrap_or(""), Some(DESKTOP_ID)), None) {
            if let Some(old) = old { crate::util::write_atomic(&path, old.as_bytes())?; }
            else { let _ = std::fs::remove_file(&path); }
        } else {
            // Another app changed unrelated entries. Merge just the original nxm line back.
            let mut defaults = false;
            let id = old.as_deref().unwrap_or("").lines().find_map(|line| {
                let t = line.trim();
                if t.starts_with('[') { defaults = t == "[Default Applications]"; }
                if defaults { t.strip_prefix(&format!("{MIME}=")).map(str::to_string) } else { None }
            });
            crate::util::write_atomic(&path, mime_default(&current, id.as_deref()).as_bytes())?;
        }
        let desktop: Option<Vec<u8>> = serde_json::from_value(snapshot["desktop"].clone())?;
        if let Some(bytes) = desktop { crate::util::write_atomic(&self.desktop_file(), &bytes)?; }
        else { let _ = std::fs::remove_file(self.desktop_file()); }
        self.refresh_db();
        Ok(())
    }

    fn forward(&self, previous: &Previous, url: &str) -> Result<()> {
        if super::nxm::game_of(url).is_none() { return Err(Error::invalid("invalid nxm link")); }
        let Previous::Desktop { id } = previous else { return Err(Error::new(ErrorCode::Unsupported, "not a desktop entry")) };
        let file = self.find_desktop(id).ok_or_else(|| Error::not_found(format!("{id} is gone")))?;
        let exec = desktop_exec(&file).ok_or_else(|| Error::new(ErrorCode::Parse, format!("{id} has no Exec line")))?;
        let argv = exec_argv(&exec, url);
        let (prog, args) = argv.split_first().ok_or_else(|| Error::new(ErrorCode::Parse, format!("{id}: empty Exec")))?;
        crate::hostenv::command(prog).args(args).stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null())
            .spawn().map(|_| ()).map_err(|e| Error::new(ErrorCode::Unsupported, format!("starting {id}: {e}")))
    }
}

// ---------------------------------------------------------------- Windows

/// Fills a Windows command line's `%1` with the link (appended when there is none).
pub fn fill_command(command: &str, url: &str) -> String {
    // Always quote the replacement, even when the previous handler forgot to quote %1.
    if command.contains("%1") { command.replace("\"%1\"", "%1").replace("%1", &format!("\"{url}\"")) }
    else { format!("{command} \"{url}\"") }
}

/// The program of a Windows command line and the rest (raw).
pub fn split_command(command: &str) -> (String, String) {
    let c = command.trim();
    if let Some(r) = c.strip_prefix('"') {
        let (p, rest) = r.split_once('"').unwrap_or((r, ""));
        (p.to_string(), rest.trim_start().to_string())
    } else {
        let (p, rest) = c.split_once(' ').unwrap_or((c, ""));
        (p.to_string(), rest.trim_start().to_string())
    }
}

/// The program a recorded Windows nxm command line starts (it comes from `handler.json`, so it is checked before it
/// runs): an absolute `X:\...\name.exe` path (no network share, no relative or search-path name), not a script host or
/// another program that runs code it is given, and not a Kiln launcher.
#[cfg(any(windows, test))]
pub fn forward_program_ok(prog: &str) -> Result<()> {
    let refuse = |why: &str| Err(Error::new(ErrorCode::Refused, format!("The previous nxm handler cannot be started: {why}.")));
    let b = prog.as_bytes();
    if prog.chars().any(|c| c.is_control() || c == '"') { return refuse("its path has control characters or quotes"); }
    if !(b.len() > 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && matches!(b[2], b'\\' | b'/')) {
        return refuse("it is not an absolute path on a local drive");
    }
    if prog.split(['\\', '/']).any(|c| c == "..") { return refuse("its path goes up a folder"); }
    let base = prog.rsplit(['/', '\\']).next().unwrap_or(prog).to_ascii_lowercase();
    let base = base.trim_end_matches(['.', ' ']);
    if !base.ends_with(".exe") { return refuse("it is not an .exe"); }
    const HOSTS: &[&str] = &["cmd.exe", "powershell.exe", "pwsh.exe", "wscript.exe", "cscript.exe", "mshta.exe", "rundll32.exe",
        "regsvr32.exe", "msiexec.exe", "conhost.exe", "explorer.exe", "wmic.exe", "forfiles.exe", "certutil.exe", "bitsadmin.exe",
        "hh.exe", "schtasks.exe", "msbuild.exe", "installutil.exe", "wsl.exe", "bash.exe"];
    if HOSTS.contains(&base) { return refuse("it is a script host or a program that runs other code"); }
    if base.contains("kiln-launcher") { return refuse("it is Kiln itself"); }
    Ok(())
}

#[cfg(any(windows, test))]
fn nxm_registry_slot(key: &str, name: Option<&str>) -> bool {
    match key.to_ascii_lowercase().as_str() {
        r"software\classes\nxm" => name.is_none() || name.is_some_and(|n| n.eq_ignore_ascii_case("URL Protocol")),
        r"software\classes\nxm\defaulticon" | r"software\classes\nxm\shell" | r"software\classes\nxm\shell\open" | r"software\classes\nxm\shell\open\command" => name.is_none(),
        _ => false,
    }
}

#[cfg(windows)]
pub mod win {
    use super::*;
    use windows_sys::Win32::System::Registry::{
        HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_WRITE, KEY_READ, REG_OPTION_NON_VOLATILE, REG_SZ, RegCloseKey, RegCreateKeyExW, RegDeleteTreeW, RegSetValueExW,
        RegOpenKeyExW, RegQueryValueExW, RegDeleteValueW, RegDeleteKeyW, RegQueryInfoKeyW,
    };

    const KEY: &str = r"Software\Classes\nxm";

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn set(root: HKEY, key: &str, name: Option<&str>, value: &str) -> Result<()> {
        let k = wide(key);
        let mut h: HKEY = std::ptr::null_mut();
        // SAFETY: NUL-terminated strings; h is closed below.
        unsafe {
            let rc = RegCreateKeyExW(root, k.as_ptr(), 0, std::ptr::null(), REG_OPTION_NON_VOLATILE, KEY_WRITE, std::ptr::null(), &mut h, std::ptr::null_mut());
            if rc != 0 {
                return Err(Error::new(ErrorCode::Permission, format!("registry {key}: error {rc}")));
            }
            let n = name.map(wide);
            let v = wide(value);
            let rc = RegSetValueExW(h, n.as_ref().map_or(std::ptr::null(), |n| n.as_ptr()), 0, REG_SZ, v.as_ptr().cast(), (v.len() * 2) as u32);
            RegCloseKey(h);
            if rc != 0 {
                return Err(Error::new(ErrorCode::Permission, format!("registry {key}: error {rc}")));
            }
        }
        Ok(())
    }

    #[derive(Serialize, Deserialize)]
    struct SavedValue { key: String, name: Option<String>, existed: bool, value: Option<(u32, Vec<u8>)> }

    fn snapshot_value(key: &str, name: Option<&str>) -> Result<SavedValue> {
        let mut h = std::ptr::null_mut();
        let k = wide(key);
        // SAFETY: valid NUL-terminated names, owned buffers and closed registry handles.
        unsafe {
            let rc = RegOpenKeyExW(HKEY_CURRENT_USER, k.as_ptr(), 0, KEY_READ, &mut h);
            if rc == 2 { return Ok(SavedValue { key: key.into(), name: name.map(str::to_string), existed: false, value: None }); }
            if rc != 0 { return Err(Error::new(ErrorCode::Permission, format!("reading nxm registration: error {rc}"))); }
            let n = name.map(wide);
            let ptr = n.as_ref().map_or(std::ptr::null(), |n| n.as_ptr());
            let mut kind = 0;
            let mut len = 0;
            let rc = RegQueryValueExW(h, ptr, std::ptr::null(), &mut kind, std::ptr::null_mut(), &mut len);
            if rc == 2 { RegCloseKey(h); return Ok(SavedValue { key: key.into(), name: name.map(str::to_string), existed: true, value: None }); }
            if rc != 0 || len > 1024 * 1024 { RegCloseKey(h); return Err(Error::invalid("cannot snapshot nxm registry value")); }
            let mut data = vec![0; len as usize];
            let rc = RegQueryValueExW(h, ptr, std::ptr::null(), &mut kind, data.as_mut_ptr(), &mut len);
            RegCloseKey(h);
            if rc != 0 { return Err(Error::invalid("cannot snapshot nxm registry value")); }
            data.truncate(len as usize);
            Ok(SavedValue { key: key.into(), name: name.map(str::to_string), existed: true, value: Some((kind, data)) })
        }
    }

    fn restore_value(saved: &SavedValue) -> Result<()> {
        let mut h = std::ptr::null_mut();
        let k = wide(&saved.key);
        // SAFETY: valid names and data saved by snapshot_value; each opened handle is closed.
        unsafe {
            let rc = RegOpenKeyExW(HKEY_CURRENT_USER, k.as_ptr(), 0, KEY_WRITE, &mut h);
            if rc == 2 && saved.value.is_none() { return Ok(()); }
            if rc != 0 { return Err(Error::new(ErrorCode::Permission, format!("restoring nxm registration: error {rc}"))); }
            let n = saved.name.as_deref().map(wide);
            let name = n.as_ref().map_or(std::ptr::null(), |n| n.as_ptr());
            let rc = if let Some((kind, bytes)) = &saved.value {
                RegSetValueExW(h, name, 0, *kind, bytes.as_ptr(), bytes.len() as u32)
            } else { RegDeleteValueW(h, name) };
            RegCloseKey(h);
            if rc != 0 && rc != 2 { return Err(Error::new(ErrorCode::Permission, format!("restoring nxm value: error {rc}"))); }
        }
        Ok(())
    }

    #[derive(Debug, Default)]
    pub struct WinRegistry;

    impl WinRegistry {
        fn command_of(root: HKEY, base: &str) -> Option<String> {
            crate::steam::win::reg_string(root, &format!(r"{base}\shell\open\command"), "")
        }
        fn exe_lower() -> String {
            std::env::current_exe().map(|p| p.display().to_string().to_lowercase()).unwrap_or_default()
        }
    }

    impl HandlerPlatform for WinRegistry {
        fn supported(&self) -> bool {
        if !crate::nexus::enabled() { return false }
            true
        }
        fn current(&self) -> Result<Option<Previous>> {
        crate::nexus::require_enabled()?;
            let me = Self::exe_lower();
            let ours = |c: &str| c.to_lowercase().contains("--nxm") && (me.is_empty() || c.to_lowercase().contains(&me) || c.to_lowercase().contains("kiln-launcher"));
            if let Some(c) = Self::command_of(HKEY_CURRENT_USER, KEY) {
                if ours(&c) {
                    return Ok(None);
                }
                let icon = crate::steam::win::reg_string(HKEY_CURRENT_USER, &format!(r"{KEY}\DefaultIcon"), "");
                return Ok(Some(Previous::Command { command: c, icon, scope: "hkcu".into() }));
            }
            Ok(Self::command_of(HKEY_LOCAL_MACHINE, r"SOFTWARE\Classes\nxm").filter(|c| !ours(c)).map(|c| Previous::Command { command: c, icon: None, scope: "hklm".into() }))
        }
        fn is_ours(&self) -> bool {
            Self::command_of(HKEY_CURRENT_USER, KEY).is_some_and(|c| c.contains("--nxm") && c.to_lowercase().contains(&Self::exe_lower()))
        }
        fn is_kiln_registration(&self) -> bool {
            // Kiln's own command line: `"<...>kiln-launcher.exe" --nxm "%1"`, wherever the launcher lives now.
            Self::command_of(HKEY_CURRENT_USER, KEY).is_some_and(|c| {
                let c = c.to_lowercase();
                c.contains("--nxm") && c.contains("kiln-launcher")
            })
        }
        fn matches_registered(&self, exe: Option<&str>) -> bool {
            let Some(exe) = exe else { return self.is_ours() };
            let expected = format!("\"{exe}\" --nxm \"%1\"");
            Self::command_of(HKEY_CURRENT_USER, KEY).is_some_and(|c| c.eq_ignore_ascii_case(&expected))
        }
        fn install(&self, exe: &Path) -> Result<()> {
        crate::nexus::require_enabled()?;
            let e = exe.display().to_string();
            if !exe.is_absolute() || e.chars().any(|c| c.is_control() || c == '"') { return Err(Error::invalid("invalid launcher path")); }
            set(HKEY_CURRENT_USER, KEY, None, "URL:NXM Protocol")?;
            set(HKEY_CURRENT_USER, KEY, Some("URL Protocol"), "")?;
            set(HKEY_CURRENT_USER, &format!(r"{KEY}\DefaultIcon"), None, &format!("\"{e}\",0"))?;
            set(HKEY_CURRENT_USER, &format!(r"{KEY}\shell\open\command"), None, &format!("\"{e}\" --nxm \"%1\""))
        }
        fn snapshot(&self) -> Result<Option<serde_json::Value>> {
        crate::nexus::require_enabled()?;
            let mut values = Vec::new();
            for (key, name) in [(KEY.to_string(), None), (KEY.to_string(), Some("URL Protocol")), (format!(r"{KEY}\DefaultIcon"), None), (format!(r"{KEY}\shell"), None), (format!(r"{KEY}\shell\open"), None), (format!(r"{KEY}\shell\open\command"), None)] {
                values.push(snapshot_value(&key, name)?);
            }
            Ok(Some(serde_json::to_value(values)?))
        }
        fn restore_snapshot(&self, previous: Option<&Previous>, snapshot: Option<&serde_json::Value>) -> Result<()> {
            let Some(snapshot) = snapshot else { return self.restore(previous) };
            let values: Vec<SavedValue> = serde_json::from_value(snapshot.clone())?;
            if values.iter().any(|v| !nxm_registry_slot(&v.key, v.name.as_deref())) {
                return Err(Error::invalid("The saved registration contains an unexpected registry value."));
            }
            // Only restore the four values install changes. Intermediate key defaults were never changed.
            for value in values.iter().filter(|v| !v.key.ends_with(r"\shell") && !v.key.ends_with(r"\shell\open")) { restore_value(value)?; }
            for value in values.iter().rev().filter(|v| !v.existed) {
                let key = wide(&value.key);
                // Delete only empty keys created by Kiln, never another manager's values/subkeys.
                let mut h = std::ptr::null_mut();
                let mut children = 0;
                let mut entries = 0;
                // SAFETY: output counters and registry handle are valid; close the handle before deletion.
                let empty = unsafe {
                    if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_READ, &mut h) != 0 { false } else {
                        let rc = RegQueryInfoKeyW(h, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null(), &mut children, std::ptr::null_mut(), std::ptr::null_mut(), &mut entries, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
                        RegCloseKey(h);
                        rc == 0 && children == 0 && entries == 0
                    }
                };
                if !empty { continue; }
                let rc = unsafe { RegDeleteKeyW(HKEY_CURRENT_USER, key.as_ptr()) };
                if rc != 0 && rc != 2 && rc != 5 { return Err(Error::new(ErrorCode::Permission, format!("removing nxm key: error {rc}"))); }
            }
            Ok(())
        }
        fn restore(&self, previous: Option<&Previous>) -> Result<()> {
            match previous {
                Some(Previous::Command { command, icon, scope }) if scope == "hkcu" => {
                    set(HKEY_CURRENT_USER, &format!(r"{KEY}\shell\open\command"), None, command)?;
                    if let Some(i) = icon {
                        set(HKEY_CURRENT_USER, &format!(r"{KEY}\DefaultIcon"), None, i)?;
                    }
                    Ok(())
                }
                _ => {
                    let k = wide(KEY);
                    // SAFETY: NUL-terminated key below HKCU (Kiln's own per-user key).
                    let rc = unsafe { RegDeleteTreeW(HKEY_CURRENT_USER, k.as_ptr()) };
                    if rc != 0 && rc != 2 {
                        return Err(Error::new(ErrorCode::Permission, format!("registry {KEY}: error {rc}")));
                    }
                    Ok(())
                }
            }
        }
        fn forward(&self, previous: &Previous, url: &str) -> Result<()> {
            use std::os::windows::process::CommandExt;
            if super::super::nxm::game_of(url).is_none() { return Err(Error::invalid("invalid nxm link")); }
            let Previous::Command { command, .. } = previous else { return Err(Error::new(ErrorCode::Unsupported, "not a command")) };
            let (prog, rest) = split_command(&fill_command(command, url));
            forward_program_ok(&prog)?;
            if !Path::new(&prog).is_file() { return Err(Error::not_found("The previous nxm handler's program is gone.")); }
            let mut c = std::process::Command::new(prog);
            if !rest.is_empty() {
                c.raw_arg(rest);
            }
            c.spawn().map(|_| ()).map_err(|e| Error::new(ErrorCode::Unsupported, format!("starting the previous handler: {e}")))
        }
    }
}

// ---------------------------------------------------------------- one launcher, the link inbox

/// Held while this launcher runs: a second process for a link sees it is taken and hands the link over.
pub struct Instance {
    _file: crate::util::LockedFile,
}

/// Makes a private state directory, refusing symbolic links and directories owned by another account.
pub fn private_dir(path: &Path) -> Result<()> {
    crate::nexus::require_enabled()?;
    private_dir_inner(path)
}

/// [`private_dir`] without the Nexus switch: only for the folders an older build already made ([`retire_leftover`]).
fn private_dir_inner(path: &Path) -> Result<()> {
    if !path.exists() {
        let mut builder = std::fs::DirBuilder::new();
        builder.recursive(true);
        #[cfg(unix)] { use std::os::unix::fs::DirBuilderExt; builder.mode(0o700); }
        builder.create(path).map_err(crate::util::io_at(path, "creating"))?;
    }
    let m = std::fs::symlink_metadata(path).map_err(crate::util::io_at(path, "checking"))?;
    if !m.is_dir() || m.file_type().is_symlink() { return Err(Error::invalid("Nexus state folder must be a real directory")); }
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if m.file_attributes() & 0x400 != 0 { return Err(Error::invalid("Nexus state folder must not be a reparse point")); }
    }
    #[cfg(unix)] {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if m.uid() != unsafe { libc::geteuid() } { return Err(Error::new(ErrorCode::Permission, "Nexus state folder belongs to another account")); }
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).map_err(crate::util::io_at(path, "securing"))?;
    }
    #[cfg(windows)] secure_windows_dir(path)?;
    Ok(())
}

/// Secures a Nexus state folder or file: owned by this user, with a protected DACL for its owner and SYSTEM only.
///
/// The owner check accepts what this user's own launcher makes. A launcher started "as administrator" makes its folders
/// owned by BUILTIN\Administrators (an elevated token's default owner), not by the user; such a folder (also one an
/// older build left by an elevated start) is taken over: its owner becomes the user, so a later start without elevation
/// can still use it. Anything owned by another account is refused.
#[cfg(windows)]
fn secure_windows_dir(path: &Path) -> Result<()> {
    use std::os::windows::ffi::OsStrExt;
    if std::fs::symlink_metadata(path)?.file_type().is_symlink() { return Err(Error::invalid("Nexus state must not use symbolic links")); }
    #[link(name = "advapi32")]
    unsafe extern "system" {
        fn ConvertStringSecurityDescriptorToSecurityDescriptorW(s: *const u16, revision: u32, descriptor: *mut *mut core::ffi::c_void, size: *mut u32) -> i32;
        fn ConvertSidToStringSidW(sid: *const core::ffi::c_void, text: *mut *mut u16) -> i32;
        fn SetFileSecurityW(path: *const u16, information: u32, descriptor: *const core::ffi::c_void) -> i32;
        fn GetNamedSecurityInfoW(path: *mut u16, object_type: u32, information: u32, owner: *mut *mut core::ffi::c_void, group: *mut *mut core::ffi::c_void, dacl: *mut *mut core::ffi::c_void, sacl: *mut *mut core::ffi::c_void, descriptor: *mut *mut core::ffi::c_void) -> u32;
        fn OpenProcessToken(process: *mut core::ffi::c_void, access: u32, token: *mut *mut core::ffi::c_void) -> i32;
        fn GetTokenInformation(token: *mut core::ffi::c_void, class: u32, info: *mut core::ffi::c_void, len: u32, needed: *mut u32) -> i32;
        fn EqualSid(first: *const core::ffi::c_void, second: *const core::ffi::c_void) -> i32;
        fn IsWellKnownSid(sid: *const core::ffi::c_void, kind: i32) -> i32;
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn LocalFree(pointer: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
        fn GetCurrentProcess() -> *mut core::ffi::c_void;
        fn CloseHandle(handle: *mut core::ffi::c_void) -> i32;
    }
    const TOKEN_USER: u32 = 1;
    const TOKEN_GROUPS: u32 = 2;
    const TOKEN_OWNER: u32 = 4;
    const WIN_BUILTIN_ADMINISTRATORS_SID: i32 = 26;
    /// A token information class read into pointer-aligned words (TOKEN_USER and TOKEN_OWNER start with the SID pointer).
    unsafe fn token_info(token: *mut core::ffi::c_void, class: u32) -> Option<Vec<usize>> {
        let mut needed = 0;
        // SAFETY: a size query (null buffer), then a buffer of at least `needed` bytes.
        unsafe {
            GetTokenInformation(token, class, std::ptr::null_mut(), 0, &mut needed);
            if needed == 0 { return None; }
            let mut words = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
            (GetTokenInformation(token, class, words.as_mut_ptr().cast(), (words.len() * std::mem::size_of::<usize>()) as u32, &mut needed) != 0).then_some(words)
        }
    }
    let mut name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    // Check ownership before applying an OWNER RIGHTS DACL: a precreated foreign directory must not retain access.
    let mut owner = std::ptr::null_mut();
    let mut original = std::ptr::null_mut();
    let mut token = std::ptr::null_mut();
    // The user's SID as SDDL text when the owner must become the user, None when the user owns it already.
    let new_owner: Option<String>;
    // SAFETY: valid NUL-terminated name and system-owned handles/descriptors freed before returning; the SID pointers
    // read from the token buffers live as long as those buffers.
    unsafe {
        let rc = GetNamedSecurityInfoW(name.as_mut_ptr(), 1, 1, &mut owner, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut original);
        if rc != 0 { return Err(Error::new(ErrorCode::Permission, format!("checking Nexus folder owner: error {rc}"))); }
        if OpenProcessToken(GetCurrentProcess(), 8, &mut token) == 0 { LocalFree(original); return Err(Error::new(ErrorCode::Permission, "cannot check Nexus folder owner")); }
        let user = token_info(token, TOKEN_USER);
        let default_owner = token_info(token, TOKEN_OWNER);
        let groups = token_info(token, TOKEN_GROUPS);
        CloseHandle(token);
        let Some(user) = user.filter(|w| w[0] != 0) else { LocalFree(original); return Err(Error::new(ErrorCode::Permission, "cannot check Nexus folder owner")) };
        let user_sid = user[0] as *const core::ffi::c_void;
        let owned_by_user = EqualSid(owner, user_sid) != 0;
        // what this process makes: an elevated token's default owner is BUILTIN\Administrators
        let owned_by_default = default_owner.as_ref().is_some_and(|w| w[0] != 0 && EqualSid(owner, w[0] as *const core::ffi::c_void) != 0);
        // what an elevated start of this user made, seen without elevation (the Administrators group is deny-only then)
        let owned_by_our_admins = IsWellKnownSid(owner, WIN_BUILTIN_ADMINISTRATORS_SID) != 0 && groups.as_ref().is_some_and(|w| {
            // TOKEN_GROUPS: a DWORD count, then SID_AND_ATTRIBUTES { PSID, DWORD } entries from the next pointer-aligned word
            let count = w[0] as u32 as usize;
            (0..count).filter_map(|i| w.get(1 + 2 * i)).any(|&sid| sid != 0 && IsWellKnownSid(sid as *const core::ffi::c_void, WIN_BUILTIN_ADMINISTRATORS_SID) != 0)
        });
        new_owner = if owned_by_user {
            None
        } else if owned_by_default || owned_by_our_admins {
            let mut text = std::ptr::null_mut();
            if ConvertSidToStringSidW(user_sid, &mut text) == 0 { LocalFree(original); return Err(Error::new(ErrorCode::Permission, "cannot check Nexus folder owner")); }
            let len = (0..).take_while(|&i| *text.add(i) != 0).count();
            let sid = String::from_utf16_lossy(std::slice::from_raw_parts(text, len));
            LocalFree(text.cast());
            Some(sid)
        } else {
            None
        };
        LocalFree(original);
        if !owned_by_user && new_owner.is_none() { return Err(Error::new(ErrorCode::Permission, "Nexus state folder belongs to another account")); }
    }
    // Protected DACL, inherited by files/subdirectories: only object owner and SYSTEM have access. The owner becomes the
    // user in the same call when it was Administrators.
    let owner_part = new_owner.as_deref().map(|sid| format!("O:{sid}")).unwrap_or_default();
    let sddl: Vec<u16> = format!("{owner_part}D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)").encode_utf16().chain(Some(0)).collect();
    let information = if new_owner.is_some() { 0x80000005 } else { 0x80000004 }; // PROTECTED_DACL | DACL (| OWNER)
    let mut descriptor = std::ptr::null_mut();
    // SAFETY: NUL-terminated buffers and descriptor allocated by the system, released with LocalFree.
    unsafe {
        if ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.as_ptr(), 1, &mut descriptor, std::ptr::null_mut()) == 0 {
            return Err(crate::util::io_at(path, "securing")(std::io::Error::last_os_error()));
        }
        let ok = SetFileSecurityW(name.as_ptr(), information, descriptor);
        LocalFree(descriptor);
        if ok == 0 { return Err(crate::util::io_at(path, "securing")(std::io::Error::last_os_error())); }
    }
    Ok(())
}

fn open_lock(root: &Path, name: &str) -> Result<std::fs::File> {
    private_dir_inner(root)?;
    let p = root.join(name);
    let mut opts = std::fs::OpenOptions::new();
    opts.create(true).truncate(false).write(true);
    #[cfg(unix)] {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600).custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)] { use std::os::windows::fs::OpenOptionsExt; opts.custom_flags(0x00200000); } // FILE_FLAG_OPEN_REPARSE_POINT
    let f = opts.open(&p).map_err(crate::util::io_at(&p, "opening"))?;
    if f.metadata()?.file_type().is_symlink() || !f.metadata()?.is_file() { return Err(Error::invalid("Nexus lock must be a regular file")); }
    #[cfg(windows)] secure_windows_dir(&p)?;
    #[cfg(unix)] {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if f.metadata()?.uid() != unsafe { libc::geteuid() } { return Err(Error::new(ErrorCode::Permission, "Nexus lock belongs to another account")); }
        f.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    }
    Ok(f)
}

/// Serializes a state read/modify/write operation across launcher processes.
pub fn lock_state(root: &Path, name: &str) -> Result<Instance> {
    crate::nexus::require_enabled()?;
    lock_state_inner(root, name)
}

fn lock_state_inner(root: &Path, name: &str) -> Result<Instance> {
    if name.contains(['/', '\\']) || name == "." || name == ".." { return Err(Error::invalid("invalid lock name")); }
    let f = open_lock(root, name)?;
    f.lock()?;
    Ok(Instance { _file: crate::util::LockedFile(f) })
}

/// Takes the process-lifetime lock; only its owner may consume the inbox.
pub fn acquire_instance(root: &Path) -> Result<Option<Instance>> {
        crate::nexus::require_enabled()?;
    let f = open_lock(root, "instance.lock")?;
    match f.try_lock() {
        Ok(()) => Ok(Some(Instance { _file: crate::util::LockedFile(f) })),
        Err(std::fs::TryLockError::WouldBlock) => Ok(None),
        Err(std::fs::TryLockError::Error(e)) => Err(Error::from(e)),
    }
}

/// What a process started for an nxm link finds ([`find_instance`]).
pub enum Found {
    /// No launcher holds `instance.lock`. Some: this process holds it now (keep it while the window lives); None: there
    /// is no lock file yet, nothing was created.
    Free(Option<Instance>),
    /// A launcher holds it: it takes links from the inbox.
    Running,
}

/// Whether a launcher runs with this Nexus folder, without writing anything (a process started for a link, before the
/// data folder's gates open): `instance.lock` is opened read-only, never created, its folder never secured or made.
pub fn find_instance(root: &Path) -> Result<Found> {
    crate::nexus::require_enabled()?;
    let p = root.join("instance.lock");
    match std::fs::symlink_metadata(&p) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Found::Free(None)),
        Err(e) => return Err(crate::util::io_at(&p, "checking")(e)),
        Ok(m) if !m.is_file() => return Err(Error::invalid("Nexus lock must be a regular file")),
        Ok(_) => {}
    }
    let mut opts = std::fs::OpenOptions::new();
    opts.read(true);
    #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; opts.custom_flags(libc::O_NOFOLLOW); }
    #[cfg(windows)] { use std::os::windows::fs::OpenOptionsExt; opts.custom_flags(0x00200000); } // FILE_FLAG_OPEN_REPARSE_POINT
    let f = opts.open(&p).map_err(crate::util::io_at(&p, "opening"))?;
    if !f.metadata()?.is_file() { return Err(Error::invalid("Nexus lock must be a regular file")); }
    #[cfg(unix)] {
        use std::os::unix::fs::MetadataExt;
        if f.metadata()?.uid() != unsafe { libc::geteuid() } { return Err(Error::new(ErrorCode::Permission, "Nexus lock belongs to another account")); }
    }
    match f.try_lock() {
        Ok(()) => Ok(Found::Free(Some(Instance { _file: crate::util::LockedFile(f) }))),
        Err(std::fs::TryLockError::WouldBlock) => Ok(Found::Running),
        Err(std::fs::TryLockError::Error(e)) => Err(Error::from(e)),
    }
}

fn inbox(root: &Path) -> PathBuf {
    root.join("inbox")
}

/// Puts a link into the inbox (readable by the user only: it carries a download key).
pub fn queue_link(root: &Path, url: &str) -> Result<PathBuf> {
        crate::nexus::require_enabled()?;
    #[derive(Serialize)]
    struct L<'a> {
        url: &'a str,
    }
    super::nxm::parse(url)?;
    private_dir(root)?;
    private_dir(&inbox(root))?;
    let name = format!("{:013}-{}.json", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0),
        crate::hash::hex(&crate::util::random_bytes::<4>()?));
    let p = inbox(root).join(name);
    super::keystore::write_private(&p, &L { url })?;
    Ok(p)
}

/// Takes every queued link, oldest first (links older than a day are dropped unread).
pub fn take_links(root: &Path) -> Vec<String> {
    if !crate::nexus::enabled() { return Vec::new() }
    #[derive(Deserialize)]
    struct L {
        url: String,
    }
    use std::io::Read;
    let Ok(_lock) = lock_state(root, "inbox.lock") else { return vec![]; };
    if private_dir(root).and_then(|_| private_dir(&inbox(root))).is_err() { return vec![]; }
    let mut files: Vec<PathBuf> = std::fs::read_dir(inbox(root)).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "json")).collect();
    files.sort();
    let mut out = Vec::new();
    for path in files {
        let mut opts = std::fs::OpenOptions::new();
        opts.read(true);
        #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; opts.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK); }
        #[cfg(windows)] { use std::os::windows::fs::OpenOptionsExt; opts.custom_flags(0x00200000); }
        if let Ok(mut f) = opts.open(&path) {
            #[cfg(windows)] if secure_windows_dir(&path).is_err() { let _ = std::fs::remove_file(&path); continue; }
            let valid = f.metadata().is_ok_and(|m| {
                let fresh = m.modified().ok().and_then(|t| t.elapsed().ok()).is_some_and(|age| age.as_secs() < 86_400);
                #[cfg(unix)] { use std::os::unix::fs::MetadataExt; if m.uid() != unsafe { libc::geteuid() } || m.mode() & 0o077 != 0 { return false; } }
                !m.file_type().is_symlink() && m.is_file() && m.len() <= 16_384 && fresh
            });
            let mut bytes = Vec::new();
            if valid && f.by_ref().take(16_385).read_to_end(&mut bytes).is_ok() && bytes.len() <= 16_384 {
                if let Ok(l) = serde_json::from_slice::<L>(&bytes) && super::nxm::parse(&l.url).is_ok() { out.push(l.url); }
            }
        }
        let _ = std::fs::remove_file(&path);
    }
    out
}

// ---------------------------------------------------------------- a registration an older build left behind

/// What [`retire_leftover`] did with an nxm registration an older build (dev.19) made, in a build with Nexus off.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Leftover {
    /// Kiln recorded no registration, or already gave the links back.
    Nothing,
    /// Kiln was still the system's nxm handler. The handler before it is back (`previous`; None: there was none, and
    /// the system has no nxm handler now, as before Kiln).
    GivenBack { previous: Option<Previous> },
    /// Another program has the links since. Kiln dropped only its own record and changed nothing in the system.
    Forgotten,
}

/// Gives back the nxm registration an older Kiln made, when this build has Nexus off: with the option on in dev.19, the
/// links would otherwise keep going to a launcher that no longer takes them. Only Kiln's own registration is undone:
/// when the system's nxm handler is Kiln's (this launcher or a moved one), the association before Kiln is restored
/// exactly as `unregister` does it (the snapshot from `register`, else the recorded previous handler); when another
/// manager has the links, only Kiln's record is dropped. Without a record (`handler.json`), nothing is read or created.
/// A failed restore keeps the record, so the next start tries again.
pub fn retire_leftover(root: &Path, p: &dyn HandlerPlatform) -> Result<Leftover> {
    if !state_path(root).is_file() { return Ok(Leftover::Nothing); }
    let _lock = lock_state_inner(root, "handler.lock")?;
    let s = read_state(root)?;
    if !s.registered { return Ok(Leftover::Nothing); }
    let ours = p.matches_registered(s.exe.as_deref()) || p.is_kiln_registration();
    // A record that names Kiln itself as the handler before it is never given back (it would register Kiln again).
    let previous = s.previous.clone().filter(|prev| !is_kiln_handler(prev));
    let outcome = if ours {
        p.restore_snapshot(previous.as_ref(), s.snapshot.as_ref())?;
        // The saved association (mimeapps.list text, registry values, Kiln's old .desktop file) may itself have named
        // Kiln: a build with Nexus off never leaves Kiln as the handler. Fall back to the recorded handler, then to none.
        if p.is_kiln_registration() {
            p.restore(previous.as_ref())?;
            if p.is_kiln_registration() { p.restore(None)?; }
        }
        if p.is_kiln_registration() {
            return Err(Error::new(ErrorCode::Io, "Kiln is still the nxm handler after giving the links back."));
        }
        Leftover::GivenBack { previous }
    } else {
        Leftover::Forgotten
    };
    let file = state_path(root);
    match std::fs::remove_file(&file) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(crate::util::io_at(&file, "removing")(e)),
    }
    Ok(outcome)
}

/// The handler Kiln recorded before its own registration (for a link that still reaches Kiln when giving the links back
/// failed). Reads `handler.json` only.
pub fn recorded_previous(root: &Path) -> Option<Previous> {
    if !state_path(root).is_file() { return None; }
    read_state(root).ok().filter(|s| s.registered).and_then(|s| s.previous).filter(|p| !is_kiln_handler(p))
}

/// The handler is a Kiln launcher (any copy): its desktop id, or a command line with `--nxm` that runs kiln-launcher.
/// A link is never forwarded to it (the link would come straight back, one new process per round) and it is never
/// restored as "the handler before Kiln".
pub fn is_kiln_handler(previous: &Previous) -> bool {
    match previous {
        Previous::Desktop { id } => id.trim() == DESKTOP_ID,
        Previous::Command { command, .. } => {
            let c = command.to_ascii_lowercase();
            c.contains("--nxm") && c.contains("kiln-launcher")
        }
    }
}

/// Hands one link to the handler before Kiln, in a build with Nexus off (any game's link). Returns its name.
pub fn forward_to_previous(p: &dyn HandlerPlatform, previous: &Previous, url: &str) -> Result<String> {
    if super::nxm::game_of(url).is_none() { return Err(Error::invalid("invalid nxm link")); }
    if is_kiln_handler(previous) { return Err(Error::new(ErrorCode::Refused, "the recorded handler is Kiln itself")); }
    p.forward(previous, url)?;
    Ok(previous.label())
}

#[cfg(test)]
mod safety_tests {
    use super::*;

    #[test]
    fn recorded_windows_commands_start_only_a_local_exe_that_is_not_a_script_host_or_kiln() {
        for ok in [r"C:\Program Files\Black Tree Gaming Ltd\Vortex\Vortex.exe", r"D:\MO2\nxmhandler.exe", "E:/Tools/mm.EXE"] {
            forward_program_ok(ok).unwrap_or_else(|e| panic!("{ok}: {e}"));
        }
        for bad in ["cmd.exe", r"C:\Windows\System32\cmd.exe", r"C:\Windows\System32\mshta.exe", r"C:\Windows\System32\rundll32.exe",
                    r"C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe", r"\\server\share\mm.exe", r"\\?\C:\mm.exe",
                    r"mm.exe", r"C:\mm.bat", r"C:\mm.cmd", r"C:\mm.js", r"C:\a\..\Windows\System32\cmd.exe",
                    r"C:\Apps\Kiln\kiln-launcher.exe", r"C:\Windows\explorer.exe", "C:\\mm.exe\n"] {
            assert!(forward_program_ok(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn nxm_default_is_read_from_the_defaults_section_only() {
        assert_eq!(mime_default_id("[Added Associations]\nx-scheme-handler/nxm=a.desktop;\n[Default Applications]\nx-scheme-handler/nxm=kiln-launcher-nxm.desktop;b.desktop\n").as_deref(), Some(DESKTOP_ID));
        assert_eq!(mime_default_id("[Added Associations]\nx-scheme-handler/nxm=kiln-launcher-nxm.desktop\n"), None);
    }

    fn temp() -> PathBuf {
        let p = std::env::temp_dir().join(format!("kiln-handler-{}-{}", std::process::id(), crate::hash::hex(&crate::util::random_bytes::<8>().unwrap())));
        std::fs::create_dir(&p).unwrap(); p
    }

    struct Fake {
        current: std::sync::Mutex<String>,
        root: PathBuf,
        fail: bool,
    }
    impl HandlerPlatform for Fake {
        fn supported(&self) -> bool { true }
        fn current(&self) -> Result<Option<Previous>> { Ok(Some(Previous::Desktop { id: self.current.lock().unwrap().clone() })) }
        fn is_ours(&self) -> bool { *self.current.lock().unwrap() == DESKTOP_ID }
        fn install(&self, _: &Path) -> Result<()> {
            assert!(state(&self.root).registered, "recovery must be persisted before changing the OS");
            *self.current.lock().unwrap() = DESKTOP_ID.into();
            if self.fail { return Err(Error::invalid("simulated failure")); }
            Ok(())
        }
        fn restore(&self, previous: Option<&Previous>) -> Result<()> {
            if let Some(Previous::Desktop { id }) = previous { *self.current.lock().unwrap() = id.clone(); }
            Ok(())
        }
        fn forward(&self, _: &Previous, _: &str) -> Result<()> { Ok(()) }
    }

    /// Whether the user of this process owns `path` (its owner SID is the token's user SID).
    #[cfg(windows)]
    fn owned_by_this_user(path: &Path) -> bool {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
        use windows_sys::Win32::Security::{EqualSid, GetFileSecurityW, GetSecurityDescriptorOwner, GetTokenInformation, OWNER_SECURITY_INFORMATION, TOKEN_QUERY, TOKEN_USER, TokenUser};
        use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
        let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        // SAFETY: NUL-terminated name, buffers sized by the size queries, the token handle closed before returning.
        unsafe {
            let mut needed = 0;
            GetFileSecurityW(name.as_ptr(), OWNER_SECURITY_INFORMATION, std::ptr::null_mut(), 0, &mut needed);
            let mut sd = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
            assert_ne!(GetFileSecurityW(name.as_ptr(), OWNER_SECURITY_INFORMATION, sd.as_mut_ptr().cast(), needed, &mut needed), 0);
            let (mut owner, mut defaulted) = (std::ptr::null_mut(), 0);
            assert_ne!(GetSecurityDescriptorOwner(sd.as_mut_ptr().cast(), &mut owner, &mut defaulted), 0);
            let mut token: HANDLE = std::ptr::null_mut();
            assert_ne!(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token), 0);
            GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut needed);
            let mut user = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
            assert_ne!(GetTokenInformation(token, TokenUser, user.as_mut_ptr().cast(), needed, &mut needed), 0);
            CloseHandle(token);
            EqualSid(owner, (*(user.as_ptr() as *const TOKEN_USER)).User.Sid) != 0
        }
    }

    /// A launcher started as administrator makes its folders owned by BUILTIN\Administrators, not by the user (an
    /// elevated token's default owner; the win1 runner's SSH session is elevated too). Its Nexus state works and becomes
    /// the user's, so a start without elevation can use it later; without elevation the user owns it from the start.
    #[cfg(windows)]
    #[test]
    fn a_folder_this_account_made_is_secured_and_becomes_the_users() {
        let root = temp();
        let made_here = root.join("state");
        std::fs::create_dir(&made_here).unwrap(); // owned by the token's default owner, as private_dir makes it
        private_dir_inner(&made_here).expect("a folder this process made is its own");
        assert!(owned_by_this_user(&made_here), "the folder is the user's after securing");
        let lock = open_lock(&made_here, "test.lock").expect("a lock file in it");
        drop(lock);
        assert!(owned_by_this_user(&made_here.join("test.lock")));
        // secured again (the next start): still accepted, still the user's
        private_dir_inner(&made_here).unwrap();
        assert!(owned_by_this_user(&made_here));
        std::fs::remove_dir_all(root).unwrap();
    }

    #[cfg(windows)]
    #[link(name = "advapi32")]
    unsafe extern "system" {
        fn ConvertSecurityDescriptorToStringSecurityDescriptorW(sd: *const core::ffi::c_void, revision: u32, information: u32, text: *mut *mut u16, len: *mut u32) -> i32;
        fn ConvertSidToStringSidW(sid: *const core::ffi::c_void, text: *mut *mut u16) -> i32;
    }

    /// Text Windows allocated (LocalFree), as a String.
    #[cfg(windows)]
    unsafe fn local_text(text: *mut u16) -> String {
        // SAFETY: a NUL-terminated string the system allocated, freed here.
        unsafe {
            let len = (0..).take_while(|&i| *text.add(i) != 0).count();
            let s = String::from_utf16_lossy(std::slice::from_raw_parts(text, len));
            windows_sys::Win32::Foundation::LocalFree(text.cast());
            s
        }
    }

    /// This process's user SID as text (`S-1-5-21-...`).
    #[cfg(windows)]
    fn user_sid() -> String {
        use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
        use windows_sys::Win32::Security::{GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenUser};
        use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
        // SAFETY: a buffer sized by the size query, the token handle closed before returning.
        unsafe {
            let mut token: HANDLE = std::ptr::null_mut();
            assert_ne!(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token), 0);
            let mut needed = 0;
            GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut needed);
            let mut user = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
            assert_ne!(GetTokenInformation(token, TokenUser, user.as_mut_ptr().cast(), needed, &mut needed), 0);
            CloseHandle(token);
            let mut text = std::ptr::null_mut();
            assert_ne!(ConvertSidToStringSidW((*(user.as_ptr() as *const TOKEN_USER)).User.Sid, &mut text), 0);
            local_text(text)
        }
    }

    /// `path`'s owner, group and access list, in a buffer for the security calls.
    #[cfg(windows)]
    fn security_of(path: &Path) -> Vec<usize> {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Security::{DACL_SECURITY_INFORMATION, GetFileSecurityW, GROUP_SECURITY_INFORMATION, OWNER_SECURITY_INFORMATION};
        let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        // SAFETY: NUL-terminated name, a buffer sized by the size query.
        unsafe {
            let mut needed = 0;
            GetFileSecurityW(name.as_ptr(), information, std::ptr::null_mut(), 0, &mut needed);
            let mut sd = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
            assert_ne!(GetFileSecurityW(name.as_ptr(), information, sd.as_mut_ptr().cast(), needed, &mut needed), 0, "{}", path.display());
            sd
        }
    }

    /// `path`'s access list in SDDL (`D:P(A;OICI;FA;;;SY)...`).
    #[cfg(windows)]
    fn dacl_sddl(path: &Path) -> String {
        let sd = security_of(path);
        let mut text = std::ptr::null_mut();
        // SAFETY: a security descriptor GetFileSecurityW filled; the text is freed by local_text.
        unsafe {
            assert_ne!(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.as_ptr().cast(), 1, windows_sys::Win32::Security::DACL_SECURITY_INFORMATION, &mut text, std::ptr::null_mut()), 0);
            local_text(text)
        }
    }

    /// Whether a start of this user WITHOUT elevation may read and write `path`: Windows' own access check with this
    /// process's token restricted the way a start without elevation has it (BUILTIN\Administrators for deny only). Run
    /// elevated (as the win1 runner is), this tells what a later normal start can do with what an elevated one made.
    #[cfg(windows)]
    fn usable_without_elevation(path: &Path) -> bool {
        use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
        use windows_sys::Win32::Security::{AccessCheck, CreateRestrictedToken, CreateWellKnownSid, DuplicateToken, GENERIC_MAPPING, MapGenericMask,
            SECURITY_MAX_SID_SIZE, SID_AND_ATTRIBUTES, SecurityImpersonation, TOKEN_DUPLICATE, TOKEN_QUERY, WinBuiltinAdministratorsSid};
        use windows_sys::Win32::Storage::FileSystem::{FILE_ALL_ACCESS, FILE_GENERIC_EXECUTE, FILE_GENERIC_READ, FILE_GENERIC_WRITE};
        use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
        let mut sd = security_of(path);
        // SAFETY: buffers of the documented sizes; every handle is closed before returning.
        unsafe {
            let mut admins = vec![0usize; (SECURITY_MAX_SID_SIZE as usize).div_ceil(std::mem::size_of::<usize>())];
            let mut size = SECURITY_MAX_SID_SIZE;
            assert_ne!(CreateWellKnownSid(WinBuiltinAdministratorsSid, std::ptr::null_mut(), admins.as_mut_ptr().cast(), &mut size), 0);
            let (mut token, mut restricted, mut client): (HANDLE, HANDLE, HANDLE) = (std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
            assert_ne!(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &mut token), 0);
            let deny_only = SID_AND_ATTRIBUTES { Sid: admins.as_mut_ptr().cast(), Attributes: 0 };
            assert_ne!(CreateRestrictedToken(token, 0, 1, &deny_only, 0, std::ptr::null(), 0, std::ptr::null(), &mut restricted), 0);
            assert_ne!(DuplicateToken(restricted, SecurityImpersonation, &mut client), 0);
            let mapping = GENERIC_MAPPING { GenericRead: FILE_GENERIC_READ, GenericWrite: FILE_GENERIC_WRITE, GenericExecute: FILE_GENERIC_EXECUTE, GenericAll: FILE_ALL_ACCESS };
            let mut want = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
            MapGenericMask(&mut want, &mapping);
            let mut privileges = [0usize; 64];
            let mut privileges_len = std::mem::size_of_val(&privileges) as u32;
            let (mut granted, mut status) = (0u32, 0);
            let ok = AccessCheck(sd.as_mut_ptr().cast(), client, want, &mapping, privileges.as_mut_ptr().cast(), &mut privileges_len, &mut granted, &mut status);
            let error = std::io::Error::last_os_error();
            CloseHandle(client); CloseHandle(restricted); CloseHandle(token);
            assert_ne!(ok, 0, "access check on {}: {error}", path.display());
            status != 0 && granted & want == want
        }
    }

    /// A launcher started as administrator writes its Nexus records as BUILTIN\Administrators (the elevated token's
    /// default owner). A later start of the same player without elevation must still read and replace them, as it does
    /// the folder and its locks; and nobody else is let in: the folder's protected access list names only the user and
    /// SYSTEM.
    #[cfg(windows)]
    #[test]
    fn what_an_elevated_start_writes_stays_usable_without_elevation_and_private() {
        let root = temp();
        let state = root.join("state");
        private_dir_inner(&state).unwrap();
        drop(open_lock(&state, "test.lock").unwrap());
        let inbox = state.join("inbox");
        private_dir_inner(&inbox).unwrap();
        let record = state.join("record.json");
        let link = inbox.join("0000000000001-00000000.json");
        for p in [&record, &link] {
            if crate::nexus::enabled() {
                crate::nexus::keystore::write_private(p, &serde_json::json!({ "url": "nxm://enshrouded/mods/1/files/2" })).unwrap();
            } else {
                std::fs::write(p, b"{}").unwrap(); // as write_private makes it: a new file in the folder
            }
        }
        for p in [&state, &state.join("test.lock"), &inbox, &record, &link] {
            assert!(usable_without_elevation(p), "a start without elevation cannot use {}: {}", p.display(), dacl_sddl(p));
        }
        let user = user_sid();
        for dir in [&state, &inbox] {
            let sddl = dacl_sddl(dir);
            assert!(sddl.starts_with("D:P"), "protected, nothing inherited: {sddl}");
            for ace in sddl.split('(').skip(1) {
                let ace = ace.trim_end_matches(')');
                let sid = ace.rsplit(';').next().unwrap();
                assert!(ace.starts_with("A;") && (sid == user || sid == "SY" || sid == "OW"), "only the user and SYSTEM: {sddl}");
            }
        }
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn registration_is_serialized_and_does_not_override_a_later_choice() {
        let root = temp();
        let os = std::sync::Arc::new(Fake { current: std::sync::Mutex::new("original.desktop".into()), root: root.clone(), fail: false });
        std::thread::scope(|scope| {
            for _ in 0..2 {
                let os = os.clone(); let root = root.clone();
                scope.spawn(move || { register(&root, os.as_ref(), Path::new("/kiln")).unwrap(); });
            }
        });
        assert_eq!(state(&root).previous, Some(Previous::Desktop { id: "original.desktop".into() }));
        *os.current.lock().unwrap() = "new-choice.desktop".into();
        unregister(&root, os.as_ref()).unwrap();
        assert_eq!(*os.current.lock().unwrap(), "new-choice.desktop");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_registration_rolls_back_from_persisted_recovery() {
        let root = temp();
        let os = Fake { current: std::sync::Mutex::new("original.desktop".into()), root: root.clone(), fail: true };
        assert!(register(&root, &os, Path::new("/kiln")).is_err());
        assert_eq!(*os.current.lock().unwrap(), "original.desktop");
        assert!(state(&root).previous.is_some());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn invalid_or_future_recovery_state_is_preserved_without_changing_the_handler() {
        let root = temp();
        let os = Fake { current: std::sync::Mutex::new(DESKTOP_ID.into()), root: root.clone(), fail: false };
        for bytes in [b"{broken".as_slice(), br#"{"format":3,"registered":true}"#.as_slice()] {
            std::fs::write(state_path(&root), bytes).unwrap();
            assert!(register(&root, &os, Path::new("/kiln")).is_err());
            assert!(unregister(&root, &os).is_err());
            assert_eq!(*os.current.lock().unwrap(), DESKTOP_ID);
            assert_eq!(std::fs::read(state_path(&root)).unwrap(), bytes);
        }
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn mime_edits_only_nxm_default() {
        let original = "# comment\n[Default Applications]\ntext/plain=editor.desktop\nx-scheme-handler/nxm=first.desktop;second.desktop;\nx-scheme-handler/https=browser.desktop\n[Added Associations]\nx-scheme-handler/nxm=extra.desktop;\n";
        let edited = mime_default(original, Some(DESKTOP_ID));
        assert!(edited.contains("text/plain=editor.desktop\n"));
        assert!(edited.contains("x-scheme-handler/https=browser.desktop\n"));
        assert!(edited.contains("[Added Associations]\nx-scheme-handler/nxm=extra.desktop;\n"));
        assert!(!edited.contains("first.desktop"));
    }

    #[test]
    fn desktop_path_escapes_field_codes() {
        assert_eq!(exec_quote("/apps/Kiln%u.AppImage"), "\"/apps/Kiln%%u.AppImage\"");
        assert_eq!(fill_command("manager.exe %1", "nxm://other/mods/1/files/2?key=a&expires=3"), "manager.exe \"nxm://other/mods/1/files/2?key=a&expires=3\"");
    }

    #[test]
    fn registry_snapshots_cannot_redirect_restoration_to_other_schemes() {
        assert!(nxm_registry_slot(r"Software\Classes\nxm", Some("URL Protocol")));
        assert!(nxm_registry_slot(r"Software\Classes\nxm\shell\open\command", None));
        assert!(!nxm_registry_slot(r"Software\Classes\https\shell\open\command", None));
        assert!(!nxm_registry_slot(r"Software\Classes\nxm\..\https", None));
        assert!(!nxm_registry_slot(r"Software\Classes\nxm", Some("other")));
    }

    #[cfg(unix)]
    #[test]
    fn inbox_is_private_bounded_and_rejects_links_and_symlinks() {
        use std::os::unix::fs::{symlink, PermissionsExt};
        let root = temp();
        let outside = temp();
        let valid = "nxm://enshrouded/mods/1/files/2?key=a&expires=1";
        assert!(queue_link(&root, "nxm://skyrim/mods/1/files/2").is_err());
        queue_link(&root, valid).unwrap();
        assert_eq!(std::fs::metadata(root.join("inbox")).unwrap().permissions().mode() & 0o777, 0o700);
        std::fs::write(outside.join("secret"), r#"{"url":"nxm://enshrouded/mods/3/files/4"}"#).unwrap();
        symlink(outside.join("secret"), root.join("inbox/other.json")).unwrap();
        std::fs::write(root.join("inbox/big.json"), vec![b' '; 17000]).unwrap();
        let public = root.join("inbox/public.json");
        std::fs::write(&public, r#"{"url":"nxm://enshrouded/mods/5/files/6"}"#).unwrap();
        std::fs::set_permissions(&public, std::fs::Permissions::from_mode(0o666)).unwrap();
        assert_eq!(take_links(&root), vec![valid]);
        assert!(outside.join("secret").is_file());
        symlink(outside.join("secret"), root.join("instance.lock")).unwrap();
        assert!(acquire_instance(&root).is_err());
        let linked = root.join("linked"); symlink(&outside, &linked).unwrap();
        assert!(queue_link(&linked, valid).is_err());
        std::fs::remove_dir_all(root).unwrap(); std::fs::remove_dir_all(outside).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn a_process_for_a_link_finds_the_running_launcher_without_writing() {
        use std::os::unix::fs::symlink;
        let root = temp();
        let nexus = root.join("nexus");
        // no launcher took the lock in this folder yet: nothing found, nothing made (not even the private folder)
        assert!(matches!(find_instance(&nexus).unwrap(), Found::Free(None)));
        assert!(!nexus.exists());
        let running = acquire_instance(&nexus).unwrap().expect("the first launcher");
        assert!(matches!(find_instance(&nexus).unwrap(), Found::Running));
        drop(running);
        let Found::Free(Some(mine)) = find_instance(&nexus).unwrap() else { panic!("the lock was free") };
        assert!(acquire_instance(&nexus).unwrap().is_none(), "this process holds it now");
        drop(mine);
        assert_eq!(std::fs::metadata(nexus.join("instance.lock")).unwrap().len(), 0);
        // a link where the lock should be is never followed
        let outside = temp();
        std::fs::write(outside.join("lock"), b"").unwrap();
        std::fs::remove_file(nexus.join("instance.lock")).unwrap();
        symlink(outside.join("lock"), nexus.join("instance.lock")).unwrap();
        assert!(find_instance(&nexus).is_err());
        std::fs::remove_dir_all(root).unwrap(); std::fs::remove_dir_all(outside).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn desktop_specific_default_takes_precedence_and_is_restored() {
        let root = temp(); let config = root.join("config"); let data = root.join("data");
        std::fs::create_dir_all(&config).unwrap(); std::fs::create_dir_all(data.join("applications")).unwrap();
        let generic = "[Default Applications]\nx-scheme-handler/nxm=generic.desktop;\ntext/plain=editor.desktop\n";
        let specific = "[Default Applications]\nx-scheme-handler/nxm=other.desktop;fallback.desktop;\nx-scheme-handler/https=browser.desktop\n";
        std::fs::write(config.join("mimeapps.list"), generic).unwrap();
        std::fs::write(config.join("reviewdesk-mimeapps.list"), specific).unwrap();
        let os = LinuxMime { data_home: data, config_home: config.clone(), data_dirs: vec![], env: vec![("XDG_CURRENT_DESKTOP".into(), "ReviewDesk".into())] };
        let snapshot = os.snapshot().unwrap(); os.set_default(Some(DESKTOP_ID)).unwrap();
        assert!(std::fs::read_to_string(config.join("reviewdesk-mimeapps.list")).unwrap().contains(DESKTOP_ID));
        assert_eq!(std::fs::read_to_string(config.join("mimeapps.list")).unwrap(), generic);
        os.restore_snapshot(None, snapshot.as_ref()).unwrap();
        assert_eq!(std::fs::read_to_string(config.join("reviewdesk-mimeapps.list")).unwrap(), specific);
        assert_eq!(std::fs::read_to_string(config.join("mimeapps.list")).unwrap(), generic);
        std::fs::remove_dir_all(root).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn saved_mime_paths_cannot_redirect_restoration_outside_user_config() {
        let root = temp();
        let outside = root.join("other-mimeapps.list");
        std::fs::write(&outside, "keep me").unwrap();
        let os = LinuxMime { data_home: root.join("data"), config_home: root.join("config"), data_dirs: vec![], env: vec![] };
        let snapshot = serde_json::json!({"mime": null, "desktop": null, "path": outside});
        assert!(os.restore_snapshot(None, Some(&snapshot)).is_err());
        assert_eq!(std::fs::read_to_string(outside).unwrap(), "keep me");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn linux_snapshot_restores_exact_default_and_preserves_other_edits() {
        let root = temp();
        let data = root.join("data"); let config = root.join("config");
        std::fs::create_dir_all(data.join("applications")).unwrap();
        std::fs::create_dir_all(&config).unwrap();
        let os = LinuxMime { data_home: data, config_home: config.clone(), data_dirs: vec![], env: vec![] };
        let original = "[Default Applications]\nnxm-unrelated=keep\nx-scheme-handler/nxm=first.desktop;second.desktop;\ntext/plain=old.desktop\n";
        std::fs::write(config.join("mimeapps.list"), original).unwrap();
        let saved = os.snapshot().unwrap();
        os.set_default(Some(DESKTOP_ID)).unwrap();
        os.restore_snapshot(None, saved.as_ref()).unwrap();
        assert_eq!(std::fs::read_to_string(config.join("mimeapps.list")).unwrap(), original);
        os.set_default(Some(DESKTOP_ID)).unwrap();
        let concurrent = std::fs::read_to_string(config.join("mimeapps.list")).unwrap().replace("text/plain=old.desktop", "text/plain=new.desktop");
        std::fs::write(config.join("mimeapps.list"), concurrent).unwrap();
        os.restore_snapshot(None, saved.as_ref()).unwrap();
        let restored = std::fs::read_to_string(config.join("mimeapps.list")).unwrap();
        assert!(restored.contains("x-scheme-handler/nxm=first.desktop;second.desktop;\n"));
        assert!(restored.contains("text/plain=new.desktop"));
        std::fs::remove_dir_all(root).unwrap();
    }
}
