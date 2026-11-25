//! One loopback address per server, for the life of the launcher's data folder: `web-panel-origins.json` there.
//!
//! Browsers keep cookies per host name, not per port, so panels of different servers on 127.0.0.1 shared their
//! `__Host-wa-session` cookies and a tab of server A sent server B's session to A. Each server gets its own address in
//! 127.64.0.0 - 127.254.255.255 (all of 127.0.0.0/8 is loopback on Linux and Windows; 127.0.x.x is left to the system:
//! 127.0.0.1, systemd-resolved's 127.0.0.53, Debian's 127.0.1.1), picked from a hash of the server id and kept: an
//! address is never given to another server, even after its server is removed, so no old cookie reaches a new
//! server. The port is remembered too, so the panel keeps its origin (and its local storage) between opens.
//!
//! (Review of 0.2.0-dev.31.12)
//! - The address belongs to the server *and its panel's certificate* ([`key`]): a server whose files point the
//!   launcher at another panel (a hostile server's boot.json naming another server's data folder) gets that panel on
//!   another address, so the session cookie that panel sets is never sent to this server's own panel later. A new
//!   certificate costs a new address (and the panel's local storage); Open web panel signs in again by itself.
//! - An entry read from the file is used only when it is an address this module gives out (127.64.0.0 -
//!   127.254.255.255, last byte 1-254): a file edited to say 0.0.0.0 or a LAN address never makes a gateway listen
//!   there.
//! - Reads and writes hold `web-panel-origins.json.lock`, so two launchers on one data folder don't lose each other's
//!   entries.

use crate::Result;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::net::Ipv4Addr;
use std::path::Path;

pub const FILE: &str = "web-panel-origins.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Slot {
    pub ip: Ipv4Addr,
    /// 0: none chosen yet.
    pub port: u16,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct OriginsFile {
    format: u32,
    servers: BTreeMap<String, Slot>,
}

/// The registry key of a server's panel: the server id, and the pinned certificate's SHA-256 when it has TLS.
pub fn key(server_id: &str, pin: Option<&str>) -> String {
    match pin {
        Some(p) => format!("{server_id}@{}", p.to_ascii_lowercase()),
        None => server_id.to_string(),
    }
}

/// Whether `ip` is one this module gives out ([`derive`]'s range).
pub fn ours(ip: Ipv4Addr) -> bool {
    let o = ip.octets();
    o[0] == 127 && (64..=254).contains(&o[1]) && (1..=254).contains(&o[3])
}

fn lock(path: &Path) -> Result<crate::state::HeldLock> {
    let mut l = path.as_os_str().to_owned();
    l.push(".lock");
    crate::state::lock_file(Path::new(&l), std::time::Duration::from_secs(10), &|| false, "another Kiln Launcher is writing web-panel-origins.json")
}

fn read(path: &Path) -> OriginsFile {
    std::fs::read(path)
        .ok()
        .and_then(|b| serde_json::from_slice::<OriginsFile>(&b).ok())
        .filter(|f| f.format == 1)
        .unwrap_or(OriginsFile { format: 1, servers: BTreeMap::new() })
}

/// The `n`-th candidate address for `server_id`.
pub fn derive(server_id: &str, n: u32) -> Ipv4Addr {
    use sha2::Digest;
    let h = sha2::Sha256::digest(format!("kiln-web-panel/1\n{n}\n{server_id}").as_bytes());
    Ipv4Addr::new(127, 64 + h[0] % 191, h[1], 1 + h[2] % 254)
}

/// The slot of `server_id` (a [`key`]), made (and written) on first use. An unreadable file starts a new one:
/// addresses already given out are then picked again from the same hashes, so they stay what they were.
pub fn slot(path: &Path, server_id: &str) -> Result<Slot> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let _held = lock(path)?;
    let mut f = read(path);
    if let Some(s) = f.servers.get(server_id).filter(|s| ours(s.ip)) {
        return Ok(*s);
    }
    let taken: Vec<Ipv4Addr> = f.servers.iter().filter(|(k, _)| k.as_str() != server_id).map(|(_, s)| s.ip).collect();
    let ip = (0..).map(|n| derive(server_id, n)).find(|ip| !taken.contains(ip)).expect("a free address");
    let s = Slot { ip, port: 0 };
    f.servers.insert(server_id.to_string(), s);
    crate::util::write_json_atomic(path, &f)?;
    Ok(s)
}

/// Remembers the port the server's panel was opened on.
pub fn remember_port(path: &Path, server_id: &str, port: u16) -> Result<()> {
    let _held = lock(path)?;
    let mut f = read(path);
    match f.servers.get_mut(server_id) {
        Some(s) if s.port != port => s.port = port,
        _ => return Ok(()),
    }
    crate::util::write_json_atomic(path, &f)
}
