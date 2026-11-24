//! Opens the installed Web Admin over loopback or through the instance's SSH host.
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

const ENDPOINT: &str = "kiln/data/kiln-webadmin/launcher-endpoint.json";
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
        let invalid = || Error::invalid("invalid Kiln Web Admin listener description");
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
        let bytes = match remote {
            Some(spec) => remote::read_file(spec, ENDPOINT),
            None => {
                let dir = server
                    .dir
                    .as_deref()
                    .ok_or_else(|| Error::invalid("server has no folder"))?;
                std::fs::read(dir.join(ENDPOINT)).map_err(Error::from)
            }
        }
        .map_err(|_| {
            Error::new(
                ErrorCode::AdminUnavailable,
                "Kiln Web Admin is not listening. Check that the current mod is loaded.",
            )
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
            Error::new(ErrorCode::AdminUnavailable, "Kiln Web Admin did not answer")
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
        let args = serde_json::json!({ "consumer": "kiln-webadmin", "label": "Kiln Launcher" });
        let ticket = admin
            .call("login_mint", args.as_object().unwrap().clone())
            .map_err(|_| {
                Error::new(
                    ErrorCode::AdminUnavailable,
                    "Kiln.Admin could not issue a one-time Web Admin login.",
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
        .map_err(|_| Error::new(ErrorCode::Internal, "could not configure Web Admin TLS"))?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    let mut tls = ClientConnection::new(Arc::new(config), ServerName::IpAddress(ip.into()))
        .map_err(|_| Error::new(ErrorCode::Internal, "could not start Web Admin TLS"))?;
    while tls.is_handshaking() {
        tls.complete_io(&mut socket).map_err(|e| {
            Error::new(
                ErrorCode::Network,
                format!("Web Admin TLS could not verify the server certificate: {e}"),
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
