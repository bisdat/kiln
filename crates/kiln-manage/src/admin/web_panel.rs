//! Opens the installed web panel (Kilnwarden, or Kiln Web Admin, its old name: [`ids`]) over loopback or through the
//! instance's SSH host.
use super::client::{AdminClient, Transport};
use super::tunnel::SshTunnel;
use crate::server::{
    instance::ServerInstance,
    remote::{self, RemoteSpec},
};
use crate::{Error, ErrorCode, Result};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{WebPkiSupportedAlgorithms, verify_tls12_signature, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{ClientConfig, ClientConnection, DigitallySignedStruct, SignatureScheme};
use serde::Deserialize;
use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream};
use std::sync::Arc;
use std::time::Duration;

pub mod ids;
const TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Deserialize)]
struct Endpoint {
    format: u8,
    host: IpAddr,
    port: u16,
    sha256: Option<String>,
}

impl Endpoint {
    fn parse(bytes: &[u8]) -> Result<Self> {
        let invalid = || Error::invalid("invalid web panel listener description");
        if bytes.len() > 4096 {
            return Err(invalid());
        }
        let mut endpoint: Self = serde_json::from_slice(bytes).map_err(|_| invalid())?;
        if endpoint.format != 1
            || endpoint.port == 0
            || endpoint.host.is_unspecified()
            || endpoint.host.is_multicast()
        {
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

/// A forward stays open while the launcher runs so tabs can keep their session.
#[derive(Default)]
pub struct WebPanelTunnels(HashMap<String, WebPanelTunnel>);

struct WebPanelTunnel {
    remote: RemoteSpec,
    target: SocketAddr,
    ssh: SshTunnel,
}

impl WebPanelTunnels {
    pub fn clear(&mut self) {
        self.0.clear();
    }

    // Only the native launcher opens the returned URL. Its fragment may contain a one-use owner ticket.
    pub fn url(&mut self, server: &ServerInstance) -> Result<String> {
        let remote = server.remote.as_ref();
        let dir = match remote {
            Some(_) => None,
            None => Some(
                server
                    .dir
                    .as_deref()
                    .ok_or_else(|| Error::invalid("server has no folder"))?,
            ),
        };
        let (panel, bytes) = ids::find(|m| match (remote, dir) {
            (Some(spec), _) => remote::read_file(spec, &m.endpoint()),
            (None, Some(dir)) => std::fs::read(dir.join(m.endpoint())).map_err(Error::from),
            (None, None) => Err(Error::invalid("server has no folder")),
        })?;
        let endpoint = Endpoint::parse(&bytes)?;
        let target = SocketAddr::new(endpoint.host, endpoint.port);

        let address = if let Some(spec) = remote {
            let existing = self.0.get_mut(&server.id).and_then(|t| {
                (&t.remote == spec && t.target == target && t.ssh.alive())
                    .then_some(t.ssh.local_port)
            });
            let port = match existing {
                Some(port) => port,
                None => {
                    let tunnel = SshTunnel::open_to(spec, target)?;
                    let port = tunnel.local_port;
                    self.0.insert(
                        server.id.clone(),
                        WebPanelTunnel {
                            remote: spec.clone(),
                            target,
                            ssh: tunnel,
                        },
                    );
                    port
                }
            };
            SocketAddr::new(Ipv4Addr::LOCALHOST.into(), port)
        } else {
            target
        };

        let socket = TcpStream::connect_timeout(&address, TIMEOUT).map_err(|_| {
            Error::new(
                ErrorCode::AdminUnavailable,
                format!("{} did not answer", panel.name),
            )
        })?;
        let Some(pin) = endpoint.sha256 else {
            return if address.ip().is_loopback() {
                Ok(format!("http://{address}/signin"))
            } else {
                Err(Error::new(
                    ErrorCode::Unsupported,
                    "Opening an HTTP web panel requires a loopback listener or SSH.",
                ))
            };
        };
        verify_listener(socket, address.ip(), pin)?;

        let transport = if remote.is_some() {
            Transport::Ssh
        } else {
            Transport::Tcp
        };
        let admin = AdminClient::connect(server, transport)?;
        let ticket = admin
            .call("login_mint", panel.login_mint_args())
            .map_err(|_| {
                Error::new(
                    ErrorCode::AdminUnavailable,
                    "Kiln.Admin could not issue a one-time web panel login.",
                )
            })?;
        let secret = ticket["secret"]
            .as_str()
            .filter(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
            .ok_or_else(|| {
                Error::new(
                    ErrorCode::Parse,
                    "Kiln.Admin returned an invalid one-time login.",
                )
            })?;
        Ok(format!("https://{address}/launcher#secret={secret}"))
    }
}

fn verify_listener(mut socket: TcpStream, ip: IpAddr, pin: String) -> Result<()> {
    socket.set_read_timeout(Some(TIMEOUT))?;
    socket.set_write_timeout(Some(TIMEOUT))?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let verifier = Arc::new(PinnedLeaf {
        pin,
        algorithms: provider.signature_verification_algorithms,
    });
    let config = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|_| {
            Error::new(
                ErrorCode::Internal,
                "could not configure the web panel's TLS",
            )
        })?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    let mut tls = ClientConnection::new(Arc::new(config), ServerName::IpAddress(ip.into()))
        .map_err(|_| Error::new(ErrorCode::Internal, "could not start the web panel's TLS"))?;
    while tls.is_handshaking() {
        tls.complete_io(&mut socket).map_err(|e| {
            Error::new(
                ErrorCode::Network,
                format!("the web panel's TLS could not verify the server certificate: {e}"),
            )
        })?;
    }
    Ok(())
}

#[derive(Debug)]
struct PinnedLeaf {
    pin: String,
    algorithms: WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for PinnedLeaf {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        _: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> std::result::Result<ServerCertVerified, rustls::Error> {
        if crate::hash::sha256_hex(end_entity.as_ref()) == self.pin {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(
                rustls::CertificateError::ApplicationVerificationFailure,
            ))
        }
    }

    fn verify_tls12_signature(
        &self,
        msg: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(msg, cert, dss, &self.algorithms)
    }

    fn verify_tls13_signature(
        &self,
        msg: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(msg, cert, dss, &self.algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algorithms.supported_schemes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;
    use std::path::{Path, PathBuf};

    fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!(
            "kiln-webpanel-id-{tag}-{}-{}",
            std::process::id(),
            crate::hash::hex(&crate::util::random_bytes::<6>().unwrap())
        ));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn server(dir: &Path) -> ServerInstance {
        serde_json::from_value(serde_json::json!({
            "id": "lab", "name": "lab", "kind": "local", "dir": dir, "port": 15636, "queryPort": 15637, "wine": null,
            "policy": "verified-only", "source": "launcher", "remote": null
        }))
        .unwrap()
    }

    /// A plain-HTTP panel of mod `id` listening on loopback: its `launcher-endpoint.json` in `<dir>/kiln/data/<id>/`
    /// (sha256 null, so `url` opens `/signin` without a Kiln.Admin login). The listener stays open with the test.
    fn listen(dir: &Path, id: &str) -> TcpListener {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let data = dir.join("kiln/data").join(id);
        std::fs::create_dir_all(&data).unwrap();
        let doc = serde_json::json!({"format": 1, "host": "127.0.0.1", "port": l.local_addr().unwrap().port(), "sha256": null});
        std::fs::write(data.join("launcher-endpoint.json"), doc.to_string()).unwrap();
        l
    }

    fn signin(l: &TcpListener) -> String {
        format!("http://{}/signin", l.local_addr().unwrap())
    }

    #[test]
    fn kilnwarden_is_found_in_its_own_data_folder() {
        let dir = temp("kilnwarden");
        let panel = listen(&dir, "kilnwarden");
        assert_eq!(
            WebPanelTunnels::default().url(&server(&dir)).unwrap(),
            signin(&panel)
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn kiln_web_admin_test_builds_still_open() {
        let dir = temp("legacy");
        let panel = listen(&dir, "kiln-webadmin");
        assert_eq!(
            WebPanelTunnels::default().url(&server(&dir)).unwrap(),
            signin(&panel)
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn with_both_endpoints_kilnwarden_wins() {
        let dir = temp("both");
        let new = listen(&dir, "kilnwarden");
        let _old = listen(&dir, "kiln-webadmin");
        assert_eq!(
            WebPanelTunnels::default().url(&server(&dir)).unwrap(),
            signin(&new)
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn no_panel_listening_names_kilnwarden() {
        let dir = temp("none");
        std::fs::create_dir_all(dir.join("kiln/data/kiln-admin")).unwrap();
        let e = WebPanelTunnels::default().url(&server(&dir)).unwrap_err();
        assert_eq!(e.code, ErrorCode::AdminUnavailable);
        assert!(
            e.message.contains("Kilnwarden is not listening"),
            "{}",
            e.message
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn endpoint_requires_ip_port_and_optional_fingerprint() {
        let digest = ["AB"; 32].join(":");
        let doc = format!(r#"{{"format":1,"host":"::1","port":15737,"sha256":"{digest}"}}"#);
        let endpoint = Endpoint::parse(doc.as_bytes()).unwrap();
        assert_eq!(endpoint.sha256.unwrap(), "ab".repeat(32));
        for invalid in [
            br#"{"format":1,"host":"evil.example","port":1}"#.as_slice(),
            br#"{"format":1,"host":"0.0.0.0","port":123}"#,
            br#"{"format":1,"host":"127.0.0.1","port":0}"#,
            br#"{"format":1,"host":"127.0.0.1","port":123,"sha256":"not a fingerprint"}"#,
        ] {
            assert!(Endpoint::parse(invalid).is_err());
        }
    }
}
