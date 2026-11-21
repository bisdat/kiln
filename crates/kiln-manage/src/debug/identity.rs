//! What identifies a debug target (chapter 12a §4 steps 1, 5 and §5 types): the core's descriptor, the checked attach
//! tuple ([`DebugIdentity`]), the target an editor is shown ([`DebugTarget`]) and the session's state
//! ([`DebugSessionState`]).
//!
//! The descriptor is written by the native core (`native/core/src/debug_descriptor.cpp`) when kiln.json's
//! `"developer_debugging"` is true: one file per process, `<install>/kiln/state/debug/<windowsPid>.json`
//! (`schema/state-debug.schema.json`). A descriptor proves nothing by itself; the resolver compares it with what the
//! Windows side sees (the held process and the control pipe's answer) before anything attaches.

use super::errors::{DebugCode, DebugError};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// `<install>/kiln/state/<DESCRIPTOR_DIR>/`: the descriptors, and the proxy's session file.
pub const DESCRIPTOR_DIR: &str = "debug";
/// The proxy's state while a session is attached: `<install>/kiln/state/debug/session.json`.
pub const SESSION_FILE: &str = "session.json";
/// The control pipe's name: this prefix, the Windows PID, `-`, the session nonce.
pub const PIPE_PREFIX: &str = r"\\.\pipe\kiln-debug-";
/// The largest frame of the control pipe and of the helper protocol, and the largest descriptor file.
pub const MAX_FRAME: usize = 16 * 1024;
pub const DESCRIPTOR_FORMAT: u32 = 1;

/// Which kind of process: a dedicated server, or a game client (which may host a world of its own).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TargetKind {
    Server,
    Client,
}

impl TargetKind {
    pub fn as_str(self) -> &'static str {
        match self {
            TargetKind::Server => "server",
            TargetKind::Client => "client",
        }
    }
}

/// The core's descriptor, format 1, as it is on disk.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Descriptor {
    pub format: u32,
    /// Kiln's version in that process.
    pub kiln: String,
    /// `server` or `client`.
    pub side: TargetKind,
    /// 128 random bits, 32 lower-case hex digits; new at every start.
    pub session_nonce: String,
    pub windows_pid: u32,
    /// GetProcessTimes' creation FILETIME (100 ns since 1601) as an unsigned decimal string.
    #[serde(rename = "creationTime100ns")]
    pub creation_time_100ns: String,
    /// The process's image, as QueryFullProcessImageNameW names it (a Windows path).
    pub image: String,
    pub image_sha256: String,
    /// The .NET runtime the core loaded (`8.0.11`), empty when none.
    pub runtime_version: String,
    /// `\\.\pipe\kiln-debug-<windowsPid>-<sessionNonce>`.
    pub pipe: String,
    /// The process runs elevated (native Windows; false under Wine).
    pub elevated: bool,
    /// The .NET host had started when the descriptor was written (the pipe answers the current value).
    pub managed_ready: bool,
    /// When it was written (RFC 3339 UTC).
    pub written: String,
}

fn is_hex(s: &str, len: usize) -> bool {
    s.len() == len && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// An unsigned decimal string without sign or leading zeros (`creationTime100ns`), as its value.
pub fn parse_decimal(s: &str) -> Option<u64> {
    if s.is_empty() || (s.len() > 1 && s.starts_with('0')) || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

impl Descriptor {
    /// The format's rules. `file_pid`: the number in the file's name, which must be the descriptor's PID.
    pub fn validate(&self, file_pid: Option<u32>) -> Result<(), String> {
        if self.format != DESCRIPTOR_FORMAT {
            return Err(format!("format {} (this kiln reads format {DESCRIPTOR_FORMAT})", self.format));
        }
        if !is_hex(&self.session_nonce, 32) {
            return Err("sessionNonce is not 32 lower-case hex digits".into());
        }
        if self.windows_pid == 0 {
            return Err("windowsPid is 0".into());
        }
        if file_pid.is_some_and(|p| p != self.windows_pid) {
            return Err(format!("windowsPid {} differs from the file's name", self.windows_pid));
        }
        if parse_decimal(&self.creation_time_100ns).is_none_or(|t| t == 0) {
            return Err("creationTime100ns is not a positive decimal string".into());
        }
        if !is_hex(&self.image_sha256, 64) {
            return Err("imageSha256 is not 64 lower-case hex digits".into());
        }
        if self.image.trim().is_empty() {
            return Err("image is empty".into());
        }
        if self.pipe != pipe_name(self.windows_pid, &self.session_nonce) {
            return Err("pipe is not the name made from windowsPid and sessionNonce".into());
        }
        Ok(())
    }
}

/// `\\.\pipe\kiln-debug-<pid>-<nonce>`: the name the core gives its control pipe.
pub fn pipe_name(windows_pid: u32, nonce: &str) -> String {
    format!("{PIPE_PREFIX}{windows_pid}-{nonce}")
}

/// One file of `<state>/debug/`: the descriptor, or why it was not read.
#[derive(Debug, Clone)]
pub struct DescriptorFile {
    pub path: PathBuf,
    pub read: Result<Descriptor, String>,
}

/// The descriptors in `<state>/debug/` (`<digits>.json` only; other files there are not descriptors), by PID. An
/// absent folder is an empty list.
pub fn read_descriptors(state_dir: &Path) -> Vec<DescriptorFile> {
    let dir = state_dir.join(DESCRIPTOR_DIR);
    let mut out: Vec<(u32, DescriptorFile)> = Vec::new();
    let Ok(rd) = std::fs::read_dir(&dir) else { return Vec::new() };
    for e in rd.flatten() {
        let name = e.file_name();
        let Some(stem) = name.to_str().and_then(|n| n.strip_suffix(".json")) else { continue };
        let Some(file_pid) = parse_decimal(stem).and_then(|p| u32::try_from(p).ok()) else { continue };
        let path = e.path();
        let read = read_one(&path, file_pid);
        out.push((file_pid, DescriptorFile { path, read }));
    }
    out.sort_by_key(|(p, _)| *p);
    out.into_iter().map(|(_, f)| f).collect()
}

fn read_one(path: &Path, file_pid: u32) -> Result<Descriptor, String> {
    let meta = std::fs::metadata(path).map_err(|e| e.to_string())?;
    if meta.len() > MAX_FRAME as u64 {
        return Err(format!("{} bytes (a descriptor is at most {MAX_FRAME})", meta.len()));
    }
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    let d: Descriptor = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    d.validate(Some(file_pid))?;
    Ok(d)
}

/// A Wine/Proton prefix by its canonical path and its folder's device and inode: a prefix moved or replaced under
/// the same name is another prefix.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PrefixIdentity {
    pub path: PathBuf,
    pub device: u64,
    pub inode: u64,
}

impl PrefixIdentity {
    pub fn of(prefix: &Path) -> Result<PrefixIdentity, DebugError> {
        let path = prefix
            .canonicalize()
            .map_err(|e| DebugError::new(DebugCode::NotFound, format!("the Wine prefix {} cannot be read: {e}", prefix.display())))?;
        let meta = std::fs::metadata(&path)
            .map_err(|e| DebugError::new(DebugCode::NotFound, format!("the Wine prefix {} cannot be read: {e}", path.display())))?;
        #[cfg(unix)]
        let (device, inode) = {
            use std::os::unix::fs::MetadataExt;
            (meta.dev(), meta.ino())
        };
        #[cfg(not(unix))]
        let (device, inode) = {
            let _ = meta;
            (0, 0)
        };
        Ok(PrefixIdentity { path, device, inode })
    }
}

/// A Linux process of a runner (Wine/Proton) by its birth: boot id, PID and start ticks (stat field 22). Identifies the
/// runner context's lifetime only, never the debuggee.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RunnerContext {
    pub linux_boot_id: String,
    pub pid: u32,
    pub start_ticks: u64,
}

impl RunnerContext {
    /// Read from `<proc>/sys/kernel/random/boot_id` and `<proc>/<pid>/stat` (`proc` is `/proc`, or a fixture tree).
    pub fn read(proc_root: &Path, pid: u32) -> Option<RunnerContext> {
        let boot = std::fs::read_to_string(proc_root.join("sys/kernel/random/boot_id")).ok()?;
        let stat = std::fs::read_to_string(proc_root.join(pid.to_string()).join("stat")).ok()?;
        Some(RunnerContext { linux_boot_id: boot.trim().to_string(), pid, start_ticks: crate::proc::parse_start_ticks(&stat)? })
    }
}

/// What the process is doing, as its core's atomics say (chapter 12a §4 step 7). Advisory: every attach needs consent
/// on a client whatever it says.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum SessionMode {
    /// No world.
    Menu,
    /// The process runs a server world (a dedicated server's, or a client hosting one) and no remote session.
    Hosted,
    /// Connected to a remote server. Not reported by this release: there is no connection observer.
    Joined,
    /// Cannot be said honestly (a client world without a server world, no world tracking, a faulted tick).
    #[default]
    Unknown,
}

impl SessionMode {
    pub fn parse(s: &str) -> SessionMode {
        match s {
            "menu" => SessionMode::Menu,
            "hosted" => SessionMode::Hosted,
            "joined" => SessionMode::Joined,
            _ => SessionMode::Unknown,
        }
    }
}

/// The checked attach tuple (chapter 12a §4 step 5): `(installId, prefixIdentity, runnerSha256, windowsPid,
/// creationTime100ns, imageSha256, sessionNonce)`. Two identities authorize the same attach only when every field is
/// equal; no PID is ever compared alone, and a host PID is never part of it.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DebugIdentity {
    pub install_id: String,
    /// The prefix the target runs in; None on native Windows.
    pub prefix_identity: Option<PrefixIdentity>,
    /// SHA-256 of the runner's executable (the wine that runs the prefix); None on native Windows (no runner).
    pub runner_sha256: Option<String>,
    pub windows_pid: u32,
    #[serde(rename = "creationTime100ns")]
    pub creation_time_100ns: String,
    pub image_sha256: String,
    pub session_nonce: String,
}

impl DebugIdentity {
    /// The first field that differs (its JSON name), None when the two are the same identity.
    pub fn first_difference(&self, other: &DebugIdentity) -> Option<&'static str> {
        if self.install_id != other.install_id {
            return Some("installId");
        }
        if self.prefix_identity != other.prefix_identity {
            return Some("prefixIdentity");
        }
        if self.runner_sha256 != other.runner_sha256 {
            return Some("runnerSha256");
        }
        if self.windows_pid != other.windows_pid {
            return Some("windowsPid");
        }
        if self.creation_time_100ns != other.creation_time_100ns {
            return Some("creationTime100ns");
        }
        if !self.image_sha256.eq_ignore_ascii_case(&other.image_sha256) {
            return Some("imageSha256");
        }
        if self.session_nonce != other.session_nonce {
            return Some("sessionNonce");
        }
        None
    }
}

/// What `kiln debug --json` resolves and an editor shows (chapter 12a §5).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DebugTarget {
    pub identity: DebugIdentity,
    /// The Windows image path of the process.
    pub image: String,
    pub runtime_version: String,
    /// The .NET host runs (the pipe's current answer).
    pub managed_ready: bool,
    pub session_mode: SessionMode,
    /// The Linux runner process the helper ran beside (Wine/Proton); None on native Windows.
    pub runner_context: Option<RunnerContext>,
    /// The Linux PID of the process under Wine, for display only: never compared, never used to decide anything.
    pub host_pid: Option<u32>,
}

/// Where a session is. Written to the session file and sent to the editor at every change.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionState {
    /// Identity and consent checked; the adapter is starting and attaching.
    Attaching,
    /// Attached, the debuggee runs.
    Running,
    /// Attached, the debuggee is stopped (a breakpoint, a step, a pause).
    Paused,
    /// A disconnect was sent; the adapter detaches.
    Detaching,
    /// The session is over and the debuggee was left running.
    Detached,
    /// The session is over because the debuggee ended or is no longer the resolved process.
    Ended,
}

/// The state of an attached session (chapter 12a §5).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DebugSessionState {
    pub identity: DebugIdentity,
    pub state: SessionState,
    pub session_mode: SessionMode,
    /// Why it changed: `timeout_continue`, `session_mode_changed`, `client_disconnect`, `client_gone`,
    /// `adapter_exited`, `debuggee_exited`, `identity_changed`, `attach_failed`.
    pub reason: Option<String>,
    /// RFC 3339 UTC, while paused.
    pub paused_since: Option<String>,
    /// The pause watchdog's limit; None when the session has none (a server without `--max-pause-ms`).
    pub max_pause_ms: Option<u64>,
}

/// Two Windows paths name the same file: `\\?\` dropped, `/` read as `\`, compared without case.
pub fn same_windows_path(a: &str, b: &str) -> bool {
    fn norm(s: &str) -> String {
        let s = s.strip_prefix(r"\\?\").unwrap_or(s);
        s.replace('/', "\\").to_lowercase()
    }
    norm(a) == norm(b)
}

/// The host path of a Windows path seen in a prefix: its drive letter through `<prefix>/dosdevices` (Z: is `/`).
/// On native Windows (`prefix` None) the path itself. None when it cannot be mapped.
pub fn host_path_of(image: &str, prefix: Option<&Path>) -> Option<PathBuf> {
    let image = image.strip_prefix(r"\\?\").unwrap_or(image);
    if cfg!(windows) && prefix.is_none() {
        return Some(PathBuf::from(image));
    }
    let b = image.as_bytes();
    if b.len() < 3 || !b[0].is_ascii_alphabetic() || b[1] != b':' || !matches!(b[2], b'\\' | b'/') {
        return None;
    }
    let mut out = crate::winpath::drive_target(prefix, b[0] as char)?;
    for c in image[3..].split(['\\', '/']).filter(|c| !c.is_empty() && *c != ".") {
        if c == ".." {
            return None;
        }
        out.push(c);
    }
    Some(out)
}

/// The descriptor's image is this install's executable (`install_exe`: its host path). Same name in another folder
/// is another install.
pub fn image_in_install(image: &str, install_exe: &Path, prefix: Option<&Path>) -> bool {
    let Some(host) = host_path_of(image, prefix) else { return false };
    let canon = |p: &Path| p.canonicalize().unwrap_or_else(|_| crate::util::clean(p));
    crate::steam::same_path(&crate::steam::strip_verbatim(canon(&host)), &crate::steam::strip_verbatim(canon(install_exe)))
}

#[cfg(test)]
mod tests {
    use super::*;

    pub(crate) fn descriptor(pid: u32, nonce: &str) -> Descriptor {
        Descriptor {
            format: 1,
            kiln: "0.2.0-dev".into(),
            side: TargetKind::Server,
            session_nonce: nonce.into(),
            windows_pid: pid,
            creation_time_100ns: "134070000000000000".into(),
            image: r"Z:\srv\labs\a\enshrouded_server.exe".into(),
            image_sha256: "ab".repeat(32),
            runtime_version: "8.0.11".into(),
            pipe: pipe_name(pid, nonce),
            elevated: false,
            managed_ready: true,
            written: "2026-11-13T20:00:00Z".into(),
        }
    }

    #[test]
    fn descriptor_rules() {
        let n = "0123456789abcdef0123456789abcdef";
        let d = descriptor(32, n);
        assert_eq!(d.validate(Some(32)), Ok(()));
        assert!(d.validate(Some(33)).unwrap_err().contains("file's name"));
        let mut x = d.clone();
        x.session_nonce = "0123456789ABCDEF0123456789ABCDEF".into();
        assert!(x.validate(None).is_err(), "upper-case nonce");
        let mut x = d.clone();
        x.creation_time_100ns = "0134".into();
        assert!(x.validate(None).is_err(), "leading zero");
        x.creation_time_100ns = "-5".into();
        assert!(x.validate(None).is_err(), "sign");
        x.creation_time_100ns = "0".into();
        assert!(x.validate(None).is_err(), "zero");
        let mut x = d.clone();
        x.pipe = pipe_name(33, n);
        assert!(x.validate(None).is_err(), "pipe of another pid");
        let mut x = d;
        x.format = 2;
        assert!(x.validate(None).is_err());
        // the JSON names are the schema's
        let v = serde_json::to_value(descriptor(7, n)).unwrap();
        for k in ["format", "kiln", "side", "sessionNonce", "windowsPid", "creationTime100ns", "image", "imageSha256", "runtimeVersion", "pipe", "elevated", "managedReady", "written"] {
            assert!(v.get(k).is_some(), "{k}");
        }
        assert!(v["creationTime100ns"].is_string());
    }

    #[test]
    fn descriptors_are_read_by_pid_and_bad_ones_say_why() {
        let t = std::env::temp_dir().join(format!("kiln-debug-desc-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let dir = t.join(DESCRIPTOR_DIR);
        std::fs::create_dir_all(&dir).unwrap();
        let n = "0123456789abcdef0123456789abcdef";
        std::fs::write(dir.join("32.json"), serde_json::to_vec(&descriptor(32, n)).unwrap()).unwrap();
        std::fs::write(dir.join("40.json"), serde_json::to_vec(&descriptor(41, n)).unwrap()).unwrap();
        std::fs::write(dir.join("50.json"), b"{not json").unwrap();
        std::fs::write(dir.join("60.json"), vec![b' '; MAX_FRAME + 1]).unwrap();
        std::fs::write(dir.join(SESSION_FILE), b"{}").unwrap();
        std::fs::write(dir.join("032.json"), b"{}").unwrap();
        let got = read_descriptors(&t);
        assert_eq!(got.len(), 4, "{got:?}");
        assert!(got[0].read.is_ok());
        assert!(got[1].read.as_ref().unwrap_err().contains("file's name"));
        assert!(got[2].read.is_err());
        assert!(got[3].read.as_ref().unwrap_err().contains("at most"));
        assert!(read_descriptors(&t.join("nowhere")).is_empty());
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[test]
    fn decimal_strings() {
        assert_eq!(parse_decimal("134070000000000000"), Some(134_070_000_000_000_000));
        assert_eq!(parse_decimal("18446744073709551615"), Some(u64::MAX));
        for bad in ["", "01", "+1", " 1", "1e3", "18446744073709551616"] {
            assert_eq!(parse_decimal(bad), None, "{bad}");
        }
    }

    #[test]
    fn every_tuple_field_counts() {
        let a = DebugIdentity {
            install_id: "i".into(),
            prefix_identity: Some(PrefixIdentity { path: "/p".into(), device: 1, inode: 2 }),
            runner_sha256: Some("r".into()),
            windows_pid: 32,
            creation_time_100ns: "5".into(),
            image_sha256: "aa".into(),
            session_nonce: "n".into(),
        };
        assert_eq!(a.first_difference(&a.clone()), None);
        let cases: Vec<(&str, Box<dyn Fn(&mut DebugIdentity)>)> = vec![
            ("installId", Box::new(|i| i.install_id = "j".into())),
            ("prefixIdentity", Box::new(|i| i.prefix_identity.as_mut().unwrap().inode = 3)),
            ("prefixIdentity", Box::new(|i| i.prefix_identity = None)),
            ("runnerSha256", Box::new(|i| i.runner_sha256 = None)),
            ("windowsPid", Box::new(|i| i.windows_pid = 33)),
            ("creationTime100ns", Box::new(|i| i.creation_time_100ns = "6".into())),
            ("imageSha256", Box::new(|i| i.image_sha256 = "ab".into())),
            ("sessionNonce", Box::new(|i| i.session_nonce = "m".into())),
        ];
        for (field, change) in cases {
            let mut b = a.clone();
            change(&mut b);
            assert_eq!(a.first_difference(&b), Some(field));
        }
        // digests compare without case
        let mut b = a.clone();
        b.image_sha256 = "AA".into();
        assert_eq!(a.first_difference(&b), None);
    }

    #[test]
    fn windows_paths() {
        assert!(same_windows_path(r"Z:\Srv\A.exe", r"\\?\z:\srv\a.exe"));
        assert!(same_windows_path("C:/x/y.exe", r"c:\X\Y.EXE"));
        assert!(!same_windows_path(r"Z:\srv\a\x.exe", r"Z:\srv\b\x.exe"));
    }

    #[cfg(unix)]
    #[test]
    fn image_belongs_to_one_install_only() {
        let t = std::env::temp_dir().join(format!("kiln-debug-img-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        for d in ["a", "b"] {
            std::fs::create_dir_all(t.join(d)).unwrap();
            std::fs::write(t.join(d).join("enshrouded_server.exe"), b"").unwrap();
        }
        let z = |d: &str| crate::winpath::unix_to_z(&t.join(d).join("enshrouded_server.exe")).unwrap();
        let exe_a = t.join("a/enshrouded_server.exe");
        assert!(image_in_install(&z("a"), &exe_a, None));
        assert!(!image_in_install(&z("b"), &exe_a, None), "same name, other folder");
        assert!(!image_in_install(r"C:\windows\notepad.exe", &exe_a, None), "unmapped drive");
        assert!(!image_in_install(r"Z:\..\etc", &exe_a, None));
        // a prefix's own drive mapping
        let pfx = t.join("pfx");
        std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
        std::os::unix::fs::symlink(t.join("a"), pfx.join("dosdevices/s:")).unwrap();
        assert!(image_in_install(r"S:\enshrouded_server.exe", &exe_a, Some(&pfx)));
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[test]
    fn runner_context_from_a_proc_tree() {
        let t = std::env::temp_dir().join(format!("kiln-debug-proc-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        std::fs::create_dir_all(t.join("sys/kernel/random")).unwrap();
        std::fs::create_dir_all(t.join("77")).unwrap();
        std::fs::write(t.join("sys/kernel/random/boot_id"), "b0b0-1\n").unwrap();
        // a comm with spaces and ')' must not shift the fields
        std::fs::write(t.join("77/stat"), "77 (wine) (x) S) S 1 77 77 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 98765 0 0").unwrap();
        assert_eq!(RunnerContext::read(&t, 77), Some(RunnerContext { linux_boot_id: "b0b0-1".into(), pid: 77, start_ticks: 98765 }));
        assert_eq!(RunnerContext::read(&t, 78), None);
        std::fs::remove_dir_all(&t).unwrap();
    }
}
