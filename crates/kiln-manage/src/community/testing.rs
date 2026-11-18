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
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl FakeRelay {
    pub fn start(handler: impl Fn(&Seen) -> Answer + Send + Sync + 'static) -> FakeRelay {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let seen = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let handler: Arc<Handler> = Arc::new(handler);
        let (s2, st2) = (seen.clone(), stop.clone());
        let thread = std::thread::spawn(move || {
            for conn in listener.incoming() {
                if st2.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(mut c) = conn else { continue };
                let _ = c.set_read_timeout(Some(std::time::Duration::from_secs(5)));
                if let Some(req) = read_request(&mut c) {
                    let a = handler(&req);
                    s2.lock().unwrap().push(req);
                    let reason = match a.status { 200 => "OK", 201 => "Created", 204 => "No Content", _ => "Answer" };
                    let ctype = if a.body.is_empty() { "" } else { "Content-Type: application/json\r\n" };
                    let _ = write!(c, "HTTP/1.1 {} {reason}\r\n{ctype}Content-Length: {}\r\nConnection: close\r\n\r\n{}", a.status, a.body.len(), a.body);
                }
            }
        });
        FakeRelay { addr, seen, stop, thread: Some(thread) }
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
