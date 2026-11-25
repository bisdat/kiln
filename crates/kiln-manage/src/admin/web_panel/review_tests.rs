//! Regression tests from the review of "Open web panel" (0.2.0-dev.31.12). Each one failed before its fix
//! (docs: /mnt/Dongus/kiln-wave/dev31/out/webadmin-launcher/review/REVIEW.md).

use super::gateway::{self, Gateway, Limits, Upstream};
use super::*;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Held by every test that sets `KILN_SSH` (a process-wide variable; ssh is spawned with it).
pub(crate) static SSH_ENV: Mutex<()> = Mutex::new(());

fn temp(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("kiln-webpanel-review-{tag}-{}-{}", std::process::id(), crate::hash::hex(&crate::util::random_bytes::<6>().unwrap())));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn server(id: &str, dir: &Path, prefix: Option<&Path>) -> ServerInstance {
    serde_json::from_value(serde_json::json!({
        "id": id, "name": id, "kind": "local", "dir": dir, "port": 15636, "queryPort": 15637, "wine": null,
        "policy": "verified-only", "source": "launcher", "remote": null, "prefix": prefix
    }))
    .unwrap()
}

fn http_get(addr: SocketAddr, path: &str) -> (u16, String) {
    let mut s = TcpStream::connect(addr).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    write!(s, "GET {path} HTTP/1.1\r\nHost: {addr}\r\n\r\n").unwrap();
    let mut out = String::new();
    s.read_to_string(&mut out).unwrap();
    let status = out.split(' ').nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
    (status, out)
}

/// A TLS listener with a fresh self-signed certificate (openssl at test time): (address, the leaf's SHA-256).
fn tls_panel() -> Option<(SocketAddr, String)> {
    use rustls::pki_types::pem::PemObject;
    use rustls::pki_types::{CertificateDer, PrivateKeyDer};
    let dir = temp("tls");
    let ok = std::process::Command::new("openssl")
        .args(["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-subj", "/CN=panel", "-days", "1"])
        .arg("-keyout")
        .arg(dir.join("k.pem"))
        .arg("-out")
        .arg(dir.join("c.pem"))
        .output()
        .ok()?
        .status
        .success();
    if !ok {
        return None;
    }
    let cert = CertificateDer::from_pem_file(dir.join("c.pem")).unwrap();
    let key = PrivateKeyDer::from_pem_file(dir.join("k.pem")).unwrap();
    let _ = std::fs::remove_dir_all(&dir);
    let pin = crate::hash::sha256_hex(cert.as_ref());
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let cfg = Arc::new(
        rustls::ServerConfig::builder_with_provider(provider).with_safe_default_protocol_versions().unwrap().with_no_client_auth().with_single_cert(vec![cert], key).unwrap(),
    );
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    std::thread::spawn(move || {
        for s in l.incoming().flatten() {
            let cfg = cfg.clone();
            std::thread::spawn(move || {
                let mut s = s;
                let Ok(mut conn) = rustls::ServerConnection::new(cfg) else { return };
                while conn.is_handshaking() {
                    if conn.complete_io(&mut s).is_err() {
                        return;
                    }
                }
                let _ = conn.complete_io(&mut s);
            });
        }
    });
    Some((addr, pin))
}

/// Kiln.Admin's endpoint.json + admin-token in `dir` (a kiln-admin folder), for a KAP mock on `port`.
fn write_kiln_admin(dir: &Path, port: u16, token: &[u8; 32]) {
    std::fs::create_dir_all(dir).unwrap();
    std::fs::write(dir.join("admin-token"), format!("{}\n", crate::hash::hex(token))).unwrap();
    std::fs::write(
        dir.join("endpoint.json"),
        serde_json::to_vec(&serde_json::json!({"format": 1, "protocol": "kap/1", "host": "127.0.0.1", "port": port, "pid": 1,
            "started": "2026-11-25T12:00:00Z", "server": {"id": "srv-test", "name": "Test"}, "token": "admin-token"})).unwrap(),
    )
    .unwrap();
}

/// Kiln Web Admin's launcher-endpoint.json in `dir` (a kiln-webadmin folder).
fn write_web_admin(dir: &Path, port: u16, pin: &str) {
    std::fs::create_dir_all(dir).unwrap();
    std::fs::write(dir.join(endpoint::FILE), serde_json::to_vec(&serde_json::json!({ "format": 1, "host": "127.0.0.1", "port": port, "sha256": pin })).unwrap()).unwrap();
}

fn host_of(origin: &str) -> String {
    origin.rsplit_once(':').unwrap().0.to_string()
}

fn aliases_work() -> bool {
    TcpListener::bind((Ipv4Addr::new(127, 77, 0, 9), 0)).is_ok()
}

// ------------------------------------------------------------------------------------------------- F1: one data root

/// The server booted with a profile data folder that holds Kiln.Admin but no Kiln Web Admin listener; kiln/data holds
/// a listener (another panel: a stale file, or one a hostile server put there). The login Kiln.Admin of the profile
/// folder mints must never go to that other panel.
#[test]
fn the_login_never_goes_to_a_panel_of_another_data_folder() {
    let Some((other_panel, other_pin)) = tls_panel() else {
        eprintln!("skipped: no openssl command");
        return;
    };
    let dir = temp("mix");
    let profile = dir.join("kiln/profiles-copy/p1/data");
    std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
    std::fs::write(dir.join("kiln/state/boot.json"), br#"{"profile":{"mode":"active","dataDir":"profiles-copy/p1/data"}}"#).unwrap();
    let token = [9u8; 32];
    let kap = crate::admin::tests::Mock::start(token, false);
    write_kiln_admin(&profile.join("kiln-admin"), kap.port, &token);
    write_web_admin(&dir.join("kiln/data/kiln-webadmin"), other_panel.port(), &other_pin);
    let mut panels = WebPanels::new(dir.join(origins::FILE));
    let r = panels.open(&server("mix", &dir, None));
    if let Ok(opened) = &r {
        let (host, path) = opened.url.strip_prefix("http://").unwrap().split_once('/').unwrap();
        let (_, page) = http_get(host.parse().unwrap(), &format!("/{path}"));
        panic!("the profile folder's login went to kiln/data's panel: {}", page.contains("secret="));
    }
    assert_eq!(r.unwrap_err().code, ErrorCode::AdminUnavailable, "Kiln Web Admin is not listening in the booted folder");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A profile data folder on the Wine prefix's C: drive: the launcher finds Kiln Web Admin there (through the prefix),
/// and Kiln.Admin in the same place.
#[cfg(unix)]
#[test]
fn kiln_admin_follows_a_c_drive_data_folder_like_the_panel() {
    let Some((panel, pin)) = tls_panel() else {
        eprintln!("skipped: no openssl command");
        return;
    };
    let dir = temp("cdrive");
    let pfx = temp("cdrive-pfx");
    let data = pfx.join("drive_c/kiln/p1/data");
    std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
    std::fs::write(dir.join("kiln/state/boot.json"), br#"{"profile":{"dataDir":"C:\\kiln\\p1\\data"}}"#).unwrap();
    let token = [3u8; 32];
    let kap = crate::admin::tests::Mock::start(token, false);
    write_kiln_admin(&data.join("kiln-admin"), kap.port, &token);
    write_web_admin(&data.join("kiln-webadmin"), panel.port(), &pin);
    let mut panels = WebPanels::new(dir.join(origins::FILE));
    let opened = panels.open(&server("cdrive", &dir, Some(&pfx))).unwrap();
    assert!(!opened.url.contains("secret"));
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&pfx);
}

// ------------------------------------------------------------------------------------------------- F8: origins

/// A server whose files point the launcher at a panel with another certificate gets it on another address: the session
/// cookie that panel sets is never sent to this server's own panel.
#[test]
fn another_certificate_gets_another_origin() {
    let (Some((a, pin_a)), Some((b, pin_b))) = (tls_panel(), tls_panel()) else {
        eprintln!("skipped: no openssl command");
        return;
    };
    let dir = temp("percert");
    let token = [5u8; 32];
    let kap = crate::admin::tests::Mock::start(token, false);
    write_kiln_admin(&dir.join("kiln/data/kiln-admin"), kap.port, &token);
    let mut panels = WebPanels::new(dir.join(origins::FILE));
    write_web_admin(&dir.join("kiln/data/kiln-webadmin"), a.port(), &pin_a);
    let first = panels.open(&server("percert", &dir, None)).unwrap().info.origin;
    write_web_admin(&dir.join("kiln/data/kiln-webadmin"), b.port(), &pin_b);
    let second = panels.open(&server("percert", &dir, None)).unwrap().info.origin;
    if aliases_work() {
        assert_ne!(host_of(&first), host_of(&second), "panel B's cookies would reach panel A");
    }
    // and back: panel A gets its own address again
    write_web_admin(&dir.join("kiln/data/kiln-webadmin"), a.port(), &pin_a);
    assert_eq!(host_of(&panels.open(&server("percert", &dir, None)).unwrap().info.origin), host_of(&first));
    panels.close_all();
    let _ = std::fs::remove_dir_all(&dir);
}

/// An entry edited to name an address outside the range never makes the gateway listen there.
#[test]
fn a_stored_address_outside_the_range_is_not_used() {
    let dir = temp("tampered");
    let reg = dir.join(origins::FILE);
    for ip in ["0.0.0.0", "192.168.1.20", "127.0.0.1", "127.64.0.0"] {
        std::fs::write(&reg, format!(r#"{{"format":1,"servers":{{"alpha":{{"ip":"{ip}","port":0}}}}}}"#)).unwrap();
        let s = origins::slot(&reg, "alpha").unwrap();
        let o = s.ip.octets();
        assert!(o[0] == 127 && o[1] >= 64 && o[3] != 0 && o[3] != 255, "{ip} -> {}", s.ip);
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// Two launchers on one data folder: no entry is lost.
#[test]
fn concurrent_writers_keep_every_entry() {
    let dir = temp("concurrent");
    let reg = Arc::new(dir.join(origins::FILE));
    let threads: Vec<_> = (0..6)
        .map(|t| {
            let reg = reg.clone();
            std::thread::spawn(move || (0..15).map(|i| (format!("srv-{t}-{i}"), origins::slot(&reg, &format!("srv-{t}-{i}")).unwrap())).collect::<Vec<_>>())
        })
        .collect();
    let given: Vec<(String, origins::Slot)> = threads.into_iter().flat_map(|t| t.join().unwrap()).collect();
    let file: serde_json::Value = serde_json::from_slice(&std::fs::read(&*reg).unwrap()).unwrap();
    let kept = file["servers"].as_object().unwrap();
    for (id, slot) in &given {
        assert_eq!(kept.get(id).and_then(|v| v["ip"].as_str()), Some(slot.ip.to_string().as_str()), "{id} was lost");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

// ------------------------------------------------------------------------------------------------- F6: addresses

#[test]
fn mapped_broadcast_and_multicast_addresses_are_nobodys() {
    for bad in ["::ffff:0.0.0.0", "::ffff:224.0.0.1", "255.255.255.255", "::ffff:255.255.255.255"] {
        let doc = format!(r#"{{"format":1,"host":"{bad}","port":15737,"sha256":null}}"#);
        assert!(endpoint::Endpoint::parse(doc.as_bytes()).is_err(), "{bad}");
        assert!(!gateway::is_local_address(bad.parse().unwrap()), "{bad} is not this machine's");
    }
    let ep = endpoint::Endpoint::parse(br#"{"format":1,"host":"::ffff:127.0.0.1","port":15737,"sha256":null}"#).unwrap();
    assert_eq!(ep.host, "127.0.0.1".parse::<std::net::IpAddr>().unwrap(), "checked and connected to as IPv4");
}

// ------------------------------------------------------------------------------------------------- ssh stand-in

/// `ssh` stand-in (python3): `-W host:port` connects stdin/stdout there (after `$KILN_FAKE_SSH_DELAY` seconds, the
/// time a slow ssh takes to connect and sign in); otherwise the command after the host runs here under `sh -c`.
#[cfg(unix)]
pub(crate) fn fake_ssh() -> Option<PathBuf> {
    std::process::Command::new("python3").arg("--version").output().ok()?.status.success().then_some(())?;
    let dir = temp("fakessh");
    let p = dir.join("ssh");
    std::fs::write(
        &p,
        "#!/bin/sh\nt=\nwhile [ $# -gt 0 ]; do case \"$1\" in -W) t=$2; shift;; --) shift; shift; break;; esac; shift; done\n\
         if [ -z \"$t\" ]; then exec sh -c \"$*\"; fi\nsleep \"${KILN_FAKE_SSH_DELAY:-0}\"\n\
         exec python3 -c '\nimport socket,sys,threading\nh,p=sys.argv[1].rsplit(\":\",1)\ns=socket.create_connection((h.strip(\"[]\"),int(p)))\ndef up():\n  while True:\n    b=sys.stdin.buffer.read1(65536)\n    if not b: break\n    s.sendall(b)\n  s.shutdown(socket.SHUT_WR)\nthreading.Thread(target=up,daemon=True).start()\nwhile True:\n  b=s.recv(65536)\n  if not b: break\n  sys.stdout.buffer.write(b); sys.stdout.buffer.flush()\n' \"$t\"\n",
    )
    .unwrap();
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
    Some(p)
}

/// Sets KILN_SSH (and the stand-in's delay) for the guard's life.
#[cfg(unix)]
struct SshEnv {
    _held: std::sync::MutexGuard<'static, ()>,
    dir: PathBuf,
}

#[cfg(unix)]
impl SshEnv {
    fn set(ssh: &Path, delay_s: u32) -> SshEnv {
        let held = SSH_ENV.lock().unwrap_or_else(|p| p.into_inner());
        // SAFETY: every test that touches KILN_SSH / KILN_FAKE_SSH_DELAY holds SSH_ENV.
        unsafe {
            std::env::set_var("KILN_SSH", ssh);
            std::env::set_var("KILN_FAKE_SSH_DELAY", delay_s.to_string());
        }
        SshEnv { _held: held, dir: ssh.parent().unwrap().to_path_buf() }
    }
}

#[cfg(unix)]
impl Drop for SshEnv {
    fn drop(&mut self) {
        // SAFETY: as in set; the lock is still held here.
        unsafe {
            std::env::remove_var("KILN_SSH");
            std::env::remove_var("KILN_FAKE_SSH_DELAY");
        }
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

// ------------------------------------------------------------------------------------------------- F2: slow ssh

/// KAP over ssh where ssh takes 7 s to connect and sign in (a jump host, slow DNS on the server): before
/// 0.2.0-dev.31.12 the tunnel waited up to 15 s for ssh and the handshake had its own 6 s; the hello must still get
/// that time now that ssh runs per connection.
#[cfg(unix)]
#[test]
fn kap_over_a_slow_ssh_still_connects() {
    let Some(ssh) = fake_ssh() else {
        eprintln!("skipped: no python3 for the ssh stand-in");
        return;
    };
    let _env = SshEnv::set(&ssh, 7);
    let dir = temp("slowssh");
    let token = [4u8; 32];
    let kap = crate::admin::tests::Mock::start(token, false);
    write_kiln_admin(&dir.join("kiln/data/kiln-admin"), kap.port, &token);
    let remote = crate::server::remote::RemoteSpec { host: "lab-box".into(), port: None, dir: dir.display().to_string(), instance: None };
    let t0 = Instant::now();
    let c = crate::admin::client::AdminClient::connect_ssh(&remote, "slow").unwrap_or_else(|e| panic!("after {:?}: {}", t0.elapsed(), e.message));
    assert_eq!(c.call("ping", Default::default()).unwrap(), serde_json::json!({"pong": true}));
    drop(c);
    let _ = std::fs::remove_dir_all(&dir);
}

// ------------------------------------------------------------------------------------------------- F4: ssh children

/// The tunnel's held port: a burst of connections (any local program can connect) runs at most 8 ssh children; one
/// more is closed at once instead of signing in to the server again.
#[cfg(unix)]
#[test]
fn a_tunnel_runs_a_bounded_number_of_ssh_children() {
    let Some(ssh) = fake_ssh() else {
        eprintln!("skipped: no python3 for the ssh stand-in");
        return;
    };
    let _env = SshEnv::set(&ssh, 0);
    // a server that keeps every connection open and silent
    let hold = TcpListener::bind("127.0.0.1:0").unwrap();
    let hold_port = hold.local_addr().unwrap().port();
    let kept = Arc::new(Mutex::new(Vec::new()));
    let k = kept.clone();
    std::thread::spawn(move || {
        for s in hold.incoming().flatten() {
            k.lock().unwrap().push(s);
        }
    });
    let remote = crate::server::remote::RemoteSpec { host: "lab-box".into(), port: None, dir: "/srv/lab".into(), instance: None };
    let t = crate::admin::tunnel::SshTunnel::open(&remote, hold_port).unwrap();
    let local = SocketAddr::from((Ipv4Addr::LOCALHOST, t.local_port));
    let mut open = Vec::new();
    for _ in 0..8 {
        let c = TcpStream::connect(local).unwrap();
        open.push(c);
    }
    // all 8 reached the server through their own ssh
    let t0 = Instant::now();
    while kept.lock().unwrap().len() < 8 && t0.elapsed() < Duration::from_secs(15) {
        std::thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(kept.lock().unwrap().len(), 8);
    let mut ninth = TcpStream::connect(local).unwrap();
    ninth.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    let mut b = [0u8; 1];
    assert!(matches!(ninth.read(&mut b), Ok(0)), "the 9th connection is closed, not given a 9th ssh");
    std::thread::sleep(Duration::from_millis(300));
    assert_eq!(kept.lock().unwrap().len(), 8, "no 9th sign-in on the server");
    drop(open);
    t.close().unwrap();
}

// ------------------------------------------------------------------------------------------------- the gateway over ssh

/// The bootstrap page and the gateway keep working through the stand-in ssh (a smoke test for the moved peer check).
#[cfg(unix)]
#[test]
fn the_gateway_over_ssh_serves_this_user() {
    let Some(ssh) = fake_ssh() else {
        eprintln!("skipped: no python3 for the ssh stand-in");
        return;
    };
    let _env = SshEnv::set(&ssh, 0);
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let echo = l.local_addr().unwrap();
    std::thread::spawn(move || {
        for s in l.incoming().flatten() {
            std::thread::spawn(move || {
                let mut r = s.try_clone().unwrap();
                let mut w = s;
                let _ = std::io::copy(&mut r, &mut w);
            });
        }
    });
    let remote = crate::server::remote::RemoteSpec { host: "lab-box".into(), port: None, dir: "/srv/lab".into(), instance: None };
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 8), 0, Upstream::Ssh(remote, echo), Limits::default()).unwrap();
    let mut c = TcpStream::connect(gw.addr()).unwrap();
    c.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    c.write_all(b"this user").unwrap();
    let mut buf = [0u8; 9];
    c.read_exact(&mut buf).unwrap();
    assert_eq!(&buf, b"this user");
    gw.close();
}

// ------------------------------------------------------------------------------------------------- F3: unknown owner

/// Linux: a connection whose owner the socket table does not show is refused (after a second look), never served.
#[test]
fn an_unknown_owner_is_refused_on_linux() {
    let mut calls = 0;
    let v = peer::decide(|| {
        calls += 1;
        None
    });
    if cfg!(target_os = "linux") {
        assert!(!v, "unknown owner: refused");
        assert_eq!(calls, 2, "after a second look");
        let mut n = 0;
        assert!(peer::decide(|| {
            n += 1;
            (n > 1).then_some(true)
        }), "found on the second look");
        assert!(!peer::decide(|| Some(false)));
    } else {
        assert!(v, "no check off Linux");
    }
    assert!(peer::decide(|| Some(true)));
}
