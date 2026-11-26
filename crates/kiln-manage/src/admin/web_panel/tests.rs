//! "Open web panel" (0.2.0-dev.31.12): per-server origins, the held and pinned gateway, the bootstrap page, idle
//! closing, the profile data folder, and the ssh path through a stand-in ssh.

use super::gateway::{self, Gateway, Limits, Upstream};
use super::*;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::path::Path;
use std::sync::Arc;
use std::time::{Duration, Instant};

fn temp(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("kiln-webpanel-{tag}-{}-{}", std::process::id(), crate::hash::hex(&crate::util::random_bytes::<6>().unwrap())));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn server(id: &str, dir: &Path) -> ServerInstance {
    serde_json::from_value(serde_json::json!({
        "id": id, "name": id, "kind": "local", "dir": dir, "port": 15636, "queryPort": 15637, "wine": null,
        "policy": "verified-only", "source": "launcher", "remote": null
    }))
    .unwrap()
}

/// Waits up to `secs` for `f`.
fn wait(secs: u64, mut f: impl FnMut() -> bool) -> bool {
    let t = Instant::now();
    while t.elapsed() < Duration::from_secs(secs) {
        if f() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

fn echo_server() -> SocketAddr {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let a = l.local_addr().unwrap();
    std::thread::spawn(move || {
        for s in l.incoming().flatten() {
            std::thread::spawn(move || {
                let mut r = s.try_clone().unwrap();
                let mut w = s;
                let _ = std::io::copy(&mut r, &mut w);
            });
        }
    });
    a
}

fn http_get(addr: SocketAddr, path: &str) -> (u16, String, String) {
    let mut s = TcpStream::connect(addr).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    write!(s, "GET {path} HTTP/1.1\r\nHost: {addr}\r\n\r\n").unwrap();
    let mut out = String::new();
    s.read_to_string(&mut out).unwrap();
    let status = out.split(' ').nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
    let (head, body) = out.split_once("\r\n\r\n").unwrap_or((&out, ""));
    (status, head.to_string(), body.to_string())
}

/// Whether this machine lets a program bind 127.x.y.z beyond 127.0.0.1 (Linux and Windows do; macOS only with aliases).
fn aliases_work() -> bool {
    TcpListener::bind((Ipv4Addr::new(127, 77, 0, 9), 0)).is_ok()
}

#[test]
fn endpoint_requires_ip_port_and_optional_fingerprint() {
    let digest = ["AB"; 32].join(":");
    let doc = format!(r#"{{"format":1,"host":"::1","port":15737,"sha256":"{digest}"}}"#);
    let ep = endpoint::Endpoint::parse(doc.as_bytes()).unwrap();
    assert_eq!(ep.sha256.unwrap(), "ab".repeat(32));
    for invalid in [
        br#"{"format":1,"host":"evil.example","port":1}"#.as_slice(),
        br#"{"format":1,"host":"0.0.0.0","port":123}"#,
        br#"{"format":1,"host":"127.0.0.1","port":0}"#,
        br#"{"format":1,"host":"127.0.0.1","port":123,"sha256":"not a fingerprint"}"#,
    ] {
        assert!(endpoint::Endpoint::parse(invalid).is_err());
    }
}

#[test]
fn the_endpoint_follows_a_profile_data_folder() {
    let dir = temp("datadir");
    let stale = dir.join("kiln/data/kiln-webadmin");
    std::fs::create_dir_all(&stale).unwrap();
    std::fs::write(stale.join(endpoint::FILE), br#"{"format":1,"host":"127.0.0.1","port":1111,"sha256":null}"#).unwrap();
    let s = server("datadir", &dir);
    assert_eq!(endpoint::read(&s).unwrap().port, 1111, "no profile data folder: kiln/data");
    // the server booted with a profile whose dataDir is a copy-mode folder under kiln/
    std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
    std::fs::write(dir.join("kiln/state/boot.json"), br#"{"profile":{"mode":"active","dataDir":"profiles-copy/p1/data"}}"#).unwrap();
    let live = dir.join("kiln/profiles-copy/p1/data/kiln-webadmin");
    std::fs::create_dir_all(&live).unwrap();
    std::fs::write(live.join(endpoint::FILE), br#"{"format":1,"host":"127.0.0.1","port":2222,"sha256":null}"#).unwrap();
    assert_eq!(endpoint::read(&s).unwrap().port, 2222, "the booted profile's folder wins over a stale kiln/data file");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn origins_are_per_server_kept_and_never_given_twice() {
    let dir = temp("origins");
    let reg = dir.join(origins::FILE);
    let a = origins::slot(&reg, "alpha").unwrap();
    assert_eq!(origins::slot(&reg, "alpha").unwrap(), a, "the same server keeps its address");
    let b = origins::slot(&reg, "bravo").unwrap();
    assert_ne!(a.ip, b.ip, "another server, another host for the browser's cookies");
    for s in [a, b] {
        let o = s.ip.octets();
        assert!(o[0] == 127 && o[1] >= 64 && o[3] != 0 && o[3] != 255, "{:?}", s.ip);
    }
    origins::remember_port(&reg, "alpha", 40123).unwrap();
    assert_eq!(origins::slot(&reg, "alpha").unwrap().port, 40123);
    // a server whose first candidate is taken (here by a removed server's entry) gets its next one
    let text = std::fs::read_to_string(&reg).unwrap();
    let mut v: serde_json::Value = serde_json::from_str(&text).unwrap();
    v["servers"]["removed"] = serde_json::json!({ "ip": origins::derive("charlie", 0), "port": 0 });
    std::fs::write(&reg, v.to_string()).unwrap();
    assert_eq!(origins::slot(&reg, "charlie").unwrap().ip, origins::derive("charlie", 1));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_gateway_holds_its_port_and_forwards_bytes() {
    let echo = echo_server();
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 2), 0, Upstream::Direct(echo), Limits::default()).unwrap();
    let addr = gw.addr();
    assert_eq!(gw.isolated, aliases_work());
    if gw.isolated {
        assert_eq!(addr.ip(), std::net::IpAddr::V4(Ipv4Addr::new(127, 77, 1, 2)));
    }
    // held: nobody else can take the port while the gateway lives
    let taken = TcpListener::bind(addr).unwrap_err();
    assert_eq!(taken.kind(), std::io::ErrorKind::AddrInUse);
    let mut c = TcpStream::connect(addr).unwrap();
    c.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    c.write_all(b"ping through the gateway").unwrap();
    let mut buf = [0u8; 24];
    c.read_exact(&mut buf).unwrap();
    assert_eq!(&buf, b"ping through the gateway");
    assert!(wait(2, || gw.connections() == 1));
    drop(c);
    assert!(wait(2, || gw.connections() == 0));
    gw.close();
    assert!(wait(2, || TcpStream::connect_timeout(&addr, Duration::from_millis(200)).is_err()), "closed: the port is gone");
}

#[test]
fn the_gateway_closes_when_no_browser_is_connected() {
    let echo = echo_server();
    let limits = Limits { no_connection: Duration::from_millis(300), no_traffic: Duration::from_secs(30), ..Limits::default() };
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 3), 0, Upstream::Direct(echo), limits).unwrap();
    let mut c = TcpStream::connect(gw.addr()).unwrap();
    c.write_all(b"x").unwrap();
    std::thread::sleep(Duration::from_millis(700));
    assert!(gw.alive(), "an open connection (the panel's tab) keeps it");
    drop(c);
    assert!(wait(3, || !gw.alive()), "the tab closed: the gateway closes");
    // and when connections stay open but nothing moves
    let limits = Limits { no_connection: Duration::from_secs(30), no_traffic: Duration::from_millis(300), ..Limits::default() };
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 4), 0, Upstream::Direct(echo), limits).unwrap();
    let mut idle = TcpStream::connect(gw.addr()).unwrap();
    idle.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    assert!(wait(3, || !gw.alive()), "no traffic: closed");
    let mut b = [0u8; 1];
    assert!(matches!(idle.read(&mut b), Ok(0) | Err(_)), "and its connections ended");
}

#[test]
fn the_bootstrap_page_is_one_use_and_holds_the_secret() {
    let echo = echo_server();
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 5), 0, Upstream::Direct(echo), Limits::default()).unwrap();
    let target = format!("https://{}/launcher#secret={}", gw.addr(), "ab".repeat(32));
    let url = gw.offer(target.clone(), Duration::from_secs(5)).unwrap();
    assert!(!url.contains("secret"), "the browser's command line gets no secret: {url}");
    let (host, path) = url.strip_prefix("http://").unwrap().split_once('/').unwrap();
    let boot: SocketAddr = host.parse().unwrap();
    assert_eq!(boot.ip(), gw.addr().ip());
    assert_ne!(boot.port(), gw.addr().port(), "its own port: an https upgrade of it can't reach the panel");
    let (status, head, body) = http_get(boot, &format!("/{path}"));
    assert_eq!(status, 200, "{head}");
    assert!(body.contains(&format!("location.replace({})", serde_json::to_string(&target).unwrap())), "{body}");
    let (_, csp) = gateway::bootstrap_page(&target);
    assert!(head.contains(&format!("Content-Security-Policy: {csp}")) && csp.contains("script-src 'sha256-"));
    assert!(head.contains("Cache-Control: no-store") && head.contains("Referrer-Policy: no-referrer"));
    assert_eq!(http_get(boot, &format!("/{path}")).0, 404, "one use");
    assert_eq!(http_get(boot, &format!("/k/{}", "0".repeat(64))).0, 404, "an unknown handle");
    let short = gw.offer(target, Duration::from_millis(50)).unwrap();
    std::thread::sleep(Duration::from_millis(120));
    let p = short.split_once("/k/").unwrap().1;
    assert_eq!(http_get(boot, &format!("/k/{p}")).0, 404, "expired");
}

#[test]
fn the_socket_table_names_the_owner() {
    // /proc/net/tcp lines: local, remote, ..., uid
    let table = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n\
                 \x20  0: 0100007F:9C40 0201A87F:1F90 01 00000000:00000000 00:00000000 00000000  1000        0 1 1 0\n";
    if cfg!(target_endian = "little") {
        let local: SocketAddr = "127.0.0.1:40000".parse().unwrap();
        let remote: SocketAddr = "127.168.1.2:8080".parse().unwrap();
        assert_eq!(peer::owner(table, local, remote), Some(1000));
        assert_eq!(peer::owner(table, remote, local), None);
        assert_eq!(peer::parse_addr("0000000000000000FFFF00000100007F:0050"), Some("127.0.0.1:80".parse().unwrap()), "v4-mapped");
    }
    #[cfg(target_os = "linux")]
    {
        let l = TcpListener::bind((Ipv4Addr::new(127, 77, 1, 6), 0)).unwrap();
        let c = TcpStream::connect(l.local_addr().unwrap()).unwrap();
        let (_, peer_addr) = l.accept().unwrap();
        assert_eq!(peer_addr, c.local_addr().unwrap());
        assert_eq!(peer::same_user(l.local_addr().unwrap(), peer_addr), Some(true), "this process is this user");
    }
}

#[test]
fn a_panel_on_another_machine_is_refused() {
    assert!(gateway::is_local_address("127.0.0.1".parse().unwrap()));
    assert!(gateway::is_local_address("::1".parse().unwrap()));
    assert!(!gateway::is_local_address("192.0.2.10".parse().unwrap()), "TEST-NET-1 is nobody's own address");
    let dir = temp("lan");
    let data = dir.join("kiln/data/kiln-webadmin");
    std::fs::create_dir_all(&data).unwrap();
    std::fs::write(data.join(endpoint::FILE), format!(r#"{{"format":1,"host":"192.0.2.10","port":15737,"sha256":"{}"}}"#, "ab".repeat(32))).unwrap();
    let mut panels = WebPanels::new(dir.join(origins::FILE));
    let e = panels.open(&server("lan", &dir)).unwrap_err();
    assert_eq!(e.code, ErrorCode::Unsupported);
    assert!(e.message.contains("another machine"), "{}", e.message);
    assert!(panels.list().is_empty(), "no gateway was started");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A TLS listener with a fresh self-signed certificate (openssl at test time): (address, the leaf's SHA-256). RSA,
/// since ring can't load the EC key macOS's LibreSSL writes; an extension, so LibreSSL writes a v3 certificate (webpki refuses v1).
fn tls_panel() -> Option<(SocketAddr, String)> {
    use rustls::pki_types::pem::PemObject;
    use rustls::pki_types::{CertificateDer, PrivateKeyDer};
    let dir = temp("tls");
    let ok = std::process::Command::new("openssl")
        .args(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=panel", "-days", "1", "-addext", "basicConstraints=critical,CA:FALSE"])
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

#[test]
fn open_pins_through_the_gateway_and_hands_the_browser_a_one_use_address() {
    let Some((panel, pin)) = tls_panel() else {
        eprintln!("skipped: no openssl command");
        return;
    };
    let dir = temp("open");
    // the server booted with dataDir: profile (a Z: path under Wine): both Kiln.Admin and Kiln Web Admin write there
    let data = temp("open-profile-data");
    std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
    let game_path = if cfg!(windows) { data.display().to_string() } else { format!("Z:{}", data.display().to_string().replace('/', "\\")) };
    std::fs::write(dir.join("kiln/state/boot.json"), serde_json::to_vec(&serde_json::json!({ "profile": { "dataDir": game_path } })).unwrap()).unwrap();
    let wa = data.join("kiln-webadmin");
    std::fs::create_dir_all(&wa).unwrap();
    std::fs::write(wa.join(endpoint::FILE), serde_json::to_vec(&serde_json::json!({ "format": 1, "host": "127.0.0.1", "port": panel.port(), "sha256": pin })).unwrap()).unwrap();
    let token = [7u8; 32];
    let kap = crate::admin::tests::Mock::start(token, false);
    let ka = data.join("kiln-admin");
    std::fs::create_dir_all(&ka).unwrap();
    std::fs::write(ka.join("admin-token"), format!("{}\n", crate::hash::hex(&token))).unwrap();
    std::fs::write(
        ka.join("endpoint.json"),
        serde_json::to_vec(&serde_json::json!({"format": 1, "protocol": "kap/1", "host": "127.0.0.1", "port": kap.port, "pid": 1,
            "started": "2026-11-25T12:00:00Z", "server": {"id": "srv-test", "name": "Test"}, "token": "admin-token"})).unwrap(),
    )
    .unwrap();

    let mut panels = WebPanels::new(dir.join(origins::FILE));
    let opened = panels.open(&server("open", &dir)).unwrap();
    let info = &opened.info;
    assert_eq!(info.via, Via::Local);
    assert_eq!(info.fingerprint.as_deref(), Some(pin.as_str()));
    assert!(info.origin.starts_with("https://127."), "{}", info.origin);
    assert!(!opened.url.contains("secret"), "{}", opened.url);
    let (host, path) = opened.url.strip_prefix("http://").unwrap().split_once('/').unwrap();
    let (status, _, body) = http_get(host.parse().unwrap(), &format!("/{path}"));
    assert_eq!(status, 200);
    let origin = info.origin.clone();
    assert!(body.contains(&format!("{origin}/launcher#secret={}", "5e".repeat(32))), "the minted secret, for this server's origin: {body}");
    // the browser's own connection takes the pinned path: a pinned handshake through the gateway passes
    pin::verify(origin.strip_prefix("https://").unwrap().parse().unwrap(), &pin).unwrap();
    // opening again reuses the gateway; another server gets another origin
    let again = panels.open(&server("open", &dir)).unwrap();
    assert_eq!(again.info.origin, origin);
    let other = panels.open(&server("open-2", &dir)).unwrap();
    if aliases_work() {
        let host_of = |o: &str| o.rsplit_once(':').unwrap().0.to_string();
        assert_ne!(host_of(&other.info.origin), host_of(&origin), "cookies of one panel never reach the other");
    }
    assert_eq!(panels.list().len(), 2);
    // a wrong pin: nothing opens and the gateway goes
    std::fs::write(wa.join(endpoint::FILE), serde_json::to_vec(&serde_json::json!({ "format": 1, "host": "127.0.0.1", "port": panel.port(), "sha256": "cd".repeat(32) })).unwrap()).unwrap();
    let e = panels.open(&server("open", &dir)).unwrap_err();
    assert_eq!(e.code, ErrorCode::Network, "{}", e.message);
    assert_eq!(panels.list().len(), 1);
    assert!(panels.close("open-2"));
    assert!(panels.list().is_empty());
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&data);
}

/// `ssh` stand-in for the tests: `-W host:port` connects its stdin and stdout to that address (python3).
#[cfg(unix)]
fn fake_ssh() -> Option<PathBuf> {
    std::process::Command::new("python3").arg("--version").output().ok()?.status.success().then_some(())?;
    let dir = temp("fakessh");
    let p = dir.join("ssh");
    std::fs::write(
        &p,
        "#!/bin/sh\nwhile [ $# -gt 0 ]; do [ \"$1\" = -W ] && t=$2; shift; done\nexec python3 -c '\nimport socket,sys,threading\nh,p=sys.argv[1].rsplit(\":\",1)\ns=socket.create_connection((h.strip(\"[]\"),int(p)))\ndef up():\n  while True:\n    b=sys.stdin.buffer.read1(65536)\n    if not b: break\n    s.sendall(b)\n  s.shutdown(socket.SHUT_WR)\nthreading.Thread(target=up,daemon=True).start()\nwhile True:\n  b=s.recv(65536)\n  if not b: break\n  sys.stdout.buffer.write(b); sys.stdout.buffer.flush()\n' \"$t\"\n",
    )
    .unwrap();
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
    Some(p)
}

#[cfg(unix)]
#[test]
fn ssh_forwards_hold_their_port_and_use_ssh_w_per_connection() {
    let Some(ssh) = fake_ssh() else {
        eprintln!("skipped: no python3 for the ssh stand-in");
        return;
    };
    // ssh_stdio reads KILN_SSH when it spawns; every test that sets it holds SSH_ENV
    let _held = super::review_tests::SSH_ENV.lock().unwrap_or_else(|p| p.into_inner());
    // SAFETY: the tests that read or write KILN_SSH hold SSH_ENV.
    unsafe { std::env::set_var("KILN_SSH", &ssh) };
    let echo = echo_server();
    let remote = crate::server::remote::RemoteSpec { host: "lab-box".into(), port: None, dir: "/srv/lab".into(), instance: None };
    // the KAP tunnel: the launcher holds 127.0.0.1:<port>; each connection is its own ssh -W
    let t = crate::admin::tunnel::SshTunnel::open(&remote, echo.port()).unwrap();
    let local = SocketAddr::from((Ipv4Addr::LOCALHOST, t.local_port));
    assert_eq!(TcpListener::bind(local).unwrap_err().kind(), std::io::ErrorKind::AddrInUse, "held, not released for ssh to take");
    for msg in [&b"first"[..], b"second"] {
        let mut c = TcpStream::connect(local).unwrap();
        c.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        c.write_all(msg).unwrap();
        let mut buf = vec![0u8; msg.len()];
        c.read_exact(&mut buf).unwrap_or_else(|e| panic!("{e}: ssh said {:?}; script {}", t.last_error(), std::fs::read_to_string(&ssh).unwrap()));
        assert_eq!(buf, msg);
    }
    t.close().unwrap();
    assert!(wait(2, || TcpStream::connect_timeout(&local, Duration::from_millis(200)).is_err()));
    // the web panel gateway over ssh
    let gw = Gateway::start(Ipv4Addr::new(127, 77, 1, 7), 0, Upstream::Ssh(remote, echo), Limits::default()).unwrap();
    let mut c = TcpStream::connect(gw.addr()).unwrap();
    c.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    c.write_all(b"over ssh -W").unwrap();
    let mut buf = [0u8; 11];
    c.read_exact(&mut buf).unwrap();
    assert_eq!(&buf, b"over ssh -W");
    drop(c);
    gw.close();
    unsafe { std::env::remove_var("KILN_SSH") };
    let _ = std::fs::remove_dir_all(ssh.parent().unwrap());
}

/// (0.2.0-dev.31.14) A plain-HTTP endpoint of mod `id` in the data folder `data` (`<data root>/<id>/`).
fn panel_endpoint(data: &Path, id: &str, port: u16) {
    let dir = data.join(id);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join(endpoint::FILE), format!(r#"{{"format":1,"host":"127.0.0.1","port":{port},"sha256":null}}"#)).unwrap();
}

#[test]
fn kilnwarden_kiln_web_admin_or_both_kilnwarden_first() {
    let dir = temp("ids");
    let s = server("ids", &dir);
    let data = dir.join("kiln/data");
    let found = |s: &ServerInstance| endpoint::find_in(s, &endpoint::root(s).unwrap()).map(|(m, ep)| (m.id, ep.port));
    let e = found(&s).unwrap_err();
    assert_eq!(e.code, ErrorCode::AdminUnavailable);
    assert!(e.message.starts_with("Kilnwarden is not listening"), "{}", e.message);
    panel_endpoint(&data, "kiln-webadmin", 1111);
    assert_eq!(found(&s).unwrap(), ("kiln-webadmin", 1111), "a Kiln Web Admin test build");
    panel_endpoint(&data, "kilnwarden", 2222);
    assert_eq!(found(&s).unwrap(), ("kilnwarden", 2222), "both: Kilnwarden");
    std::fs::remove_dir_all(data.join("kiln-webadmin")).unwrap();
    assert_eq!(found(&s).unwrap(), ("kilnwarden", 2222), "Kilnwarden alone");
    assert_eq!(endpoint::read(&s).unwrap().port, 2222);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn kilnwarden_follows_a_profile_data_folder_too() {
    let dir = temp("ids-datadir");
    let s = server("ids-datadir", &dir);
    // a stale old copy's file in kiln/data does not count once the server booted with a profile data folder
    panel_endpoint(&dir.join("kiln/data"), "kiln-webadmin", 1111);
    std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
    std::fs::write(dir.join("kiln/state/boot.json"), br#"{"profile":{"mode":"active","dataDir":"profiles-copy/p1/data"}}"#).unwrap();
    panel_endpoint(&dir.join("kiln/profiles-copy/p1/data"), "kilnwarden", 3333);
    let (m, ep) = endpoint::find_in(&s, &endpoint::root(&s).unwrap()).unwrap();
    assert_eq!((m, ep.port), (ids::KILNWARDEN, 3333));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_plain_http_kilnwarden_opens_through_its_gateway() {
    let dir = temp("ids-open");
    let s = server("ids-open", &dir);
    let upstream = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    panel_endpoint(&dir.join("kiln/data"), "kilnwarden", upstream.local_addr().unwrap().port());
    let mut panels = WebPanels::new(dir.join("web-panel-origins.json"));
    let opened = panels.open(&s).unwrap();
    assert!(opened.url.starts_with("http://") && opened.url.ends_with("/signin"), "{}", opened.url);
    assert_eq!(opened.info.fingerprint, None);
    panels.close_all();
    let _ = std::fs::remove_dir_all(&dir);
}

/// (review of 0.2.0-dev.31.14) `kiln/state/mods.json` with `mods` (id, state) as the running server's Kiln writes it.
fn mods_json(server_dir: &Path, mods: &[(&str, &str)]) {
    let mods: Vec<_> = mods.iter().map(|(id, state)| serde_json::json!({"id": id, "state": state})).collect();
    std::fs::create_dir_all(server_dir.join("kiln/state")).unwrap();
    std::fs::write(
        server_dir.join(crate::paths::game::MODS_JSON),
        serde_json::to_vec(&serde_json::json!({"format": 1, "pid": 1, "written": "2026-11-25T23:00:00Z", "generation": 1, "mods": mods})).unwrap(),
    )
    .unwrap();
}

/// (review of 0.2.0-dev.31.14) Kilnwarden left its file behind (a crash skips its removal) and the old copy is back:
/// Kilnwarden stays off next to a loaded old copy (its M4), so the old copy's file is the live one. Before the fix the
/// launcher took Kilnwarden's stale file and every open failed the pin check against the old copy's certificate.
#[test]
fn a_stale_kilnwarden_file_next_to_a_loaded_old_copy_is_not_the_panel() {
    let dir = temp("ids-stale-new");
    let s = server("ids-stale-new", &dir);
    let data = dir.join("kiln/data");
    panel_endpoint(&data, "kilnwarden", 2222);
    panel_endpoint(&data, "kiln-webadmin", 1111);
    let found = |s: &ServerInstance| endpoint::find_in(s, &endpoint::root(s).unwrap()).map(|(m, ep)| (m.id, ep.port)).unwrap();
    mods_json(&dir, &[("kiln-admin", "loaded"), ("kilnwarden", "loaded"), ("kiln-webadmin", "loaded")]);
    assert_eq!(found(&s), ("kiln-webadmin", 1111), "the old copy is loaded: Kilnwarden is off");
    mods_json(&dir, &[("kiln-webadmin", "pending"), ("kilnwarden", "loaded")]);
    assert_eq!(found(&s), ("kiln-webadmin", 1111), "the old copy still loading counts too (as in M4)");
    // disabled by Kiln's fault policy (loaded, then switched off; `reload` retries it): Kilnwarden stays off next to
    // it as well (its M4: Pending, Loaded or Disabled), so its file is the stale one
    mods_json(&dir, &[("kilnwarden", "loaded"), ("kiln-webadmin", "disabled")]);
    assert_eq!(found(&s), ("kiln-webadmin", 1111), "the old copy disabled by the fault policy");
    // the old copy gone (or switched off by the player: a .disabled file, Kiln's `skipped`): its file is the stale one,
    // Kilnwarden's the live one
    mods_json(&dir, &[("kilnwarden", "loaded"), ("kiln-webadmin", "skipped")]);
    assert_eq!(found(&s), ("kilnwarden", 2222));
    mods_json(&dir, &[("kilnwarden", "loaded")]);
    assert_eq!(found(&s), ("kilnwarden", 2222));
    // no mods.json (or one this launcher can't read): Kilnwarden first, as before
    std::fs::write(dir.join(crate::paths::game::MODS_JSON), b"not json").unwrap();
    assert_eq!(found(&s), ("kilnwarden", 2222));
    std::fs::remove_file(dir.join(crate::paths::game::MODS_JSON)).unwrap();
    assert_eq!(found(&s), ("kilnwarden", 2222));
    // the old copy loaded but without a file (not listening yet): Kilnwarden's file is all there is
    mods_json(&dir, &[("kilnwarden", "loaded"), ("kiln-webadmin", "loaded")]);
    std::fs::remove_dir_all(data.join("kiln-webadmin")).unwrap();
    assert_eq!(found(&s), ("kilnwarden", 2222));
    let _ = std::fs::remove_dir_all(&dir);
}

/// (review of 0.2.0-dev.31.14) The same over ssh, in a server folder whose name needs quoting: both endpoint folders
/// and mods.json are read on the remote machine, in the data root it booted with.
#[cfg(unix)]
#[test]
fn over_ssh_both_ids_and_the_loaded_old_copy_in_a_folder_that_needs_quoting() {
    let Some(ssh) = super::review_tests::fake_ssh() else {
        eprintln!("skipped: no python3 for the ssh stand-in");
        return;
    };
    let _env = super::review_tests::SshEnv::set(&ssh, 0);
    let dir = temp("ids-ssh").join("it's a $(server) folder");
    std::fs::create_dir_all(&dir).unwrap();
    let remote = crate::server::remote::RemoteSpec { host: "lab-box".into(), port: None, dir: dir.display().to_string(), instance: None };
    let mut s = server("ids-ssh", &dir);
    s.dir = None;
    s.remote = Some(remote);
    let found = |s: &ServerInstance| endpoint::find_in(s, &endpoint::root(s).unwrap()).map(|(m, ep)| (m.id, ep.port));
    let e = found(&s).unwrap_err();
    assert!(e.message.starts_with("Kilnwarden is not listening"), "{}", e.message);
    let data = dir.join("kiln/data");
    panel_endpoint(&data, "kiln-webadmin", 1111);
    assert_eq!(found(&s).unwrap(), ("kiln-webadmin", 1111));
    panel_endpoint(&data, "kilnwarden", 2222);
    assert_eq!(found(&s).unwrap(), ("kilnwarden", 2222));
    mods_json(&dir, &[("kilnwarden", "loaded"), ("kiln-webadmin", "loaded")]);
    assert_eq!(found(&s).unwrap(), ("kiln-webadmin", 1111), "the old copy loaded: its panel");
    // a profile data folder on the remote machine
    std::fs::write(dir.join("kiln/state/boot.json"), br#"{"profile":{"mode":"active","dataDir":"profiles-copy/p 1/data"}}"#).unwrap();
    panel_endpoint(&dir.join("kiln/profiles-copy/p 1/data"), "kilnwarden", 3333);
    assert_eq!(found(&s).unwrap(), ("kilnwarden", 3333), "only the booted data root counts");
    let _ = std::fs::remove_dir_all(dir.parent().unwrap());
}
