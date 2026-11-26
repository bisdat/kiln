//! "Open web panel" (Servers Overview): opens a server's web panel (Kilnwarden, or Kiln Web Admin, its old name:
//! [`ids`]) in the browser, signed in as the owner.
//!
//! (0.2.0-dev.31.12) Every panel opens through its own gateway ([`gateway`]) on its own loopback address
//! ([`origins`]), so:
//! - panels of different servers never share cookies (a browser keeps cookies per host, not per port);
//! - the browser's connection takes the path the launcher pinned the certificate on: to an address of this machine,
//!   or through ssh to the remote server; a panel on another machine's address is refused;
//! - the launcher binds the port and holds it (no free-port race), for this user's programs only (Linux);
//! - the one-time login never appears on a command line: the browser gets a one-use bootstrap address instead;
//! - the gateway (and its ssh connections) closes when the panel's tab is closed or idle, when "Close" is pressed, or
//!   when the launcher exits;
//! - `launcher-endpoint.json` is read from the profile's data folder when the server booted with `dataDir: profile`,
//!   and the one-time login is minted by the Kiln.Admin of that same folder (the data root is read once per open);
//! - a server's origin belongs to its panel's certificate too: a panel with another certificate (another panel the
//!   server's files point at) gets another address, so cookies one panel set never reach another.

pub mod endpoint;
pub mod gateway;
pub mod ids;
pub mod origins;
pub mod pin;
/// The same-user check ([`crate::admin::peer`]; the SSH tunnel uses it too).
pub use crate::admin::peer;

use crate::admin::client::AdminClient;
use crate::server::instance::ServerInstance;
use crate::{Error, ErrorCode, Result};
use gateway::{Gateway, Limits, Upstream};
use serde::Serialize;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::PathBuf;

/// How the gateway reaches the panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Via {
    Local,
    Ssh,
}

/// An open panel, as the window shows it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WebPanelInfo {
    pub server_id: String,
    /// `https://127.x.y.z:port` (or `http://` for a panel without TLS): this server's own origin.
    pub origin: String,
    /// The pinned certificate's SHA-256 (hex), None for a panel without TLS.
    pub fingerprint: Option<String>,
    pub via: Via,
    /// False when this server's own loopback address could not be used (127.0.0.1 then: cookies shared).
    pub isolated: bool,
    /// The gateway closes this long after the last browser connection ends.
    pub idle_close_s: u64,
    /// Browser connections open now.
    pub connections: usize,
}

/// What `open` did: the address to hand the browser (one use; it holds no secret), and the panel.
#[derive(Debug, Clone)]
pub struct Opened {
    pub url: String,
    pub info: WebPanelInfo,
}

struct Open {
    gateway: Gateway,
    fingerprint: Option<String>,
}

/// The launcher's open panels, one gateway per server.
pub struct WebPanels {
    registry: PathBuf,
    limits: Limits,
    open: HashMap<String, Open>,
}

impl WebPanels {
    /// `registry`: the launcher data folder's `web-panel-origins.json` ([`origins::FILE`]).
    pub fn new(registry: PathBuf) -> WebPanels {
        WebPanels { registry, limits: Limits::default(), open: HashMap::new() }
    }

    pub fn with_limits(registry: PathBuf, limits: Limits) -> WebPanels {
        WebPanels { registry, limits, open: HashMap::new() }
    }

    fn info(&self, id: &str, o: &Open) -> WebPanelInfo {
        let addr = o.gateway.addr();
        WebPanelInfo {
            server_id: id.to_string(),
            origin: format!("{}://{addr}", if o.fingerprint.is_some() { "https" } else { "http" }),
            fingerprint: o.fingerprint.clone(),
            via: if matches!(o.gateway.upstream, Upstream::Ssh(..)) { Via::Ssh } else { Via::Local },
            isolated: o.gateway.isolated,
            idle_close_s: self.limits.no_connection.as_secs(),
            connections: o.gateway.connections(),
        }
    }

    /// Opens (or reuses) the server's gateway, checks the panel's certificate through it, mints the one-time login
    /// and returns the bootstrap address for the browser.
    pub fn open(&mut self, server: &ServerInstance) -> Result<Opened> {
        self.sweep();
        // one data root for everything below: the panel's listener and the Kiln.Admin that mints its login
        let root = endpoint::root(server)?;
        let (panel, ep) = endpoint::find_in(server, &root)?;
        let target = SocketAddr::new(ep.host, ep.port);
        let upstream = match &server.remote {
            Some(spec) => Upstream::Ssh(spec.clone(), target),
            None if gateway::is_local_address(target.ip()) => Upstream::Direct(target),
            None => return Err(gateway::not_local(target)),
        };
        let reuse = self.open.get(&server.id).is_some_and(|o| o.gateway.alive() && o.gateway.upstream == upstream && o.fingerprint == ep.sha256);
        if !reuse {
            if let Some(old) = self.open.remove(&server.id) {
                old.gateway.close();
            }
            let key = origins::key(&server.id, ep.sha256.as_deref());
            let slot = origins::slot(&self.registry, &key)?;
            let gw = Gateway::start(slot.ip, slot.port, upstream, self.limits)?;
            if gw.isolated && gw.addr().port() != slot.port {
                let _ = origins::remember_port(&self.registry, &key, gw.addr().port());
            }
            self.open.insert(server.id.clone(), Open { gateway: gw, fingerprint: ep.sha256.clone() });
        }
        let o = self.open.get(&server.id).expect("just opened");
        let addr = o.gateway.addr();
        let Some(pin) = ep.sha256.clone() else {
            // a panel without TLS: only ever reached on this machine or through ssh, and no secret in the URL
            let info = self.info(&server.id, o);
            return Ok(Opened { url: format!("http://{addr}/signin"), info });
        };
        if let Err(e) = pin::verify(addr, &pin) {
            self.close(&server.id);
            return Err(e);
        }
        let admin = AdminClient::connect_at(server, &root)?;
        let ticket = admin
            .call("login_mint", panel.login_mint_args())
            .map_err(|_| Error::new(ErrorCode::AdminUnavailable, "Kiln.Admin could not issue a one-time web panel login."))?;
        let secret = ticket["secret"]
            .as_str()
            .filter(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
            .ok_or_else(|| Error::new(ErrorCode::Parse, "Kiln.Admin returned an invalid one-time login."))?;
        drop(admin);
        let o = self.open.get(&server.id).expect("open");
        let url = o.gateway.offer(format!("https://{addr}/launcher#secret={secret}"), self.limits.handle_ttl)?;
        Ok(Opened { url, info: self.info(&server.id, o) })
    }

    /// Closes the server's gateway; whether one was open.
    pub fn close(&mut self, server_id: &str) -> bool {
        match self.open.remove(server_id) {
            Some(o) => {
                let alive = o.gateway.alive();
                o.gateway.close();
                alive
            }
            None => false,
        }
    }

    pub fn close_all(&mut self) {
        for (_, o) in self.open.drain() {
            o.gateway.close();
        }
    }

    /// The gateways still open (those that closed by themselves are forgotten).
    pub fn list(&mut self) -> Vec<WebPanelInfo> {
        self.sweep();
        let mut v: Vec<_> = self.open.iter().map(|(id, o)| self.info(id, o)).collect();
        v.sort_by(|a, b| a.server_id.cmp(&b.server_id));
        v
    }

    fn sweep(&mut self) {
        let dead: Vec<String> = self.open.iter().filter(|(_, o)| !o.gateway.alive()).map(|(k, _)| k.clone()).collect();
        for d in dead {
            if let Some(o) = self.open.remove(&d) {
                o.gateway.close();
            }
        }
    }
}

#[cfg(test)]
mod review_tests;
#[cfg(test)]
mod tests;
