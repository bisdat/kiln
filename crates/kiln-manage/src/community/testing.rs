//! Test helpers: a temp folder, and a fake relay: a real HTTP/1.1 server on 127.0.0.1 (a port the test opened) that
//! records every request and answers from a handler. The launcher's relay client reaches it through
//! [`super::relay::LoopbackTransport`], so the requests go through the same policy checks and the same bytes on a socket
//! as with the real relay. Nothing here leaves the machine.

use super::relay::LoopbackTransport;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

pub(crate) fn tmp(tag: &str) -> crate::library::TempDir {
    crate::library::TempDir::new(&std::env::temp_dir(), &format!("kiln-community-{tag}")).unwrap()
}

/// One request as the fake relay saw it.
#[derive(Debug, Clone)]
pub(crate) struct Seen {
    pub method: String,
    /// Path and query as sent (`/kiln/relay/posts`).
    pub target: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Seen {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    pub fn json(&self) -> serde_json::Value {
        serde_json::from_slice(&self.body).unwrap_or(serde_json::Value::Null)
    }
    /// The relay's path without its base (`/posts`).
    pub fn path(&self) -> &str {
        let t = self.target.split('?').next().unwrap_or("");
        t.strip_prefix("/kiln/relay").unwrap_or(t)
    }
}

pub(crate) struct Answer {
    pub status: u16,
    pub body: String,
}

pub(crate) fn ok(body: serde_json::Value) -> Answer {
    Answer { status: 200, body: body.to_string() }
}
pub(crate) fn status(status: u16, body: serde_json::Value) -> Answer {
    Answer { status, body: if body.is_null() { String::new() } else { body.to_string() } }
}

type Handler = dyn Fn(&Seen) -> Answer + Send + Sync;

pub(crate) struct FakeRelay {
    addr: SocketAddr,
    seen: Arc<Mutex<Vec<Seen>>>,
    broken: Arc<Mutex<Vec<String>>>,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl FakeRelay {
    pub fn start(handler: impl Fn(&Seen) -> Answer + Send + Sync + 'static) -> FakeRelay {
        Self::run(handler, false)
    }

    /// A fake relay that holds every request to the real relay's own rules first ([`relay_rules`]): one that breaks
    /// them is answered as the relay answers it (400 `bad-request`) and kept in [`FakeRelay::broken`].
    pub fn strict(handler: impl Fn(&Seen) -> Answer + Send + Sync + 'static) -> FakeRelay {
        Self::run(handler, true)
    }

    fn run(handler: impl Fn(&Seen) -> Answer + Send + Sync + 'static, strict: bool) -> FakeRelay {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let seen = Arc::new(Mutex::new(Vec::new()));
        let broken = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let handler: Arc<Handler> = Arc::new(handler);
        let (s2, b2, st2) = (seen.clone(), broken.clone(), stop.clone());
        let thread = std::thread::spawn(move || {
            for conn in listener.incoming() {
                if st2.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(mut c) = conn else { continue };
                let _ = c.set_read_timeout(Some(std::time::Duration::from_secs(5)));
                if let Some(req) = read_request(&mut c) {
                    let bad = if strict { relay_rules(&req) } else { Vec::new() };
                    let a = if bad.is_empty() {
                        handler(&req)
                    } else {
                        let a = status(400, serde_json::json!({ "error": "bad-request", "message": bad.join("; ") }));
                        b2.lock().unwrap().extend(bad.into_iter().map(|b| format!("{} {}: {b}", req.method, req.target)));
                        a
                    };
                    s2.lock().unwrap().push(req);
                    let reason = match a.status { 200 => "OK", 201 => "Created", 204 => "No Content", _ => "Answer" };
                    let ctype = if a.body.is_empty() { "" } else { "Content-Type: application/json\r\n" };
                    let _ = write!(c, "HTTP/1.1 {} {reason}\r\n{ctype}Content-Length: {}\r\nConnection: close\r\n\r\n{}", a.status, a.body.len(), a.body);
                }
            }
        });
        FakeRelay { addr, seen, broken, stop, thread: Some(thread) }
    }

    /// The relay's rules the requests broke (a strict fake relay only).
    pub fn broken(&self) -> Vec<String> {
        self.broken.lock().unwrap().clone()
    }

    pub fn transport(&self) -> LoopbackTransport {
        LoopbackTransport::new(self.addr).unwrap()
    }

    pub fn seen(&self) -> Vec<Seen> {
        self.seen.lock().unwrap().clone()
    }
}

impl Drop for FakeRelay {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        let _ = TcpStream::connect(self.addr);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn read_request(c: &mut TcpStream) -> Option<Seen> {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    let head_end = loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break i;
        }
        let n = c.read(&mut chunk).ok()?;
        if n == 0 || buf.len() > 64 * 1024 {
            return None;
        }
        buf.extend_from_slice(&chunk[..n]);
    };
    let head = String::from_utf8_lossy(&buf[..head_end]).into_owned();
    let mut lines = head.split("\r\n");
    let mut first = lines.next()?.split(' ');
    let (method, target) = (first.next()?.to_string(), first.next()?.to_string());
    let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_string(), v.trim().to_string())).collect();
    let len: usize = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("content-length")).and_then(|(_, v)| v.parse().ok()).unwrap_or(0);
    let mut body = buf[head_end + 4..].to_vec();
    while body.len() < len {
        let n = c.read(&mut chunk).ok()?;
        if n == 0 {
            return None;
        }
        body.extend_from_slice(&chunk[..n]);
    }
    body.truncate(len);
    Some(Seen { method, target, headers, body })
}

/// The parts of a multipart/form-data body: (name, content type, bytes).
pub(crate) fn multipart(seen: &Seen) -> Vec<(String, String, Vec<u8>)> {
    let ctype = seen.header("content-type").unwrap_or("");
    let Some(boundary) = ctype.split("boundary=").nth(1) else { return Vec::new() };
    let delim = format!("--{boundary}");
    let body = &seen.body;
    let mut out = Vec::new();
    let mut at = 0;
    let find = |from: usize, pat: &[u8]| body[from..].windows(pat.len()).position(|w| w == pat).map(|i| i + from);
    while let Some(start) = find(at, delim.as_bytes()) {
        let after = start + delim.len();
        if body[after..].starts_with(b"--") {
            break;
        }
        let Some(head_end) = find(after, b"\r\n\r\n") else { break };
        let head = String::from_utf8_lossy(&body[after..head_end]).into_owned();
        let Some(next) = find(head_end + 4, format!("\r\n{delim}").as_bytes()) else { break };
        let name = head.split("name=\"").nth(1).and_then(|s| s.split('"').next()).unwrap_or("").to_string();
        let ct = head.lines().find_map(|l| l.strip_prefix("Content-Type: ")).unwrap_or("").trim().to_string();
        out.push((name, ct, body[head_end + 4..next].to_vec()));
        at = next + 2;
    }
    out
}

// ---------------------------------------------------------------- the real relay's rules

/// `[A-Za-z0-9_-]{43}`: a sign-in id, a challenge, a secret, a one-time code, a session key.
fn b64_43(s: &str) -> bool {
    s.len() == 43 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

/// relay/validate.mjs VERSION_RE: `1.2.3`, `-pre.release` of `[0-9A-Za-z]` words, `+build` of `[0-9A-Za-z.]`.
fn relay_version(v: &str) -> bool {
    let (core, meta) = v.split_once('+').map_or((v, None), |(c, m)| (c, Some(m)));
    let (nums, pre) = core.split_once('-').map_or((core, None), |(n, p)| (n, Some(p)));
    let word = |w: &str| !w.is_empty() && w.bytes().all(|b| b.is_ascii_alphanumeric());
    v.len() <= 64
        && nums.split('.').count() == 3
        && nums.split('.').all(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
        && pre.is_none_or(|p| p.split('.').all(word))
        && meta.is_none_or(|m| !m.is_empty() && m.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.'))
}

/// A one-line text as the relay takes it (relay/validate.mjs `line`): characters counted after the spaces are squeezed.
fn relay_line(v: &serde_json::Value, min: usize, max: usize) -> bool {
    v.as_str().is_some_and(|s| (min..=max).contains(&s.split_whitespace().collect::<Vec<_>>().join(" ").chars().count()))
}

/// The keys of a JSON object, each in `allowed` (the relay refuses an unknown field: 400).
fn only(v: &serde_json::Value, allowed: &[&str], what: &str, bad: &mut Vec<String>) {
    match v.as_object() {
        Some(o) => {
            for k in o.keys().filter(|k| !allowed.contains(&k.as_str())) {
                bad.push(format!("{what} has the unknown field {k}"));
            }
        }
        None => bad.push(format!("{what} is not a JSON object")),
    }
}

/// What the real relay refuses in a request (kilnbot master 3eb30a7: relay/app.mjs `route`, `readJson`, `noBody`,
/// `auth`; relay/validate.mjs), as sentences; empty when it takes it. The launcher's own promises are checked here too:
/// no cookie, and no session key on the public calls.
pub(crate) fn relay_rules(r: &Seen) -> Vec<String> {
    use serde_json::Value;
    let mut bad = Vec::new();
    let (path, query) = r.target.split_once('?').map_or((r.target.as_str(), None), |(p, q)| (p, Some(q)));
    let Some(sub) = path.strip_prefix("/kiln/relay") else { return vec!["not under /kiln/relay".into()] };
    // `:id` is a Discord snowflake
    let parts: Vec<&str> = sub.split('/').collect();
    let route = match parts.as_slice() {
        ["", "posts", id, rest @ ..] if id.len() >= 17 && id.len() <= 20 && id.bytes().all(|b| b.is_ascii_digit()) => {
            format!("/posts/:id{}", rest.iter().map(|x| format!("/{x}")).collect::<String>())
        }
        _ => sub.to_string(),
    };
    let key = format!("{} {route}", r.method);
    const ROUTES: [&str; 18] = ["POST /signin", "POST /signin/exchange", "POST /signin/poll", "GET /me", "DELETE /session", "DELETE /me", "POST /reports",
        "GET /posts", "POST /posts/:id/seen", "POST /posts/:id/solved", "DELETE /posts/:id", "GET /feed", "GET /known-issues", "GET /tester", "POST /tester",
        "DELETE /tester", "POST /tester/results", "GET /health"];
    if !ROUTES.contains(&key.as_str()) {
        return vec![format!("no such endpoint: {key}")];
    }
    // only DELETE /me (posts), GET /feed and GET /known-issues take a query
    match (key.as_str(), query) {
        (_, None) => {}
        ("DELETE /me", Some(q)) => {
            let ok = q.split('&').all(|kv| kv.split_once('=').is_some_and(|(k, v)| k == "posts" && ["1", "0", "true", "false", ""].contains(&v)));
            if !ok || q.matches("posts=").count() != 1 {
                bad.push("DELETE /me takes posts=1 or posts=0 only".into());
            }
        }
        ("GET /feed" | "GET /known-issues", Some(_)) => {}
        (_, Some(_)) => bad.push("this endpoint takes no query".into()),
    }
    let ctype = r.header("content-type").unwrap_or("").split(';').next().unwrap_or("").trim().to_ascii_lowercase();
    let no_body = || r.header("transfer-encoding").is_none() && r.header("content-length").is_none_or(|n| n == "0");
    if r.method == "GET" || r.method == "DELETE" || route.ends_with("/seen") || route.ends_with("/solved") {
        if !no_body() {
            bad.push("this request takes no body (Content-Length 0 or none, no Transfer-Encoding)".into());
        }
    }
    // the session: every call but the sign-in's and the public ones; the public ones never carry it (the launcher's own)
    let public = matches!(key.as_str(), "POST /signin" | "POST /signin/exchange" | "POST /signin/poll" | "GET /feed" | "GET /known-issues" | "GET /health");
    match (public, r.header("authorization")) {
        (true, Some(_)) => bad.push("a public call carries the session key".into()),
        (false, a) if !a.and_then(|a| a.strip_prefix("Bearer ")).is_some_and(b64_43) => bad.push("no session key of 43 base64url characters (401)".into()),
        _ => {}
    }
    if r.header("cookie").is_some() {
        bad.push("a cookie was sent".into());
    }
    if !r.header("user-agent").is_some_and(|u| u.strip_prefix("kiln-launcher/").is_some_and(|v| !v.is_empty() && !v.contains(' '))) {
        bad.push("the User-Agent is not kiln-launcher/<version>".into());
    }
    // JSON bodies: application/json, at most 8 KiB, an object with the fields the relay knows
    if matches!(key.as_str(), "POST /signin" | "POST /signin/exchange" | "POST /signin/poll" | "POST /tester") {
        if ctype != "application/json" {
            bad.push("not Content-Type: application/json (415)".into());
        }
        if r.body.len() > 8 * 1024 {
            bad.push("a JSON body over 8 KiB".into());
        }
        let v: Value = serde_json::from_slice(&r.body).unwrap_or(Value::Null);
        match key.as_str() {
            "POST /signin" => {
                only(&v, &["mode", "join", "challenge", "port", "device", "version"], "the sign-in", &mut bad);
                let mode = v["mode"].as_str().unwrap_or("");
                if !["browser", "qr"].contains(&mode) {
                    bad.push("mode must be browser or qr".into());
                }
                if !v.get("join").is_none_or(Value::is_boolean) {
                    bad.push("join must be true or false".into());
                }
                if !v["challenge"].as_str().is_some_and(b64_43) {
                    bad.push("challenge must be 43 base64url characters".into());
                }
                if let Some(p) = v.get("port").filter(|p| !p.is_null()) {
                    if mode != "browser" || !p.as_u64().is_some_and(|p| (1024..=65535).contains(&p)) {
                        bad.push("port: a browser sign-in's, 1024 to 65535".into());
                    }
                }
                if !v.get("device").is_none_or(|d| d == "desktop" || d == "deck") {
                    bad.push("device must be desktop or deck".into());
                }
                if !v.get("version").is_none_or(|x| x.is_null() || x.as_str().is_some_and(relay_version)) {
                    bad.push("version must be a launcher version".into());
                }
            }
            "POST /signin/exchange" | "POST /signin/poll" => {
                let keys: &[&str] = if key.ends_with("exchange") { &["id", "code", "verifier"] } else { &["id", "secret"] };
                only(&v, keys, "the request", &mut bad);
                for k in keys {
                    if !v[*k].as_str().is_some_and(b64_43) {
                        bad.push(format!("{k} must be 43 base64url characters"));
                    }
                }
            }
            _ => {
                only(&v, &["hardware", "beta"], "the request", &mut bad);
                if !v.get("beta").is_none_or(Value::is_boolean) {
                    bad.push("beta must be true or false".into());
                }
                let hw = &v["hardware"];
                only(hw, &["gpu", "driver", "vram", "os", "runner", "covers"], "hardware", &mut bad);
                for k in ["gpu", "driver", "vram", "os", "runner"] {
                    if !hw.get(k).is_none_or(|x| relay_line(x, if k == "gpu" { 1 } else { 0 }, 120)) {
                        bad.push(format!("hardware.{k}: one line of at most 120 characters"));
                    }
                }
                if !hw["gpu"].is_string() {
                    bad.push("hardware.gpu is needed".into());
                }
                if !hw.get("covers").is_none_or(|c| c.as_array().is_some_and(|a| a.len() <= 8 && a.iter().all(|x| relay_line(x, 1, 60)))) {
                    bad.push("hardware.covers: at most 8 items of 60 characters".into());
                }
            }
        }
    }
    // the uploads: multipart/form-data with the parts the relay knows
    if matches!(key.as_str(), "POST /reports" | "POST /tester/results") {
        if ctype != "multipart/form-data" || !r.header("content-type").is_some_and(|c| c.contains("boundary=")) {
            bad.push("not a multipart form (415)".into());
            return bad;
        }
        if r.body.len() > 9 * 1024 * 1024 {
            bad.push("a request over 9 MiB".into());
        }
        let form = multipart(r);
        let part = |n: &str| form.iter().find(|p| p.0 == n);
        let names: Vec<&str> = form.iter().map(|p| p.0.as_str()).collect();
        if key == "POST /reports" {
            for n in names.iter().filter(|n| !["draft", "zip", "report"].contains(n)) {
                bad.push(format!("the form has the unknown part {n}"));
            }
            let draft: Value = part("draft").and_then(|p| serde_json::from_slice(&p.2).ok()).unwrap_or(Value::Null);
            only(&draft, &["title", "kind", "when", "text", "zip", "known"], "the draft", &mut bad);
            if !relay_line(&draft["title"], 3, 90) {
                bad.push("the draft's title: one line of 3 to 90 characters".into());
            }
            if !["noload", "mod", "crash", "launcher", "server", "other"].contains(&draft["kind"].as_str().unwrap_or("")) {
                bad.push("the draft's kind".into());
            }
            if !draft.get("when").is_none_or(|w| w.is_null() || ["start", "menu", "load", "play", "join", "other"].contains(&w.as_str().unwrap_or(""))) {
                bad.push("the draft's when".into());
            }
            if !draft.get("text").is_none_or(|t| t.as_str().is_some_and(|t| t.chars().count() <= 1500 && t.split('\n').count() <= 40)) {
                bad.push("the draft's text: 1500 characters, 40 lines".into());
            }
            let zip = draft.get("zip").and_then(Value::as_str).unwrap_or("team");
            if !["team", "public", "none"].contains(&zip) {
                bad.push("the draft's zip".into());
            }
            if (zip == "none") != (part("report").is_some() && part("zip").is_none()) || (zip != "none" && part("zip").is_none()) {
                bad.push("zip team or public sends the part zip; none sends report (report.json alone)".into());
            }
            if part("zip").is_some_and(|p| p.2.len() > 8 * 1024 * 1024) || part("report").is_some_and(|p| p.2.len() > 512 * 1024) {
                bad.push("a part over its size".into());
            }
        } else {
            for n in names.iter().filter(|n| !["results", "zip"].contains(n)) {
                bad.push(format!("the form has the unknown part {n}"));
            }
            let res: Value = part("results").and_then(|p| serde_json::from_slice(&p.2).ok()).unwrap_or(Value::Null);
            only(&res, &["build", "steps"], "the results", &mut bad);
            if !res["build"].as_str().is_some_and(relay_version) {
                bad.push("build must be a Kiln version".into());
            }
            let steps = res["steps"].as_array().cloned().unwrap_or_default();
            if steps.is_empty() || steps.len() > 40 {
                bad.push("1 to 40 steps".into());
            }
            for st in &steps {
                only(st, &["id", "title", "result", "note"], "a step", &mut bad);
                let id = st["id"].as_str().unwrap_or("");
                if id.is_empty() || id.len() > 64 || !id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-') || id.starts_with('-') {
                    bad.push("a step's id: lower case and plain".into());
                }
                if !relay_line(&st["title"], 1, 100) || !["worked", "failed", "skipped"].contains(&st["result"].as_str().unwrap_or(""))
                    || !st.get("note").is_none_or(|n| relay_line(n, 0, 200))
                {
                    bad.push("a step's title, result or note".into());
                }
            }
        }
    }
    bad
}
