//! Signing in with Discord, always through the relay on frostdev.io (PLAN 3.3). The launcher never sees a Discord token.
//!
//! **Desktop (browser):** a one-time listener on `127.0.0.1:<random port>` ([`Loopback`]); `POST /signin` with the
//! port and the challenge; the system browser opens the answer's page; after Discord the relay sends the browser to
//! `http://127.0.0.1:<port>/done?id=<id>&c=<one-time code>` (or `&error=canceled|denied|expired`); the launcher trades the
//! code and the secret for a session (`POST /signin/exchange`). When the loopback is blocked (a firewall, a sandbox)
//! the attempt also polls the relay with the secret, as the Deck does.
//!
//! **Deck (QR):** `POST /signin` without a port; the Deck shows the QR code and the short code, and polls
//! (`POST /signin/poll`, at most every 2 seconds) until the phone approves or declines.
//!
//! The secret is 32 random bytes, sent only as `base64url(sha256(secret))` at the start and as itself to the exchange
//! and the polls, so the session goes only to whoever started the attempt. An attempt lasts at most 10 minutes.

use super::relay::{Grant, PollState, Relay, SigninMode, SigninRequest, SigninStarted};
use crate::{Error, ErrorCode, Result};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::time::{Duration, Instant};

/// How long an attempt may take (the relay's codes live as long).
pub const ATTEMPT_MAX: Duration = Duration::from_secs(10 * 60);
/// The Deck's polls are at least this far apart.
pub const POLL_EVERY: Duration = Duration::from_secs(2);
/// The desktop polls the relay too, after this, in case the loopback never hears back.
pub const FALLBACK_AFTER: Duration = Duration::from_secs(8);

/// The attempt's secret.
pub struct Secret(String);

impl Secret {
    pub fn new() -> Result<Secret> {
        Ok(Secret(super::b64url(&crate::util::random_bytes::<32>()?)))
    }

    /// `base64url(sha256(secret))`.
    pub fn challenge(&self) -> String {
        super::b64url(&Sha256::digest(self.0.as_bytes()))
    }

    pub fn reveal(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Debug for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Secret(<hidden>)")
    }
}

/// What the browser tab says after the round trip (the UI passes its own words, from en.json).
#[derive(Debug, Clone, Default, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PageTexts {
    /// "Signed in. You can close this tab and go back to the launcher."
    pub done: String,
    /// "The sign-in did not finish. You can close this tab and go back to the launcher."
    pub failed: String,
}

/// How the browser round trip ended at the loopback.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Back {
    Code(String),
    Canceled,
    Denied,
    Expired,
}

/// The desktop's one-time listener.
pub struct Loopback {
    listener: TcpListener,
    port: u16,
}

impl Loopback {
    /// A random free port on 127.0.0.1 (never another address).
    pub fn bind() -> Result<Loopback> {
        let listener = TcpListener::bind(("127.0.0.1", 0)).map_err(|_| Error::new(ErrorCode::Busy, "The launcher could not open its sign-in listener."))?;
        listener.set_nonblocking(true)?;
        let port = listener.local_addr()?.port();
        Ok(Loopback { listener, port })
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    /// One request, if one is waiting (`None`: nobody knocked). `Some(Some(_))` when it was the relay's redirect for
    /// attempt `id`; anything else (another path, another attempt, another Host, not a GET) is answered 404 and
    /// changes nothing (`Some(None)`).
    pub fn accept(&self, id: &str, texts: &PageTexts) -> Option<Option<Back>> {
        let (mut stream, peer) = self.listener.accept().ok()?;
        if !peer.ip().is_loopback() {
            return Some(None);
        }
        let _ = stream.set_nonblocking(false);
        let _ = stream.set_read_timeout(Some(Duration::from_millis(1500)));
        let _ = stream.set_write_timeout(Some(Duration::from_millis(1500)));
        let back = read_back(&mut stream, self.port, id);
        let (status, text) = match &back {
            Some(Back::Code(_)) => ("200 OK", texts.done.as_str()),
            Some(_) => ("200 OK", texts.failed.as_str()),
            None => ("404 Not Found", ""),
        };
        let page = if text.is_empty() { String::new() } else { page(text) };
        let _ = write!(stream, "HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{page}", page.len());
        Some(back)
    }
}

fn page(text: &str) -> String {
    let esc = text.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    format!("<!doctype html><meta charset=\"utf-8\"><title>Kiln</title><body style=\"margin:0;display:grid;place-items:center;height:100vh;background:#170803;color:#f5e9db;font:18px system-ui,sans-serif\"><p style=\"max-width:32em;padding:0 24px;text-align:center\">{esc}</p></body>")
}

fn read_back(stream: &mut TcpStream, port: u16, id: &str) -> Option<Back> {
    let mut bytes = Vec::new();
    let mut buf = [0u8; 1024];
    let started = Instant::now();
    while bytes.len() <= 8192 && !bytes.windows(4).any(|w| w == b"\r\n\r\n") && started.elapsed() < Duration::from_secs(2) {
        match stream.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => bytes.extend_from_slice(&buf[..n]),
        }
    }
    let text = std::str::from_utf8(&bytes).ok()?;
    let mut lines = text.split("\r\n");
    let target = lines.next()?.strip_prefix("GET ")?.strip_suffix(" HTTP/1.1")?;
    let hosts: Vec<&str> = lines.filter_map(|l| l.split_once(':')).filter(|(k, _)| k.trim().eq_ignore_ascii_case("host")).map(|(_, v)| v.trim()).collect();
    if hosts != [format!("127.0.0.1:{port}").as_str()] {
        return None;
    }
    let query = target.strip_prefix("/done?")?;
    let mut got_id = None;
    let mut code = None;
    let mut error = None;
    for pair in query.split('&') {
        let (k, v) = pair.split_once('=')?;
        let slot = match k {
            "id" => &mut got_id,
            "c" => &mut code,
            "error" => &mut error,
            _ => continue,
        };
        if slot.is_some() {
            return None;
        }
        *slot = Some(v);
    }
    // PLAN 3.3's redirect names only the code (the port is this attempt's own); an id, when there is one, must match
    if got_id.is_some_and(|g| g != id) {
        return None;
    }
    let token_ok = |s: &str| !s.is_empty() && s.len() <= 512 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.');
    match (code, error) {
        (Some(c), None) if token_ok(c) => Some(Back::Code(c.to_string())),
        (None, Some("canceled" | "cancelled" | "access_denied")) => Some(Back::Canceled),
        (None, Some("denied")) => Some(Back::Denied),
        (None, Some("expired")) => Some(Back::Expired),
        _ => None,
    }
}

/// Where an attempt is, as the UI polls it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Progress {
    Waiting,
    Approved(Grant),
    Denied,
    Expired,
    Canceled,
}

/// One sign-in attempt, from start to its end. The launcher keeps it while the dialog is open and polls it.
pub struct Attempt {
    pub started: SigninStarted,
    pub mode: SigninMode,
    secret: Secret,
    loopback: Option<Loopback>,
    texts: PageTexts,
    begun: Instant,
    last_poll: Option<Instant>,
    done: Option<Progress>,
}

impl Attempt {
    /// Starts an attempt: binds the loopback (browser mode) and asks the relay.
    pub fn start(relay: &Relay, mode: SigninMode, join: bool, device: super::relay::Device, version: &str, texts: PageTexts) -> Result<Attempt> {
        let secret = Secret::new()?;
        let loopback = if mode == SigninMode::Browser { Some(Loopback::bind()?) } else { None };
        let req = SigninRequest { mode, join, challenge: secret.challenge(), port: loopback.as_ref().map(Loopback::port), device, version: version.to_string() };
        let started = relay.signin_start(&req)?;
        Ok(Attempt { started, mode, secret, loopback, texts, begun: Instant::now(), last_poll: None, done: None })
    }

    /// Looks once: the loopback (browser), the relay (QR; browser after [`FALLBACK_AFTER`]), the clock. Never waits
    /// for more than one request. An ending is kept: later calls answer the same.
    pub fn poll(&mut self, relay: &Relay) -> Result<Progress> {
        if let Some(d) = &self.done {
            return Ok(d.clone());
        }
        if self.begun.elapsed() > ATTEMPT_MAX {
            return Ok(self.end(Progress::Expired));
        }
        if let Some(l) = &self.loopback {
            // drain what the browser sent; the redirect for this attempt ends it
            for _ in 0..8 {
                match l.accept(&self.started.id, &self.texts) {
                    None => break,
                    Some(None) => continue,
                    Some(Some(Back::Code(code))) => {
                        let grant = relay.signin_exchange(&self.started.id, &code, self.secret.reveal())?;
                        return Ok(self.end(Progress::Approved(grant)));
                    }
                    Some(Some(Back::Canceled)) => return Ok(self.end(Progress::Canceled)),
                    Some(Some(Back::Denied)) => return Ok(self.end(Progress::Denied)),
                    Some(Some(Back::Expired)) => return Ok(self.end(Progress::Expired)),
                }
            }
        }
        let ask = match self.mode {
            SigninMode::Qr => true,
            SigninMode::Browser => self.begun.elapsed() >= FALLBACK_AFTER,
        };
        let spaced = self.last_poll.is_none_or(|t| t.elapsed() >= if self.mode == SigninMode::Qr { POLL_EVERY } else { POLL_EVERY * 2 });
        if ask && spaced {
            self.last_poll = Some(Instant::now());
            let p = match relay.signin_poll(&self.started.id, self.secret.reveal())? {
                PollState::Waiting => return Ok(Progress::Waiting),
                PollState::Approved(g) => Progress::Approved(g),
                PollState::Denied => Progress::Denied,
                PollState::Expired => Progress::Expired,
                PollState::Canceled => Progress::Canceled,
            };
            return Ok(self.end(p));
        }
        Ok(Progress::Waiting)
    }

    fn end(&mut self, p: Progress) -> Progress {
        self.loopback = None;
        self.done = Some(p.clone());
        p
    }
}

#[cfg(test)]
mod tests {
    use super::super::relay::Device;
    use super::super::settings::{relay_base, DEFAULT_RELAY};
    use super::super::testing::{ok, FakeRelay};
    use super::*;
    use serde_json::json;

    fn texts() -> PageTexts {
        PageTexts { done: "Signed in. You can close this tab and go back to the launcher.".into(), failed: "The sign-in did not finish.".into() }
    }

    fn account() -> serde_json::Value {
        json!({ "id": "731204485922648124", "name": "emberfox", "member": true, "tester": false })
    }

    /// What the browser does after Discord: GET the loopback with the relay's redirect.
    fn browser(port: u16, query: &str, host: Option<&str>) -> String {
        let mut c = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let host = host.map(str::to_string).unwrap_or(format!("127.0.0.1:{port}"));
        write!(c, "GET /done?{query} HTTP/1.1\r\nHost: {host}\r\n\r\n").unwrap();
        let mut out = String::new();
        let _ = c.read_to_string(&mut out);
        out
    }

    /// The browser knocks while the launcher polls; returns the page the tab shows and the attempt's state after.
    fn knock(a: &mut Attempt, relay: &Relay, query: &str, host: Option<&str>) -> (String, Progress) {
        let port = a.loopback.as_ref().unwrap().port();
        let (q, h) = (query.to_string(), host.map(str::to_string));
        let tab = std::thread::spawn(move || browser(port, &q, h.as_deref()));
        let mut got = Progress::Waiting;
        while !tab.is_finished() {
            got = a.poll(relay).unwrap();
            std::thread::sleep(Duration::from_millis(10));
        }
        (tab.join().unwrap(), got)
    }

    #[test]
    fn the_challenge_is_the_secrets_sha256() {
        let s = Secret::new().unwrap();
        assert_eq!(s.reveal().len(), 43);
        assert_eq!(s.challenge(), super::super::b64url(&Sha256::digest(s.reveal().as_bytes())));
        assert!(!format!("{s:?}").contains(s.reveal()));
    }

    #[test]
    fn the_desktop_round_trip_trades_the_code_and_secret_for_a_session() {
        let fake = FakeRelay::start(|r| match r.path() {
            "/signin" => ok(json!({ "id": "s1", "url": "https://frostdev.io/kiln/signin/s1", "expiresAt": "2026-11-18T12:10:00Z" })),
            "/signin/exchange" => ok(json!({ "session": "kiln_s_0123456789abcdefghijklmnop", "account": account() })),
            _ => super::super::testing::status(404, json!({})),
        });
        let t = fake.transport();
        let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0");
        let mut a = Attempt::start(&relay, SigninMode::Browser, true, Device::Pc, "0.2.0", texts()).unwrap();
        let port = a.loopback.as_ref().unwrap().port();
        let start = fake.seen()[0].json();
        assert_eq!(start["mode"], "browser");
        assert_eq!(start["join"], true);
        assert_eq!(start["port"], port);
        assert_eq!(start["device"], "desktop");
        // not the redirect: another attempt, a wrong Host, a second code; each answered 404, the attempt waits
        for (q, host) in [("id=s2&c=abc", None), ("id=s1&c=abc", Some("evil.example")), ("id=s1&c=a&c=b", None)] {
            let (page, got) = knock(&mut a, &relay, q, host);
            assert!(page.starts_with("HTTP/1.1 404"), "{q}: {page}");
            assert_eq!(got, Progress::Waiting);
        }
        // the redirect (PLAN 3.3: the code alone)
        let (page, got) = knock(&mut a, &relay, "c=onetime-code_1", None);
        assert!(page.starts_with("HTTP/1.1 200") && page.contains("Signed in. You can close this tab"), "{page}");
        let Progress::Approved(g) = got else { panic!("{got:?}") };
        assert_eq!(g.account.name, "emberfox");
        let ex = fake.seen().into_iter().find(|s| s.path() == "/signin/exchange").unwrap().json();
        assert_eq!((ex["id"].as_str(), ex["code"].as_str()), (Some("s1"), Some("onetime-code_1")));
        // the verifier is the secret behind the challenge
        let verifier = ex["verifier"].as_str().unwrap();
        assert_eq!(start["challenge"].as_str().unwrap(), super::super::b64url(&Sha256::digest(verifier.as_bytes())));
        // the listener is gone with the attempt's end
        assert!(TcpStream::connect(("127.0.0.1", port)).is_err());
    }

    #[test]
    fn the_session_of_an_approved_attempt_is_kept_and_signs_the_next_calls() {
        use super::super::session::{SessionStore, Stored};
        let fake = FakeRelay::start(|r| match r.path() {
            "/signin" => ok(json!({ "id": "q1", "code": "HV3R-9TCW", "url": "https://frostdev.io/link/HV3R9TCW", "expiresAt": "2026-11-18T12:10:00Z" })),
            "/signin/poll" => ok(json!({ "state": "approved", "session": "kiln_s_0123456789abcdefghijklmnop", "account": account() })),
            "/me" if r.header("authorization") == Some("Bearer kiln_s_0123456789abcdefghijklmnop") => ok(json!({ "account": account() })),
            _ => super::super::testing::status(401, json!({})),
        });
        let t = fake.transport();
        let base = relay_base(DEFAULT_RELAY).unwrap();
        let relay = Relay::new(base.clone(), &t, "0.2.0");
        let mut a = Attempt::start(&relay, SigninMode::Qr, false, Device::Deck, "0.2.0", texts()).unwrap();
        let Progress::Approved(g) = a.poll(&relay).unwrap() else { panic!() };
        let dir = super::super::testing::tmp("signin-kept");
        let store = SessionStore::with_vault(&crate::paths::DataDirs::at(dir.path()), None);
        store.save(&Stored { format: 1, relay: base.host.clone(), session: g.session.clone(), account: g.account.clone() }).unwrap();
        let kept = store.load(&base.host).unwrap();
        assert_eq!(Relay::new(base, &t, "0.2.0").with_session(&kept.session).me().unwrap().name, "emberfox");
        // no Discord token anywhere in what was kept: the relay's key and the account only
        let text = std::fs::read_to_string(store.file_path()).unwrap();
        assert!(!text.contains("access_token") && !text.contains("refresh_token"));
    }

    #[test]
    fn cancelled_on_discords_page_ends_the_attempt_without_an_exchange() {
        let fake = FakeRelay::start(|_| ok(json!({ "id": "s1", "url": "https://frostdev.io/kiln/signin/s1", "expiresAt": "2026-11-18T12:10:00Z" })));
        let t = fake.transport();
        let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0");
        let mut a = Attempt::start(&relay, SigninMode::Browser, false, Device::Pc, "0.2.0", texts()).unwrap();
        let (page, got) = knock(&mut a, &relay, "id=s1&error=canceled", None);
        assert!(page.contains("did not finish"));
        assert_eq!(got, Progress::Canceled);
        assert_eq!(fake.seen().len(), 1, "only the start");
    }

    #[test]
    fn the_deck_polls_with_its_secret_until_the_phone_approves() {
        let polls = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let p2 = polls.clone();
        let fake = FakeRelay::start(move |r| match r.path() {
            "/signin" => ok(json!({ "id": "q1", "code": "HV3R-9TCW", "url": "https://frostdev.io/link/HV3R9TCW", "expiresAt": "2026-11-18T12:10:00Z" })),
            "/signin/poll" => {
                if p2.fetch_add(1, std::sync::atomic::Ordering::SeqCst) == 0 { ok(json!({ "state": "waiting" })) }
                else { ok(json!({ "state": "approved", "session": "kiln_s_0123456789abcdefghijklmnop", "account": account() })) }
            }
            _ => super::super::testing::status(404, json!({})),
        });
        let t = fake.transport();
        let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0");
        let mut a = Attempt::start(&relay, SigninMode::Qr, false, Device::Deck, "0.2.0", texts()).unwrap();
        assert_eq!(a.started.code, "HV3R-9TCW");
        assert!(fake.seen()[0].json().get("port").is_none());
        assert_eq!(a.poll(&relay).unwrap(), Progress::Waiting);
        // a second look within 2 s does not ask the relay again
        assert_eq!(a.poll(&relay).unwrap(), Progress::Waiting);
        assert_eq!(polls.load(std::sync::atomic::Ordering::SeqCst), 1);
        a.last_poll = Some(Instant::now() - POLL_EVERY);
        assert!(matches!(a.poll(&relay).unwrap(), Progress::Approved(_)));
        let poll = fake.seen().into_iter().find(|s| s.path() == "/signin/poll").unwrap().json();
        assert_eq!(poll["id"], "q1");
        assert_eq!(fake.seen()[0].json()["challenge"].as_str().unwrap(), super::super::b64url(&Sha256::digest(poll["secret"].as_str().unwrap().as_bytes())));
        // the ending is kept: no more requests
        let n = fake.seen().len();
        assert!(matches!(a.poll(&relay).unwrap(), Progress::Approved(_)));
        assert_eq!(fake.seen().len(), n);
    }

    #[test]
    fn declined_and_expired_codes_end_the_attempt() {
        for (answer, want) in [(json!({ "state": "denied" }), Progress::Denied), (json!({ "state": "expired" }), Progress::Expired)] {
            let a2 = answer.clone();
            let fake = FakeRelay::start(move |r| if r.path() == "/signin" {
                ok(json!({ "id": "q1", "code": "HV3R-9TCW", "url": "https://frostdev.io/link/HV3R9TCW", "expiresAt": "2026-11-18T12:10:00Z" }))
            } else { ok(a2.clone()) });
            let t = fake.transport();
            let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0");
            let mut a = Attempt::start(&relay, SigninMode::Qr, false, Device::Deck, "0.2.0", texts()).unwrap();
            assert_eq!(a.poll(&relay).unwrap(), want);
        }
    }

    #[test]
    fn an_attempt_older_than_ten_minutes_is_expired_without_asking() {
        let fake = FakeRelay::start(|_| ok(json!({ "id": "q1", "code": "HV3R-9TCW", "url": "https://frostdev.io/link/HV3R9TCW", "expiresAt": "2026-11-18T12:10:00Z" })));
        let t = fake.transport();
        let relay = Relay::new(relay_base(DEFAULT_RELAY).unwrap(), &t, "0.2.0");
        let mut a = Attempt::start(&relay, SigninMode::Qr, false, Device::Deck, "0.2.0", texts()).unwrap();
        a.begun = Instant::now() - ATTEMPT_MAX - Duration::from_secs(1);
        assert_eq!(a.poll(&relay).unwrap(), Progress::Expired);
        assert_eq!(fake.seen().len(), 1);
    }
}
