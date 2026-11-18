//! kiln_manage::vendor: downloading vendor components into `<game>/kiln/vendor/<vendor>/<version>/`, against a local
//! HTTP server on 127.0.0.1 with fake files. No test touches the network: the transport here connects to the test's
//! own listener whatever host a URL names, after the real client's host, URL and redirect rules (kiln_manage::net)
//! have passed the request; a refused request never reaches it.

use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::net::{self, Client, Response, Transport, WireRequest};
use kiln_manage::paths::DataDirs;
use kiln_manage::task::Progress;
use kiln_manage::vendor::manifest::{self, Component, VendorManifest, NOT_PINNED};
use kiln_manage::vendor::store::{PART_SUFFIX, RECEIPT};
use kiln_manage::vendor::{agreements, download, Agreements, Store, VendorState};
use kiln_manage::ErrorCode;
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

// ---------------------------------------------------------------- a local server

/// What the server does with one request for a path.
#[derive(Clone)]
enum Behave {
    /// The bytes, honouring a Range request with a 206.
    Serve(Vec<u8>),
    /// The bytes with a 200, whatever Range asked.
    IgnoreRange(Vec<u8>),
    /// A Content-Length of the whole, then only the first `at` bytes and the connection closes.
    Truncate(Vec<u8>, usize),
    /// A redirect to this absolute URL.
    Redirect(String),
    Status(u16),
}

#[derive(Debug, Clone)]
struct Seen {
    host: String,
    path: String,
    headers: Vec<(String, String)>,
}

impl Seen {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
}

#[derive(Default)]
struct Script {
    /// By host and path; the last behaviour of a list repeats.
    routes: HashMap<(String, String), VecDeque<Behave>>,
    seen: Vec<Seen>,
}

struct Server {
    addr: SocketAddr,
    script: Arc<Mutex<Script>>,
    stop: Arc<AtomicBool>,
}

impl Server {
    fn start() -> Server {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let script = Arc::new(Mutex::new(Script::default()));
        let stop = Arc::new(AtomicBool::new(false));
        let (s, st) = (script.clone(), stop.clone());
        std::thread::spawn(move || {
            for conn in listener.incoming() {
                if st.load(Ordering::SeqCst) {
                    break;
                }
                if let Ok(c) = conn {
                    let _ = serve(c, &s);
                }
            }
        });
        Server { addr, script, stop }
    }

    /// Answers `url` with these behaviours in order (the last one repeats).
    fn on(&self, url: &str, behaves: &[Behave]) {
        let u = net::policy::parse_url(url).unwrap();
        self.script.lock().unwrap().routes.insert((u.host, u.path_and_query), behaves.iter().cloned().collect());
    }

    fn seen(&self) -> Vec<Seen> {
        self.script.lock().unwrap().seen.clone()
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        let _ = TcpStream::connect(self.addr);
    }
}

fn serve(c: TcpStream, script: &Mutex<Script>) -> std::io::Result<()> {
    c.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut r = BufReader::new(c.try_clone()?);
    let mut line = String::new();
    r.read_line(&mut line)?;
    let path = line.split(' ').nth(1).unwrap_or("/").to_string();
    let mut headers = Vec::new();
    loop {
        let mut h = String::new();
        if r.read_line(&mut h)? == 0 || h == "\r\n" {
            break;
        }
        if let Some((k, v)) = h.trim_end().split_once(':') {
            headers.push((k.trim().to_string(), v.trim().to_string()));
        }
    }
    let host = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("host")).map(|(_, v)| v.clone()).unwrap_or_default();
    let seen = Seen { host: host.clone(), path: path.clone(), headers: headers.clone() };
    let range = seen.header("range").and_then(|v| v.strip_prefix("bytes=")).and_then(|v| v.strip_suffix('-')).and_then(|v| v.parse::<usize>().ok());
    let behave = {
        let mut s = script.lock().unwrap();
        s.seen.push(seen);
        match s.routes.get_mut(&(host, path)) {
            Some(q) if q.len() > 1 => q.pop_front(),
            Some(q) => q.front().cloned(),
            None => None,
        }
    };
    let mut w = c;
    let head = |status: &str, extra: &str, len: usize| format!("HTTP/1.1 {status}\r\nContent-Length: {len}\r\n{extra}Connection: close\r\n\r\n");
    match behave {
        None => w.write_all(head("404 Not Found", "", 0).as_bytes())?,
        Some(Behave::Status(s)) => w.write_all(head(&format!("{s} Whatever"), "", 0).as_bytes())?,
        Some(Behave::Redirect(to)) => w.write_all(head("302 Found", &format!("Location: {to}\r\n"), 0).as_bytes())?,
        Some(Behave::Serve(b)) => match range {
            Some(from) if from < b.len() => {
                let cr = format!("Content-Range: bytes {from}-{}/{}\r\n", b.len() - 1, b.len());
                w.write_all(head("206 Partial Content", &cr, b.len() - from).as_bytes())?;
                w.write_all(&b[from..])?;
            }
            Some(_) => w.write_all(head("416 Range Not Satisfiable", "", 0).as_bytes())?,
            None => {
                w.write_all(head("200 OK", "", b.len()).as_bytes())?;
                w.write_all(&b)?;
            }
        },
        Some(Behave::IgnoreRange(b)) => {
            w.write_all(head("200 OK", "", b.len()).as_bytes())?;
            w.write_all(&b)?;
        }
        Some(Behave::Truncate(b, at)) => {
            w.write_all(head("200 OK", "", b.len()).as_bytes())?;
            w.write_all(&b[..at])?;
        }
    }
    w.flush()
}

/// The transport: plain HTTP to the local server, with the request's host in `Host`. It keeps the real transport's
/// rules that matter here: no redirect of its own, at most `max_response` bytes to the sink, a short body is an error.
struct Local {
    addr: SocketAddr,
}

impl Local {
    fn run(&self, req: WireRequest, mut sink: Option<&mut dyn Write>) -> kiln_manage::Result<Response> {
        let fail = || net::network_error(&req.url);
        let mut s = TcpStream::connect(self.addr).map_err(|_| fail())?;
        s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut head = format!("{} {} HTTP/1.1\r\nHost: {}\r\n", req.method.as_str(), req.url.path_and_query, req.url.host);
        for (k, v) in &req.headers {
            head.push_str(&format!("{k}: {v}\r\n"));
        }
        head.push_str("Connection: close\r\n\r\n");
        s.write_all(head.as_bytes()).map_err(|_| fail())?;
        let mut r = BufReader::new(s);
        let mut line = String::new();
        r.read_line(&mut line).map_err(|_| fail())?;
        let status: u16 = line.split(' ').nth(1).and_then(|c| c.parse().ok()).ok_or_else(fail)?;
        let mut headers = Vec::new();
        loop {
            let mut h = String::new();
            if r.read_line(&mut h).map_err(|_| fail())? == 0 || h == "\r\n" {
                break;
            }
            if let Some((k, v)) = h.trim_end().split_once(':') {
                headers.push((k.trim().to_string(), v.trim().to_string()));
            }
        }
        let len: u64 = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("content-length")).and_then(|(_, v)| v.parse().ok()).unwrap_or(0);
        let mut body = Vec::new();
        if (200..300).contains(&status) || sink.is_none() {
            let mut buf = [0u8; 16 * 1024];
            let mut total = 0u64;
            while total < len {
                let n = r.read(&mut buf).map_err(|_| fail())?;
                if n == 0 {
                    return Err(fail());
                }
                if total + n as u64 > req.max_response {
                    return Err(net::too_large(&req.url));
                }
                total += n as u64;
                match sink.as_deref_mut() {
                    Some(w) => w.write_all(&buf[..n]).map_err(|_| kiln_manage::Error::new(ErrorCode::Io, "The download could not be kept."))?,
                    None => body.extend_from_slice(&buf[..n]),
                }
            }
        }
        Ok(Response { status, headers, body })
    }
}

impl Transport for Local {
    fn exchange(&self, req: WireRequest) -> kiln_manage::Result<Response> {
        self.run(req, None)
    }
    fn download(&self, req: WireRequest, sink: &mut dyn Write) -> kiln_manage::Result<Response> {
        self.run(req, Some(sink))
    }
}

// ---------------------------------------------------------------- fixtures

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn tmp(name: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("kiln-vendor-{name}-{}", kiln_manage::hash::hex(&kiln_manage::util::random_bytes::<6>().unwrap())));
    std::fs::create_dir_all(p.join("game/kiln")).unwrap();
    Tmp(p)
}

fn bytes(seed: u8, len: usize) -> Vec<u8> {
    (0..len).map(|i| (i as u32).wrapping_mul(2_654_435_761).wrapping_add(seed as u32).to_le_bytes()[1]).collect()
}

const A_URL: &str = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/a.dll";
const B_URL: &str = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/b.dll";

/// A manifest like the shipped one: AMD's component with two fake files (pinned to `a` and `b`), version 2.4.0.
fn manifest_for(a: &[u8], b: &[u8]) -> VendorManifest {
    manifest_with("2.4.0", &[("a.dll", A_URL, a), ("b.dll", B_URL, b)])
}

fn manifest_with(version: &str, files: &[(&str, &str, &[u8])]) -> VendorManifest {
    let mut v: serde_json::Value = serde_json::from_str(manifest::BUILTIN).unwrap();
    let c = &mut v["components"][0];
    c["version"] = version.into();
    c["files"] = files.iter().map(|(name, url, data)| serde_json::json!({
        "name": name, "url": url.replace("v2.4.0", &format!("v{version}")), "size": data.len(), "sha256": kiln_manage::hash::sha256_hex(data),
    })).collect();
    manifest::parse(&v.to_string()).unwrap()
}

fn amd(m: &VendorManifest) -> Component {
    m.component("amd-fsr").unwrap().clone()
}

/// Reports, and a cancel after `cancel_after` reports.
#[derive(Default)]
struct Recorder {
    reports: Vec<(Option<f32>, String)>,
    cancel_after: Option<usize>,
}

impl Progress for Recorder {
    fn report(&mut self, fraction: Option<f32>, message: &str) {
        self.reports.push((fraction, message.to_string()));
    }
    fn canceled(&self) -> bool {
        self.cancel_after.is_some_and(|n| self.reports.len() >= n)
    }
}

struct Lab {
    t: Tmp,
    server: Server,
    transport: Local,
}

impl Lab {
    fn new(name: &str) -> Lab {
        let server = Server::start();
        let transport = Local { addr: server.addr };
        Lab { t: tmp(name), server, transport }
    }
    fn game(&self) -> PathBuf {
        self.t.0.join("game")
    }
    fn store(&self) -> Store {
        Store::at(self.game())
    }
    fn client(&self) -> Client<'_> {
        Client::new(download::policy(), &self.transport, &download::user_agent())
    }
    fn get(&self, c: &Component, p: &mut Recorder) -> kiln_manage::Result<download::DownloadReport> {
        download::download(&self.store(), c, &self.client(), "kiln-launcher test", p)
    }
    fn folder(&self) -> PathBuf {
        self.game().join("kiln/vendor/amd/2.4.0")
    }
}

fn names_in(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir).map(|d| d.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    v.sort();
    v
}

// ---------------------------------------------------------------- tests

#[test]
fn a_component_downloads_is_checked_and_gets_a_receipt() {
    let lab = Lab::new("ok");
    let (a, b) = (bytes(1, 70_000), bytes(2, 1_234));
    let m = manifest_for(&a, &b);
    let c = amd(&m);
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    let none = Agreements::default();
    let before = lab.store().component_status(&c, &none);
    assert_eq!((before.state, before.size, before.partial), (VendorState::NotDownloaded, 71_234, 0));
    assert_eq!(before.hosts, ["raw.githubusercontent.com"]);
    assert!(before.installed.is_none());

    let mut p = Recorder::default();
    let r = lab.get(&c, &mut p).unwrap();
    assert_eq!(r.folder, "kiln/vendor/amd/2.4.0");
    assert_eq!(r.fetched, 71_234);
    assert!(r.kept.is_empty() && r.replaced.is_empty() && r.warnings.is_empty());
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    assert_eq!(std::fs::read(lab.folder().join("b.dll")).unwrap(), b);
    assert_eq!(names_in(&lab.folder()), ["a.dll", "b.dll", RECEIPT], "no part file is left");
    // the receipt: what, when, the hashes, the licence agreed to
    let receipt: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(lab.folder().join(RECEIPT)).unwrap()).unwrap();
    assert_eq!(receipt["format"], 1);
    assert_eq!(receipt["component"], "amd-fsr");
    assert_eq!(receipt["vendor"], "amd");
    assert_eq!(receipt["version"], "2.4.0");
    assert_eq!(receipt["installer"], "kiln-launcher test");
    assert_eq!(receipt["license"]["name"], "MIT licence");
    assert_eq!(receipt["files"][0]["sha256"], kiln_manage::hash::sha256_hex(&a));
    assert_eq!(receipt["files"][1]["size"], 1_234);
    assert_eq!(receipt["files"][0]["url"], A_URL);
    assert!(kiln_manage::util::parse_rfc3339(receipt["installed"].as_str().unwrap()).is_some());
    // progress: fractions that only grow, ending at 1, a message naming the file
    let fr: Vec<f32> = p.reports.iter().filter_map(|(f, _)| *f).collect();
    assert!(fr.windows(2).all(|w| w[0] <= w[1]), "{fr:?}");
    assert_eq!(fr.last(), Some(&1.0));
    assert!(p.reports.iter().any(|(_, m)| m.starts_with("Downloading a.dll")));
    // two plain GETs to the vendor's host: Kiln's user agent, nothing about the player, no credential, no range
    let seen = lab.server.seen();
    assert_eq!(seen.iter().map(|s| s.path.as_str()).collect::<Vec<_>>(), [
        "/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/a.dll",
        "/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/b.dll"]);
    for s in &seen {
        assert_eq!(s.host, "raw.githubusercontent.com");
        assert_eq!(s.header("user-agent").unwrap(), format!("kiln/{}", kiln_manage::VERSION));
        let names: Vec<String> = s.headers.iter().map(|(k, _)| k.to_ascii_lowercase()).collect();
        assert_eq!(names, ["host", "user-agent", "connection"], "only what a plain request needs");
    }
    let after = lab.store().component_status(&c, &none);
    assert_eq!(after.state, VendorState::Downloaded);
    let i = after.installed.unwrap();
    assert_eq!((i.version.as_str(), i.folder.as_str(), i.size_on_disk), ("2.4.0", "kiln/vendor/amd/2.4.0", 71_234));
    assert!(i.problems.is_empty());

    // a second download checks what is there and fetches nothing
    let again = lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!((again.fetched, again.kept.len()), (0, 2));
    assert_eq!(lab.server.seen().len(), 2);
}

#[test]
fn a_file_with_another_hash_is_deleted_and_the_error_says_so() {
    let lab = Lab::new("mismatch");
    let (a, b) = (bytes(1, 5_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    let mut wrong = a.clone();
    wrong[4_000] ^= 0xff;
    lab.server.on(A_URL, &[Behave::Serve(wrong)]);
    lab.server.on(B_URL, &[Behave::Serve(b)]);
    let e = lab.get(&c, &mut Recorder::default()).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch);
    assert!(e.message.starts_with("a.dll from AMD (raw.githubusercontent.com) is not the file Kiln expects"), "{}", e.message);
    assert!(e.message.contains("Kiln deleted it"), "{}", e.message);
    assert!(names_in(&lab.folder()).is_empty(), "nothing is kept: {:?}", names_in(&lab.folder()));
    let s = lab.store().component_status(&c, &Agreements::default());
    assert_eq!((s.state, s.partial), (VendorState::NotDownloaded, 0));
}

#[test]
fn an_answer_longer_than_the_manifest_size_is_refused() {
    let lab = Lab::new("long");
    let (a, b) = (bytes(1, 5_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    let mut longer = a.clone();
    longer.extend_from_slice(&[0u8; 40_000]);
    lab.server.on(A_URL, &[Behave::Serve(longer)]);
    let e = lab.get(&c, &mut Recorder::default()).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch);
    assert!(e.message.starts_with("a.dll from AMD (raw.githubusercontent.com) is larger than the"), "{}", e.message);
    assert!(names_in(&lab.folder()).is_empty());
}

#[test]
fn a_truncated_download_keeps_its_part_and_goes_on_with_a_range_request() {
    let lab = Lab::new("resume");
    let (a, b) = (bytes(1, 100_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    lab.server.on(A_URL, &[Behave::Truncate(a.clone(), 40_000), Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    let e = lab.get(&c, &mut Recorder::default()).unwrap_err();
    assert_eq!(e.code, ErrorCode::Network);
    assert!(e.message.contains("a.dll") && e.message.contains("goes on from there"), "{}", e.message);
    let part = lab.folder().join(format!("a.dll{PART_SUFFIX}"));
    assert_eq!(std::fs::metadata(&part).unwrap().len(), 40_000);
    let s = lab.store().component_status(&c, &Agreements::default());
    assert_eq!((s.state, s.partial), (VendorState::NotDownloaded, 40_000));
    let r = lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(r.fetched, 60_000 + 300, "only the rest was fetched");
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    let seen = lab.server.seen();
    assert_eq!(seen[0].header("range"), None);
    assert_eq!(seen[1].header("range"), Some("bytes=40000-"));
    assert_eq!(lab.store().component_status(&c, &Agreements::default()).state, VendorState::Downloaded);
}

#[test]
fn a_resume_the_server_does_not_continue_starts_the_file_again_once() {
    // the server ignores the range and sends the whole file: more than the rest, refused, then a fresh request
    let lab = Lab::new("norange");
    let (a, b) = (bytes(1, 80_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    std::fs::create_dir_all(lab.folder()).unwrap();
    std::fs::write(lab.folder().join("a.dll.part"), &a[..30_000]).unwrap();
    lab.server.on(A_URL, &[Behave::IgnoreRange(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    let ranges: Vec<Option<String>> = lab.server.seen().iter().filter(|s| s.path.ends_with("a.dll")).map(|s| s.header("range").map(str::to_string)).collect();
    assert_eq!(ranges, [Some("bytes=30000-".to_string()), None]);

    // a part that is not the start of the file: the rest arrives, the whole does not verify, so once more from 0
    let lab = Lab::new("badpart");
    std::fs::create_dir_all(lab.folder()).unwrap();
    std::fs::write(lab.folder().join("a.dll.part"), vec![7u8; 30_000]).unwrap();
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    let ranges: Vec<Option<String>> = lab.server.seen().iter().filter(|s| s.path.ends_with("a.dll")).map(|s| s.header("range").map(str::to_string)).collect();
    assert_eq!(ranges, [Some("bytes=30000-".to_string()), None]);

    // 416 to the range: from 0
    let lab = Lab::new("416");
    std::fs::create_dir_all(lab.folder()).unwrap();
    std::fs::write(lab.folder().join("b.dll.part"), &b[..100]).unwrap();
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Status(416), Behave::Serve(b.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(std::fs::read(lab.folder().join("b.dll")).unwrap(), b);

    // a fresh download that does not verify is not tried again on its own
    let lab = Lab::new("twice");
    let mut wrong = a.clone();
    wrong[50_000] ^= 1;
    std::fs::create_dir_all(lab.folder()).unwrap();
    std::fs::write(lab.folder().join("a.dll.part"), &a[..10]).unwrap();
    lab.server.on(A_URL, &[Behave::Serve(wrong)]);
    let e = lab.get(&c, &mut Recorder::default()).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch);
    assert_eq!(lab.server.seen().len(), 2, "the resume and one fresh try");
    assert!(!lab.folder().join("a.dll.part").exists());
}

#[test]
fn cancel_stops_the_download_and_keeps_what_arrived() {
    let lab = Lab::new("cancel");
    let (a, b) = (bytes(1, 300_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    let e = lab.get(&c, &mut Recorder { cancel_after: Some(1), ..Default::default() }).unwrap_err();
    assert_eq!(e.code, ErrorCode::Canceled);
    let part = std::fs::metadata(lab.folder().join("a.dll.part")).unwrap().len();
    assert!(part > 0 && part < 300_000, "{part}");
    assert!(!lab.folder().join(RECEIPT).exists());
    assert_eq!(lab.store().component_status(&c, &Agreements::default()).partial, part);
    // a cancel before the first file: nothing is asked
    let before = lab.server.seen().len();
    let mut p = Recorder { cancel_after: Some(0), ..Default::default() };
    assert_eq!(lab.get(&c, &mut p).unwrap_err().code, ErrorCode::Canceled);
    assert_eq!(lab.server.seen().len(), before);
    // the next download goes on from the part
    lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    assert_eq!(lab.server.seen().last().map(|s| s.path.ends_with("b.dll")), Some(true));
    assert!(lab.server.seen().iter().any(|s| s.header("range") == Some(&format!("bytes={part}-"))));
}

#[test]
fn a_component_that_is_not_pinned_is_never_downloaded() {
    let t = tmp("unpinned");
    let offline = net::fake::FakeTransport::offline();
    let client = Client::new(download::policy(), &offline, &download::user_agent());
    let m = manifest::builtin().unwrap();
    for c in &m.components {
        let st = Store::at(t.0.join("game")).component_status(c, &Agreements::default());
        assert_eq!(st.state, VendorState::Unavailable);
        assert!(st.reason.as_deref().unwrap().contains("checksums"));
        let e = download::download(&Store::at(t.0.join("game")), c, &client, "t", &mut Recorder::default()).unwrap_err();
        assert_eq!(e.code, ErrorCode::Refused);
        assert!(e.message.contains("not available in this build"), "{}", e.message);
    }
    assert!(offline.requests().is_empty(), "nothing was asked");
    assert!(!t.0.join("game/kiln/vendor").exists(), "nothing was written");
    // one unpinned file is enough
    let (a, b) = (bytes(1, 10), bytes(2, 10));
    let mut c = amd(&manifest_for(&a, &b));
    c.files[1].sha256 = NOT_PINNED.into();
    assert!(!c.pinned());
    assert_eq!(Store::at(t.0.join("game")).component_status(&c, &Agreements::default()).state, VendorState::Unavailable);
}

#[test]
fn redirects_go_only_to_github_asset_storage_and_only_from_a_release_asset() {
    let lab = Lab::new("redirect");
    let data = bytes(3, 2_000);
    // a raw file that redirects: refused, nothing kept
    let c = amd(&manifest_with("2.4.0", &[("a.dll", A_URL, &data)]));
    lab.server.on(A_URL, &[Behave::Redirect("https://objects.githubusercontent.com/x/a.dll".into())]);
    let e = lab.get(&c, &mut Recorder::default()).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused);
    assert!(e.message.contains("redirect"), "{}", e.message);
    assert_eq!(lab.server.seen().len(), 1);

    // a release asset: one redirect to GitHub's asset storage is followed, without anything but the user agent
    let asset = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.4.0/a.zip";
    let storage = "https://release-assets.githubusercontent.com/github-production-release-asset/1/2";
    let c = amd(&manifest_with("2.4.0", &[("a.zip", asset, &data)]));
    lab.server.on(asset, &[Behave::Redirect(storage.into())]);
    lab.server.on(storage, &[Behave::Serve(data.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(std::fs::read(lab.folder().join("a.zip")).unwrap(), data);
    let last = lab.server.seen().last().unwrap().clone();
    assert_eq!(last.host, "release-assets.githubusercontent.com");

    // anywhere else, or a second hop: refused
    let lab = Lab::new("redirect2");
    lab.server.on(asset, &[Behave::Redirect("https://evil.example/a.zip".into())]);
    assert_eq!(lab.get(&c, &mut Recorder::default()).unwrap_err().code, ErrorCode::Refused);
    lab.server.on(asset, &[Behave::Redirect(storage.into())]);
    lab.server.on(storage, &[Behave::Redirect("https://objects.githubusercontent.com/again".into())]);
    assert_eq!(lab.get(&c, &mut Recorder::default()).unwrap_err().code, ErrorCode::Refused);
    lab.server.on(asset, &[Behave::Redirect("http://release-assets.githubusercontent.com/a.zip".into())]);
    assert_eq!(lab.get(&c, &mut Recorder::default()).unwrap_err().code, ErrorCode::Refused);
    assert!(!lab.server.seen().iter().any(|s| s.host == "evil.example"), "a refused redirect is never asked");
    assert!(!lab.folder().join("a.zip").exists());
}

#[test]
fn the_policy_reaches_githubs_download_hosts_and_nothing_else() {
    let p = download::policy();
    let mut hosts: Vec<&str> = p.hosts.iter().map(|h| h.host.as_str()).collect();
    hosts.sort();
    assert_eq!(hosts, ["github.com", "media.githubusercontent.com", "objects.githubusercontent.com", "raw.githubusercontent.com", "release-assets.githubusercontent.com"]);
    assert!(p.hosts.iter().all(|h| !h.credentials), "no host gets a credential");
    // a URL outside the policy is refused before anything is sent
    let offline = net::fake::FakeTransport::offline();
    let client = Client::new(p, &offline, &download::user_agent());
    let e = client.download(net::Request::get("https://example.com/x.dll"), &mut Vec::new()).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused);
    assert!(offline.requests().is_empty());
}

#[test]
fn an_update_keeps_the_old_version_until_the_new_one_verified() {
    let lab = Lab::new("update");
    let old = bytes(1, 3_000);
    let v1 = amd(&manifest_with("2.4.0", &[("a.dll", A_URL, &old)]));
    lab.server.on(A_URL, &[Behave::Serve(old.clone())]);
    lab.get(&v1, &mut Recorder::default()).unwrap();
    let new = bytes(9, 4_000);
    let v2 = amd(&manifest_with("2.5.0", &[("a.dll", A_URL, &new)]));
    let url2 = A_URL.replace("v2.4.0", "v2.5.0");
    let s = lab.store().component_status(&v2, &Agreements::default());
    assert_eq!(s.state, VendorState::UpdateAvailable);
    assert_eq!(s.installed.as_ref().unwrap().version, "2.4.0");
    // the new version fails: the old one stays as it was
    lab.server.on(&url2, &[Behave::Serve(bytes(8, 4_000))]);
    assert_eq!(lab.get(&v2, &mut Recorder::default()).unwrap_err().code, ErrorCode::HashMismatch);
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), old);
    assert!(lab.folder().join(RECEIPT).exists());
    assert_eq!(lab.store().component_status(&v2, &Agreements::default()).state, VendorState::UpdateAvailable);
    // it verifies: then the old one goes
    lab.server.on(&url2, &[Behave::Serve(new.clone())]);
    let r = lab.get(&v2, &mut Recorder::default()).unwrap();
    assert_eq!(r.replaced, ["kiln/vendor/amd/2.4.0"]);
    assert!(!lab.folder().exists());
    let s = lab.store().component_status(&v2, &Agreements::default());
    assert_eq!((s.state, s.installed.unwrap().version.as_str()), (VendorState::Downloaded, "2.5.0"));
    assert_eq!(std::fs::read(lab.game().join("kiln/vendor/amd/2.5.0/a.dll")).unwrap(), new);
    // the old manifest (an older launcher) reads the newer one as downloaded, not as an update to undo
    assert_eq!(lab.store().component_status(&v1, &Agreements::default()).state, VendorState::Downloaded);
}

#[test]
fn a_missing_or_changed_file_reads_as_damaged_and_a_download_mends_only_it() {
    let lab = Lab::new("damaged");
    let (a, b) = (bytes(1, 5_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    std::fs::remove_file(lab.folder().join("b.dll")).unwrap();
    let s = lab.store().component_status(&c, &Agreements::default());
    assert_eq!(s.state, VendorState::Damaged);
    assert_eq!(s.reason.as_deref(), Some("b.dll of it is missing or changed. Download it again."));
    assert_eq!(s.installed.as_ref().unwrap().problems, ["b.dll"]);
    assert_eq!(s.installed.unwrap().size_on_disk, 5_000);
    let r = lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!((r.kept.as_slice(), r.fetched), (&["a.dll".to_string()][..], 300));
    // a file of the right size and the wrong content: found by the download's check, fetched again
    std::fs::write(lab.folder().join("a.dll"), vec![0u8; 5_000]).unwrap();
    assert_eq!(lab.store().component_status(&c, &Agreements::default()).state, VendorState::Downloaded, "the status does not hash");
    let r = lab.get(&c, &mut Recorder::default()).unwrap();
    assert_eq!(r.kept, ["b.dll"]);
    assert_eq!(std::fs::read(lab.folder().join("a.dll")).unwrap(), a);
    // a receipt of other hashes than this build's is not this build's download
    let mut other = c.clone();
    other.files[0].sha256 = "ab".repeat(32);
    assert_eq!(lab.store().component_status(&other, &Agreements::default()).state, VendorState::Damaged);
}

#[test]
fn remove_deletes_the_component_and_leaves_what_kiln_did_not_put_there() {
    let lab = Lab::new("remove");
    let (a, b) = (bytes(1, 5_000), bytes(2, 300));
    let c = amd(&manifest_for(&a, &b));
    lab.server.on(A_URL, &[Behave::Serve(a.clone())]);
    lab.server.on(B_URL, &[Behave::Serve(b.clone())]);
    lab.get(&c, &mut Recorder::default()).unwrap();
    std::fs::write(lab.folder().join("notes.txt"), "mine").unwrap();
    let receipt = std::fs::metadata(lab.folder().join(RECEIPT)).unwrap().len();
    let r = lab.store().remove(&c).unwrap();
    assert_eq!(r.freed, 5_300 + receipt);
    assert_eq!(names_in(&lab.folder()), ["notes.txt"]);
    assert_eq!(r.kept, ["kiln/vendor/amd/2.4.0/notes.txt is not a file Kiln put there, so Kiln left it and the folder."]);
    assert_eq!(lab.store().component_status(&c, &Agreements::default()).state, VendorState::NotDownloaded);
    std::fs::remove_file(lab.folder().join("notes.txt")).unwrap();
    // an interrupted download goes too, then the folders
    std::fs::write(lab.folder().join("a.dll.part"), [1, 2, 3]).unwrap();
    let r = lab.store().remove(&c).unwrap();
    assert_eq!(r.removed, ["kiln/vendor/amd/2.4.0"]);
    assert!(r.kept.is_empty());
    assert!(!lab.game().join("kiln/vendor/amd").exists());
    assert_eq!(names_in(&lab.game().join("kiln/vendor")), [".lock"]);
    // nothing to remove: no error
    assert_eq!(lab.store().remove(&c).unwrap(), Default::default());
}

#[test]
fn one_download_or_removal_at_a_time_per_game_folder() {
    let lab = Lab::new("lock");
    let (a, b) = (bytes(1, 50), bytes(2, 50));
    let c = amd(&manifest_for(&a, &b));
    let held = lab.store().lock().unwrap();
    assert_eq!(lab.get(&c, &mut Recorder::default()).unwrap_err().code, ErrorCode::Busy);
    assert_eq!(lab.store().remove(&c).unwrap_err().code, ErrorCode::Busy);
    assert!(lab.server.seen().is_empty());
    drop(held);
    lab.server.on(A_URL, &[Behave::Serve(a)]);
    lab.server.on(B_URL, &[Behave::Serve(b)]);
    lab.get(&c, &mut Recorder::default()).unwrap();
}

#[test]
fn a_game_folder_without_kiln_or_with_a_link_on_the_way_is_refused() {
    let lab = Lab::new("guard");
    let (a, b) = (bytes(1, 50), bytes(2, 50));
    let c = amd(&manifest_for(&a, &b));
    let bare = Store::at(lab.t.0.join("no-kiln"));
    std::fs::create_dir_all(lab.t.0.join("no-kiln")).unwrap();
    assert_eq!(download::download(&bare, &c, &lab.client(), "t", &mut Recorder::default()).unwrap_err().code, ErrorCode::NotFound);
    #[cfg(unix)]
    {
        let elsewhere = lab.t.0.join("elsewhere");
        std::fs::create_dir_all(&elsewhere).unwrap();
        std::os::unix::fs::symlink(&elsewhere, lab.game().join("kiln/vendor")).unwrap();
        assert_eq!(lab.get(&c, &mut Recorder::default()).unwrap_err().code, ErrorCode::Refused);
        assert!(names_in(&elsewhere).is_empty(), "nothing was written through the link");
        assert!(lab.server.seen().is_empty());
    }
}

#[test]
fn a_dedicated_server_has_no_vendor_store() {
    let t = tmp("server");
    let mut g = GameInstall {
        id: "s".into(), kind: GameKind::Server, path: t.0.join("game"), exe: "enshrouded_server.exe".into(), app_id: kiln_manage::SERVER_APP_ID,
        library: None, platform: Platform::Wine, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy,
        source: "manual".into(),
    };
    let e = Store::for_game(&g).unwrap_err();
    assert_eq!(e.code, ErrorCode::Unsupported);
    assert!(e.message.contains("draws no frames"));
    g.kind = GameKind::Client;
    assert_eq!(Store::for_game(&g).unwrap().root(), t.0.join("game/kiln/vendor"));
}

#[test]
fn the_licence_agreement_is_what_the_status_reports() {
    let t = tmp("agree");
    let dirs = DataDirs::at(t.0.join("data"));
    let m = manifest::builtin().unwrap();
    let c = m.component("intel-xess").unwrap();
    let store = Store::at(t.0.join("game"));
    assert!(!store.component_status(c, &Agreements::load(&dirs)).license.agreed);
    agreements::record(&dirs, c, &c.license.sha256).unwrap();
    let s = store.component_status(c, &Agreements::load(&dirs));
    assert!(s.license.agreed && s.license.agreed_at.is_some());
    assert_eq!(s.license.name, "Intel Simplified Software License (October 2022)");
    assert!(!store.component_status(m.component("amd-fsr").unwrap(), &Agreements::load(&dirs)).license.agreed);
}
