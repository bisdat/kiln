//! Finds a server's KAP endpoint and token: `<server>/kiln/data/kiln-admin/{endpoint.json,admin-token}`, locally or
//! over SSH for remote servers. endpoint.json is deleted when Kiln.Admin unloads; one left behind by a crashed
//! server has a dead listener, which [`probe`] tells (its `pid` is the Windows pid under Wine, so the launcher probes
//! the port instead of looking the pid up).
//!
//! (0.2.0-dev.31.12) A server that booted with a profile whose `dataDir` is `profile` keeps Kiln.Admin's folder
//! under that data folder instead ([`super::datadir`]): the server's data root, read once, never a mix of folders.

use super::protocol::{Endpoint, ENDPOINT_FILE, TOKEN_FILE};
use crate::server::remote::RemoteSpec;
use crate::{Error, ErrorCode, Result};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// `<server>/kiln/data/kiln-admin` (the default place; [`live_admin_dir`] follows a profile's data folder).
pub fn admin_dir(server_dir: &Path) -> PathBuf {
    server_dir.join("kiln").join("data").join("kiln-admin")
}

/// Kiln.Admin's folder for the server as it booted: `<data root>/kiln-admin` ([`super::datadir::local_data_root`]).
pub fn live_admin_dir(server_dir: &Path) -> PathBuf {
    live_admin_dir_in(server_dir, None)
}

/// The same with the server's Wine prefix (data folders on drives other than Z:).
pub fn live_admin_dir_in(server_dir: &Path, prefix: Option<&Path>) -> PathBuf {
    super::datadir::local_data_root(server_dir, prefix).join("kiln-admin")
}

/// Parses endpoint.json (refuses a larger `format`).
pub fn parse_endpoint(bytes: &[u8]) -> Result<Endpoint> {
    let ep: Endpoint = serde_json::from_slice(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(bytes))
        .map_err(|e| Error::new(ErrorCode::Parse, format!("endpoint.json: {e}")))?;
    if ep.format > 1 || ep.protocol != super::protocol::PROTOCOL {
        return Err(Error::new(ErrorCode::Unsupported, format!("endpoint.json format {} / {} is newer than this launcher", ep.format, ep.protocol)));
    }
    Ok(ep)
}

/// The server's endpoint.json, None when there is none (KAP off, server stopped, an old Kiln.Admin).
pub fn endpoint(server_dir: &Path) -> Result<Option<Endpoint>> {
    endpoint_at(&live_admin_dir(server_dir))
}

/// endpoint.json in Kiln.Admin's folder `admin_dir`.
pub fn endpoint_at(admin_dir: &Path) -> Result<Option<Endpoint>> {
    let p = admin_dir.join(ENDPOINT_FILE);
    match std::fs::read(&p) {
        Ok(b) => parse_endpoint(&b).map(Some),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(crate::util::io_at(&p, "reading")(e)),
    }
}

/// The HMAC key from admin-token (32 bytes). On Linux a token readable by others is tightened to 0600 when this user
/// owns it (Kiln.Admin under Wine cannot set the mode itself).
pub fn token(server_dir: &Path) -> Result<[u8; 32]> {
    token_at(&live_admin_dir(server_dir))
}

/// admin-token in Kiln.Admin's folder `admin_dir`.
pub fn token_at(admin_dir: &Path) -> Result<[u8; 32]> {
    let p = admin_dir.join(TOKEN_FILE);
    let text = std::fs::read_to_string(&p).map_err(|e| {
        let base = Error::from(e);
        Error::new(if base.code == ErrorCode::NotFound { ErrorCode::AdminUnavailable } else { base.code }, format!("{}: {}", p.display(), base.message))
    })?;
    tighten(&p);
    super::protocol::parse_token(&text)
}

#[cfg(unix)]
fn tighten(p: &Path) {
    use std::os::unix::fs::{MetadataExt, PermissionsExt};
    if let Ok(m) = std::fs::metadata(p) {
        // SAFETY: geteuid has no preconditions.
        let me = unsafe { libc::geteuid() };
        if m.uid() == me && m.mode() & 0o077 != 0 {
            let _ = std::fs::set_permissions(p, std::fs::Permissions::from_mode(0o600));
        }
    }
}

#[cfg(not(unix))]
fn tighten(_: &Path) {}

/// A local server's live KAP endpoint and token: None when there is no endpoint.json or its listener does not answer
/// (a stale file left by a crashed server).
pub fn local(server_dir: &Path) -> Result<Option<(Endpoint, [u8; 32])>> {
    let dir = live_admin_dir(server_dir);
    let Some(ep) = endpoint_at(&dir)? else { return Ok(None) };
    if !probe(&ep.host, ep.port, Duration::from_millis(500)) {
        return Ok(None);
    }
    Ok(Some((ep, token_at(&dir)?)))
}

/// A TCP connect to the listener succeeds within `timeout`.
pub fn probe(host: &str, port: u16, timeout: Duration) -> bool {
    use std::net::ToSocketAddrs;
    let Ok(mut addrs) = (host, port).to_socket_addrs() else { return false };
    addrs.next().is_some_and(|a| std::net::TcpStream::connect_timeout(&a, timeout).is_ok())
}

/// Kiln.Admin's folder on a remote server as it booted (`<data root>/kiln-admin`, boot.json read over ssh).
pub fn remote_admin_dir(remote: &RemoteSpec) -> String {
    format!("{}/kiln-admin", super::datadir::remote_data_root(remote))
}

/// endpoint.json of a remote server (`ssh <host> cat <data root>/kiln-admin/endpoint.json`; the data root is
/// `<dir>/kiln/data` unless the server booted with a profile's data folder).
pub fn remote_endpoint(remote: &RemoteSpec) -> Result<Option<Endpoint>> {
    remote_endpoint_at(remote, &remote_admin_dir(remote))
}

fn remote_endpoint_at(remote: &RemoteSpec, admin_dir: &str) -> Result<Option<Endpoint>> {
    match crate::server::remote::read_abs_file(remote, &format!("{admin_dir}/{ENDPOINT_FILE}")) {
        Ok(b) => parse_endpoint(&b).map(Some),
        Err(e) if e.code == ErrorCode::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

/// The remote admin-token, held in memory only.
pub fn remote_token(remote: &RemoteSpec) -> Result<[u8; 32]> {
    let b = crate::server::remote::read_abs_file(remote, &format!("{}/{TOKEN_FILE}", remote_admin_dir(remote)))?;
    super::protocol::parse_token(&String::from_utf8_lossy(&b))
}

/// A remote server's endpoint and token, both from one folder (boot.json read once).
pub fn remote_endpoint_and_token(remote: &RemoteSpec) -> Result<Option<(Endpoint, [u8; 32])>> {
    remote_endpoint_and_token_at(remote, &remote_admin_dir(remote))
}

/// The same from Kiln.Admin's folder `admin_dir` (an absolute path on the remote machine).
pub fn remote_endpoint_and_token_at(remote: &RemoteSpec, admin_dir: &str) -> Result<Option<(Endpoint, [u8; 32])>> {
    let Some(ep) = remote_endpoint_at(remote, admin_dir)? else { return Ok(None) };
    let b = crate::server::remote::read_abs_file(remote, &format!("{admin_dir}/{TOKEN_FILE}"))?;
    Ok(Some((ep, super::protocol::parse_token(&String::from_utf8_lossy(&b))?)))
}
