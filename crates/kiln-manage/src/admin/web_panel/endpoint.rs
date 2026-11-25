//! `launcher-endpoint.json` (Kiln Web Admin writes it while its panel listens, docs/protocol-relay-broker.md §7):
//! `{"format":1,"host":"<address>","port":<port>,"sha256":"<certificate SHA-256 hex>"|null}`, in the mod's data folder.
//! That folder follows the profile's `dataDir` (`DataDirMode::Profile`): see [`crate::admin::datadir`].

use crate::admin::datadir;
use crate::server::instance::ServerInstance;
use crate::{Error, ErrorCode, Result};
use serde::Deserialize;
use std::net::IpAddr;

pub const FILE: &str = "launcher-endpoint.json";
pub const MOD_ID: &str = "kiln-webadmin";

#[derive(Debug, Deserialize, PartialEq, Eq)]
pub struct Endpoint {
    pub format: u8,
    pub host: IpAddr,
    pub port: u16,
    pub sha256: Option<String>,
}

impl Endpoint {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        let invalid = || Error::invalid("invalid Kiln Web Admin listener description");
        if bytes.len() > 4096 {
            return Err(invalid());
        }
        let mut endpoint: Self = serde_json::from_slice(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(bytes)).map_err(|_| invalid())?;
        // `::ffff:a.b.c.d` is a.b.c.d: checked (and connected to) as that
        endpoint.host = endpoint.host.to_canonical();
        let broadcast = matches!(endpoint.host, IpAddr::V4(v4) if v4.is_broadcast());
        if endpoint.format != 1 || endpoint.port == 0 || endpoint.host.is_unspecified() || endpoint.host.is_multicast() || broadcast {
            return Err(invalid());
        }
        if let Some(pin) = &mut endpoint.sha256 {
            *pin = pin.replace(':', "").to_ascii_lowercase();
            if pin.len() != 64 || !pin.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err(invalid());
            }
        }
        Ok(endpoint)
    }
}

fn not_listening() -> Error {
    Error::new(ErrorCode::AdminUnavailable, "Kiln Web Admin is not listening. Check that the current mod is loaded.")
}

/// The server's data root as it booted ([`datadir`]): read once by [`super::WebPanels::open`], which takes both this
/// file and Kiln.Admin's from it.
pub fn root(server: &ServerInstance) -> Result<datadir::DataRoot> {
    match &server.remote {
        Some(spec) => Ok(datadir::DataRoot::Remote(datadir::remote_data_root(spec))),
        None => {
            let dir = server.dir.as_deref().ok_or_else(|| Error::invalid("server has no folder"))?;
            Ok(datadir::DataRoot::Local(datadir::local_data_root(dir, server.prefix.as_deref())))
        }
    }
}

/// The server's endpoint (`<data root>/kiln-webadmin/launcher-endpoint.json`), locally or over ssh.
pub fn read(server: &ServerInstance) -> Result<Endpoint> {
    read_in(server, &root(server)?)
}

/// The endpoint in the data root `root`.
pub fn read_in(server: &ServerInstance, root: &datadir::DataRoot) -> Result<Endpoint> {
    match (&server.remote, root) {
        (Some(spec), datadir::DataRoot::Remote(_)) => {
            let dir = root.remote_mod(MOD_ID).unwrap_or_default();
            match crate::server::remote::read_abs_file(spec, &format!("{dir}/{FILE}")) {
                Ok(b) => Endpoint::parse(&b),
                Err(_) => Err(not_listening()),
            }
        }
        (None, datadir::DataRoot::Local(_)) => {
            let dir = root.local_mod(MOD_ID).unwrap_or_default();
            match std::fs::read(dir.join(FILE)) {
                Ok(b) => Endpoint::parse(&b),
                Err(_) => Err(not_listening()),
            }
        }
        _ => Err(Error::invalid("a local server's data folder is on this machine, a remote server's on that one")),
    }
}
