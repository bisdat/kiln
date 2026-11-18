//! The launcher's Kiln session: the key the relay gave at sign-in and the account it belongs to. It is the only key the
//! launcher holds for the Discord features; it is never a Discord token.
//!
//! It lives in the system's keyring when there is one (Linux: the Secret Service through `secret-tool`, attributes
//! `application=kiln-launcher service=kiln-community`; Windows: Credential Manager, `Kiln Launcher/kiln-community`),
//! else in `community-session.json` in the data folder, readable by the user only. `KILN_NO_KEYRING=1` uses the file.
//! A key kept for another relay host is not used (and is not sent anywhere).

use super::Account;
use crate::paths::DataDirs;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Where a key is kept.
pub trait Vault: Send + Sync {
    fn get(&self) -> Result<Option<String>>;
    fn set(&self, secret: &str) -> Result<()>;
    fn clear(&self) -> Result<()>;
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Stored {
    pub format: u32,
    /// The relay host the key belongs to.
    pub relay: String,
    pub session: String,
    /// The account as last read (so the page can say who is signed in while offline).
    pub account: Account,
}

impl Stored {
    fn parse(text: &str) -> Option<Stored> {
        if text.len() > 8 * 1024 {
            return None;
        }
        let s: Stored = serde_json::from_str(text).ok()?;
        (s.format == 1 && super::relay::check_session(&s.session).is_ok() && s.account.validate().is_ok()).then_some(s)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Place {
    Keyring,
    File,
}

pub struct SessionStore {
    file: PathBuf,
    vault: Option<Box<dyn Vault>>,
}

impl SessionStore {
    pub fn new(dirs: &DataDirs) -> SessionStore {
        let off = std::env::var("KILN_NO_KEYRING").is_ok_and(|v| !v.is_empty() && v != "0");
        SessionStore { file: dirs.root.join("community-session.json"), vault: if off { None } else { platform_vault() } }
    }

    pub fn with_vault(dirs: &DataDirs, vault: Option<Box<dyn Vault>>) -> SessionStore {
        SessionStore { file: dirs.root.join("community-session.json"), vault }
    }

    pub fn file_path(&self) -> &Path {
        &self.file
    }

    /// The kept session for `relay_host`, if any. A keyring that can't be read counts as no session.
    pub fn load(&self, relay_host: &str) -> Option<Stored> {
        let from_vault = self.vault.as_ref().and_then(|v| v.get().ok().flatten()).and_then(|t| Stored::parse(&t));
        let s = from_vault.or_else(|| std::fs::read_to_string(&self.file).ok().and_then(|t| Stored::parse(&t)))?;
        (s.relay == relay_host).then_some(s)
    }

    /// Keeps the session: the keyring first, the file when there is none (or it refuses).
    pub fn save(&self, s: &Stored) -> Result<Place> {
        let text = serde_json::to_string(s)?;
        if let Some(v) = &self.vault
            && v.set(&text).is_ok()
        {
            let _ = std::fs::remove_file(&self.file);
            return Ok(Place::Keyring);
        }
        write_private(&self.file, text.as_bytes())?;
        Ok(Place::File)
    }

    /// Forgets the session in both places.
    pub fn clear(&self) -> Result<()> {
        let ring = self.vault.as_ref().map(|v| v.clear()).unwrap_or(Ok(()));
        match std::fs::remove_file(&self.file) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(crate::util::io_at(&self.file, "removing")(e)),
        }
        ring
    }
}

/// Writes a file only the user can read (0600 on Unix), atomically.
fn write_private(path: &Path, bytes: &[u8]) -> Result<()> {
    use std::io::Write;
    let dir = path.parent().ok_or_else(|| Error::invalid("no folder"))?;
    std::fs::create_dir_all(dir).map_err(crate::util::io_at(dir, "creating"))?;
    let tmp = dir.join(format!(".community-{}.tmp", crate::hash::hex(&crate::util::random_bytes::<8>()?)));
    let mut o = std::fs::OpenOptions::new();
    o.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        o.mode(0o600);
    }
    let r = (|| {
        let mut f = o.open(&tmp).map_err(crate::util::io_at(&tmp, "writing"))?;
        f.write_all(bytes)?;
        f.sync_all()?;
        drop(f);
        crate::util::rename_replace(&tmp, path)
    })();
    if r.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    r
}

#[cfg(not(any(target_os = "linux", windows)))]
fn platform_vault() -> Option<Box<dyn Vault>> {
    None
}

// ---------------------------------------------------------------- Linux: secret-tool

#[cfg(target_os = "linux")]
fn platform_vault() -> Option<Box<dyn Vault>> {
    crate::util::which("secret-tool").map(|exe| Box::new(SecretTool { exe }) as Box<dyn Vault>)
}

#[cfg(target_os = "linux")]
struct SecretTool {
    exe: PathBuf,
}

#[cfg(target_os = "linux")]
const ATTRS: [&str; 4] = ["application", "kiln-launcher", "service", "kiln-community"];

#[cfg(target_os = "linux")]
impl SecretTool {
    /// secret-tool with a 5 s limit: a keyring waiting for an unlock nobody answers must not hang the launcher.
    fn run(&self, args: &[&str], stdin: Option<&str>) -> Result<(bool, String)> {
        use std::io::{Read, Write};
        use std::process::Stdio;
        let mut child = crate::hostenv::command(&self.exe)
            .args(args)
            .args(ATTRS)
            .stdin(if stdin.is_some() { Stdio::piped() } else { Stdio::null() })
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|e| Error::new(ErrorCode::Unsupported, format!("secret-tool: {e}")))?;
        if let Some(s) = stdin
            && let Some(mut i) = child.stdin.take()
        {
            let _ = i.write_all(s.as_bytes());
        }
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        loop {
            if let Some(st) = child.try_wait()? {
                let mut out = String::new();
                if let Some(mut o) = child.stdout.take() {
                    let _ = o.read_to_string(&mut out);
                }
                return Ok((st.success(), out));
            }
            if std::time::Instant::now() > deadline {
                let _ = child.kill();
                let _ = child.wait();
                return Err(Error::new(ErrorCode::Timeout, "the keyring did not answer"));
            }
            std::thread::sleep(std::time::Duration::from_millis(30));
        }
    }
}

#[cfg(target_os = "linux")]
impl Vault for SecretTool {
    fn get(&self) -> Result<Option<String>> {
        let (ok, out) = self.run(&["lookup"], None)?;
        Ok(ok.then(|| out.trim().to_string()).filter(|s| !s.is_empty()))
    }
    fn set(&self, secret: &str) -> Result<()> {
        let (ok, _) = self.run(&["store", "--label=Kiln Launcher: Kiln Discord sign-in"], Some(secret))?;
        if ok { Ok(()) } else { Err(Error::new(ErrorCode::Unsupported, "the keyring refused the sign-in")) }
    }
    fn clear(&self) -> Result<()> {
        let (ok, _) = self.run(&["clear"], None)?;
        if ok || self.get()?.is_none() { Ok(()) } else { Err(Error::new(ErrorCode::Io, "The keyring could not remove the Kiln Discord sign-in.")) }
    }
}

// ---------------------------------------------------------------- Windows: Credential Manager

#[cfg(windows)]
fn platform_vault() -> Option<Box<dyn Vault>> {
    Some(Box::new(WinCred))
}

#[cfg(windows)]
struct WinCred;

#[cfg(windows)]
fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

#[cfg(windows)]
const TARGET: &str = "Kiln Launcher/kiln-community";

#[cfg(windows)]
impl Vault for WinCred {
    fn get(&self) -> Result<Option<String>> {
        use windows_sys::Win32::Security::Credentials::{CRED_TYPE_GENERIC, CREDENTIALW, CredFree, CredReadW};
        let target = wide(TARGET);
        let mut p: *mut CREDENTIALW = std::ptr::null_mut();
        // SAFETY: target is NUL-terminated; on success p points to a CREDENTIALW the API allocated, freed below.
        unsafe {
            if CredReadW(target.as_ptr(), CRED_TYPE_GENERIC, 0, &mut p) == 0 {
                let code = windows_sys::Win32::Foundation::GetLastError();
                return if code == windows_sys::Win32::Foundation::ERROR_NOT_FOUND { Ok(None) } else { Err(Error::new(ErrorCode::Io, "Credential Manager could not read the Kiln Discord sign-in.")) };
            }
            if p.is_null() {
                return Ok(None);
            }
            let c = &*p;
            let blob = if c.CredentialBlobSize == 0 { &[][..] } else { std::slice::from_raw_parts(c.CredentialBlob, c.CredentialBlobSize as usize) };
            let s = String::from_utf8_lossy(blob).into_owned();
            CredFree(p as *const core::ffi::c_void);
            Ok(Some(s))
        }
    }
    fn set(&self, secret: &str) -> Result<()> {
        use windows_sys::Win32::Security::Credentials::{CRED_PERSIST_LOCAL_MACHINE, CRED_TYPE_GENERIC, CREDENTIALW, CredWriteW};
        let target = wide(TARGET);
        let user = wide("kiln");
        let mut blob = secret.as_bytes().to_vec();
        // SAFETY: every pointer in the struct outlives the call; CredWriteW copies what it keeps.
        let ok = unsafe {
            let mut c: CREDENTIALW = std::mem::zeroed();
            c.Type = CRED_TYPE_GENERIC;
            c.TargetName = target.as_ptr() as *mut u16;
            c.UserName = user.as_ptr() as *mut u16;
            c.CredentialBlobSize = blob.len() as u32;
            c.CredentialBlob = blob.as_mut_ptr();
            c.Persist = CRED_PERSIST_LOCAL_MACHINE;
            CredWriteW(&c, 0)
        };
        if ok != 0 { Ok(()) } else { Err(Error::new(ErrorCode::Unsupported, "Credential Manager refused the sign-in")) }
    }
    fn clear(&self) -> Result<()> {
        use windows_sys::Win32::Security::Credentials::{CRED_TYPE_GENERIC, CredDeleteW};
        let target = wide(TARGET);
        // SAFETY: NUL-terminated target.
        let ok = unsafe { CredDeleteW(target.as_ptr(), CRED_TYPE_GENERIC, 0) };
        if ok != 0 || unsafe { windows_sys::Win32::Foundation::GetLastError() } == windows_sys::Win32::Foundation::ERROR_NOT_FOUND {
            Ok(())
        } else {
            Err(Error::new(ErrorCode::Io, "Credential Manager could not remove the Kiln Discord sign-in."))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    #[derive(Default, Clone)]
    struct Ring(Arc<Mutex<Option<String>>>, Arc<Mutex<bool>>);
    impl Vault for Ring {
        fn get(&self) -> Result<Option<String>> {
            Ok(self.0.lock().unwrap().clone())
        }
        fn set(&self, s: &str) -> Result<()> {
            if *self.1.lock().unwrap() {
                return Err(Error::new(ErrorCode::Unsupported, "locked"));
            }
            *self.0.lock().unwrap() = Some(s.to_string());
            Ok(())
        }
        fn clear(&self) -> Result<()> {
            *self.0.lock().unwrap() = None;
            Ok(())
        }
    }

    fn stored(relay: &str) -> Stored {
        Stored { format: 1, relay: relay.into(), session: "kiln_s_0123456789abcdefghijklmnop".into(),
            account: Account { id: "731204485922648124".into(), name: "emberfox".into(), member: true, tester: false } }
    }

    #[test]
    fn the_keyring_holds_the_session_and_no_file_is_left() {
        let t = super::super::testing::tmp("session-ring");
        let d = DataDirs::at(t.path());
        let ring = Ring::default();
        let s = SessionStore::with_vault(&d, Some(Box::new(ring.clone())));
        assert_eq!(s.save(&stored("frostdev.io")).unwrap(), Place::Keyring);
        assert!(!s.file_path().exists());
        assert_eq!(s.load("frostdev.io").unwrap().account.name, "emberfox");
        s.clear().unwrap();
        assert!(ring.0.lock().unwrap().is_none());
        assert!(s.load("frostdev.io").is_none());
    }

    #[test]
    fn without_a_keyring_the_file_is_owner_only() {
        let t = super::super::testing::tmp("session-file");
        let d = DataDirs::at(t.path());
        let ring = Ring::default();
        *ring.1.lock().unwrap() = true; // refuses
        let s = SessionStore::with_vault(&d, Some(Box::new(ring)));
        assert_eq!(s.save(&stored("frostdev.io")).unwrap(), Place::File);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(s.file_path()).unwrap().permissions().mode() & 0o777, 0o600);
        }
        assert!(s.load("frostdev.io").is_some());
        s.clear().unwrap();
        assert!(!s.file_path().exists());
    }

    #[test]
    fn a_key_for_another_relay_is_not_used() {
        let t = super::super::testing::tmp("session-host");
        let d = DataDirs::at(t.path());
        let s = SessionStore::with_vault(&d, None);
        s.save(&stored("staging.frostdev.io")).unwrap();
        assert!(s.load("frostdev.io").is_none());
        assert!(s.load("staging.frostdev.io").is_some());
    }

    #[test]
    fn a_broken_entry_is_no_session() {
        let t = super::super::testing::tmp("session-broken");
        let d = DataDirs::at(t.path());
        let s = SessionStore::with_vault(&d, None);
        std::fs::write(s.file_path(), b"{\"format\":1,\"relay\":\"frostdev.io\",\"session\":\"x\"}").unwrap();
        assert!(s.load("frostdev.io").is_none());
    }
}
