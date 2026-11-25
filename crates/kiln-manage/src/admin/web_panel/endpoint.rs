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
        if endpoint.format != 1 || endpoint.port == 0 || endpoint.host.is_unspecified() || endpoint.host.is_multicast() {
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

/// The server's endpoint: the booted profile's data folder first, `kiln/data/kiln-webadmin` after; locally, or over
/// ssh for a remote server.
pub fn read(server: &ServerInstance) -> Result<Endpoint> {
    match &server.remote {
        Some(spec) => {
            for dir in datadir::remote_mod_dirs(spec, MOD_ID) {
                match crate::server::remote::read_abs_file(spec, &format!("{dir}/{FILE}")) {
                    Ok(b) => return Endpoint::parse(&b),
                    Err(e) if e.code == ErrorCode::NotFound => continue,
                    Err(_) => return Err(not_listening()),
                }
            }
            Err(not_listening())
        }
        None => {
            let dir = server.dir.as_deref().ok_or_else(|| Error::invalid("server has no folder"))?;
            for d in datadir::local_mod_dirs(dir, server.prefix.as_deref(), MOD_ID) {
                match std::fs::read(d.join(FILE)) {
                    Ok(b) => return Endpoint::parse(&b),
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                    Err(_) => return Err(not_listening()),
                }
            }
            Err(not_listening())
        }
    }
}
