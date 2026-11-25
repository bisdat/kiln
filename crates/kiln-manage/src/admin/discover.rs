//! Finds a server's KAP endpoint and token: `<server>/kiln/data/kiln-admin/{endpoint.json,admin-token}`, locally or
//! over SSH for remote servers. endpoint.json is deleted when Kiln.Admin unloads; one left behind by a crashed
//! server has a dead listener, which [`probe`] tells (its `pid` is the Windows pid under Wine, so the launcher probes
//! the port instead of looking the pid up).
//!
//! (0.2.0-dev.31.12) A server that booted with a profile whose `dataDir` is `profile` keeps Kiln.Admin's folder
//! under that data folder instead ([`super::datadir`]): the booted folder is looked at first, `kiln/data` after.

use super::protocol::{Endpoint, ENDPOINT_FILE, TOKEN_FILE};
use crate::server::remote::RemoteSpec;
use crate::{Error, ErrorCode, Result};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// `<server>/kiln/data/kiln-admin` (the default place; [`live_admin_dir`] follows a profile's data folder).
pub fn admin_dir(server_dir: &Path) -> PathBuf {
    server_dir.join("kiln").join("data").join("kiln-admin")
}

/// Kiln.Admin's folder for the server as it booted: the first candidate ([`super::datadir::local_mod_dirs`]) that
/// holds endpoint.json, else the first that holds admin-token, else the default.
pub fn live_admin_dir(server_dir: &Path) -> PathBuf {
    let dirs = super::datadir::local_mod_dirs(server_dir, None, "kiln-admin");
    dirs.iter()
        .find(|d| d.join(ENDPOINT_FILE).is_file())
        .or_else(|| dirs.iter().find(|d| d.join(TOKEN_FILE).is_file()))
        .cloned()
        .unwrap_or_else(|| admin_dir(server_dir))
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
    let p = live_admin_dir(server_dir).join(ENDPOINT_FILE);
    match std::fs::read(&p) {
        Ok(b) => parse_endpoint(&b).map(Some),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(crate::util::io_at(&p, "reading")(e)),
    }
}

/// The HMAC key from admin-token (32 bytes). On Linux a token readable by others is tightened to 0600 when this user
/// owns it (Kiln.Admin under Wine cannot set the mode itself).
pub fn token(server_dir: &Path) -> Result<[u8; 32]> {
    let p = live_admin_dir(server_dir).join(TOKEN_FILE);
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
    let Some(ep) = endpoint(server_dir)? else { return Ok(None) };
    if !probe(&ep.host, ep.port, Duration::from_millis(500)) {
        return Ok(None);
    }
    Ok(Some((ep, token(server_dir)?)))
}

/// A TCP connect to the listener succeeds within `timeout`.
pub fn probe(host: &str, port: u16, timeout: Duration) -> bool {
    use std::net::ToSocketAddrs;
    let Ok(mut addrs) = (host, port).to_socket_addrs() else { return false };
    addrs.next().is_some_and(|a| std::net::TcpStream::connect_timeout(&a, timeout).is_ok())
}

/// endpoint.json of a remote server (`ssh <host> cat <dir>/kiln/data/kiln-admin/endpoint.json`, or the booted
/// profile's data folder first).
pub fn remote_endpoint(remote: &RemoteSpec) -> Result<Option<Endpoint>> {
    Ok(remote_live(remote)?.map(|(ep, _)| ep))
}

/// The remote admin-token, held in memory only (from the folder whose endpoint.json is live, else the first that has
/// one).
pub fn remote_token(remote: &RemoteSpec) -> Result<[u8; 32]> {
    let dirs = super::datadir::remote_mod_dirs(remote, "kiln-admin");
    let dir = match remote_live(remote)? {
        Some((_, d)) => d,
        None => dirs.first().cloned().unwrap_or_default(),
    };
    let b = crate::server::remote::read_abs_file(remote, &format!("{dir}/{TOKEN_FILE}"))?;
    super::protocol::parse_token(&String::from_utf8_lossy(&b))
}

/// A remote server's endpoint and token in one pass (from the same folder).
pub fn remote_endpoint_and_token(remote: &RemoteSpec) -> Result<Option<(Endpoint, [u8; 32])>> {
    let Some((ep, dir)) = remote_live(remote)? else { return Ok(None) };
    let b = crate::server::remote::read_abs_file(remote, &format!("{dir}/{TOKEN_FILE}"))?;
    Ok(Some((ep, super::protocol::parse_token(&String::from_utf8_lossy(&b))?)))
}

/// The first remote Kiln.Admin folder with an endpoint.json: (the endpoint, the folder).
fn remote_live(remote: &RemoteSpec) -> Result<Option<(Endpoint, String)>> {
    for dir in super::datadir::remote_mod_dirs(remote, "kiln-admin") {
        match crate::server::remote::read_abs_file(remote, &format!("{dir}/{ENDPOINT_FILE}")) {
            Ok(b) => return parse_endpoint(&b).map(|ep| Some((ep, dir))),
            Err(e) if e.code == ErrorCode::NotFound => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(None)
}
