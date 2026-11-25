//! One loopback address per server, for the life of the launcher's data folder: `web-panel-origins.json` there.
//!
//! Browsers keep cookies per host name, not per port, so panels of different servers on 127.0.0.1 shared their
//! `__Host-wa-session` cookies and a tab of server A sent server B's session to A. Each server gets its own address in
//! 127.64.0.0 - 127.254.255.255 (all of 127.0.0.0/8 is loopback on Linux and Windows; 127.0.x.x is left to the system:
//! 127.0.0.1, systemd-resolved's 127.0.0.53, Debian's 127.0.1.1), picked from a hash of the server id and kept: an
//! address is never given to another server, even after its server is removed, so no old cookie reaches a new
//! server. The port is remembered too, so the panel keeps its origin (and its local storage) between opens.

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

/// The server's slot, made (and written) on first use. An unreadable file starts a new one: addresses already given
/// out are then picked again from the same hashes, so they stay what they were.
pub fn slot(path: &Path, server_id: &str) -> Result<Slot> {
    let mut f = read(path);
    if let Some(s) = f.servers.get(server_id) {
        return Ok(*s);
    }
    let taken: Vec<Ipv4Addr> = f.servers.values().map(|s| s.ip).collect();
    let ip = (0..).map(|n| derive(server_id, n)).find(|ip| !taken.contains(ip)).expect("a free address");
    let s = Slot { ip, port: 0 };
    f.servers.insert(server_id.to_string(), s);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    crate::util::write_json_atomic(path, &f)?;
    Ok(s)
}

/// Remembers the port the server's panel was opened on.
pub fn remember_port(path: &Path, server_id: &str, port: u16) -> Result<()> {
    let mut f = read(path);
    match f.servers.get_mut(server_id) {
        Some(s) if s.port != port => s.port = port,
        _ => return Ok(()),
    }
    crate::util::write_json_atomic(path, &f)
}
