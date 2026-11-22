//! The two sources against local HTTP servers, with the real updater plugin on a mock app: source order, fallback
//! on a missing manifest, a dead host, a failed download, a bad signature, a wrong signed version and a wrong sha256.
//! testdata/: `payload.bin` (starts like an ELF file, as an AppImage does), its signature `payload.bin.sig` (signed
//! for version 0.2.0-dev.2) and `test.pub`, the public half of a throwaway key used only here.
//! testdata/beta/ (dev.29): `payload.bin` signed with a second throwaway key (`beta/test.pub`) for the build ids
//! 0.2.0-dev.2, 0.2.0-dev.3, 0.2.0-dev.3+beta.1 and 0.2.0-dev.3+beta.2: test builds, the final build, a switch back.

use super::feed::{Channel, Source};
use super::*;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;

const PAYLOAD: &[u8] = include_bytes!("testdata/payload.bin");
const SIG: &str = include_str!("testdata/payload.bin.sig");
const PUBKEY: &str = include_str!("testdata/test.pub");

/// A tiny HTTP/1.1 server: fixed answers by path, a hit counter per path.
struct Server {
    port: u16,
    hits: Arc<Mutex<HashMap<String, usize>>>,
}

impl Server {
    fn start(routes: Vec<(String, u16, Vec<u8>)>) -> Server {
        Server::serve(TcpListener::bind("127.0.0.1:0").unwrap(), routes)
    }

    fn serve(l: TcpListener, routes: Vec<(String, u16, Vec<u8>)>) -> Server {
        let port = l.local_addr().unwrap().port();
        let routes: HashMap<String, (u16, Vec<u8>)> = routes.into_iter().map(|(p, s, b)| (p, (s, b))).collect();
        let hits = Arc::new(Mutex::new(HashMap::new()));
        let h = hits.clone();
        std::thread::spawn(move || {
            for conn in l.incoming() {
                let Ok(mut c) = conn else { continue };
                let mut buf = Vec::new();
                let mut chunk = [0u8; 2048];
                while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
                    match c.read(&mut chunk) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => buf.extend_from_slice(&chunk[..n]),
                    }
                }
                let req = String::from_utf8_lossy(&buf);
                let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
                *h.lock().unwrap().entry(path.clone()).or_insert(0) += 1;
                let (status, body) = routes.get(&path).cloned().unwrap_or((404, b"not found".to_vec()));
                let head = format!("HTTP/1.1 {status} X\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
                let _ = c.write_all(head.as_bytes());
                let _ = c.write_all(&body);
            }
        });
        Server { port, hits }
    }

    fn url(&self, path: &str) -> String {
        format!("http://127.0.0.1:{}{path}", self.port)
    }

    fn hits(&self, path: &str) -> usize {
        self.hits.lock().unwrap().get(path).copied().unwrap_or(0)
    }

    fn total_hits(&self) -> usize {
        self.hits.lock().unwrap().values().sum()
    }

    fn source(&self, name: &str) -> Source {
        Source { name: name.into(), stable: self.url("/stable/latest.json"), dev: self.url("/dev/latest.json"), beta: self.url("/beta/latest.json") }
    }
}

/// A port nothing listens on.
fn dead_source(name: &str) -> Source {
    let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    Source {
        name: name.into(),
        stable: format!("http://127.0.0.1:{port}/stable/latest.json"),
        dev: format!("http://127.0.0.1:{port}/dev/latest.json"),
        beta: format!("http://127.0.0.1:{port}/beta/latest.json"),
    }
}

fn manifest(version: &str, file_url: &str, sha256: Option<&str>) -> Vec<u8> {
    let mut entry = serde_json::json!({ "url": file_url, "signature": SIG.trim() });
    if let Some(s) = sha256 {
        entry["sha256"] = s.into();
    }
    serde_json::to_vec(&serde_json::json!({
        "version": version,
        "notes": "Test release.",
        "pub_date": "2026-11-06T12:00:00Z",
        "platforms": { "linux-x86_64": entry.clone(), "windows-x86_64": entry },
        "kiln": { "format": 1, "channel": "dev" },
    }))
    .unwrap()
}

/// A feed that offers `version` with `body` as the file (and `sha256` listed, if given).
fn feed(version: &str, body: &[u8], sha256: Option<&str>) -> Server {
    // the manifest names the server's own port: bind first and keep the listener (dropping it and binding again let
    // a parallel test take the port in between: "port … stayed busy")
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let file = format!("http://127.0.0.1:{port}/files/kiln.AppImage");
    let m = manifest(version, &file, sha256);
    Server::serve(l, vec![("/dev/latest.json".into(), 200, m.clone()), ("/stable/latest.json".into(), 200, m), ("/files/kiln.AppImage".into(), 200, body.to_vec())])
}

fn tampered() -> Vec<u8> {
    let mut b = PAYLOAD.to_vec();
    let last = b.len() - 2;
    b[last] ^= 0x20;
    b
}

/// A mock app at version `current` with the updater plugin and the test key.
fn app(current: &str) -> tauri::App<tauri::test::MockRuntime> {
    app_keyed(current, PUBKEY)
}

fn app_keyed(current: &str, pubkey: &str) -> tauri::App<tauri::test::MockRuntime> {
    let mut ctx = tauri::test::mock_context(tauri::test::noop_assets());
    ctx.config_mut().plugins.0.insert(
        "updater".into(),
        serde_json::json!({ "pubkey": pubkey.trim(), "requireSignedVersion": true, "dangerousInsecureTransportProtocol": true }),
    );
    ctx.package_info_mut().version = current.parse().unwrap();
    tauri::test::mock_builder().plugin(tauri_plugin_updater::Builder::new().build()).build(ctx).unwrap()
}

fn run<T>(f: impl std::future::Future<Output = T>) -> T {
    tauri::async_runtime::block_on(f)
}

fn steps(f: &[Failure]) -> Vec<(&str, Step)> {
    f.iter().map(|f| (f.source.as_str(), f.step)).collect()
}

#[test]
fn try_sources_stops_at_the_first_success() {
    let srcs = vec![dead_source("a"), dead_source("b"), dead_source("c")];
    let tried = Arc::new(Mutex::new(Vec::new()));
    let t = tried.clone();
    let (ok, failures) = run(try_sources(&srcs, move |s| {
        let t = t.clone();
        async move {
            t.lock().unwrap().push(s.name.clone());
            if s.name == "a" { Err(Failure { source: s.name, step: Step::Manifest, message: "down".into() }) } else { Ok(s.name.len()) }
        }
    }));
    assert_eq!(ok, Some(("b".to_string(), 1)));
    assert_eq!(*tried.lock().unwrap(), ["a", "b"]);
    assert_eq!(steps(&failures), [("a", Step::Manifest)]);
}

#[test]
fn check_asks_frostdev_first() {
    let (f, g) = (feed("0.2.0-dev.2", PAYLOAD, None), feed("0.2.0-dev.2", PAYLOAD, None));
    let app = app("0.2.0-dev.1");
    let (answer, failures) = run(check_sources(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev));
    let (source, update) = answer.unwrap();
    assert_eq!(source, "frostdev.io");
    assert_eq!(update.unwrap().version, "0.2.0-dev.2");
    assert!(failures.is_empty());
    assert_eq!(f.hits("/dev/latest.json"), 1);
    assert_eq!(g.total_hits(), 0, "GitHub is not asked when frostdev.io answers");
}

#[test]
fn check_falls_back_to_github_when_frostdev_is_down_or_has_no_manifest() {
    let g = feed("0.2.0-dev.2", PAYLOAD, None);
    let app = app("0.2.0-dev.1");
    let (answer, failures) = run(check_sources(app.handle(), &[dead_source("frostdev.io"), g.source("GitHub")], Channel::Dev));
    assert_eq!(answer.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Manifest)]);
    assert!(failures[0].message.starts_with("not reachable"), "{}", failures[0].message);

    let empty = Server::start(vec![]);
    let (answer, failures) = run(check_sources(app.handle(), &[empty.source("frostdev.io"), g.source("GitHub")], Channel::Dev));
    assert_eq!(answer.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Manifest)]);
    assert_eq!(failures[0].message, "no update file there");
    assert_eq!(empty.hits("/dev/latest.json"), 1);
}

#[test]
fn up_to_date_is_an_answer() {
    let (f, g) = (feed("0.2.0-dev.2", PAYLOAD, None), feed("0.2.0-dev.3", PAYLOAD, None));
    let app = app("0.2.0-dev.2");
    let (answer, failures) = run(check_sources(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev));
    let (source, update) = answer.unwrap();
    assert_eq!(source, "frostdev.io");
    assert!(update.is_none());
    assert!(failures.is_empty());
    assert_eq!(g.total_hits(), 0);
}

#[test]
fn stable_channel_reads_the_stable_manifest_and_skips_pre_releases() {
    let f = feed("0.2.0-dev.2", PAYLOAD, None);
    let app = app("0.1.0");
    let (answer, _) = run(check_sources(app.handle(), &[f.source("frostdev.io")], Channel::Stable));
    assert!(answer.unwrap().1.is_none(), "a dev build is no update for stable");
    assert_eq!(f.hits("/stable/latest.json"), 1);
    assert_eq!(f.hits("/dev/latest.json"), 0);
}

#[test]
fn download_verifies_and_returns_the_file() {
    let sha = kiln_manage::hash::sha256_hex(PAYLOAD);
    let f = feed("0.2.0-dev.2", PAYLOAD, Some(&sha));
    let app = app("0.2.0-dev.1");
    let progress = Arc::new(Mutex::new(None));
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io")], Channel::Dev, progress.clone()));
    let (source, acq) = got.unwrap();
    assert_eq!(source, "frostdev.io");
    assert_eq!(acq.bytes, PAYLOAD);
    assert!(failures.is_empty());
    let p = progress.lock().unwrap().clone().unwrap();
    assert_eq!((p.received, p.total, p.source.as_str()), (PAYLOAD.len() as u64, Some(PAYLOAD.len() as u64), "frostdev.io"));
}

#[test]
fn a_bad_signature_on_frostdev_falls_back_to_github() {
    let (f, g) = (feed("0.2.0-dev.2", &tampered(), None), feed("0.2.0-dev.2", PAYLOAD, None));
    let app = app("0.2.0-dev.1");
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev, Arc::new(Mutex::new(None))));
    let (source, acq) = got.unwrap();
    assert_eq!(source, "GitHub");
    assert_eq!(acq.bytes, PAYLOAD);
    assert_eq!(steps(&failures), [("frostdev.io", Step::Signature)]);
    assert_eq!(f.hits("/files/kiln.AppImage"), 1);
    assert_eq!(g.hits("/files/kiln.AppImage"), 1);
}

#[test]
fn a_bad_signature_is_never_accepted() {
    let (f, g) = (feed("0.2.0-dev.2", &tampered(), None), feed("0.2.0-dev.2", &tampered(), None));
    let app = app("0.2.0-dev.1");
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev, Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(steps(&failures), [("frostdev.io", Step::Signature), ("GitHub", Step::Signature)]);
    assert_eq!(failures[0].message, "the signature does not match the file");
}

#[test]
fn a_file_signed_for_another_version_is_refused() {
    // the manifest claims 0.2.0-dev.3; the file is signed as 0.2.0-dev.2 (an old release under a new number)
    let f = feed("0.2.0-dev.3", PAYLOAD, None);
    let app = app("0.2.0-dev.1");
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io")], Channel::Dev, Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(steps(&failures), [("frostdev.io", Step::Signature)]);
    assert!(failures[0].message.contains("signed for 0.2.0-dev.2"), "{}", failures[0].message);
}

#[test]
fn a_failed_download_or_a_wrong_sha256_falls_back() {
    let g = feed("0.2.0-dev.2", PAYLOAD, None);
    let app = app("0.2.0-dev.1");
    // the manifest is there, the file is not
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let m = manifest("0.2.0-dev.2", &format!("http://127.0.0.1:{port}/files/missing.AppImage"), None);
    let f = Server::serve(l, vec![("/dev/latest.json".into(), 200, m)]);
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev, Arc::new(Mutex::new(None))));
    assert_eq!(got.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Download)]);
    assert!(failures[0].message.contains("404"), "{}", failures[0].message);

    let wrong = "0".repeat(64);
    let f = feed("0.2.0-dev.2", PAYLOAD, Some(&wrong));
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], Channel::Dev, Arc::new(Mutex::new(None))));
    assert_eq!(got.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Hash)]);
}

#[test]
fn nothing_newer_is_no_download() {
    let f = feed("0.2.0-dev.2", PAYLOAD, None);
    let app = app("0.2.0-dev.2");
    let (got, failures) = run(acquire(app.handle(), &[f.source("frostdev.io")], Channel::Dev, Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(failures[0].message, "it offers no newer version");
    assert_eq!(f.hits("/files/kiln.AppImage"), 0);
}

// ---------------------------------------------------------------- dev.29: test builds and channel switches

const BETA_PUBKEY: &str = include_str!("testdata/beta/test.pub");

/// `payload.bin`'s signature (the beta test key) for a build id.
fn beta_sig(id: &str) -> &'static str {
    match id {
        "0.2.0-dev.2" => include_str!("testdata/beta/payload-0.2.0-dev.2.sig"),
        "0.2.0-dev.3" => include_str!("testdata/beta/payload-0.2.0-dev.3.sig"),
        "0.2.0-dev.3+beta.1" => include_str!("testdata/beta/payload-0.2.0-dev.3-beta.1.sig"),
        "0.2.0-dev.3+beta.2" => include_str!("testdata/beta/payload-0.2.0-dev.3-beta.2.sig"),
        _ => panic!("no test signature for {id}"),
    }
}

/// A feed with one manifest per channel: (channel, the version the manifest announces, the id the file is signed for,
/// the sha256 it lists). Every manifest points at the same file.
fn channel_feed(entries: &[(&str, &str, &str, Option<&str>)]) -> Server {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let file = format!("http://127.0.0.1:{port}/files/kiln.AppImage");
    let mut routes = vec![("/files/kiln.AppImage".to_string(), 200, PAYLOAD.to_vec())];
    for (channel, announced, signed, sha) in entries {
        let mut entry = serde_json::json!({ "url": file, "signature": beta_sig(signed).trim() });
        if let Some(s) = sha {
            entry["sha256"] = (*s).into();
        }
        let beta = feed::Build::parse(announced).and_then(|b| b.beta);
        let m = serde_json::to_vec(&serde_json::json!({
            "version": announced,
            "notes": "Test build.",
            "pub_date": "2026-11-15T12:00:00Z",
            "platforms": { "linux-x86_64": entry.clone(), "windows-x86_64": entry },
            "kiln": { "format": 1, "channel": channel, "beta": beta },
        }))
        .unwrap();
        routes.push((format!("/{channel}/latest.json"), 200, m));
    }
    Server::serve(l, routes)
}

fn offered(app: &tauri::App<tauri::test::MockRuntime>, srcs: &[Source], want: &Want) -> Option<String> {
    let (answer, failures) = run(check_sources_for(app.handle(), srcs, want));
    assert!(failures.is_empty(), "{failures:?}");
    answer.unwrap().1.map(|u| u.version)
}

#[test]
fn a_test_build_is_offered_on_the_beta_channel_and_never_on_the_release_line() {
    // a release build (0.2.0-dev.2): the beta channel names test build 1 of 0.2.0-dev.3
    let f = channel_feed(&[("beta", "0.2.0-dev.3+beta.1", "0.2.0-dev.3+beta.1", None), ("dev", "0.2.0-dev.3+beta.1", "0.2.0-dev.3+beta.1", None)]);
    let app = app_keyed("0.2.0-dev.2", BETA_PUBKEY);
    let src = [f.source("frostdev.io")];
    assert_eq!(offered(&app, &src, &Want::update(Channel::Beta, "0.2.0-dev.2".into())).as_deref(), Some("0.2.0-dev.3+beta.1"));
    assert_eq!(f.hits("/beta/latest.json"), 1);
    // the same test build in the release line's manifest (a mistake on the server) is still no update for it
    assert_eq!(offered(&app, &src, &Want::update(Channel::Dev, "0.2.0-dev.2".into())), None);
    // the download verifies the signature for the whole id, beta number included
    let (got, failures) = run(acquire_for(app.handle(), &src, &Want::update(Channel::Beta, "0.2.0-dev.2".into()), Arc::new(Mutex::new(None))));
    assert!(failures.is_empty(), "{failures:?}");
    let (_, acq) = got.unwrap();
    assert_eq!((acq.update.version.as_str(), acq.bytes.as_slice()), ("0.2.0-dev.3+beta.1", PAYLOAD));
}

#[test]
fn a_test_builds_file_cannot_pass_for_another_build() {
    let app = app_keyed("0.2.0-dev.2", BETA_PUBKEY);
    // beta 1's file announced as beta 2, and as the final build: the signed id does not match
    for announced in ["0.2.0-dev.3+beta.2", "0.2.0-dev.3"] {
        let f = channel_feed(&[("beta", announced, "0.2.0-dev.3+beta.1", None)]);
        let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::update(Channel::Beta, "0.2.0-dev.2".into()), Arc::new(Mutex::new(None))));
        assert!(got.is_none(), "{announced}");
        assert_eq!(steps(&failures), [("frostdev.io", Step::Signature)], "{announced}");
        assert!(failures[0].message.contains("signed for 0.2.0-dev.3+beta.1"), "{}", failures[0].message);
    }
}

#[test]
fn beta_one_to_beta_two_to_the_final_build() {
    let app = app_keyed("0.2.0-dev.3", BETA_PUBKEY);
    let progress = || Arc::new(Mutex::new(None));
    // test build 1 is offered test build 2 (same version, higher beta number)
    let f = channel_feed(&[("beta", "0.2.0-dev.3+beta.2", "0.2.0-dev.3+beta.2", None)]);
    let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::update(Channel::Beta, "0.2.0-dev.3+beta.1".into()), progress()));
    assert!(failures.is_empty(), "{failures:?}");
    assert_eq!(got.unwrap().1.update.version, "0.2.0-dev.3+beta.2");
    // test build 2 is offered the final build of its version, on the beta channel and following the release again
    let f = channel_feed(&[("beta", "0.2.0-dev.3", "0.2.0-dev.3", None), ("dev", "0.2.0-dev.3", "0.2.0-dev.3", None)]);
    for ch in [Channel::Beta, Channel::Dev] {
        let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::update(ch, "0.2.0-dev.3+beta.2".into()), progress()));
        assert!(failures.is_empty(), "{ch:?}: {failures:?}");
        assert_eq!(got.unwrap().1.update.version, "0.2.0-dev.3", "{ch:?}");
    }
    // and the final build is not offered a test build of its own version
    let f = channel_feed(&[("beta", "0.2.0-dev.3+beta.2", "0.2.0-dev.3+beta.2", None)]);
    assert_eq!(offered(&app, &[f.source("frostdev.io")], &Want::update(Channel::Beta, "0.2.0-dev.3".into())), None);
}

#[test]
fn going_down_to_the_release_happens_only_as_the_switch_the_player_started() {
    // test build 2 of 0.2.0-dev.3 following the release again; the release line's newest is 0.2.0-dev.2, older
    let f = channel_feed(&[("dev", "0.2.0-dev.2", "0.2.0-dev.2", Some(&kiln_manage::hash::sha256_hex(PAYLOAD)))]);
    let app = app_keyed("0.2.0-dev.3", BETA_PUBKEY);
    let cur = || "0.2.0-dev.3+beta.2".to_string();
    // a background check and an Update: nothing (never down), and no file is fetched
    assert_eq!(offered(&app, &[f.source("frostdev.io")], &Want::update(Channel::Dev, cur())), None);
    let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::update(Channel::Dev, cur()), Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(failures[0].message, "it offers no newer version");
    assert_eq!(f.hits("/files/kiln.AppImage"), 0);
    // the switch back: the older release, downloaded and verified (signature, signed version, sha256)
    let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::switch_with_base(Channel::Dev, cur(), "0.2.0-dev.2"), Arc::new(Mutex::new(None))));
    assert!(failures.is_empty(), "{failures:?}");
    let (source, acq) = got.unwrap();
    assert_eq!((source.as_str(), acq.update.version.as_str(), acq.bytes.as_slice()), ("frostdev.io", "0.2.0-dev.2", PAYLOAD));
    assert_eq!(f.hits("/files/kiln.AppImage"), 1);
}

#[test]
fn a_switch_uses_both_sources_and_every_check_of_an_update() {
    let app = app_keyed("0.2.0-dev.3", BETA_PUBKEY);
    let cur = || "0.2.0-dev.3+beta.2".to_string();
    let g = channel_feed(&[("dev", "0.2.0-dev.2", "0.2.0-dev.2", None)]);
    // frostdev.io lists a wrong sha256: GitHub's copy is taken
    let f = channel_feed(&[("dev", "0.2.0-dev.2", "0.2.0-dev.2", Some(&"0".repeat(64)))]);
    let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io"), g.source("GitHub")], &Want::switch_with_base(Channel::Dev, cur(), "0.2.0-dev.2"), Arc::new(Mutex::new(None))));
    assert_eq!(got.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Hash)]);
    // frostdev.io is down: GitHub
    let (got, failures) = run(acquire_for(app.handle(), &[dead_source("frostdev.io"), g.source("GitHub")], &Want::switch_with_base(Channel::Dev, cur(), "0.2.0-dev.2"), Arc::new(Mutex::new(None))));
    assert_eq!(got.unwrap().0, "GitHub");
    assert_eq!(steps(&failures), [("frostdev.io", Step::Manifest)]);
    // a file signed for another build is refused on a switch too
    let bad = channel_feed(&[("dev", "0.2.0-dev.2", "0.2.0-dev.3+beta.1", None)]);
    let (got, failures) = run(acquire_for(app.handle(), &[bad.source("frostdev.io")], &Want::switch_with_base(Channel::Dev, cur(), "0.2.0-dev.2"), Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(steps(&failures), [("frostdev.io", Step::Signature)]);
}

#[test]
fn a_switch_to_the_beta_never_goes_down() {
    // the final build of 0.2.0-dev.3 runs; the beta channel still names its test build 2: nothing to switch to
    let f = channel_feed(&[("beta", "0.2.0-dev.3+beta.2", "0.2.0-dev.3+beta.2", None)]);
    let app = app_keyed("0.2.0-dev.3", BETA_PUBKEY);
    let (got, failures) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::switch(Channel::Beta, "0.2.0-dev.3".into()), Arc::new(Mutex::new(None))));
    assert!(got.is_none());
    assert_eq!(failures[0].message, "it offers no other build to switch to");
    assert_eq!(f.hits("/files/kiln.AppImage"), 0);
    // from the release before, it is a switch up
    let (got, _) = run(acquire_for(app.handle(), &[f.source("frostdev.io")], &Want::switch(Channel::Beta, "0.2.0-dev.2".into()), Arc::new(Mutex::new(None))));
    assert_eq!(got.unwrap().1.update.version, "0.2.0-dev.3+beta.2");
}

#[test]
fn failed_restore_does_not_mark_the_beta_as_left() {
    let root = std::env::temp_dir().join(format!("kiln-switch-restore-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    // The user asked for a copy that has since disappeared. Do not restart
    // or claim a release ran: the next beta must retain the original copy.
    let result = prepare_switch_restart(&root, "0.2.0-dev.28", Channel::Dev, true);
    assert!(result.is_err());
    assert!(!root.join(crate::snapshot::LAST_BUILD).exists());
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn release_selection_never_falls_back_below_the_newest_offer() {
    let sources = feed::release_sources();
    for order in [["0.2.0-dev.2", "0.2.0-dev.3"], ["0.2.0-dev.3", "0.2.0-dev.2"]] {
        let attempted = Arc::new(Mutex::new(Vec::new()));
        let seen = attempted.clone();
        let offers = sources.iter().cloned().zip(order.map(String::from)).collect();
        let (got, _) = run(acquire_release_offers(offers, |v: &String| v.as_str(), move |source, version| {
            seen.lock().unwrap().push(version.clone());
            async move {
                if version == "0.2.0-dev.3" { Err(failure(&source, (Step::Signature, "bad signature".into()))) }
                else { Ok(version) }
            }
        }));
        assert!(got.is_none());
        assert_eq!(*attempted.lock().unwrap(), ["0.2.0-dev.3"]);
    }
}

#[test]
fn accepted_restore_choice_is_in_every_status_after_reopening() {
    struct Reset(Option<Inner>);
    impl Drop for Reset { fn drop(&mut self) { *state() = self.0.take().unwrap(); } }
    let install = kind::InstallInfo { kind: kind::Kind::Appimage, in_place: true, reason: None };
    let original = std::mem::replace(&mut *state(), Inner { status: initial_status("0.2.0-dev.29", install), pending: None, busy: false, restore: false });
    let _reset = Reset(Some(original));
    begin(Some(Channel::Dev), true).unwrap();
    for _ in 0..2 {
        assert_eq!(serde_json::to_value(status()).unwrap()["restoreRequested"], true);
    }
}

#[test]
fn the_first_status_waits_for_the_start() {
    // the window's first update_status can come before start() read update.json: it waits for it (bounded)
    let t = std::thread::spawn(|| wait_started(Duration::from_secs(5)));
    std::thread::sleep(Duration::from_millis(50));
    mark_started();
    assert!(t.join().unwrap(), "update_status answers once start() has the state");
    assert!(wait_started(Duration::from_millis(1)), "and at once after that");
}
